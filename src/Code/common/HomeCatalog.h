#pragma once
#include "HomeProjectionPlan.h"
#include <array>
#include <istream>
#include <map>
#include <memory>
#include <set>
#include <sstream>

namespace HomeCatalog
{
inline constexpr std::string_view Plugin="skyrimcoophomeinstances.esp";
struct Ref
{
    TradeProperties::FormKey Source,Base,Target,TargetBase;
};
struct Entrance
{
    TradeProperties::FormKey Door,Outside,Exit;
    std::array<uint32_t,12> Pose{};
};
using MovableResidents=std::map<std::string,TradeProperties::FormKey>;
struct Cell
{
    TradeProperties::FormKey Root,Copy;
    std::vector<Ref> Refs;
    std::vector<Entrance> Entrances;
    std::set<std::string> Unsupported;
    std::set<std::string> ResidentCandidates;
    std::map<std::string,TradeProperties::FormKey> ResidentBases;
    std::vector<TradeProperties::FormKey> Navigation;
    std::set<std::string> GuestDoors;
    // The profile-wide pool is immutable and shared across home cells.
    std::shared_ptr<const MovableResidents> Movable;
};
using Catalog=std::map<std::string,Cell>;
inline std::optional<Catalog> Read(std::istream& in)
{
    std::string line;
    if (!std::getline(in,line) || (line!="STM-HOME-INSTANCES 2" && line!="STM-HOME-INSTANCES 3")) return {};
    const bool withMovable=line=="STM-HOME-INSTANCES 3";
    auto movable=std::make_shared<MovableResidents>();
    Catalog result;
    std::set<std::string> sources,doors;
    std::set<uint32_t> targets;
    size_t lines=0;
    while (std::getline(in,line))
    {
        if (++lines>1000000 || line.empty() || line.size()>2048) return {};
        std::istringstream row(line);
        std::string tag,root,source,base,targetBase,outside,reason,extra;
        uint32_t id{};
        if (!(row>>tag>>root)) return {};
        auto key=TradeProperties::DecodeForm(root);
        if (!key || !key->Valid() || key->Encode()!=root || key->Plugin==Plugin) return {};
        if (tag=="M")
        {
            if (!withMovable || !(row>>base) || row>>extra || movable->size()>=131072) return {};
            auto b=TradeProperties::DecodeForm(base);
            if (!b || !b->Valid() || b->Encode()!=base || b->Plugin==Plugin ||
                (key->Plugin=="skyrim.esm" && key->Local==0x14) ||
                (b->Plugin=="skyrim.esm" && b->Local==7) ||
                !movable->emplace(root,*b).second) return {};
            continue;
        }
        if (tag=="H")
        {
            if (!(row>>id) || row>>extra || id<0x800 || id>0xFFFFFF ||
                !targets.insert(id).second || result.count(root)) return {};
            Cell c;c.Root=*key;c.Copy={std::string(Plugin),id};result.emplace(root,std::move(c));
            continue;
        }
        auto it=result.find(root);
        if (it==result.end()) return {};
        auto& c=it->second;
        if (!(row>>source)) return {};
        auto src=TradeProperties::DecodeForm(source);
        if (!src || !src->Valid() || src->Encode()!=source || src->Plugin==Plugin) return {};
        if (tag=="R")
        {
            if (!(row>>base>>id>>targetBase) || row>>extra || id<0x800 || id>0xFFFFFF ||
                !targets.insert(id).second || !sources.insert(source).second) return {};
            auto b=TradeProperties::DecodeForm(base),tb=TradeProperties::DecodeForm(targetBase);
            if (!b || !tb || !b->Valid() || !tb->Valid() || b->Encode()!=base ||
                tb->Encode()!=targetBase || b->Plugin==Plugin ||
                (tb->Plugin!=Plugin && base!=targetBase)) return {};
            c.Refs.push_back({*src,*b,{std::string(Plugin),id},*tb});
        }
        else if (tag=="T")
        {
            if (!withMovable || row>>extra || !c.GuestDoors.insert(source).second ||
                std::none_of(c.Refs.begin(),c.Refs.end(),[&](const Ref& r)
                {return r.Source.Encode()==source && r.Base.Encode()==r.TargetBase.Encode();})) return {};
        }
        else if (tag=="N")
        {
            if (!(row>>id) || row>>extra || id<0x800 || id>0xFFFFFF ||
                !targets.insert(id).second || !sources.insert(source).second) return {};
            c.Navigation.push_back({std::string(Plugin),id});
        }
        else if (tag=="A")
        {
            if (!(row>>base) || row>>extra || !sources.insert(source).second ||
                c.ResidentBases.size()>=128) return {};
            const auto b=TradeProperties::DecodeForm(base);
            if (!b || !b->Valid() || b->Encode()!=base || b->Plugin==Plugin) return {};
            c.ResidentCandidates.insert(source);
            c.ResidentBases.emplace(source,*b);
        }
        else if (tag=="U")
        {
            if (!(row>>reason) || row>>extra || !sources.insert(source).second) return {};
            c.Unsupported.insert(source);
            if (reason=="ACHR") c.ResidentCandidates.insert(source);
        }
        else if (tag=="D")
        {
            if (!(row>>outside>>id) || !doors.insert(source).second) return {};
            auto ext=TradeProperties::DecodeForm(outside);
            if (!ext || !ext->Valid() || ext->Encode()!=outside || ext->Plugin==Plugin ||
                ext->Encode()==root) return {};
            Entrance e;e.Door=*src;e.Outside=*ext;e.Exit={std::string(Plugin),id};
            for (auto& n:e.Pose)
                if (!(row>>n) || !std::isfinite(std::bit_cast<float>(n))) return {};
            if (row>>extra || std::none_of(c.Refs.begin(),c.Refs.end(),
                [&](const Ref& r){return r.Target.Local==id;})) return {};
            c.Entrances.push_back(std::move(e));
        }
        else return {};
    }
    if (!in.eof() || result.empty()) return {};
    if (withMovable)
        for (auto& [_,cell]:result)
        {
            for (const auto& [key,base]:cell.ResidentBases)
            {
                const auto it=movable->find(key);
                if (it==movable->end() || it->second.Encode()!=base.Encode()) return {};
            }
            for (const auto& key:cell.GuestDoors)
            {
                const auto binding=std::find_if(cell.Refs.begin(),cell.Refs.end(),
                    [&](const Ref& r){return r.Source.Encode()==key;});
                if (binding==cell.Refs.end() || std::any_of(cell.Entrances.begin(),cell.Entrances.end(),
                    [&](const Entrance& e){return e.Exit.Encode()==binding->Target.Encode();})) return {};
            }
            cell.Movable=movable;
        }
    return result;
}
// A moved-in reference is an observed occupant, not a grant of ownership.
inline const TradeProperties::FormKey* ResidentBase(const Cell& cell,const std::string& key)
{
    if (cell.Unsupported.count(key)) return nullptr;
    const auto placed=cell.ResidentBases.find(key);
    if (placed!=cell.ResidentBases.end()) return &placed->second;
    if (!cell.Movable || std::any_of(cell.Refs.begin(),cell.Refs.end(),
        [&](const Ref& r){return r.Source.Encode()==key;})) return nullptr;
    const auto it=cell.Movable->find(key);
    return it==cell.Movable->end()?nullptr:&it->second;
}
inline std::optional<std::vector<HomeProjection::Binding>> Bind(const Cell& cell,const HomeSnapshot::State& state)
{
    if (!cell.Unsupported.empty() || cell.Entrances.empty() || state.Omitted ||
        cell.Refs.size()!=state.Refs.size() || cell.ResidentBases.size()!=state.ResidentKeys.size()) return {};
    for (const auto& [key,_]:cell.ResidentBases) if (!state.ResidentKeys.count(key)) return {};
    std::map<std::string,const Ref*> refs;
    for (const auto& r:cell.Refs) refs.emplace(r.Source.Encode(),&r);
    std::vector<HomeProjection::Binding> bindings;
    for (const auto& row:state.Refs)
    {
        auto it=refs.find(row.Key.Encode());
        if (it==refs.end() || it->second->Base.Encode()!=row.Base.Encode()) return {};
        const auto& r=*it->second;
        // Only a container may be rebound to a dedicated blank base.
        if ((row.Type==28)!=(r.TargetBase.Plugin==Plugin)) return {};
        bindings.push_back({r.Source,r.Target,r.TargetBase,row.Type});
    }
    if (!HomeProjection::Plan(state,bindings,Plugin)) return {};
    return bindings;
}
}
