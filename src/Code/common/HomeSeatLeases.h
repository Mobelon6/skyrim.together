#pragma once
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>

namespace HomeSeatLeases
{
using Clock=std::chrono::steady_clock;
// Public furniture is session-scoped, not part of the durable home journal.
inline constexpr uint32_t WorkbenchOperation=11, WorkbenchScope=UINT32_MAX;
inline constexpr bool IsWorkbenchType(uint8_t type) noexcept {return type>=1&&type<=7;}
struct Usage
{
    Clock::time_point Grace{};
    bool Seen{};
    bool Keep(bool observed,Clock::time_point now) noexcept
    {
        if (observed) Seen=true;
        return observed||(!Seen&&now<Grace);
    }
};
struct Claim
{
    uint64_t Campaign{},Character{},Connection{},Nonce{};
    uint32_t Player{},Slot{},Cell{};
    std::string Ref;
    uint32_t CellMod{};
    uint64_t Location{};
    bool operator==(const Claim&) const=default;
};
struct Lease {Claim Who;Clock::time_point Until;};
using Key=std::tuple<uint64_t,uint32_t,std::string>;
struct Table
{
    std::map<Key,Lease> Seats;
    template<class Valid> void Prune(Clock::time_point now,Valid valid)
    {
        std::erase_if(Seats,[&](const auto& p){return p.second.Until<=now||!valid(p.second.Who);});
    }
    bool Change(uint32_t action,const Claim& c,Clock::time_point now)
    {
        if (!c.Campaign||!c.Character||!c.Nonce||!c.Slot||!c.Cell||c.Ref.empty()||action>2) return false;
        Prune(now,[](const Claim&){return true;});
        const Key key{c.Campaign,c.Slot,c.Ref};
        auto it=Seats.find(key);
        if (action==0)
        {
            if (it!=Seats.end()&&it->second.Who==c) {Seats.erase(it);return true;}
            return false;
        }
        if (it!=Seats.end())
        {
            if (!(it->second.Who==c)) return false;
            it->second.Until=now+std::chrono::seconds(30);return true;
        }
        if (action!=1) return false; // late renewal never recreates a released seat
        for (const auto& [_,lease]:Seats)
            if (lease.Who.Campaign==c.Campaign&&lease.Who.Character==c.Character) return false;
        Seats.emplace(key,Lease{c,now+std::chrono::seconds(30)});
        return true;
    }
};
}
