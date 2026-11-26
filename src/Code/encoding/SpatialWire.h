#pragma once
#include "Opcodes.h"

namespace SpatialWire
{
// Keep authentication, journal, trade and other durable operations outside
// the location fence. Only unreliable movement and its reliable location
// fence change their wire representation in this revision.
inline constexpr bool Location(ClientOpcode op) noexcept
{
    return op==kEnterExteriorCellRequest||op==kEnterInteriorCellRequest||op==kShiftGridCellRequest;
}
inline constexpr bool Stamped(ClientOpcode op) noexcept
{return Location(op)||op==kClientReferencesMoveRequest;}
inline constexpr bool Stamped(ServerOpcode op) noexcept
{return op==kServerReferencesMoveRequest;}
}
