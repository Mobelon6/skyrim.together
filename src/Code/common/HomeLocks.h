#pragma once
#include "HomeSnapshot.h"

namespace HomeLocks
{
struct LockState
{
    uint32_t State{},Level{},Revision{};
};
using Locks=std::map<std::string,LockState>;
struct Change
{
    uint32_t Slot{};
    std::string Key;
    LockState Value;
};
inline std::string Encode(const Change& c)
{
    return std::to_string(c.Slot)+" "+c.Key+" "+std::to_string(c.Value.Revision)+" "+
        std::to_string(c.Value.State)+" "+std::to_string(c.Value.Level);
}
inline std::optional<Change> Decode(std::string_view bytes)
{
    Change c;std::string extra;
    std::istringstream in{std::string(bytes)};
    if (!(in>>c.Slot>>c.Key>>c.Value.Revision>>c.Value.State>>c.Value.Level) ||
        in>>extra || !c.Slot || c.Value.State<1 || c.Value.State>2 || c.Value.Level>255 ||
        !c.Value.Revision || Encode(c)!=bytes) return {};
    const auto key=TradeProperties::DecodeForm(c.Key);
    if (!key || !key->Valid() || key->Encode()!=c.Key) return {};
    return c;
}
inline std::optional<Locks> Read(const CampaignJournal::Journal& journal,uint32_t slot)
{
    auto baseline=HomeSnapshot::Read(journal,slot);
    if (baseline.State!=HomeSnapshot::Status::Ready) return {};
    auto state=HomeSnapshot::Decode(baseline.Bytes);
    if (!state) return {};
    Locks locks;
    for (const auto& r:state->Refs)
        if ((r.Type==28 || r.Type==29) && r.LockState && !(r.Flags&2))
            locks.emplace(r.Key.Encode(),LockState{r.LockState,r.LockLevel,0});
    bool published=false;size_t position=0;
    for (const auto& row:journal.Operations())
    {
        ++position;
        if (row.Id==HomeSnapshot::Marker(slot) && row.Kind=="home-static-v1") published=true;
        if (row.Kind!="home-lock-v1") continue;
        auto c=Decode(row.Payload);
        if (!c || row.Id!="home-lock-"+std::to_string(position)) return {};
        if (c->Slot!=slot) continue;
        auto it=locks.find(c->Key);
        if (!published || it==locks.end() || it->second.Revision==UINT32_MAX ||
            c->Value.Revision!=it->second.Revision+1 ||
            (c->Value.State==it->second.State && c->Value.Level==it->second.Level)) return {};
        it->second=c->Value;
    }
    return locks;
}
inline bool Set(CampaignJournal::Journal& journal,uint64_t owner,uint32_t slot,
                std::string_view key,uint32_t expected,uint32_t state,uint32_t level)
{
    auto home=HomeRegistry::BySlot(journal,slot);
    if (!home || home->Owner!=owner || !owner || state<1 || state>2 || level>255) return false;
    auto locks=Read(journal,slot);
    if (!locks) return false;
    auto it=locks->find(std::string(key));
    if (it==locks->end() || it->second.Revision!=expected || expected==UINT32_MAX) return false;
    if (it->second.State==state && it->second.Level==level) return true;
    if (journal.Current().Revision==UINT32_MAX) return false;
    return journal.Append(journal.Current(),{"home-lock-"+std::to_string(journal.Current().Revision+1),
        "home-lock-v1",Encode({slot,std::string(key),{state,level,expected+1}})})==CampaignJournal::Result::Committed;
}
inline bool Apply(const CampaignJournal::Journal& journal,uint32_t slot,HomeSnapshot::State& state)
{
    auto locks=Read(journal,slot);
    if (!locks) return false;
    for (auto& r:state.Refs)
        if (auto it=locks->find(r.Key.Encode());it!=locks->end())
        {r.LockState=it->second.State;r.LockLevel=it->second.Level;}
    return state.Valid();
}
}
