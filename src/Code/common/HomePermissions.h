#pragma once
#include "HomeRegistry.h"
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <set>
#include <tuple>

namespace HomePermissions
{
enum class Right : uint32_t { None, Visit, Modify };
struct Change
{
    uint32_t Slot{};
    uint64_t Owner{}, Guest{};
    Right Access{};
};
inline std::string Encode(const Change& c)
{
    return std::to_string(c.Slot)+" "+std::to_string(c.Owner)+" "+
        std::to_string(c.Guest)+" "+std::to_string(static_cast<uint32_t>(c.Access));
}
inline std::optional<Change> Decode(std::string_view bytes)
{
    Change c;uint32_t right{};std::string extra;
    std::istringstream in{std::string(bytes)};
    if (!(in>>c.Slot>>c.Owner>>c.Guest>>right) || in>>extra ||
        !c.Slot || !c.Owner || !c.Guest || c.Owner==c.Guest || right>2) return {};
    c.Access=static_cast<Right>(right);
    if (Encode(c)!=bytes) return {};
    return c;
}
struct GroupChange
{
    uint64_t Owner{},Guest{};
    Right Access{};
    std::vector<uint32_t> Slots;
};
inline constexpr size_t MaxGroupRooms=128;
inline std::string Encode(const GroupChange& c)
{
    std::string out=std::to_string(c.Owner)+" "+std::to_string(c.Guest)+" "+
        std::to_string(static_cast<uint32_t>(c.Access))+" "+std::to_string(c.Slots.size());
    for (auto slot:c.Slots) out+=" "+std::to_string(slot);
    return out;
}
inline std::optional<GroupChange> DecodeGroup(std::string_view bytes)
{
    GroupChange c;uint32_t right{},count{};std::string extra;
    std::istringstream in{std::string(bytes)};
    if (!(in>>c.Owner>>c.Guest>>right>>count)||!c.Owner||!c.Guest||c.Owner==c.Guest||
        right>2||count<2||count>MaxGroupRooms) return {};
    c.Access=static_cast<Right>(right);
    for (uint32_t i=0;i<count;++i)
    {
        uint32_t slot{};
        if (!(in>>slot)||!slot||slot>HomeRegistry::MaxSlot||(!c.Slots.empty()&&slot<=c.Slots.back())) return {};
        c.Slots.push_back(slot);
    }
    if (in>>extra||Encode(c)!=bytes) return {};
    return c;
}
// Explicit owner consent for a plot, including its known but unregistered rooms.
// Store stable room keys, not a mutable list of registry slots or a live catalog lookup.
struct EstateChange
{
    uint64_t Owner{},Guest{};
    Right Access{};
    std::string Plot;
    std::set<std::string> Rooms;
    bool operator==(const EstateChange&) const=default;
};
using EstateKey=std::tuple<uint64_t,std::string,uint64_t>;
inline bool StableKey(const std::string& key)
{
    const auto form=TradeProperties::DecodeForm(key);
    return form&&form->Valid()&&form->Encode()==key;
}
inline std::string Encode(const EstateChange& c)
{
    std::string s=std::to_string(c.Owner)+" "+std::to_string(c.Guest)+" "+
        std::to_string(static_cast<uint32_t>(c.Access))+" "+c.Plot+" "+std::to_string(c.Rooms.size());
    for (const auto& room:c.Rooms) s+=" "+room;
    return s;
}
inline std::optional<EstateChange> DecodeEstate(std::string_view bytes)
{
    EstateChange c;uint32_t right{},count{};std::string extra;
    std::istringstream in{std::string(bytes)};
    if (!(in>>c.Owner>>c.Guest>>right>>c.Plot>>count)||!c.Owner||!c.Guest||c.Owner==c.Guest||
        right>2||!StableKey(c.Plot)||!count||count>MaxGroupRooms) return {};
    c.Access=static_cast<Right>(right);
    for (uint32_t i=0;i<count;++i)
    {
        std::string room;
        if (!(in>>room)||!StableKey(room)||!c.Rooms.insert(room).second) return {};
    }
    if (in>>extra||Encode(c)!=bytes) return {};
    return c;
}
inline bool AccessOperation(std::string_view kind)
{return kind=="home-access-v1"||kind=="home-access-group-v1"||kind=="home-access-estate-v1";}
inline std::optional<std::vector<Change>> Changes(std::string_view kind,std::string_view bytes)
{
    if (kind=="home-access-v1")
    {
        const auto c=Decode(bytes);
        if (c) return std::vector<Change>{*c};
    }
    else if (kind=="home-access-group-v1")
    {
        const auto c=DecodeGroup(bytes);
        if (c)
        {
            std::vector<Change> result;
            for (auto slot:c->Slots) result.push_back({slot,c->Owner,c->Guest,c->Access});
            return result;
        }
    }
    return {};
}
struct Rules
{
    std::map<std::pair<uint32_t,uint64_t>,Right> Guests;
    std::map<EstateKey,EstateChange> Estates;
    uint32_t Revision{};
    size_t Registered{};
};
inline bool Apply(Rules& rules,uint32_t slot,uint64_t guest,Right access)
{
    const auto key=std::pair{slot,guest};
    const auto it=rules.Guests.find(key);
    if ((it==rules.Guests.end()?Right::None:it->second)==access) return false;
    if (access==Right::None) rules.Guests.erase(key);
    else rules.Guests[key]=access;
    return true;
}
// Replay only the requested journal prefix. Explicit estate grants apply to
// every matching room registered by that prefix, even if registered later than
// the grant. Legacy slot grants never acquire this behavior.
inline std::optional<Rules> ReadAt(const CampaignJournal::Journal& journal,size_t limit)
{
    const auto homes=HomeRegistry::Read(journal);
    const auto& ops=journal.Operations();
    if (!homes||limit>ops.size()) return {};
    Rules out;
    for (size_t i=0;i<limit;++i) if (ops[i].Kind=="home-register-v1") ++out.Registered;
    if (out.Registered>homes->size()) return {};
    size_t seen=0;
    for (size_t i=0;i<limit;++i)
    {
        const auto& row=ops[i];
        if (row.Kind=="home-register-v1") {++seen;continue;}
        if (!AccessOperation(row.Kind)) continue;
        if (out.Revision==UINT32_MAX||row.Id!="home-access-"+std::to_string(out.Revision+1)) return {};
        if (row.Kind=="home-access-estate-v1")
        {
            const auto c=DecodeEstate(row.Payload);
            if (!c) return {};
            const EstateKey key{c->Owner,c->Plot,c->Guest};
            const auto old=out.Estates.find(key);
            // The catalog is frozen for a campaign profile. Never silently
            // reinterpret previously granted room membership after profile edits.
            if (old!=out.Estates.end()&&old->second.Rooms!=c->Rooms) return {};
            bool observed=false;
            for (size_t k=0;k<out.Registered;++k)
            {
                const auto& home=(*homes)[k];
                if (home.Owner!=c->Owner||!c->Rooms.count(home.Root.Encode())) continue;
                if (k<seen) observed=true;
                Apply(out,home.Slot,c->Guest,c->Access);
            }
            if (!observed) return {};
            out.Estates[key]=*c;
        }
        else
        {
            const auto changes=Changes(row.Kind,row.Payload);
            if (!changes) return {};
            bool changed=false;
            for (const auto& c:*changes)
            {
                if (c.Slot>seen||(*homes)[c.Slot-1].Owner!=c.Owner) return {};
                if (Apply(out,c.Slot,c.Guest,c.Access)) changed=true;
            }
            if (!changed) return {};
        }
        ++out.Revision;
    }
    return out;
}
inline std::optional<Rules> Read(const CampaignJournal::Journal& journal)
{return ReadAt(journal,journal.Operations().size());}
inline Right EffectiveAt(const CampaignJournal::Journal& journal,uint32_t slot,uint64_t character,size_t limit)
{
    if (!character) return Right::None;
    const auto home=HomeRegistry::BySlot(journal,slot);
    const auto rules=ReadAt(journal,limit);
    if (!home||!rules||slot>rules->Registered) return Right::None;
    if (home->Owner==character) return Right::Modify;
    const auto it=rules->Guests.find({slot,character});
    return it==rules->Guests.end()?Right::None:it->second;
}
inline Right Effective(const CampaignJournal::Journal& journal,uint32_t slot,uint64_t character)
{return EffectiveAt(journal,slot,character,journal.Operations().size());}
inline Right EstateRight(const Rules& rules,uint64_t owner,const std::string& plot,uint64_t guest)
{
    if (!owner||!guest) return Right::None;
    if (owner==guest) return Right::Modify; // presence only, not proof of land purchase
    const auto it=rules.Estates.find({owner,plot,guest});
    return it==rules.Estates.end()?Right::None:it->second.Access;
}
inline bool SetEstate(CampaignJournal::Journal& journal,uint64_t actor,const std::string& plot,
    const std::set<std::string>& rooms,uint64_t guest,Right access,uint32_t expected)
{
    const auto homes=HomeRegistry::Read(journal);
    const auto rules=Read(journal);
    const EstateChange c{actor,guest,access,plot,rooms};
    const auto payload=Encode(c);
    if (!homes||!rules||rules->Revision!=expected||!DecodeEstate(payload)) return false;
    const auto old=rules->Estates.find({actor,plot,guest});
    if (old!=rules->Estates.end()&&old->second.Rooms!=rooms) return false;
    bool observed=false,changed=old==rules->Estates.end()||old->second.Access!=access;
    for (const auto& home:*homes)
    {
        if (home.Owner!=actor||!rooms.count(home.Root.Encode())) continue;
        observed=true;
        const auto it=rules->Guests.find({home.Slot,guest});
        if ((it==rules->Guests.end()?Right::None:it->second)!=access) changed=true;
    }
    if (!observed) return false;
    if (!changed) return true;
    if (expected==UINT32_MAX) return false;
    return journal.Append(journal.Current(),{"home-access-"+std::to_string(expected+1),
        "home-access-estate-v1",payload})==CampaignJournal::Result::Committed;
}
inline bool Set(CampaignJournal::Journal& journal,uint64_t actor,uint32_t slot,
                uint64_t guest,Right right,uint32_t expected)
{
    auto home=HomeRegistry::BySlot(journal,slot);
    auto rules=Read(journal);
    if (!home || !rules || !actor || actor!=home->Owner || !guest || guest==actor ||
        static_cast<uint32_t>(right)>2 || rules->Revision!=expected) return false;
    const auto it=rules->Guests.find({slot,guest});
    const auto prior=it==rules->Guests.end()?Right::None:it->second;
    if (prior==right) return true;
    if (rules->Revision==UINT32_MAX) return false;
    return journal.Append(journal.Current(),{"home-access-"+std::to_string(rules->Revision+1),
        "home-access-v1",Encode({slot,actor,guest,right})})==CampaignJournal::Result::Committed;
}
inline bool SetGroup(CampaignJournal::Journal& journal,uint64_t actor,const std::vector<uint32_t>& slots,
                     uint64_t guest,Right right,uint32_t expected)
{
    const auto homes=HomeRegistry::Read(journal);
    const auto rules=Read(journal);
    const GroupChange change{actor,guest,right,slots};
    const auto bytes=Encode(change);
    if (!homes||!rules||rules->Revision!=expected||!DecodeGroup(bytes)) return false;
    bool changed=false;
    for (auto slot:slots)
    {
        if (slot>homes->size()||(*homes)[slot-1].Owner!=actor) return false;
        const auto it=rules->Guests.find({slot,guest});
        if ((it==rules->Guests.end()?Right::None:it->second)!=right) changed=true;
    }
    if (!changed) return true;
    if (expected==UINT32_MAX) return false;
    return journal.Append(journal.Current(),{"home-access-"+std::to_string(expected+1),
        "home-access-group-v1",bytes})==CampaignJournal::Result::Committed;
}
}
