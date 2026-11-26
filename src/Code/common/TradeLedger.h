#pragma once
#include "QuestJournalDisk.h"
#include "CampaignJournal.h"
#include "TradeProperties.h"
#include "HomeSnapshot.h"
#include "HomeLocks.h"
#include "HomeObjects.h"
#include "HomePlacement.h"
#include "HomePermissions.h"
#include <tuple>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <vector>

class TradeLedger
{
public:
    struct Row
    {
        std::string user, peer, plugin;
        uint32_t base{};
        int count{}; // Positive credits the backpack; negative reserves the deposit in escrow.
        int64_t before{-1}; // -1 = legacy receipt without an inventory checkpoint.
        bool refund{}; // R record: consume only the depositor's own escrow.
        std::string variant{};
    };
    explicit TradeLedger(std::filesystem::path path) : root(std::move(path)) {}
    TradeLedger(CampaignJournal::Journal& journal,uint64_t campaign) : authority(&journal), campaignId(campaign) {}
    uint64_t Stream()
    {
        if (authority && !authority->Ready()) throw std::runtime_error("campaign trade authority unavailable");
        if (stream)
        {
            // The server ledger outlives publication of new home baselines.
            // Import only newly committed baselines, without replaying existing receipts.
            if (authority)
            {
                const auto& ops=authority->Operations();
                if (legacyScanned>ops.size()) throw std::runtime_error("campaign journal truncated");
                while (legacyScanned<ops.size())
                {
                    const auto& op=ops[legacyScanned];
                    if (op.Kind=="home-static-v1" && !LoadLegacy(op.Id))
                        throw std::runtime_error("invalid new legacy placement baseline");
                    ++legacyScanned;
                }
            }
            return stream;
        }
        if (authority)
        {
            if (!campaignId) throw std::runtime_error("invalid campaign trade stream");
            accounts.clear(); stock.clear(); contributions.clear(); homeStock.clear(); looseAccounts.clear(); placements.clear(); confirmed.clear(); last=0;
            size_t position=0;
            for (const auto& op:authority->Operations())
            {
                ++position;
                if (op.Kind=="trade-v1")
                {
                    if (op.Id!="trade-"+std::to_string(last+1)) throw std::runtime_error("invalid campaign trade sequence");
                    Replay(op.Payload,position); ++last;
                }
                else if (op.Kind=="home-static-v1")
                {
                    if (!LoadLegacy(op.Id)) throw std::runtime_error("invalid legacy placement baseline");
                }
                else if (op.Kind=="home-placement-pose-v1")
                {
                    const auto move=HomePlacement::DecodeMove(op.Payload);
                    if (!move || op.Id!="home-placement-"+std::to_string(position) || !ApplyPlacementMove(*move))
                        throw std::runtime_error("invalid placement pose");
                }
            }
            legacyScanned=authority->Operations().size();
            stream=campaignId; return stream;
        }
        uint64_t id{};
        if (std::filesystem::exists(root))
            for (const auto& file : std::filesystem::directory_iterator(root))
                if (file.is_directory())
                {
                    if (id) throw std::runtime_error("ambiguous trade ledger");
                    id = std::stoull(file.path().filename().string());
                }
        if (!id) id = QuestJournalDisk::CreateStream(root);
        if (!id) throw std::runtime_error("cannot initialize trade ledger");
        accounts.clear(); stock.clear(); contributions.clear(); homeStock.clear(); looseAccounts.clear(); placements.clear(); confirmed.clear(); last = 0;
        const auto size = QuestJournalDisk::Contiguous(root,id);
        const auto dir = root/std::to_string(id);
        for (const auto& entry : std::filesystem::directory_iterator(dir))
            if (entry.path().extension()==".tsv" && std::stoul(entry.path().stem().string())>size)
                throw std::runtime_error("trade ledger sequence gap");
        for (uint32_t n=1;n<=size;++n)
        {
            std::ifstream f(dir/(std::to_string(n)+".tsv"), std::ios::binary);
            std::string line; std::getline(f,line);
            const auto prefix=std::to_string(n)+"\t";
            if (!line.starts_with(prefix)) throw std::runtime_error("invalid trade receipt");
            Replay(line.substr(prefix.size()));
            ++last;
        }
        stream=id;
        return stream;
    }
    const std::vector<Row>& History(const std::string& user) {return accounts[user];}
    struct ItemKey
    {
        std::string first; uint32_t second{}; std::string variant;
        ItemKey(std::string plugin,uint32_t base,std::string properties={}):
            first(std::move(plugin)),second(base),variant(std::move(properties)){}
        ItemKey(const std::pair<std::string,uint32_t>& old):ItemKey(old.first,old.second){}
        bool operator<(const ItemKey& other) const
        {return std::tie(first,second,variant)<std::tie(other.first,other.second,other.variant);}
    };
    using Items = std::map<ItemKey,int64_t>;
    const Items& Stock(const std::string& a,const std::string& b) {return stock[std::minmax(a,b)];}
    struct Refund { std::string peer,plugin; uint32_t base{},count{}; std::string variant; };
    // Reconstruct the balance of each depositor from the same durable ledger.
    // Existing T records are fungible; a normal take consumes one's own
    // contribution first, then the peer's. No separate persistent store.
    std::optional<Refund> NextRefund(const std::string& user) const
    {
        for (const auto& [owners,items]:contributions)
            if (owners.first==user)
                for (const auto& [item,count]:items)
                    if (count>0)
                        return Refund{owners.second,item.first,item.second,
                            static_cast<uint32_t>(std::min<int64_t>(count,9999)),item.variant};
        return {};
    }
    bool HasOutstanding(const std::string& user)
    {
        return accounts[user].size()!=confirmed[user] || NextRefund(user).has_value();
    }
    // The acknowledged cursor protects escrow, not the age of a loaded save.
    // In a campaign an older valid save may replay its own checkpointed receipts
    // without re-crediting escrow. Uncheckpointed historical receipts cannot be
    // recovered safely. Legacy sessions retain the conservative refusal.
    bool AcceptCursor(const std::string& user,uint64_t savedStream,uint32_t cursor)
    {
        if (user.empty() || !Stream() || (savedStream && savedStream!=stream) ||
            (!savedStream && cursor)) return false;
        const auto total=accounts[user].size();
        const auto done=confirmed[user];
        if (cursor>total || total>static_cast<uint64_t>(done)+1) return false;
        if (cursor<done)
        {
            if (!authority) return false;
            for (size_t i=cursor;i<done;++i)
                if (accounts[user][i].before<0) return false;
        }
        return true;
    }
    bool MatchesReceipt(const std::string& user,uint64_t savedStream,uint32_t cursor,uint32_t receipt,
                        const std::string& plugin,uint32_t base,int count,int64_t before,const std::string& variant={})
    {
        if (!AcceptCursor(user,savedStream,cursor) || receipt!=cursor+1 ||
            receipt>accounts[user].size()) return false;
        const auto& r=accounts[user][receipt-1];
        return r.plugin==plugin && r.base==base && r.count==count && r.before==before && r.variant==variant;
    }
    // This is a pure precondition check for a receipt whose cursor is not yet
    // applied. Never infer application from a matching after-count alone.
    static bool CanApply(const Row& row,int64_t current)
    {
        return row.before>=0 && current==row.before && ValidCounts(row);
    }
    bool Ack(const std::string& user,uint32_t pos)
    {
        if (pos<=confirmed[user]) return true;
        if (pos>accounts[user].size()) return false;
        while (confirmed[user]<pos)
        {
            const auto n=confirmed[user]+1;
            std::ostringstream out; out<<"A "<<std::quoted(user)<<' '<<n;
            if (!Store(out.str())) return false;
            Confirmed(user,n);
        }
        return true;
    }
    // All item movements use the same receipt cursor, including house
    // containers. A house account is never a player or refundable escrow.
    static std::string HomeAccount(uint32_t slot,const TradeProperties::FormKey& ref)
    {return "home/"+std::to_string(slot)+"/"+ref.Encode();}
    bool CommitHome(uint64_t character,uint32_t slot,const TradeProperties::FormKey& ref,
                    const TradeProperties::FormKey& item,std::string variant,
                    int count,int64_t before,uint32_t cursor)
    {
        if (!authority || !Stream() || !character || !ref.Valid() || !item.Valid() ||
            HomePermissions::Effective(*authority,slot,character)!=HomePermissions::Right::Modify) return false;
        Row r{std::to_string(character),HomeAccount(slot,ref),item.Plugin,item.Local,
            count,before,false,std::move(variant)};
        return CommitRow(r,cursor,true);
    }
    // The same H receipt and ACK govern inventory and placed objects.
    // A negative receipt creates a new empty account; ACK makes it visible.
    bool CommitPlacement(uint64_t character,uint32_t slot,const TradeProperties::FormKey& item,
                         std::string variant,uint32_t count,int64_t before,uint32_t cursor,
                         const HomePlacement::Pose& pose)
    {
        if (!authority || !Stream() || !character || !slot || slot>HomeRegistry::MaxSlot ||
            !count || count>9999 || cursor>=0xFFFFFE || !item.Valid() || !HomePlacement::Valid(pose) ||
            HomePermissions::Effective(*authority,slot,character)!=HomePermissions::Right::Modify) return false;
        HomePlacement::Key key{slot,character,cursor+1,pose};
        Row r{std::to_string(character),key.Encode(),item.Plugin,item.Local,-int(count),before,false,std::move(variant)};
        return CommitRow(r,cursor,true);
    }
    bool TakePlacement(uint64_t character,std::string_view account,const TradeProperties::FormKey& item,
                       std::string variant,uint32_t count,int64_t before,uint32_t cursor)
    {
        const auto key=HomePlacement::Decode(account);
        if (!authority || !Stream() || !character || !key || !item.Valid() || !count || count>9999 ||
            HomePermissions::Effective(*authority,key->Slot,character)!=HomePermissions::Right::Modify) return false;
        Row r{std::to_string(character),std::string(account),item.Plugin,item.Local,int(count),before,false,std::move(variant)};
        return CommitRow(r,cursor,true);
    }
    std::optional<HomePlacement::Stack> PlacedStack(std::string_view account)
    {
        if (!authority || !Stream()) return {};
        auto found=placements.find(std::string(account));
        if (found==placements.end()) return {};
        const auto& s=found->second;
        const auto& items=homeStock.at(found->first);
        const auto it=items.find({s.Item.Base.Plugin,s.Item.Base.Local,s.Item.Variant});
        if (it==items.end() || it->second!=s.Item.Count) return {};
        return s;
    }
    std::vector<HomePlacement::Stack> Placed(uint32_t slot)
    {
        std::vector<HomePlacement::Stack> out;
        if (!authority || !Stream()) return out;
        for (const auto& [key,s]:placements)
            if (s.Id.Slot==slot)
                if (auto active=PlacedStack(key)) out.push_back(std::move(*active));
        return out;
    }
    bool MovePlacement(uint64_t character,const HomePlacement::Move& move)
    {
        const auto current=PlacedStack(move.Account);
        if (!current || !character || current->Revision==UINT32_MAX ||
            move.Revision!=current->Revision+1 || !HomePlacement::Valid(move.Position) ||
            HomePermissions::Effective(*authority,current->Id.Slot,character)!=HomePermissions::Right::Modify ||
            authority->Current().Revision==UINT32_MAX) return false;
        if (!HomePlacement::Different(current->Position,move.Position)) return true;
        if (authority->Append(authority->Current(),{"home-placement-"+std::to_string(authority->Current().Revision+1),
            "home-placement-pose-v1",move.Encode()})!=CampaignJournal::Result::Committed) return false;
        return ApplyPlacementMove(move);
    }
    std::optional<HomeSnapshot::State> HomeState(uint32_t slot)
    {
        if (!authority || !Stream()) return {};
        auto baseline=HomeSnapshot::Read(*authority,slot);
        if (baseline.State!=HomeSnapshot::Status::Ready) return {};
        auto state=HomeSnapshot::Decode(baseline.Bytes);
        if (!state) return {};
        for (auto& ref:state->Refs)
        {
            if (ref.Inventory!=1 && ref.Inventory!=3) continue;
            const auto account=HomeAccount(slot,ref.Key);
            auto* items=LoadHome(account,authority->Operations().size()+1);
            if (!items) return {};
            ref.Items.clear();
            for (const auto& [key,count]:*items)
            {
                if (!count) continue;
                if (count<0 || count>INT32_MAX) return {};
                ref.Items.push_back({{key.first,key.second},static_cast<uint32_t>(count),key.variant});
            }
            if (ref.Inventory==3 && ref.Items.empty()) ref.Flags|=1u;
            std::sort(ref.Items.begin(),ref.Items.end(),[](const auto& a,const auto& b)
            {return std::pair{a.Base.Encode(),a.Variant}<std::pair{b.Base.Encode(),b.Variant};});
        }
        if (!HomeLocks::Apply(*authority,slot,*state) || !HomeObjects::Apply(*authority,slot,*state) ||
            state->Encode().size()>HomeSnapshot::MaxBytes) return {};
        return state;
    }
    bool Commit(const Row& r,uint32_t cursor)
    {
        if (r.peer.starts_with("home/") || r.user.starts_with("home/")) return false;
        return CommitRow(r,cursor,false);
    }
private:
    bool CommitRow(const Row& r,uint32_t cursor,bool home)
    {
        if (r.user.empty() || r.peer.empty() || r.user==r.peer || r.plugin.empty() || !r.base ||
            cursor!=accounts[r.user].size() || cursor!=confirmed[r.user] || cursor>=0xFFFFFE ||
            !r.count || std::abs(static_cast<long long>(r.count))>9999 || !ValidCounts(r) ||
            (r.refund && (r.count<0 || r.before<0))) return false;
        if (home)
        {
            if (!ValidHome(r,authority->Operations().size()+1)) return false;
        }
        else if (r.count>0)
        {
            const auto& items=r.refund ? contributions[{r.user,r.peer}] : Stock(r.user,r.peer);
            auto i=items.find({r.plugin,r.base,r.variant});
            if (i==items.end() || i->second<r.count) return false;
        }
        std::ostringstream out;
        out<<(home?"H ":r.refund?"R ":"T ")<<std::quoted(r.user)<<' '<<std::quoted(r.peer)<<' '<<std::quoted(r.plugin)<<' '<<r.base<<' '<<r.count;
        if (r.before>=0) out<<' '<<r.before;
        if (!r.variant.empty()) out<<' '<<std::quoted(r.variant);
        if (!Store(out.str())) return false;
        Append(r); return true;
    }
    static bool HomePeer(const Row& r) {return r.peer.starts_with("home/");}
    bool BaselinePublished(uint32_t slot,size_t beforePosition) const
    {
        if (!authority || !beforePosition) return false;
        const auto marker=HomeSnapshot::Marker(slot);
        const auto& ops=authority->Operations();
        const auto limit=std::min(ops.size(),beforePosition-1);
        for (size_t i=0;i<limit;++i)
            if (ops[i].Id==marker && ops[i].Kind=="home-static-v1") return true;
        return false;
    }
    bool LoadLegacy(const std::string& marker)
    {
        constexpr std::string_view prefix="home-static-";
        uint32_t slot{};
        if (!marker.starts_with(prefix) ||
            !TradeProperties::Number(std::string_view(marker).substr(prefix.size()),slot) ||
            HomeSnapshot::Marker(slot)!=marker) return false;
        const auto home=HomeRegistry::BySlot(*authority,slot);
        const auto baseline=HomeSnapshot::Read(*authority,slot);
        const auto state=baseline.State==HomeSnapshot::Status::Ready?HomeSnapshot::Decode(baseline.Bytes):std::nullopt;
        if (!home || !state) return false;
        for (const auto& d:state->Legacy)
        {
            HomePlacement::Key key{slot,home->Owner,0,d.Transform,d.Source};
            const auto account=key.Encode();
            if (!HomePlacement::Decode(account) || placements.count(account) || homeStock.count(account)) return false;
            Items items;
            items.emplace(ItemKey{d.Value.Base.Plugin,d.Value.Base.Local,d.Value.Variant},d.Value.Count);
            homeStock.emplace(account,std::move(items));
            placements.emplace(account,HomePlacement::Stack{key,d.Value,d.Transform,0});
        }
        return true;
    }
    bool ApplyPlacementMove(const HomePlacement::Move& m)
    {
        const auto it=placements.find(m.Account);
        if (it==placements.end() || it->second.Revision==UINT32_MAX ||
            m.Revision!=it->second.Revision+1 || !HomePlacement::Valid(m.Position) ||
            !HomePlacement::Different(it->second.Position,m.Position)) return false;
        auto& s=it->second;
        const auto& items=homeStock.at(m.Account);
        const auto item=items.find({s.Item.Base.Plugin,s.Item.Base.Local,s.Item.Variant});
        if (item==items.end() || item->second!=s.Item.Count) return false;
        s.Position=m.Position;s.Revision=m.Revision;
        return true;
    }
    Items* LoadHome(const std::string& account,size_t beforePosition)
    {
        if (!authority || !account.starts_with("home/")) return nullptr;
        if (auto it=homeStock.find(account);it!=homeStock.end()) return &it->second;
        const auto sep=account.find('/',5);
        uint32_t slot{};
        if (sep==std::string::npos || !TradeProperties::Number(std::string_view(account).substr(5,sep-5),slot)) return nullptr;
        auto key=TradeProperties::DecodeForm(std::string_view(account).substr(sep+1));
        if (!key || !key->Valid() || HomeAccount(slot,*key)!=account) return nullptr;
        // A historical operation cannot spend a baseline published later.
        if (!BaselinePublished(slot,beforePosition)) return nullptr;
        auto baseline=HomeSnapshot::Read(*authority,slot);
        auto state=baseline.State==HomeSnapshot::Status::Ready?HomeSnapshot::Decode(baseline.Bytes):std::nullopt;
        if (!state) return nullptr;
        for (const auto& ref:state->Refs)
        {
            if (ref.Key.Encode()!=key->Encode()) continue;
            if (ref.Inventory!=1 && ref.Inventory!=3) return nullptr;
            if (ref.Inventory==3) looseAccounts.insert(account);
            Items items;
            for (const auto& item:ref.Items)
                items.emplace(ItemKey{item.Base.Plugin,item.Base.Local,item.Variant},item.Count);
            return &homeStock.emplace(account,std::move(items)).first->second;
        }
        return nullptr;
    }
    bool ValidHome(const Row& r,size_t beforePosition)
    {
        uint64_t character{};
        auto parsed=std::from_chars(r.user.data(),r.user.data()+r.user.size(),character);
        if (!authority || !character || parsed.ec!=std::errc{} ||
            parsed.ptr!=r.user.data()+r.user.size() || std::to_string(character)!=r.user ||
            r.refund || r.before<0 || !TradeProperties::FormKey{r.plugin,r.base}.Valid()) return false;
        if (const auto placed=HomePlacement::Decode(r.peer))
        {
            if (r.count<0)
            {
                // Only the creator's next backpack receipt may create this ID.
                // Neither an old save nor an ACK retry can create a second stack.
                if (placed->LegacySource || placed->Creator!=character || placed->Receipt!=accounts[r.user].size()+1 ||
                    placements.count(r.peer) || !BaselinePublished(placed->Slot,beforePosition)) return false;
                const auto baseline=HomeSnapshot::Read(*authority,placed->Slot);
                if (baseline.State!=HomeSnapshot::Status::Ready || !HomeSnapshot::Decode(baseline.Bytes)) return false;
                size_t active=0;
                for (const auto& [key,s]:placements)
                    if (s.Id.Slot==placed->Slot)
                    {
                        const auto& stock=homeStock.at(key);
                        const auto balance=stock.find({s.Item.Base.Plugin,s.Item.Base.Local,s.Item.Variant});
                        if (balance==stock.end() || balance->second>0) ++active;
                    }
                return active<2048;
            }
            const auto found=placements.find(r.peer);
            if (found==placements.end()) return false;
            const auto& s=found->second;
            const auto& stock=homeStock.at(r.peer);
            const auto balance=stock.find({r.plugin,r.base,r.variant});
            return s.Item.Base.Plugin==r.plugin && s.Item.Base.Local==r.base &&
                s.Item.Variant==r.variant && s.Item.Count==static_cast<uint32_t>(r.count) &&
                balance!=stock.end() && balance->second==r.count;
        }
        auto* items=LoadHome(r.peer,beforePosition);
        if (!items) return false;
        auto item=items->find({r.plugin,r.base,r.variant});
        // A world reference is one stack, not an arbitrary container.
        // Its whole stack is removed by the same durable receipt.
        if (looseAccounts.count(r.peer))
            return r.count>0 && item!=items->end() && item->second==r.count;
        if (r.count>0) return item!=items->end() && item->second>=r.count;
        // All pending deposits reserve capacity but remain unavailable until
        // the shared backpack receipt is acknowledged. Check base totals,
        // not just the exact variant (engine counts are signed 32-bit).
        int64_t total=0;
        for (const auto& [key,count]:*items)
            if (key.first==r.plugin && key.second==r.base) total+=count;
        for (const auto& [user,rows]:accounts)
            if (!rows.empty() && rows.size()>confirmed[user])
            {
                const auto& pending=rows.back();
                if (pending.count<0 && pending.peer==r.peer && pending.plugin==r.plugin && pending.base==r.base)
                    total-=pending.count;
            }
        return total-static_cast<int64_t>(r.count)<=INT32_MAX;
    }
    static bool ValidCounts(const Row& r)
    {
        if (!TradeProperties::Decode(r.variant) || (!r.variant.empty() && r.before<0)) return false;
        return r.before==-1 || (r.before>=0 && r.before<=INT32_MAX &&
            r.before+static_cast<int64_t>(r.count)>=0 &&
            r.before+static_cast<int64_t>(r.count)<=INT32_MAX);
    }
    void Replay(const std::string& data,size_t beforePosition=0)
    {
        std::istringstream in(data);
        char kind{}; Row r;
        if (!(in>>kind>>std::quoted(r.user)) || r.user.empty()) throw std::runtime_error("invalid trade receipt");
        if (kind=='A')
        {
            uint32_t pos{};std::string extra;
            if (!(in>>pos) || in>>extra || pos!=confirmed[r.user]+1 || pos>accounts[r.user].size())
                throw std::runtime_error("invalid acknowledgement");
            Confirmed(r.user,pos);
        }
        else if (kind=='T' || kind=='R' || kind=='H')
        {
            r.refund=kind=='R';
            std::string extra;
            if (!(in>>std::quoted(r.peer)>>std::quoted(r.plugin)>>r.base>>r.count))
                throw std::runtime_error("invalid trade operation");
            in>>std::ws;
            if (!in.eof())
            {
                if (!(in>>r.before) || r.before<0) throw std::runtime_error("invalid trade checkpoint");
                in>>std::ws;
                if (!in.eof() && (!(in>>std::quoted(r.variant)) || r.variant.empty() || in>>extra))
                    throw std::runtime_error("invalid trade properties");
            }
            if (!ValidCounts(r) || (r.refund && (r.count<=0 || r.before<0)) || r.peer.empty() || r.user==r.peer || r.plugin.empty() || !r.base || !r.count ||
                std::abs(static_cast<long long>(r.count))>9999 ||
                accounts[r.user].size()!=confirmed[r.user] || accounts[r.user].size()>=0xFFFFFE)
                throw std::runtime_error("invalid trade operation");
            if (kind=='H')
            {
                if (!beforePosition || !HomePeer(r) || !ValidHome(r,beforePosition)) throw std::runtime_error("invalid home receipt");
            }
            else if (HomePeer(r) || r.user.starts_with("home/")) throw std::runtime_error("reserved home account");
            else if (r.count>0)
            {
                const auto& items=r.refund ? contributions[{r.user,r.peer}] : Stock(r.user,r.peer);
                auto it=items.find({r.plugin,r.base,r.variant});
                if (it==items.end() || it->second<r.count) throw std::runtime_error("invalid trade stock");
            }
            Append(r);
        }
        else throw std::runtime_error("invalid trade operation type");
    }
    bool Store(const std::string& data)
    {
        if (last==UINT32_MAX) return false;
        if (authority)
        {
            const auto result=authority->Append(authority->Current(),{"trade-"+std::to_string(last+1),"trade-v1",data});
            if (result!=CampaignJournal::Result::Committed) return false;
        }
        else if (!QuestJournalDisk::Store(root,stream,last+1,data)) return false;
        ++last; return true;
    }
    void Append(const Row& r)
    {
        accounts[r.user].push_back(r);
        if (HomePeer(r))
        {
            if (r.count<0)
                if (const auto key=HomePlacement::Decode(r.peer))
                {
                    homeStock.emplace(r.peer,Items{});
                    placements.emplace(r.peer,HomePlacement::Stack{*key,
                        {{r.plugin,r.base},static_cast<uint32_t>(-r.count),r.variant},key->Initial,0});
                }
            if (r.count>0) homeStock.at(r.peer)[{r.plugin,r.base,r.variant}]-=r.count;
            return;
        }
        if (r.count>0)
        {
            const auto item=ItemKey{r.plugin,r.base,r.variant};
            auto& own=contributions[{r.user,r.peer}][item];
            const auto used=std::min<int64_t>(own,r.count);
            own-=used;
            // Commit and Replay have checked stock (and own stock for R).
            if (r.count>used) contributions[{r.peer,r.user}][item]-=r.count-used;
            stock[std::minmax(r.user,r.peer)][item]-=r.count;
        }
    }
    void Confirmed(const std::string& user,uint32_t pos)
    {
        confirmed[user]=pos;
        const auto& r=accounts[user][pos-1];
        if (HomePeer(r))
        {
            if (r.count<0) homeStock.at(r.peer)[{r.plugin,r.base,r.variant}]-=r.count;
            return;
        }
        if (r.count<0)
        {
            const auto item=ItemKey{r.plugin,r.base,r.variant};
            contributions[{r.user,r.peer}][item]-=r.count;
            stock[std::minmax(r.user,r.peer)][item]-=r.count;
        }
    }
    CampaignJournal::Journal* authority{};
    uint64_t campaignId{};
    std::filesystem::path root;
    uint64_t stream{};
    uint32_t last{};
    std::map<std::string,std::vector<Row>> accounts;
    std::map<std::string,uint32_t> confirmed;
    std::map<std::pair<std::string,std::string>,Items> stock;
    std::map<std::pair<std::string,std::string>,Items> contributions;
    std::map<std::string,Items> homeStock;
    std::set<std::string> looseAccounts;
    std::map<std::string,HomePlacement::Stack> placements;
    size_t legacyScanned{};
};
