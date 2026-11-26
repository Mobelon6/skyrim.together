#pragma once
#include <cstdint>
#include <cstddef>
#include <limits>
#include <map>
#include <vector>
#include <optional>
#include <string>
#include <utility>

namespace InventoryEvidence
{
inline constexpr uint32_t Player=0x14;
inline constexpr std::size_t MaxEvents=4096;
using Counts=std::map<uint32_t,int64_t>;
struct Transfer
{
    uint32_t Source{},Destination{},Item{};
    int32_t Count{};
    uint32_t Reference{};
    uint16_t Unique{};
    bool Override{};
};
enum class Status { MatchedNet, Unexplained, Mismatch, Invalid };
struct Row { uint32_t Item; int64_t Before,After,Expected; Status State; };
struct Batch { bool Valid=true; std::vector<Row> Rows; };
inline const char* Label(enum Status status)
{
    switch(status)
    {
        case Status::MatchedNet:return "matched-net-only";
        case Status::Unexplained:return "unexplained";
        case Status::Mismatch:return "mismatch";
        default:return "invalid";
    }
}
inline bool Accumulate(Counts& counts,uint32_t id,int64_t delta)
{
    if (!id) return false;
    auto& value=counts[id];
    if ((delta>0 && value>std::numeric_limits<int64_t>::max()-delta) ||
        (delta<0 && value<std::numeric_limits<int64_t>::min()-delta)) return false;
    value+=delta;
    return true;
}
inline Batch Compare(const Counts& before,const Counts& after,const std::vector<Transfer>& events)
{
    Batch result;
    Counts expected,keys;
    for (const auto& [id,n]:before) { keys[id]=0; if (!id || n<0 || n>INT32_MAX) result.Valid=false; }
    for (const auto& [id,n]:after) { keys[id]=0; if (!id || n<0 || n>INT32_MAX) result.Valid=false; }
    if (events.size()>MaxEvents) result.Valid=false;
    for (const auto& event:events)
    {
        const bool from=event.Source==Player,to=event.Destination==Player;
        if (!event.Item || event.Count<=0 || (!from && !to))
        { result.Valid=false; continue; }
        keys[event.Item]=0;
        if (!Accumulate(expected,event.Item,(to?int64_t(event.Count):0)-(from?int64_t(event.Count):0)))
            result.Valid=false;
    }
    const auto value=[](const Counts& counts,uint32_t id){ auto i=counts.find(id);return i==counts.end()?int64_t(0):i->second; };
    for (const auto& [id,unused]:keys)
    {
        const auto old=value(before,id),now=value(after,id),delta=value(expected,id);
        const bool observed=expected.count(id)!=0;
        if (!observed && old==now) continue;
        // Agreement is aggregate count evidence, never proof of a unique item,
        // the other endpoint, ordering, or an authoritative operation.
        const auto status=!result.Valid ? Status::Invalid : !observed ? Status::Unexplained :
            now-old==delta ? Status::MatchedNet : Status::Mismatch;
        result.Rows.push_back({id,old,now,delta,status});
    }
    return result;
}

inline constexpr std::size_t MaxEndpoints=16;
struct PluginSlot { uint16_t Index; bool Light; std::string Name; };
struct FormKey { std::string Plugin; uint32_t Local; };
inline std::string CanonicalPlugin(std::string name)
{
    for (auto& ch:name) if (ch>='A' && ch<='Z') ch=static_cast<char>(ch-'A'+'a');
    return name;
}
inline std::optional<FormKey> Resolve(uint32_t form,const std::vector<PluginSlot>& slots)
{
    if (!form || (form>>24)==0xFF) return {};
    const bool light=(form>>24)==0xFE;
    const auto index=static_cast<uint16_t>(light ? ((form>>12)&0xFFF) : (form>>24));
    const auto local=form&(light?0xFFFu:0xFFFFFFu);
    if (!local) return {};
    const PluginSlot* found=nullptr;
    for (const auto& slot:slots)
        if (slot.Light==light && slot.Index==index)
        {
            if (found || slot.Name.empty()) return {};
            found=&slot;
        }
    if (!found) return {};
    const auto name=CanonicalPlugin(found->Name);
    // A duplicate filename in the effective table is ambiguous even if the slot is unique.
    for (const auto& slot:slots)
        if (&slot!=found && CanonicalPlugin(slot.Name)==name) return {};
    return FormKey{name,local};
}

struct Endpoint { uint32_t Server,Entity; Counts Items; };
using Endpoints=std::map<uint32_t,Endpoint>;
enum class PairStatus { MatchedBothNets, MissingSnapshot, ChangedEndpoint, PlayerMismatch, OtherMismatch, Invalid };
struct PairRow { uint32_t Container,Item; int64_t PlayerNet,OtherNet; PairStatus State; };
inline const char* PairLabel(PairStatus s)
{
    switch(s)
    {
        case PairStatus::MatchedBothNets:return "matched-both-nets-only";
        case PairStatus::MissingSnapshot:return "missing-counterpart-snapshot";
        case PairStatus::ChangedEndpoint:return "changed-counterpart-entity";
        case PairStatus::PlayerMismatch:return "player-net-unverified";
        case PairStatus::OtherMismatch:return "counterpart-net-mismatch";
        default:return "invalid";
    }
}
inline std::vector<PairRow> CompareEndpoints(const Endpoints& before,const Endpoints& after,
    const std::vector<Transfer>& events,const Batch& player)
{
    std::map<std::pair<uint32_t,uint32_t>,int64_t> expected;
    for (const auto& e:events)
    {
        if (!e.Item || e.Count<=0) continue;
        const bool from=e.Source==Player,to=e.Destination==Player;
        if (from==to) continue;
        const auto counterpart=from?e.Destination:e.Source;
        if (!counterpart || (counterpart>>24)==0xFF) continue;
        expected[{counterpart,e.Item}]+=from?-int64_t(e.Count):int64_t(e.Count);
    }
    std::vector<PairRow> result;
    const auto value=[](const Counts& counts,uint32_t id)
    {
        auto it=counts.find(id);return it==counts.end()?int64_t(0):it->second;
    };
    for (const auto& [key,net]:expected)
    {
        const auto [container,item]=key;
        PairRow row{container,item,net,0,PairStatus::MissingSnapshot};
        const auto old=before.find(container),now=after.find(container);
        bool playerMatched=false;
        for (const auto& p:player.Rows)
            if (p.Item==item) playerMatched=p.State==Status::MatchedNet;
        if (!player.Valid) row.State=PairStatus::Invalid;
        else if (!playerMatched) row.State=PairStatus::PlayerMismatch;
        else if (old!=before.end() && now!=after.end())
        {
            if (old->second.Server!=now->second.Server || old->second.Entity!=now->second.Entity)
                row.State=PairStatus::ChangedEndpoint;
            else
            {
                row.OtherNet=value(now->second.Items,item)-value(old->second.Items,item);
                row.State=row.OtherNet==-net?PairStatus::MatchedBothNets:PairStatus::OtherMismatch;
            }
        }
        result.push_back(row);
    }
    return result;
}
}
