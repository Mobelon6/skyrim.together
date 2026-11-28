#pragma once
#include <array>
#include <cstdint>
#include <cmath>
#include <string_view>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <locale>
#include <set>
#include <sstream>
#include <vector>
#include "TradeProperties.h"
#include "HomeResidenceEvidence.h"

// The winning NPC_ VMAD RentRoomScript.Bed bindings in the archived
// 2026-11-20 active profile (95 plugins). Container points are explicit
// original REFR/base pairs, on the rented bed's floor and nearer than
// competing beds; no runtime nearest-container guess. Zero = no safe point.
// Original NPC inventory is NEVER projected or used as a seed.
namespace HomeInn
{
struct Room
{
    std::string Plugin;
    uint32_t Cell,Bed,Container,Base;
    float X,Y,Z;
    uint32_t KeeperA,KeeperB;
    std::string BedMod{},RefMod{},BaseMod{"skyrim.esm"},AltBedMod{};
    uint32_t AlternateBed{};
    bool Rental{true};
    TradeProperties::FormKey CellKey()const{return {Plugin,Cell};}
    TradeProperties::FormKey BedKey()const{return {BedMod.empty()?Plugin:BedMod,Bed};}
    TradeProperties::FormKey RefKey()const{return {RefMod.empty()?Plugin:RefMod,Container};}
    TradeProperties::FormKey BaseKey()const{return {BaseMod,Base};}
    TradeProperties::FormKey AltKey()const{return AlternateBed?TradeProperties::FormKey{AltBedMod,AlternateBed}:TradeProperties::FormKey{};}
    std::string Binding()const
    {
        return std::string(Rental?"R ":"G ")+CellKey().Encode()+" "+BedKey().Encode()+" "+
            AltKey().Encode()+" "+RefKey().Encode()+" "+BaseKey().Encode();
    }
};
inline const std::array<Room,18> Rooms{{
    {"skyrim.esm",0x13A7C,0x13E41,0xE53AA,0x24CA5,1524.370f,795.490f,0.000f,0x26F0F,0x19A28}, // Dragon Bridge
    {"skyrim.esm",0x13814,0x1759F,0xDDA12,0x3AC21,1691.804f,-711.451f,0.000f,0x1C184,0x1C183}, // Winterhold
    {"skyrim.esm",0x1605E,0x616CA,0xC903E,0xC2A06,-193.325f,170.342f,318.700f,0x1A670,0x13BA3}, // Whiterun
    {"skyrim.esm",0x1677F,0x1686C,0xD26A0,0x24CA6,2976.000f,-1968.000f,0.000f,0x1414E,0x1413E}, // New Gnisis
    {"skyrim.esm",0x16789,0x167CC,0xCB961,0x21366,177.356f,-3981.616f,64.000f,0x1414B,0x1412A}, // Candlehearth Hall
    {"skyrim.esm",0x138BE,0x1748F,0xE5980,0x9AF19,1210.286f,607.028f,0.000f,0x13B77,0x13B76}, // Old Hroldan
    {"skyrim.esm",0x13A5C,0x64622,0x0,0x0,0.000f,0.000f,0.000f,0x136BC,0x136BB}, // Ivarstead: no unambiguous room container
    {"skyrim.esm",0x13870,0x174B0,0x0,0x0,0.000f,0.000f,0.000f,0x136B6,0x0}, // Rorikstead: dresser equally near another bed
    {"skyrim.esm",0x13907,0x172DB,0xE558D,0x24CA5,1715.223f,794.264f,0.000f,0x13663,0x13662}, // Kynesgrove
    {"skyrim.esm",0x3A184,0x9D182,0x9D18F,0x9AF19,26.286f,607.028f,0.000f,0x13655,0x13654}, // Falkreath
    {"skyrim.esm",0x13A5D,0x17061,0x27A9B,0x3AC21,1377.992f,-873.776f,0.000f,0x13627,0x0}, // Nightgate
    {"skyrim.esm",0x13A7F,0x13D42,0xE06D3,0x24CA5,1108.370f,731.490f,0.000f,0x13621,0x1361A}, // Dawnstar
    {"skyrim.esm",0x138CE,0x1738D,0xE0158,0x24CA5,1328.084f,729.745f,0.000f,0x135ED,0x0}, // Morthal
    {"skyrim.esm",0x133C6,0x5EDA7,0x6014C,0x20673,-705.438f,-711.451f,0.000f,0x13479,0x13478}, // Riverwood
    {"skyrim.esm",0x16DFE,0x7939F,0x0,0x0,0.000f,0.000f,0.000f,0x133A3,0x13398}, // Markarth: no unambiguous room container
    {"skyrim.esm",0x16BDF,0x44361,0xB5677,0x20673,581.654f,51.159f,320.000f,0x13373,0x13365}, // Riften
    {"skyrim.esm",0x16A0E,0x608A6,0xD4954,0x6B30E,773.306f,-1741.009f,287.752f,0x132A7,0x13266}, // Solitude
    {"dragonborn.esm",0x17EC0,0x25032,0x35915,0x9AF19,-2440.885f,-505.389f,-382.371f,0x18290,0x0}, // Raven Rock
}};
// Generated profile evidence contains candidates, never a permission grant.
// Explicit legacy rental points remain as compatibility exceptions for the
// archived default profile. A supplied complete profile replaces that default.
struct Index
{
    std::vector<Room> Points;
    std::set<std::string> Keepers;
    bool Legacy{};
};
inline std::optional<Index> Read(std::istream& input)
{
    Index out;
    std::string line;
    if(!std::getline(input,line)||line!="STM-RESIDENCE-POINTS 1")return {};
    std::set<std::string> refs;
    while(std::getline(input,line))
    {
        if(line.empty()||line.size()>2048||out.Points.size()+out.Keepers.size()>=100000)return {};
        std::istringstream row(line);row.imbue(std::locale::classic());
        char tag{};std::string cell,bed,alt,ref,base,extra;
        if(!(row>>tag>>cell))return {};
        const auto ck=TradeProperties::DecodeForm(cell);
        if(!ck||!ck->Valid()||ck->Encode()!=cell)return {};
        if(tag=='K')
        {
            if(row>>extra||!out.Keepers.insert(cell).second)return {};
            continue;
        }
        Room r{};
        if((tag!='R'&&tag!='G')||!(row>>bed>>alt>>ref>>base>>r.X>>r.Y>>r.Z)||
            row>>extra||!std::isfinite(r.X)||!std::isfinite(r.Y)||!std::isfinite(r.Z))return {};
        const auto bk=TradeProperties::DecodeForm(bed),ak=TradeProperties::DecodeForm(alt),
            rk=TradeProperties::DecodeForm(ref),basek=TradeProperties::DecodeForm(base);
        if(!bk||!bk->Valid()||bk->Encode()!=bed||!ak||ak->Encode()!=alt||
            !rk||!rk->Valid()||rk->Encode()!=ref||!basek||!basek->Valid()||basek->Encode()!=base||
            bed==ref||bed==alt||alt==ref||!refs.insert(ref).second)return {};
        r.Plugin=ck->Plugin;r.Cell=ck->Local;r.BedMod=bk->Plugin;r.Bed=bk->Local;
        r.AltBedMod=ak->Plugin;r.AlternateBed=ak->Local;r.RefMod=rk->Plugin;r.Container=rk->Local;
        r.BaseMod=basek->Plugin;r.Base=basek->Local;r.Rental=tag=='R';
        out.Points.push_back(std::move(r));
    }
    if(!input.eof())return {};
    return out;
}
inline const std::optional<Index>& Profile()
{
    static const auto profile=[]()->std::optional<Index>
    {
        if(const auto* root=std::getenv("STM_CAMPAIGN_ROOT");root&&*root)
        {
            const auto path=std::filesystem::path(root)/"CoopResidenceIndex.txt";
            std::error_code ec;
            const bool exists=std::filesystem::exists(path,ec);
            if(ec)return {};
            if(exists){std::ifstream input(path);return Read(input);}
        }
        std::istringstream input{ResidenceEvidence};
        auto index=Read(input);
        if(index)index->Legacy=true;
        return index;
    }();
    return profile;
}
inline const Room* Find(std::string_view plugin,uint32_t cell) noexcept
{
    try
    {
        const auto& index=Profile();if(!index)return nullptr;
        if(index->Legacy)for(const auto& r:Rooms)if(r.Plugin==plugin&&r.Cell==cell)return &r;
        for(const auto& r:index->Points)if(r.Plugin==plugin&&r.Cell==cell)return &r;
    }
    catch (...) {}
    return nullptr;
}
inline const Room* Storage(std::string_view plugin,uint32_t cell,std::string_view refPlugin,uint32_t ref) noexcept
{
    try
    {
        if(!ref)return nullptr;
        const auto& index=Profile();if(!index)return nullptr;
        const auto matches=[&](const Room& r)
        {
            return r.Plugin==plugin&&r.Cell==cell&&r.Container==ref&&r.RefKey().Plugin==refPlugin;
        };
        if(index->Legacy)for(const auto& r:Rooms)if(matches(r))return &r;
        for(const auto& r:index->Points)
        {
            if(!matches(r))continue;
            // Do not create a second rental point for a previously selected bed.
            if(index->Legacy&&r.Rental)
                for(const auto& old:Rooms)
                    if(old.Container&&old.CellKey().Encode()==r.CellKey().Encode()&&
                        (old.BedKey().Encode()==r.BedKey().Encode()||old.BedKey().Encode()==r.AltKey().Encode()))
                        return nullptr;
            return &r;
        }
    }
    catch (...) {}
    return nullptr;
}
inline bool Keeper(std::string_view plugin,uint32_t base) noexcept
{
    try
    {
        if(!base)return false;
        const auto& index=Profile();if(!index)return false;
        if(index->Keepers.count(TradeProperties::FormKey{std::string(plugin),base}.Encode()))return true;
        if(index->Legacy)for(const auto& r:Rooms)
            if(r.Plugin==plugin&&(r.KeeperA==base||r.KeeperB==base))return true;
    }
    catch (...) {}
    return false;
}
inline bool Near(const Room& r,float x,float y,float z,float distance=420.f) noexcept
{
    if(!r.Container||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z))return false;
    const auto dx=x-r.X,dy=y-r.Y,dz=z-r.Z;
    return dx*dx+dy*dy+dz*dz<=distance*distance;
}
}
