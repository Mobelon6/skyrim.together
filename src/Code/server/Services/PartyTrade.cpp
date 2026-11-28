#include <GameServer.h>
#include <World.h>
#include <Game/Player.h>
#include <Services/OverlayService.h>
#include <Messages/RequestPartyTrade.h>
#include <Messages/NotifyPartyTrade.h>
#include <TradeLedger.h>
#include <Services/CampaignAccess.h>
#include <memory>
#include <cstdlib>
#include <InventoryEvidence.h>
#include <Services/HomeVisit.h>
#include <HomeEstateConstruction.h>

namespace
{
using Clock=std::chrono::steady_clock;
using namespace std::chrono_literals;
struct Session {uint32_t a,b; bool open{}; Clock::time_point expires; std::string accountA,accountB;};
std::map<uint32_t,Session> sessions;
struct HomeSession {uint32_t Connection{},Slot{};uint64_t Character{};TradeProperties::FormKey Ref;Clock::time_point Until;uint32_t SharedCell{};};
std::map<uint32_t,HomeSession> homeSessions;
uint32_t serial{};
TradeLedger legacy("config/CoopTrade");
std::unique_ptr<TradeLedger> persistent;
TradeLedger& Ledger()
{
    auto& link=CampaignAccess::Authority();
    using Mode=CampaignReplication::Link::Mode;
    if (link.State()==Mode::Disabled) return legacy;
    if (link.State()!=Mode::Ready) throw std::runtime_error("campaign storage unavailable");
    const auto* flag=std::getenv("STM_CAMPAIGN_TRADE");
    if (!flag || std::string_view(flag)!="1") throw std::runtime_error("experimental campaign trade disabled");
    // Until an explicit migration is implemented, do not switch an existing
    // legacy account to a fresh stream. Keep its files untouched.
    if (std::filesystem::exists("config/CoopTrade") && !std::filesystem::is_empty("config/CoopTrade"))
        throw std::runtime_error("legacy trade history requires migration");
    if (!persistent) persistent=std::make_unique<TradeLedger>(link.Journal,link.Campaign);
    return *persistent;
}
std::string Name(Player& player)
{
    if (CampaignAccess::Authority().State()==CampaignReplication::Link::Mode::Disabled)
        return {player.GetUsername().begin(),player.GetUsername().end()};
    const auto id=CampaignAccess::Character(player);
    return id?std::to_string(id):std::string{};
}
bool Near(World& world, Player& a, Player& b)
{
    if (!a.GetCharacter() || !b.GetCharacter() || Name(a).empty() || Name(b).empty() ||
        !a.GetCellComponent().IsInRange(b.GetCellComponent(),false)) return false;
    auto* p=world.try_get<MovementComponent>(*a.GetCharacter());
    auto* q=world.try_get<MovementComponent>(*b.GetCharacter());
    return p && q && glm::distance(p->Position,q->Position)<=420.f;
}
void Send(Player& to, PartyTrade event)
{
    auto& link=CampaignAccess::Authority();
    if (link.State()!=CampaignReplication::Link::Mode::Disabled)
    {
        event.Campaign=link.Campaign;
        event.Character=CampaignAccess::Character(to);
    }
    NotifyPartyTrade n; n.Event=std::move(event); to.Send(n);
}
bool ActiveAccount(const std::string& user)
{
    for (const auto& [_,s]:sessions)
        if (s.accountA==user || s.accountB==user) return true;
    for (const auto& [_,h]:homeSessions)
        if (std::to_string(h.Character)==user) return true;
    return false;
}
std::string Plugin(World& w,uint32_t mod)
{
    auto& mods=w.ctx().at<ModsComponent>();
    for (const auto* list:{&mods.GetStandardMods(),&mods.GetLiteMods()})
        for (const auto& [name,entry]:*list) if (entry.id==mod) return {name.begin(),name.end()};
    return {};
}
void ShowStock(TradeLedger& ledger,Player& p,Player& q,uint32_t session)
{
    const auto& items=ledger.Stock(Name(p),Name(q));
    PartyTrade reset{TradeOp::Reset,q.GetId(),session};
    for (const auto& [_,count]:items) if (count>0) ++reset.Count;
    Send(p,reset);
    for (const auto& [item,count]:items)
    {
        if (count<=0) continue;
        PartyTrade event{TradeOp::Entry,q.GetId(),session};
        event.Count=static_cast<uint32_t>(std::min<int64_t>(count,INT32_MAX));
        event.Item.BaseId=item.second; event.Text=item.first.c_str(); event.Variant=item.variant.c_str(); Send(p,event);
    }
}
std::optional<HomeSnapshot::Ref> HomeContainer(TradeLedger& ledger,World& world,Player& player,const HomeSession& h)
{
    const auto& cell=player.GetCellComponent();
    if(h.SharedCell)
    {
        if(!HomeEstateConstruction::SharedMode()||h.Connection!=player.GetId()||
            h.Character!=CampaignAccess::Character(player)||!player.GetCharacter()||
            cell.WorldSpaceId||HomeScope::Private(cell.Cell.ModId)||cell.Cell.BaseId!=h.SharedCell||
            InventoryEvidence::CanonicalPlugin(Plugin(world,cell.Cell.ModId))!="skyrim.esm"||
            !world.GetPartyService().GetPlayerParty(&player))return {};
        auto row=ledger.SharedContainer({h.SharedCell,h.Ref});
        const auto* movement=world.try_get<MovementComponent>(*player.GetCharacter());
        if(!row||!movement)return {};
        const glm::vec3 pos{std::bit_cast<float>(row->Transform[0]),std::bit_cast<float>(row->Transform[1]),std::bit_cast<float>(row->Transform[2])};
        return glm::distance(movement->Position,pos)<=420.f?row:std::nullopt;
    }
    if (h.Connection!=player.GetId() || h.Character!=CampaignAccess::Character(player) ||
        !HomeVisit::CanEnter(player,cell.Cell.ModId,cell.Cell.BaseId) ||
        (cell.Cell.ModId&HomeRegistry::MaxSlot)!=h.Slot || !player.GetCharacter()) return {};
    auto state=ledger.HomeState(h.Slot);
    if (!state) return {};
    auto* movement=world.try_get<MovementComponent>(*player.GetCharacter());
    if (!movement) return {};
    for (const auto& r:state->Refs)
        if (r.Key.Encode()==h.Ref.Encode() && (r.Inventory==1 || r.Inventory==3) && !(r.Flags&3) && r.LockState!=2)
        {
            const glm::vec3 position{std::bit_cast<float>(r.Transform[0]),std::bit_cast<float>(r.Transform[1]),std::bit_cast<float>(r.Transform[2])};
            if (glm::distance(movement->Position,position)<=420.f) return r;
        }
    return {};
}
bool PlacementNear(World& world,Player& player,uint32_t slot,const HomePlacement::Pose& pose)
{
    const auto& cell=player.GetCellComponent();
    if (!slot || !HomeRegistry::Scoped(cell.Cell.ModId) ||
        (cell.Cell.ModId&HomeRegistry::MaxSlot)!=slot ||
        !HomeVisit::CanEnter(player,cell.Cell.ModId,cell.Cell.BaseId) ||
        !player.GetCharacter() || !HomePlacement::Valid(pose)) return false;
    const auto* movement=world.try_get<MovementComponent>(*player.GetCharacter());
    if (!movement) return false;
    const glm::vec3 position{std::bit_cast<float>(pose[0]),std::bit_cast<float>(pose[1]),std::bit_cast<float>(pose[2])};
    return glm::distance(movement->Position,position)<=420.f;
}
void ShowHomeStock(Player& p,uint32_t session,const HomeSnapshot::Ref& ref)
{
    PartyTrade reset{TradeOp::Reset,0,session};
    reset.Count=static_cast<uint32_t>(ref.Items.size());Send(p,reset);
    for (const auto& item:ref.Items)
    {
        PartyTrade e{TradeOp::Entry,0,session};
        e.Count=item.Count;e.Text=item.Base.Plugin.c_str();e.Item.BaseId=item.Base.Local;
        e.Variant=item.Variant.c_str();Send(p,e);
    }
}
}

void OverlayService::OnTrade(const PacketEvent<RequestPartyTrade>& packet) noexcept
{
    try
    {
        auto& p=*packet.pPlayer; const auto& e=packet.Packet.Event; const auto id=p.GetId();
        if (!p.GetCharacter()) return;
        auto& link=CampaignAccess::Authority();
        const bool campaign=link.State()!=CampaignReplication::Link::Mode::Disabled;
        if (campaign)
        {
            const CampaignIdentity::Stamp stamp{e.Campaign,e.Character,{e.SaveRevision,e.SaveTag}};
            if (!CampaignAccess::Admit(m_world,p,stamp,{e.Profile.data(),e.Profile.size()},{e.Credential.data(),e.Credential.size()})) return;
        }
        else if (e.Campaign || e.Character || !e.Credential.empty()) return;
        const auto user=Name(p);
        if (user.empty()) return;
        if (!campaign)
            for (auto* other:m_world.GetPlayerManager())
                if (other!=&p && other->GetUsername()==p.GetUsername()) return;
        auto& ledger=Ledger();
        const auto uuid=ledger.Stream();
        auto next=[&] {
            const auto& log=ledger.History(user);
            if (e.Cursor<log.size())
            {
                const auto& r=log[e.Cursor];
                PartyTrade op{TradeOp::Receipt,static_cast<uint32_t>(r.count>0),0,e.Cursor+1,static_cast<uint32_t>(std::abs(r.count)),uuid};
                op.Text=r.plugin.c_str(); op.Item.BaseId=r.base;op.Variant=r.variant.c_str();
                if (r.before>=0) op.BeforePlusOne=static_cast<uint32_t>(r.before+1);
                Send(p,op);
            }
        };
        const auto now=Clock::now();
        for (auto it=sessions.begin();it!=sessions.end();)
        {
            auto* a=m_world.GetPlayerManager().GetById(it->second.a);
            auto* b=m_world.GetPlayerManager().GetById(it->second.b);
            if (!a || !b || Name(*a)!=it->second.accountA || Name(*b)!=it->second.accountB ||
                now>=it->second.expires || !Near(m_world,*a,*b))
            {
                if (a) Send(*a,{TradeOp::Close,0,it->first});
                if (b) Send(*b,{TradeOp::Close,0,it->first});
                it=sessions.erase(it);
            } else ++it;
        }
        for (auto it=homeSessions.begin();it!=homeSessions.end();)
        {
            auto* player=m_world.GetPlayerManager().GetById(it->second.Connection);
            if (!player || now>=it->second.Until || !HomeContainer(ledger,m_world,*player,it->second))
            {
                if (player) Send(*player,{TradeOp::Close,0,it->first});
                it=homeSessions.erase(it);
            }
            else ++it;
        }
        if (e.Op!=TradeOp::Close && !ledger.AcceptCursor(user,e.Stream,e.Cursor))
        { Send(p,{TradeOp::RecoveryConflict,0,e.Session,e.Cursor,0,uuid});return; }
        if (e.Op==TradeOp::Poll)
        {
            if (!ledger.Ack(user,e.Cursor)) return;
            if (e.Cursor<ledger.History(user).size())
            {
                // A stale save receives one receipt at a time. Ack of an already
                // confirmed receipt is a no-op and never credits escrow again.
                next();
                return;
            }
            if (!ActiveAccount(user))
                if (const auto refund=ledger.NextRefund(user))
                {
                    PartyTrade offer{TradeOp::ReturnOffer,0,0,e.Cursor,refund->count,uuid};
                    offer.Item.BaseId=refund->base;offer.Text=refund->plugin.c_str();offer.Variant=refund->variant.c_str();
                    Send(p,offer);return;
                }
            Send(p,{TradeOp::Ready,0,e.Session,e.Cursor,0,uuid});
        }
        else if (e.Op!=TradeOp::Close && e.Cursor<ledger.History(user).size())
        {
            next();
            return;
        }
        if (e.Op==TradeOp::Return)
        {
            // The server chooses the return, never a client-specified owner.
            // A replay with an old cursor has already been routed to next().
            if (ActiveAccount(user) || e.Session || !e.BeforePlusOne ||
                e.BeforePlusOne>0x80000000u) return;
            const auto refund=ledger.NextRefund(user);
            if (!refund || refund->base!=e.Item.BaseId || refund->count!=e.Count ||
                refund->plugin!=Plugin(m_world,e.Item.ModId) || refund->variant!=std::string(e.Variant.data(),e.Variant.size())) return;
            const auto before=static_cast<int64_t>(e.BeforePlusOne)-1;
            if (!ledger.Ack(user,e.Cursor) || !ledger.Commit(
                {user,refund->peer,refund->plugin,refund->base,int(refund->count),before,true,refund->variant},e.Cursor))
            {
                Send(p,{TradeOp::Error,0,0});return;
            }
            next();return;
        }
        // A zero-session Take is a direct pickup from a stable house ref.
        // It never enters the ordinary trade or native container session.
        if (e.Op==TradeOp::HomePlace || e.Op==TradeOp::HomeTake || e.Op==TradeOp::HomeMove)
        {
            // All placed stacks are scoped to the admitted home instance.
            // Use the existing authenticated wallet cursor, not native drop replication.
            if (!campaign || e.Session || !e.Peer || e.Peer>HomeRegistry::MaxSlot ||
                e.Text.size()>320 || ActiveAccount(user) || ledger.HasOutstanding(user))
            {Send(p,{TradeOp::Error,0,0});return;}
            const auto character=CampaignAccess::Character(p);
            const std::string_view payload(e.Text.data(),e.Text.size());
            bool ok=false;
            if (e.Op==TradeOp::HomeMove)
            {
                const auto move=HomePlacement::DecodeMove(payload);
                const auto key=move?HomePlacement::Decode(move->Account):std::nullopt;
                const auto stack=move?ledger.PlacedStack(move->Account):std::nullopt;
                if (key && stack && key->Slot==e.Peer && !e.Count && !e.BeforePlusOne &&
                    PlacementNear(m_world,p,e.Peer,stack->Position) &&
                    PlacementNear(m_world,p,e.Peer,move->Position))
                    ok=ledger.MovePlacement(character,*move);
            }
            else if (e.Count && e.Count<=9999 && e.BeforePlusOne && e.BeforePlusOne<=0x80000000u)
            {
                const TradeProperties::FormKey item{InventoryEvidence::CanonicalPlugin(Plugin(m_world,e.Item.ModId)),e.Item.BaseId};
                const std::string variant(e.Variant.data(),e.Variant.size());
                if (e.Op==TradeOp::HomePlace)
                {
                    const auto pose=HomePlacement::DecodePose(payload);
                    if (pose && PlacementNear(m_world,p,e.Peer,*pose) && ledger.Ack(user,e.Cursor))
                        ok=ledger.CommitPlacement(character,e.Peer,item,variant,e.Count,
                            int64_t(e.BeforePlusOne)-1,e.Cursor,*pose);
                }
                else
                {
                    const auto key=HomePlacement::Decode(payload);
                    const auto stack=ledger.PlacedStack(payload);
                    if (key && stack && key->Slot==e.Peer && PlacementNear(m_world,p,e.Peer,stack->Position) &&
                        ledger.Ack(user,e.Cursor))
                        ok=ledger.TakePlacement(character,payload,item,variant,e.Count,
                            int64_t(e.BeforePlusOne)-1,e.Cursor);
                }
            }
            if (!ok) Send(p,{TradeOp::Error,0,0});
            else if (e.Op!=TradeOp::HomeMove) next();
            return;
        }
        if (e.Op==TradeOp::Take && !e.Session)
        {
            if (!campaign || !e.Peer || e.Peer>HomeRegistry::MaxSlot ||
                ActiveAccount(user) || ledger.HasOutstanding(user) ||
                e.Text.size()>220 || !e.Count || e.Count>9999 ||
                !e.BeforePlusOne || e.BeforePlusOne>0x80000000u) return;
            auto ref=TradeProperties::DecodeForm({e.Text.data(),e.Text.size()});
            if (!ref || !ref->Valid()) return;
            HomeSession h{id,e.Peer,CampaignAccess::Character(p),*ref,now+20s};
            const auto row=HomeContainer(ledger,m_world,p,h);
            const TradeProperties::FormKey item{InventoryEvidence::CanonicalPlugin(Plugin(m_world,e.Item.ModId)),e.Item.BaseId};
            const std::string variant(e.Variant.data(),e.Variant.size());
            if (!row || row->Inventory!=3 || row->Items.size()!=1 ||
                row->Items.front().Base.Encode()!=item.Encode() || row->Items.front().Count!=e.Count ||
                row->Items.front().Variant!=variant || !ledger.Ack(user,e.Cursor) ||
                !ledger.CommitHome(h.Character,h.Slot,h.Ref,item,variant,int(e.Count),
                    int64_t(e.BeforePlusOne)-1,e.Cursor))
            {Send(p,{TradeOp::Error,0,0});return;}
            next();return;
        }
        if(e.Op==TradeOp::HomeOpen&&e.Peer==0)
        {
            if(!campaign||!HomeEstateConstruction::SharedMode()||e.Session||e.Count||
                !e.Item.BaseId||ActiveAccount(user)||ledger.HasOutstanding(user)||
                e.Text.size()>1400||!m_world.GetPartyService().GetPlayerParty(&p))return;
            const auto& cell=p.GetCellComponent();
            if(cell.WorldSpaceId||HomeScope::Private(cell.Cell.ModId)||
                InventoryEvidence::CanonicalPlugin(Plugin(m_world,cell.Cell.ModId))!="skyrim.esm")return;
            const auto character=CampaignAccess::Character(p);
            HomeSharedStorage::Address address{cell.Cell.BaseId,
                {InventoryEvidence::CanonicalPlugin(Plugin(m_world,e.Item.ModId)),e.Item.BaseId}};
            auto& j=link.Journal;
            if(!HomeCityPurchase::NativeContainerRoute(j,address.Cell)||!address.Ref.Valid()||!character)return;
            if(!HomeSharedStorage::Read(j,address))
            {
                const std::string_view bytes(e.Text.data(),e.Text.size());
                const auto row=HomeSharedStorage::Validate(address,bytes);
                const auto* movement=m_world.try_get<MovementComponent>(*p.GetCharacter());
                if(!row||!movement)return;
                const glm::vec3 pos{std::bit_cast<float>(row->Transform[0]),std::bit_cast<float>(row->Transform[1]),std::bit_cast<float>(row->Transform[2])};
                if(glm::distance(movement->Position,pos)>420.f||
                    !HomeSharedStorage::Seed(j,address,bytes))
                {Send(p,{TradeOp::Error,0,0});return;}
            }
            HomeSession h{id,0,character,address.Ref,now+20s,address.Cell};
            auto container=HomeContainer(ledger,m_world,p,h);
            if(!container){Send(p,{TradeOp::Error,0,0});return;}
            do{if(!++serial)++serial;}while(sessions.count(serial)||homeSessions.count(serial));
            homeSessions.emplace(serial,std::move(h));
            PartyTrade open{TradeOp::Open,0,serial,0,0,uuid};
            open.Item=e.Item;open.Text="Общий сундук";Send(p,open);
            ShowHomeStock(p,serial,*container);return;
        }
        if (e.Op==TradeOp::HomeOpen)
        {
            if (!campaign || e.Session || e.Count || !e.Peer || e.Peer>HomeRegistry::MaxSlot ||
                ActiveAccount(user) || ledger.HasOutstanding(user) || e.Text.size()>220) return;
            auto key=TradeProperties::DecodeForm({e.Text.data(),e.Text.size()});
            if (!key || !key->Valid()) return;
            HomeSession h{id,e.Peer,CampaignAccess::Character(p),*key,now+20s};
            auto container=HomeContainer(ledger,m_world,p,h);
            if (!container || container->Inventory!=1) {Send(p,{TradeOp::Error,0,0});return;}
            do {if (!++serial) ++serial;} while (sessions.count(serial)||homeSessions.count(serial));
            homeSessions.emplace(serial,std::move(h));
            PartyTrade open{TradeOp::Open,0,serial,0,0,uuid};
            open.Text="Домашний контейнер";Send(p,open);
            ShowHomeStock(p,serial,*container);return;
        }
        if (auto found=homeSessions.find(e.Session);found!=homeSessions.end())
        {
            auto& h=found->second;
            if (h.Connection!=id || h.Character!=CampaignAccess::Character(p)) return;
            if (e.Op==TradeOp::Close)
            {Send(p,{TradeOp::Close,0,e.Session});homeSessions.erase(found);return;}
            auto container=HomeContainer(ledger,m_world,p,h);
            if (!container)
            {Send(p,{TradeOp::Close,0,e.Session});homeSessions.erase(found);return;}
            h.Until=now+20s;
            if (e.Op==TradeOp::Deposit || e.Op==TradeOp::Take)
            {
                if (!e.Count || e.Count>9999 || !e.Item.BaseId || !e.BeforePlusOne || e.BeforePlusOne>0x80000000u) return;
                auto plugin=Plugin(m_world,e.Item.ModId);
                TradeProperties::FormKey key{InventoryEvidence::CanonicalPlugin(plugin),e.Item.BaseId};
                const std::string variant(e.Variant.data(),e.Variant.size());
                const auto count=(e.Op==TradeOp::Take?1:-1)*int(e.Count);
                const auto before=int64_t(e.BeforePlusOne)-1;
                if (!ledger.Ack(user,e.Cursor) || !(h.SharedCell?
                    ledger.CommitShared(h.Character,{h.SharedCell,h.Ref},key,variant,count,before,e.Cursor):
                    ledger.CommitHome(h.Character,h.Slot,h.Ref,key,variant,count,before,e.Cursor)))
                    Send(p,{TradeOp::Error,0,e.Session});
                next();
            }
            if (e.Op==TradeOp::Poll || e.Op==TradeOp::Deposit || e.Op==TradeOp::Take)
                if (auto updated=HomeContainer(ledger,m_world,p,h)) ShowHomeStock(p,e.Session,*updated);
            return;
        }
        if (e.Op==TradeOp::Invite)
        {
            auto* other=m_world.GetPlayerManager().GetById(e.Peer);
            if (!other || other==&p || Name(*other)==user || !Near(m_world,p,*other)) return;
            if (ledger.HasOutstanding(user) || ledger.HasOutstanding(Name(*other)))
            {Send(p,{TradeOp::Error,0,0});return;}
            if (ActiveAccount(user) || ActiveAccount(Name(*other))) return;
            do {if (!++serial) ++serial;} while (sessions.count(serial)||homeSessions.count(serial));
            sessions[serial]={id,e.Peer,false,now+30s,user,Name(*other)};
            Send(*other,{TradeOp::Offer,id,serial,0,0,uuid,{},p.GetUsername()});
            return;
        }
        auto found=sessions.find(e.Session);
        if (found==sessions.end()) return;
        auto& s=found->second;
        if (id!=s.a && id!=s.b) return;
        auto& peer=*m_world.GetPlayerManager().GetById(id==s.a?s.b:s.a);
        if (e.Op==TradeOp::Close)
        {
            Send(p,{TradeOp::Close,0,e.Session}); Send(peer,{TradeOp::Close,0,e.Session}); sessions.erase(found); return;
        }
        if (e.Op==TradeOp::Accept && id==s.b && !s.open)
        {
            s.open=true;
            Send(p,{TradeOp::Open,peer.GetId(),e.Session,0,0,uuid,{},peer.GetUsername()});
            Send(peer,{TradeOp::Open,id,e.Session,0,0,uuid,{},p.GetUsername()});
        }
        if (!s.open) return;
        s.expires=now+20s;
        if (e.Op==TradeOp::Deposit || e.Op==TradeOp::Take)
        {
            if (!e.Count || e.Count>9999 || !e.Item.BaseId) return;
            const auto plugin=Plugin(m_world,e.Item.ModId);
            if (!e.BeforePlusOne || e.BeforePlusOne>0x80000000u)
            { Send(p,{TradeOp::Error,0,e.Session});return; }
            const int64_t before=static_cast<int64_t>(e.BeforePlusOne)-1;
            const std::string variant(e.Variant.data(),e.Variant.size());
            bool ok=TradeProperties::Decode(variant).has_value() && ledger.Ack(user,e.Cursor) && ledger.Commit({user,Name(peer),plugin,e.Item.BaseId,(e.Op==TradeOp::Take?1:-1)*int(e.Count),before,false,variant},e.Cursor);
            if (!ok) Send(p,{TradeOp::Error,0,e.Session});
            next();
        }
        if (e.Op==TradeOp::Poll || e.Op==TradeOp::Accept || e.Op==TradeOp::Deposit || e.Op==TradeOp::Take)
        {
            ShowStock(ledger,p,peer,e.Session);
            if (e.Op!=TradeOp::Poll) ShowStock(ledger,peer,p,e.Session);
        }
    }
    catch(const std::exception& ex) {spdlog::error("Co-op trade: {}",ex.what()); Send(*packet.pPlayer,{TradeOp::Error,0,packet.Packet.Event.Session});}
}
