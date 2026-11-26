#pragma once
#include "HomeSnapshot.h"
#include <map>
#include <set>
#include <vector>

namespace HomeProjection
{
struct Binding
{
    TradeProperties::FormKey Source,Target,TargetBase;
    uint32_t Type{};
};
struct Step
{
    const HomeSnapshot::Ref* State{};
    Binding Destination;
};
inline std::optional<std::vector<Step>> Plan(const HomeSnapshot::State& state,
    const std::vector<Binding>& bindings,std::string_view isolatedPlugin)
{
    if (!state.Valid() || state.Omitted || bindings.size()!=state.Refs.size() ||
        isolatedPlugin.empty()) return {};
    std::map<std::string,Binding> bySource;
    std::set<std::string> targets;
    for (const auto& b:bindings)
    {
        if (!b.Source.Valid() || !b.Target.Valid() || !b.TargetBase.Valid() ||
            b.Target.Plugin!=isolatedPlugin || b.Source.Plugin==isolatedPlugin ||
            !targets.insert(b.Target.Encode()).second ||
            !bySource.emplace(b.Source.Encode(),b).second) return {};
    }
    std::vector<Step> result;
    for (const auto& row:state.Refs)
    {
        auto it=bySource.find(row.Key.Encode());
        if (it==bySource.end() || row.Type!=it->second.Type ||
            row.Inventory==2 || (HomeSnapshot::LooseType(row.Type) && row.Inventory!=3) || (row.Flags&2)) return {};
        // Source deletion is not reproduced by destroying a reusable shell.
        // Missing/unsupported inventory is never interpreted as empty.
        result.push_back({&row,it->second});
    }
    return result;
}
}
