#pragma once
#include <array>
#include <cstdint>
#include <cmath>
#include <string_view>

// The winning NPC_ VMAD RentRoomScript.Bed bindings in the archived
// 2026-11-20 active profile (95 plugins). Container points are explicit
// original REFR/base pairs, on the rented bed's floor and nearer than
// competing beds; no runtime nearest-container guess. Zero = no safe point.
// Original NPC inventory is NEVER projected or used as a seed.
namespace HomeInn
{
struct Room
{
    std::string_view Plugin;
    uint32_t Cell,Bed,Container,Base;
    float X,Y,Z;
    uint32_t KeeperA,KeeperB;
};
inline constexpr std::array<Room,18> Rooms{{
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
inline const Room* Find(std::string_view plugin,uint32_t cell) noexcept
{for(const auto& r:Rooms)if(r.Plugin==plugin&&r.Cell==cell)return &r;return nullptr;}
inline const Room* Storage(std::string_view plugin,uint32_t cell,std::string_view refPlugin,uint32_t ref) noexcept
{
    const auto* r=Find(plugin,cell);
    return r&&r->Container&&r->Plugin==refPlugin&&r->Container==ref?r:nullptr;
}
inline bool Keeper(std::string_view plugin,uint32_t base) noexcept
{
    if(!base)return false;
    for(const auto& r:Rooms)if(r.Plugin==plugin&&(r.KeeperA==base||r.KeeperB==base))return true;
    return false;
}
inline bool Near(const Room& r,float x,float y,float z,float distance=420.f) noexcept
{
    if(!r.Container||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z))return false;
    const auto dx=x-r.X,dy=y-r.Y,dz=z-r.Z;
    return dx*dx+dy*dy+dz*dz<=distance*distance;
}
}
