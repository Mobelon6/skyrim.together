#pragma once
#include "HomeCatalog.h"
#include "HomeResidents.h"
#include <fstream>
#include <filesystem>
#include <tuple>

namespace HomeNativeCapabilities
{
using Row=std::tuple<char,std::string,std::string,std::string>;
inline std::optional<std::set<Row>> Read(std::istream& in)
{
    std::string line;
    if (!std::getline(in,line)||line!="STM-NATIVE-HOME-OBJECTS 1") return {};
    std::set<Row> rows;
    std::set<std::string> residentRefs;
    while (std::getline(in,line))
    {
        if (line.empty()||line.size()>2048||rows.size()>=1000000) return {};
        std::istringstream s(line);
        char tag{};std::string root,ref,base,extra;
        if (!(s>>tag>>root>>ref>>base)||(s>>extra)||(tag!='D'&&tag!='S'&&tag!='A'&&tag!='M'&&tag!='L')) return {};
        if ((tag=='A'||tag=='M')&&!residentRefs.insert(ref).second) return {};
        for (const auto* key:{&root,&ref,&base})
        {
            const auto k=TradeProperties::DecodeForm(*key);
            if (!k||!k->Valid()||k->Encode()!=*key||k->Plugin==HomeCatalog::Plugin||
                k->Plugin=="skyrimcoopguesthomes.esp") return {};
        }
        if (!rows.emplace(tag,root,ref,base).second) return {};
    }
    if (!in.eof()) return {};
    return rows;
}
inline const std::optional<std::set<Row>>& Loaded()
{
    static const auto rows=[]()
    {
        std::ifstream f(std::filesystem::path("Data")/"SkyrimTogetherReborn"/"config"/"CoopNativeHomeObjects.txt");
        return Read(f);
    }();
    return rows;
}
inline bool Allows(char tag,const std::string& root,const std::string& ref,const std::string& base)
{
    try {const auto& rows=Loaded();return rows&&rows->count({tag,root,ref,base});}
    catch (...) {return false;}
}
inline bool LinkedCell(const std::string& root)
{
    static const auto cells=[]()
    {
        std::set<std::string> result;
        if (const auto& rows=Loaded();rows)
            for (const auto& [tag,cell,ref,base]:*rows)
                if (tag=='L') result.insert(cell);
        return result;
    }();
    return cells.count(root)!=0;
}
using Bindings=HomeCatalog::MovableResidents;
struct ResidentIndex
{
    std::map<std::string,Bindings> Placed;
    std::shared_ptr<Bindings> Movable=std::make_shared<Bindings>();
};
inline ResidentIndex Index(const std::set<Row>& rows)
{
    ResidentIndex result;
    for (const auto& [tag,cell,ref,base]:rows)
        if (tag=='A'||tag=='M')
            if (auto key=TradeProperties::DecodeForm(base))
            {
                result.Movable->emplace(ref,*key);
                if (tag=='A') result.Placed[cell].emplace(ref,*key);
            }
    return result;
}
inline const ResidentIndex& ResidentBindings()
{
    static const auto index=[]()
    {
        const auto& rows=Loaded();
        return rows?Index(*rows):ResidentIndex{};
    }();
    return index;
}
inline std::shared_ptr<const Bindings> Movable()
{return ResidentBindings().Movable;}
inline Bindings Residents(const std::string& root)
{
    const auto& placed=ResidentBindings().Placed;
    const auto it=placed.find(root);
    return it==placed.end()?Bindings{}:it->second;
}
inline bool ResidentMatches(const Bindings& pool,const std::string& ref,const std::string& base)
{
    const auto it=pool.find(ref);
    return it!=pool.end()&&it->second.Encode()==base;
}
inline bool Covers(const Bindings& bases,const HomeSnapshot::State& state,
    const HomeResidents::Residents& records,const Bindings* movable=nullptr)
{
    if (state.ResidentKeys.size()!=bases.size()||records.size()<bases.size()||
        records.size()>HomeResidents::MaxResidents) return false;
    for (const auto& [key,base]:bases)
        if (!state.ResidentKeys.count(key)||!records.count(key)) return false;
    for (const auto& [key,record]:records)
    {
        const auto placed=bases.find(key);
        const auto& pool=placed!=bases.end()?bases:(movable?*movable:bases);
        if (!record.Revision||!record.Value.Valid()||record.Value.Reference.Encode()!=key||
            !ResidentMatches(pool,key,record.Value.Base.Encode())||(record.Value.Flags&2u)) return false;
        if (std::any_of(state.Refs.begin(),state.Refs.end(),[&](const auto& row){return row.Key.Encode()==key;}))
            return false;
    }
    return true;
}
}
