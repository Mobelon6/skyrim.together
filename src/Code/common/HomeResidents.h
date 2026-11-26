#pragma once
#include "HomeSnapshot.h"

// Observation of a stable placed NPC, not a second inventory or ownership grant.
namespace HomeResidents
{
inline constexpr size_t MaxResidents=128;
struct State
{
    TradeProperties::FormKey Reference,Base;
    uint32_t Flags{},Scale{}; // disabled=1, dead=2, deleted=4, confirmed elsewhere=8
    bool Away() const {return (Flags&8u)!=0;}
    bool Hidden() const {return (Flags&13u)!=0;}
    std::array<uint32_t,6> Transform{};
    bool Valid() const
    {
        if (!Reference.Valid() || !Base.Valid() || Flags>15 || !Scale || Scale>65535 ||
            Reference.Plugin=="skyrimcoophomeinstances.esp" || Base.Plugin=="skyrimcoophomeinstances.esp" ||
            (Reference.Plugin=="skyrim.esm" && Reference.Local==0x14) ||
            (Base.Plugin=="skyrim.esm" && Base.Local==7)) return false;
        for (auto v:Transform) if (!std::isfinite(std::bit_cast<float>(v))) return false;
        return true;
    }
    std::string Encode() const
    {
        auto s=Reference.Encode()+" "+Base.Encode()+" "+std::to_string(Flags)+" "+std::to_string(Scale);
        for (auto v:Transform) s+=" "+std::to_string(v);
        return s;
    }
    bool SameStatus(const State& other) const
    {
        return Reference.Encode()==other.Reference.Encode() && Base.Encode()==other.Base.Encode() &&
            Flags==other.Flags && Scale==other.Scale;
    }
    bool Significant(const State& other) const
    {
        if (!SameStatus(other)) return true;
        if (Away()) return false; // outside pose is not a pose in this instance
        for (size_t i=0;i<Transform.size();++i)
            if (std::abs(std::bit_cast<float>(Transform[i])-std::bit_cast<float>(other.Transform[i]))>
                (i<3?64.f:0.5f)) return true;
        return false;
    }
};
inline std::optional<State> Decode(std::string_view bytes)
{
    if (bytes.empty() || bytes.size()>HomeSnapshot::PartBytes) return {};
    std::istringstream in{std::string(bytes)};
    std::string ref,base,extra;State s;
    if (!(in>>ref>>base>>s.Flags>>s.Scale)) return {};
    for (auto& v:s.Transform) if (!(in>>v)) return {};
    auto r=TradeProperties::DecodeForm(ref),b=TradeProperties::DecodeForm(base);
    if (!r || !b || in>>extra) return {};
    s.Reference=*r;s.Base=*b;
    if (!s.Valid() || s.Encode()!=bytes) return {};
    return s;
}
struct Record
{
    uint32_t Revision{},SaveRevision{};
    State Value;
    uint32_t JournalRevision{}; // derived from the row position, not serialized
};
using Residents=std::map<std::string,Record>;
inline std::string Payload(uint32_t slot,const Record& r)
{
    return std::to_string(slot)+" "+std::to_string(r.Revision)+" "+std::to_string(r.SaveRevision)+" "+r.Value.Encode();
}
inline std::optional<Residents> Read(const CampaignJournal::Journal& journal,uint32_t slot)
{
    const auto homes=HomeRegistry::Read(journal);
    if (!homes || !slot || slot>homes->size()) return {};
    Residents result;
    size_t position=0;
    for (const auto& row:journal.Operations())
    {
        ++position;
        if (row.Kind!="home-resident-v1") continue;
        if (row.Id!="home-resident-"+std::to_string(position)) return {};
        std::istringstream in(row.Payload);
        uint32_t house{};Record r;
        std::string rest;
        if (!(in>>house>>r.Revision>>r.SaveRevision) || !std::getline(in,rest) ||
            rest.empty() || rest.front()!=' ' || !house || house>homes->size() ||
            !r.Revision || r.SaveRevision>=position) return {};
        auto state=Decode(std::string_view(rest).substr(1));
        if (!state) return {};
        r.Value=*state;r.JournalRevision=static_cast<uint32_t>(position);
        if (Payload(house,r)!=row.Payload) return {};
        if (house!=slot) continue;
        const auto key=r.Value.Reference.Encode();
        const auto it=result.find(key);
        if (it==result.end())
        {
            if (r.Revision!=1 || result.size()>=MaxResidents) return {};
        }
        else if (it->second.Revision==UINT32_MAX || r.Revision!=it->second.Revision+1 ||
            r.SaveRevision<it->second.SaveRevision || r.Value.Base.Encode()!=it->second.Value.Base.Encode() ||
            !r.Value.Significant(it->second.Value)) return {};
        result[key]=std::move(r);
    }
    return result;
}
inline bool Set(CampaignJournal::Journal& journal,uint64_t owner,uint32_t slot,uint32_t expected,
                uint32_t saveRevision,const State& state)
{
    auto home=HomeRegistry::BySlot(journal,slot);
    if (!home || !owner || home->Owner!=owner || !state.Valid() ||
        saveRevision>journal.Current().Revision || expected==UINT32_MAX) return false;
    auto current=Read(journal,slot);
    if (!current) return false;
    const auto it=current->find(state.Reference.Encode());
    if (it==current->end())
    {
        if (expected || current->size()>=MaxResidents) return false;
    }
    else
    {
        if (it->second.Revision!=expected || saveRevision<it->second.SaveRevision ||
            state.Base.Encode()!=it->second.Value.Base.Encode()) return false;
        if (!state.Significant(it->second.Value)) return true;
    }
    if (journal.Current().Revision==UINT32_MAX) return false;
    return journal.Append(journal.Current(),{"home-resident-"+std::to_string(journal.Current().Revision+1),
        "home-resident-v1",Payload(slot,{expected+1,saveRevision,state})})==CampaignJournal::Result::Committed;
}
// Reconciliation of a stable original must not create an actor, replay a
// death/resurrection, reset an outfit, or mutate an unrelated leveled base.
enum class Recovery { Matched, Restore, Conflict };
inline Recovery RecoveryPlan(const State& committed,const State& observed)
{
    if (!committed.Valid() || !observed.Valid() ||
        committed.Reference.Encode()!=observed.Reference.Encode() ||
        committed.Base.Encode()!=observed.Base.Encode() ||
        committed.Scale!=observed.Scale || ((committed.Flags^observed.Flags)&14u))
        return Recovery::Conflict;
    if (committed.Flags&4u) return Recovery::Matched;
    if (committed.Away()) return committed.SameStatus(observed)?Recovery::Matched:Recovery::Conflict;
    if ((committed.Flags^observed.Flags)&1u) return Recovery::Restore;
    if (committed.Flags&1u) return Recovery::Matched;
    return committed.Significant(observed)?Recovery::Restore:Recovery::Matched;
}
// A fresh session can accept a newly observed crossing only if its applied
// checkpoint includes the earlier observation. Never teleport a resident
// between cells or infer an absence from unload/missing reference.
inline bool CanObservePresence(const Record& old,uint32_t applied,const State& observed)
{
    const auto& s=old.Value;
    return old.JournalRevision && applied>=old.JournalRevision &&
        s.Valid() && observed.Valid() && s.Reference.Encode()==observed.Reference.Encode() &&
        s.Base.Encode()==observed.Base.Encode() && s.Scale==observed.Scale &&
        ((s.Flags^observed.Flags)&15u)==8u;
}
// A fresh observation session cannot overwrite a conflicting life state.
inline bool CanResume(const Record& old,uint32_t saveRevision,const State& observed)
{
    return saveRevision>=old.SaveRevision && observed.SameStatus(old.Value);
}
}
