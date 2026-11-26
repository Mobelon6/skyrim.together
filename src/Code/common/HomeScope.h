#pragma once
#include <cstdint>
#include <string>

namespace HomeScope
{
// Estate presence is separate from the canonical physical cell/world/grid.
// Ownership of land, a registered home and the right to visit are separate.
struct EstateContext
{
    uint64_t Campaign{},Owner{};
    std::string Plot;
    bool Valid() const noexcept
    {return (!Campaign&&!Owner&&Plot.empty())||(Campaign&&Owner&&!Plot.empty()&&Plot.size()<=216);}
    bool operator==(const EstateContext&) const noexcept=default;
};
inline bool EstateCompatible(const EstateContext& a,const EstateContext& b) noexcept
{return a.Valid()&&b.Valid()&&a==b;}
// Default zero context is the ordinary/shared world. Private presence is
// assigned only after authoritative admission and overlay readback.
// Canonical plugin/form keys remain unchanged; the cell namespace is an
// additional discriminator for reference entities, not for base forms.
inline constexpr uint32_t Registered=0xFFFD0000u;
inline constexpr uint32_t Pending=0xFFFC0000u;
inline constexpr bool Private(uint32_t mod) noexcept
{
    const auto prefix=mod&0xFFFF0000u;
    return prefix==Registered || prefix==Pending;
}
template<class Id>
bool Compatible(const Id& a,const Id& b) noexcept
{
    return (!Private(a.ModId) && !Private(b.ModId)) || a==b;
}
// Reject foreign and quarantined projections before resolving a local NPC.
template<class Id>
bool SpawnAllowed(const Id& current,const Id& incoming,bool projection) noexcept
{
    if (projection && ((current.ModId&0xFFFF0000u)!=Registered)) return false;
    return Compatible(current,incoming);
}
template<class Id>
bool ReferenceMatches(const Id& reference,const Id& cell,const Id& requested,const Id& requestedCell) noexcept
{
    return reference==requested && Compatible(cell,requestedCell);
}
template<class Id>
bool AdmittedCell(const Id& sender,const Id& cell,const Id& world) noexcept
{
    return Compatible(sender,cell) && (!Private(cell.ModId) || !world);
}
}
