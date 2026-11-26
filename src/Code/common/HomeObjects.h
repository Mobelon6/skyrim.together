#pragma once
#include "HomeSnapshot.h"
#include "HomePermissions.h"
#include "HomeLocks.h"

namespace HomeObjects
{
// Changes to presentation do not create or replenish an inventory account.
struct State
{
    uint32_t Disabled{},Open{}; // 0 unknown, 1 open, 3 closed
    std::array<uint32_t,6> Transform{};
    uint32_t GuestDoor{}; // owner-confirmed plain door capability; zero in old rows
    bool Valid() const
    {
        if (Disabled>1 || (Open!=0 && Open!=1 && Open!=3) || GuestDoor>1 || (GuestDoor && !Open)) return false;
        for (auto x:Transform) if (!std::isfinite(std::bit_cast<float>(x))) return false;
        return true;
    }
    std::string Encode() const
    {
        auto s=std::to_string(Disabled)+" "+std::to_string(Open);
        for (auto x:Transform) s+=" "+std::to_string(x);
        if (GuestDoor) s+=" 1"; // retain exact canonical legacy encoding when absent
        return s;
    }
    bool Significant(const State& other) const
    {
        if (Disabled!=other.Disabled || Open!=other.Open || GuestDoor!=other.GuestDoor) return true;
        for (size_t i=0;i<Transform.size();++i)
            if (std::abs(std::bit_cast<float>(Transform[i])-std::bit_cast<float>(other.Transform[i]))>
                (i<3?1.f:0.02f)) return true;
        return false;
    }
};
struct Record
{
    uint32_t Revision{},SaveRevision{},Type{},Inventory{};
    State Value;
};
using Records=std::map<std::string,Record>;
struct Change
{
    uint32_t Slot{},Revision{},SaveRevision{};
    std::string Key;
    State Value;
    std::string Encode() const
    {
        return std::to_string(Slot)+" "+std::to_string(Revision)+" "+std::to_string(SaveRevision)+" "+Key+" "+Value.Encode();
    }
};
inline std::optional<Change> Decode(std::string_view bytes)
{
    Change c;std::string extra;
    std::istringstream in{std::string(bytes)};
    if (!(in>>c.Slot>>c.Revision>>c.SaveRevision>>c.Key>>c.Value.Disabled>>c.Value.Open)) return {};
    for (auto& v:c.Value.Transform) if (!(in>>v)) return {};
    if (in>>extra)
    {
        if (extra!="1" || in>>extra) return {};
        c.Value.GuestDoor=1;
    }
    const auto key=TradeProperties::DecodeForm(c.Key);
    if (in>>extra || !c.Slot || !c.Revision || !key || !key->Valid() || key->Encode()!=c.Key ||
        !c.Value.Valid() || c.Encode()!=bytes) return {};
    return c;
}
inline bool Eligible(const HomeSnapshot::Ref& ref)
{
    return !(ref.Flags&2u) && ref.Inventory!=2 &&
        (!HomeSnapshot::LooseType(ref.Type) || ref.Inventory==3);
}
inline bool Allowed(const Record& previous,const State& state)
{
    return state.Valid() && (previous.Type==29 || (!state.Open && !state.GuestDoor)) &&
        (!previous.Value.Open || state.Open) &&
        (previous.Inventory!=3 || state.Disabled==previous.Value.Disabled);
}
inline std::optional<Records> Read(const CampaignJournal::Journal& journal,uint32_t slot)
{
    const auto baseline=HomeSnapshot::Read(journal,slot);
    if (baseline.State!=HomeSnapshot::Status::Ready) return {};
    const auto state=HomeSnapshot::Decode(baseline.Bytes);
    if (!state) return {};
    Records records;
    for (const auto& r:state->Refs)
        if (Eligible(r)) records.emplace(r.Key.Encode(),Record{0,0,r.Type,r.Inventory,{r.Flags&1u,0,r.Transform}});
    bool published=false;size_t position=0;
    for (const auto& row:journal.Operations())
    {
        ++position;
        if (row.Id==HomeSnapshot::Marker(slot) && row.Kind=="home-static-v1") published=true;
        if (row.Kind!="home-object-v1") continue;
        const auto c=Decode(row.Payload);
        if (!c || row.Id!="home-object-"+std::to_string(position) || c->SaveRevision>=position) return {};
        if (c->Slot!=slot) continue;
        auto it=records.find(c->Key);
        if (!published || it==records.end() || it->second.Revision==UINT32_MAX ||
            c->Revision!=it->second.Revision+1 || c->SaveRevision<it->second.SaveRevision ||
            !Allowed(it->second,c->Value) || !c->Value.Significant(it->second.Value)) return {};
        it->second.Revision=c->Revision;it->second.SaveRevision=c->SaveRevision;it->second.Value=c->Value;
    }
    return records;
}
inline bool Set(CampaignJournal::Journal& journal,uint64_t owner,const Change& change,uint32_t expected)
{
    const auto home=HomeRegistry::BySlot(journal,change.Slot);
    if (!home || !owner || home->Owner!=owner || expected==UINT32_MAX || change.Revision!=expected+1 ||
        change.SaveRevision>journal.Current().Revision) return false;
    auto view=Read(journal,change.Slot);
    if (!view) return false;
    const auto it=view->find(change.Key);
    if (it==view->end() || expected!=it->second.Revision ||
        change.SaveRevision<it->second.SaveRevision || !Allowed(it->second,change.Value)) return false;
    if (!change.Value.Significant(it->second.Value)) return true;
    if (journal.Current().Revision==UINT32_MAX) return false;
    return journal.Append(journal.Current(),{"home-object-"+std::to_string(journal.Current().Revision+1),
        "home-object-v1",change.Encode()})==CampaignJournal::Result::Committed;
}
// A guest requests an explicit target state, not a toggle to replay. Only the
// owner-observed capability grants this operation; pose, visibility, lock and
// owner checkpoint are unchanged. All validations occur on the authority.
inline bool SetGuestDoor(CampaignJournal::Journal& journal,uint64_t actor,uint32_t slot,
    const std::string& key,uint32_t expected,uint32_t desired,const std::array<float,3>& position)
{
    const auto home=HomeRegistry::BySlot(journal,slot);
    if (!home || !actor || home->Owner==actor || expected==UINT32_MAX ||
        (desired!=1 && desired!=3) ||
        HomePermissions::Effective(journal,slot,actor)!=HomePermissions::Right::Modify) return false;
    const auto objects=Read(journal,slot);
    const auto locks=HomeLocks::Read(journal,slot);
    if (!objects || !locks) return false;
    const auto it=objects->find(key);
    if (it==objects->end()) return false;
    const auto& record=it->second;
    const auto& value=record.Value;
    if (!record.Revision || record.Revision!=expected || record.Type!=29 || record.Inventory ||
        !value.GuestDoor || value.Disabled || !value.Open || desired==value.Open) return false;
    const auto lock=locks->find(key);
    if (lock!=locks->end() && lock->second.State==2) return false;
    double distance=0;
    for (size_t i=0;i<position.size();++i)
    {
        if (!std::isfinite(position[i])) return false;
        const double delta=double(position[i])-std::bit_cast<float>(value.Transform[i]);
        distance+=delta*delta;
    }
    if (distance>420.0*420.0) return false;
    auto changed=value;changed.Open=desired;
    return Set(journal,home->Owner,{slot,expected+1,record.SaveRevision,key,changed},expected);
}
inline bool Apply(const CampaignJournal::Journal& journal,uint32_t slot,HomeSnapshot::State& state)
{
    const auto records=Read(journal,slot);
    if (!records) return false;
    for (auto& r:state.Refs)
    {
        const auto it=records->find(r.Key.Encode());
        if (it==records->end() || !it->second.Revision) continue;
        r.Transform=it->second.Value.Transform;
        if (r.Inventory!=3) r.Flags=(r.Flags&~1u)|it->second.Value.Disabled;
    }
    return state.Valid();
}
}
