#pragma once
#include "CampaignJournal.h"
#include "HomeScope.h"
#include "TradeProperties.h"
#include <algorithm>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// One registry per authoritative campaign journal. A slot is a compact wire
// namespace, never a connection ID, save-local FormID or ownership epoch.
// The first registered interior is the root of the home. Other cells and
// exterior boundaries must be explicitly associated with that root later.
namespace HomeRegistry
{
inline constexpr uint32_t ScopePrefix=HomeScope::Registered;
inline constexpr uint32_t PendingPrefix=HomeScope::Pending;
inline constexpr uint32_t MaxSlot=0xFFFFu;
inline bool Scoped(uint32_t mod) {return (mod&0xFFFF0000u)==ScopePrefix;}
inline bool Pending(uint32_t mod) {return (mod&0xFFFF0000u)==PendingPrefix;}
struct Home
{
    uint64_t Owner{};
    TradeProperties::FormKey Root{};
    uint32_t Slot{};
    bool Same(uint64_t owner,const TradeProperties::FormKey& root) const
    {return Owner==owner && Root.Plugin==root.Plugin && Root.Local==root.Local;}
};
inline std::string Id(uint32_t slot) {return "home-register-"+std::to_string(slot);}
inline std::string Payload(const Home& h)
{return std::to_string(h.Owner)+" "+h.Root.Encode();}
inline std::optional<std::vector<Home>> Read(const CampaignJournal::Journal& journal)
{
    if (!journal.Ready()) return {};
    std::vector<Home> homes;
    for (const auto& row:journal.Operations())
    {
        if (row.Kind!="home-register-v1") continue;
        if (homes.size()>=MaxSlot) return {};
        Home h;h.Slot=static_cast<uint32_t>(homes.size()+1);
        const auto sep=row.Payload.find(' ');
        if (sep==std::string::npos || row.Id!=Id(h.Slot)) return {};
        auto owner=std::string_view(row.Payload).substr(0,sep);
        auto parsed=std::from_chars(owner.data(),owner.data()+owner.size(),h.Owner);
        auto root=TradeProperties::DecodeForm(std::string_view(row.Payload).substr(sep+1));
        if (parsed.ec!=std::errc{} || parsed.ptr!=owner.data()+owner.size() || !h.Owner ||
            !root || !root->Valid()) return {};
        h.Root=*root;
        if (Payload(h)!=row.Payload || std::any_of(homes.begin(),homes.end(),
            [&](const Home& old){return old.Same(h.Owner,h.Root);})) return {};
        homes.push_back(std::move(h));
    }
    return homes;
}
inline std::optional<Home> Find(const CampaignJournal::Journal& journal,uint64_t owner,const TradeProperties::FormKey& root)
{
    auto homes=Read(journal);
    if (!homes) return {};
    for (const auto& h:*homes) if (h.Same(owner,root)) return h;
    return {};
}
inline std::optional<Home> BySlot(const CampaignJournal::Journal& journal,uint32_t slot)
{
    auto homes=Read(journal);
    if (!homes || !slot || slot>homes->size()) return {};
    return (*homes)[slot-1];
}
inline std::optional<Home> Register(CampaignJournal::Journal& journal,uint64_t owner,const TradeProperties::FormKey& root)
{
    if (!owner || !root.Valid()) return {};
    auto homes=Read(journal);
    if (!homes) return {};
    for (const auto& h:*homes) if (h.Same(owner,root)) return h;
    if (homes->size()>=MaxSlot) return {};
    Home h{owner,root,static_cast<uint32_t>(homes->size()+1)};
    if (journal.Append(journal.Current(),{Id(h.Slot),"home-register-v1",Payload(h)})!=CampaignJournal::Result::Committed) return {};
    return h;
}
inline bool OwnsCell(const CampaignJournal::Journal& journal,uint64_t owner,uint32_t mod,uint32_t base)
{
    if (!Scoped(mod) || !owner) return false;
    auto h=BySlot(journal,mod&MaxSlot);
    return h && h->Owner==owner && h->Root.Local==base;
}
}
