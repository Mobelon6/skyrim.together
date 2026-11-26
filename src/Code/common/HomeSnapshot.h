#pragma once
#include "HomeRegistry.h"
#include <array>
#include <bit>
#include <cmath>
#include <map>
#include <sstream>
#include <set>
#include <string>
#include <vector>

// The first observation of loaded static references, not an authoritative
// inventory transaction or a complete playable house. A committed baseline
// is immutable until a revision/receipt protocol can reconcile local saves.
namespace HomeSnapshot
{
inline constexpr size_t PartBytes=1400;
inline constexpr uint32_t MaxParts=1024;
inline constexpr size_t MaxBytes=PartBytes*MaxParts;
// Stable pickable records have one ledger account per placed reference.
inline bool LooseType(uint32_t type) noexcept
{
    switch(type)
    {
        case 23: case 26: case 27: case 30: case 32: case 33:
        case 41: case 42: case 45: case 46: case 52: return true;
        default: return false;
    }
}
struct Item
{
    TradeProperties::FormKey Base;
    uint32_t Count{};
    std::string Variant;
};
// The source locator is scoped to the owner save, never a network FormID.
struct LegacyStack
{
    uint32_t Source{};
    Item Value;
    std::array<uint32_t,6> Transform{};
    bool Valid() const
    {
        if ((Source>>24)!=0xFF || !(Source&0xFFFFFFu) || !Value.Base.Valid() ||
            !Value.Count || Value.Count>9999 || !TradeProperties::Decode(Value.Variant)) return false;
        for (size_t i=0;i<Transform.size();++i)
        {
            const auto n=std::bit_cast<float>(Transform[i]);
            if (!std::isfinite(n) || std::abs(n)>(i<3?10000000.f:1000.f)) return false;
        }
        return true;
    }
};
struct Ref
{
    TradeProperties::FormKey Key,Base;
    uint32_t Type{},Flags{},Scale{},Inventory{},LockState{},LockLevel{};
    std::array<uint32_t,6> Transform{};
    std::vector<Item> Items;
};
struct State
{
    uint32_t Omitted{};
    std::vector<Ref> Refs;
    std::set<std::string> ResidentKeys;
    std::vector<LegacyStack> Legacy;
    bool Valid() const
    {
        if (Refs.empty() || Refs.size()>16384 || ResidentKeys.size()>128 || Legacy.size()>2048) return false;
        uint32_t priorSource{};
        for (const auto& item:Legacy)
        {
            if (!item.Valid() || item.Source<=priorSource) return false;
            priorSource=item.Source;
        }
        for (const auto& key:ResidentKeys)
        {
            const auto form=TradeProperties::DecodeForm(key);
            if (!form || !form->Valid() || form->Encode()!=key || form->Plugin=="skyrimcoophomeinstances.esp") return false;
        }
        std::string prior;
        for (const auto& r:Refs)
        {
            if (!r.Key.Valid() || !r.Base.Valid() || r.Type>=0x87 ||
                r.Flags>3 || r.Scale>65535 || r.Inventory>3 || r.LockState>2 || r.LockLevel>255 ||
                (!r.LockState && r.LockLevel) ||
                (r.Type!=28 && r.Inventory!=0 && r.Inventory!=3) || (r.Type==28 && (r.Inventory==0 || r.Inventory==3)) ||
                (r.Inventory!=1 && r.Inventory!=3 && !r.Items.empty())) return false;
            if (r.Inventory==3 && (!LooseType(r.Type) || r.LockState ||
                ((r.Flags&3u)?!r.Items.empty():r.Items.size()!=1) ||
                (!r.Items.empty() && (r.Items.front().Base.Encode()!=r.Base.Encode() ||
                                      r.Items.front().Count>9999)))) return false;
            for (auto v:r.Transform) if (!std::isfinite(std::bit_cast<float>(v))) return false;
            const auto key=r.Key.Encode();
            if (ResidentKeys.count(key)) return false;
            if (!prior.empty() && key<=prior) return false;
            prior=key;
            std::pair<std::string,std::string> last;
            bool first=true;
            std::map<std::string,uint64_t> totals;
            for (const auto& item:r.Items)
            {
                if (!item.Base.Valid() || !item.Count || item.Count>INT32_MAX ||
                    !TradeProperties::Decode(item.Variant)) return false;
                const auto encoded=item.Base.Encode();
                auto order=std::pair{encoded,item.Variant};
                if (!first && order<=last) return false;
                first=false;last=std::move(order);
                if ((totals[encoded]+=item.Count)>INT32_MAX) return false;
            }
        }
        return true;
    }
    std::string Encode() const
    {
        bool loose=false;
        for (const auto& ref:Refs) if (ref.Inventory==3) {loose=true;break;}
        std::string out=std::string(!Legacy.empty()?"static-v4 ":loose?"static-v3 ":ResidentKeys.empty()?"static-v1 ":"static-v2 ")+std::to_string(Omitted)+"\n";
        for (const auto& key:ResidentKeys) out+="A "+key+"\n";
        for (const auto& r:Refs)
        {
            out+="R "+r.Key.Encode()+" "+r.Base.Encode()+" "+std::to_string(r.Type)+" "+
                std::to_string(r.Flags)+" "+std::to_string(r.Scale)+" "+std::to_string(r.Inventory)+" "+
                std::to_string(r.LockState)+" "+std::to_string(r.LockLevel);
            for (auto v:r.Transform) out+=" "+std::to_string(v);
            out+="\n";
            for (const auto& item:r.Items)
                out+="I "+item.Base.Encode()+" "+std::to_string(item.Count)+" "+
                    (item.Variant.empty()?"-":item.Variant)+"\n";
        }
        for (const auto& d:Legacy)
        {
            out+="D "+std::to_string(d.Source)+" "+d.Value.Base.Encode()+" "+std::to_string(d.Value.Count)+" "+
                (d.Value.Variant.empty()?"-":d.Value.Variant);
            for (auto n:d.Transform) out+=" "+std::to_string(n);
            out+="\n";
        }
        return out;
    }
};
inline std::optional<State> Decode(std::string_view bytes)
{
    if (bytes.empty() || bytes.size()>MaxBytes || bytes.back()!='\n') return {};
    State s;
    std::istringstream input{std::string(bytes)};
    std::string line,tag,key,base,variant,extra;
    if (!std::getline(input,line)) return {};
    std::istringstream header(line);
    if (!(header>>tag>>s.Omitted) || (tag!="static-v1" && tag!="static-v2" && tag!="static-v3" && tag!="static-v4") || header>>extra) return {};
    const bool withResidents=tag!="static-v1";
    const bool withLegacy=tag=="static-v4";
    while (std::getline(input,line))
    {
        std::istringstream row(line);
        if (!(row>>tag)) return {};
        if (tag=="A")
        {
            if (!withResidents || !s.Refs.empty() || !(row>>key) || row>>extra ||
                s.ResidentKeys.size()>=128 || !s.ResidentKeys.insert(key).second) return {};
        }
        else if (tag=="R")
        {
            Ref r;
            if (!(row>>key>>base>>r.Type>>r.Flags>>r.Scale>>r.Inventory>>r.LockState>>r.LockLevel)) return {};
            for (auto& v:r.Transform) if (!(row>>v)) return {};
            auto k=TradeProperties::DecodeForm(key),b=TradeProperties::DecodeForm(base);
            if (!k || !b || row>>extra || s.Refs.size()>=16384) return {};
            r.Key=*k;r.Base=*b;s.Refs.push_back(std::move(r));
        }
        else if (tag=="D" && withLegacy)
        {
            LegacyStack d;
            if (!(row>>d.Source>>base>>d.Value.Count>>variant) || s.Legacy.size()>=2048) return {};
            for (auto& n:d.Transform) if (!(row>>n)) return {};
            auto b=TradeProperties::DecodeForm(base);
            if (!b || row>>extra) return {};
            d.Value.Base=*b;d.Value.Variant=variant=="-"?"":variant;
            if (!d.Valid()) return {};
            s.Legacy.push_back(std::move(d));
        }
        else if (tag=="I" && !s.Refs.empty() && s.Legacy.empty())
        {
            Item item;
            if (!(row>>base>>item.Count>>variant) || row>>extra) return {};
            auto b=TradeProperties::DecodeForm(base);
            if (!b) return {};
            item.Base=*b;item.Variant=variant=="-"?"":variant;
            s.Refs.back().Items.push_back(std::move(item));
        }
        else return {};
    }
    if (!s.Valid() || s.Encode()!=bytes) return {};
    return s;
}
inline uint64_t Digest(std::string_view bytes)
{
    uint64_t v=14695981039346656037ull;
    for (unsigned char c:bytes) {v^=c;v*=1099511628211ull;}
    return v;
}
inline std::string Marker(uint32_t slot) {return "home-static-"+std::to_string(slot);}
inline std::string Prefix(uint32_t slot,uint64_t digest)
{return Marker(slot)+"-"+std::to_string(digest)+"-";}
enum class Status {Missing,Ready,Invalid};
struct Baseline {Status State=Status::Missing;std::string Bytes;};
inline Baseline Read(const CampaignJournal::Journal& journal,uint32_t slot)
{
    if (!journal.Ready() || !HomeRegistry::BySlot(journal,slot)) return {Status::Invalid,{}};
    const auto marker=Marker(slot);
    const CampaignJournal::Operation* committed=nullptr;
    size_t end=0;
    for (size_t i=0;i<journal.Operations().size();++i)
    {
        const auto& op=journal.Operations()[i];
        if (op.Id!=marker) continue;
        if (committed || op.Kind!="home-static-v1") return {Status::Invalid,{}};
        committed=&op;end=i;
    }
    if (!committed) return {};
    uint64_t digest{};uint32_t parts{};size_t size{};
    std::istringstream in(committed->Payload);
    std::string extra;
    if (!(in>>digest>>parts>>size) || in>>extra || !parts || parts>MaxParts ||
        !size || size>MaxBytes || parts!=(size+PartBytes-1)/PartBytes ||
        committed->Payload!=std::to_string(digest)+" "+std::to_string(parts)+" "+std::to_string(size))
        return {Status::Invalid,{}};
    std::vector<std::string_view> chunks(parts);
    const auto prefix=Prefix(slot,digest);
    for (size_t i=0;i<end;++i)
    {
        const auto& op=journal.Operations()[i];
        if (!op.Id.starts_with(prefix)) continue;
        uint32_t part{};
        if (op.Kind!="home-static-part-v1" || !TradeProperties::Number(std::string_view(op.Id).substr(prefix.size()),part) ||
            part>=parts || !chunks[part].empty() || op.Payload.size()!=(part+1==parts?size-PartBytes*part:PartBytes))
            return {Status::Invalid,{}};
        chunks[part]=op.Payload;
    }
    std::string out;out.reserve(size);
    for (const auto chunk:chunks) {if (chunk.empty()) return {Status::Invalid,{}};out+=chunk;}
    if (out.size()!=size || Digest(out)!=digest || !Decode(out)) return {Status::Invalid,{}};
    return {Status::Ready,std::move(out)};
}
inline bool Publish(CampaignJournal::Journal& journal,uint32_t slot,std::string_view bytes)
{
    if (!Decode(bytes)) return false;
    const auto baseline=Read(journal,slot);
    if (baseline.State!=Status::Missing) return baseline.State==Status::Ready && baseline.Bytes==bytes;
    const auto digest=Digest(bytes);
    const auto prefix=Prefix(slot,digest);
    const auto count=(bytes.size()+PartBytes-1)/PartBytes;
    for (size_t i=0;i<count;++i)
    {
        const auto result=journal.Append(journal.Current(),{prefix+std::to_string(i),"home-static-part-v1",
            std::string(bytes.substr(i*PartBytes,PartBytes))});
        if (result!=CampaignJournal::Result::Committed && result!=CampaignJournal::Result::Duplicate) return false;
    }
    // No reader sees a partially published snapshot. A retry can reuse exact
    // chunks left by interruption; a conflicting baseline never replaces it.
    return journal.Append(journal.Current(),{Marker(slot),"home-static-v1",
        std::to_string(digest)+" "+std::to_string(count)+" "+std::to_string(bytes.size())})==CampaignJournal::Result::Committed;
}
struct Upload
{
    uint64_t DigestValue{};
    uint32_t Parts{},Next{};
    std::string Bytes;
    bool Add(uint64_t digest,uint32_t count,uint32_t index,std::string_view bytes)
    {
        if (!count || count>MaxParts || index>=count || bytes.empty() ||
            bytes.size()>PartBytes || (index+1<count && bytes.size()!=PartBytes)) return false;
        if (index==0) {*this={};DigestValue=digest;Parts=count;}
        if (DigestValue!=digest || Parts!=count || Next!=index) return false;
        Bytes+=bytes;++Next;
        return true;
    }
    bool Complete() const {return Parts && Next==Parts && Digest(Bytes)==DigestValue;}
};
}
