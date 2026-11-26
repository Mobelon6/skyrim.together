#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/GridCellCoords.h>
#include <HomeScope.h>

struct CellIdComponent
{
    CellIdComponent() {}

    CellIdComponent(GameId aCellId)
        : Cell(aCellId)
    {
    }

    CellIdComponent(GameId aCellId, GameId aWorldSpaceId, GridCellCoords aCenterCoords)
        : Cell(aCellId)
        , WorldSpaceId(aWorldSpaceId)
        , CenterCoords(aCenterCoords)
    {
    }

    bool operator==(const CellIdComponent& acRhs) const noexcept { return Cell == acRhs.Cell; }

    bool operator!=(const CellIdComponent& acRhs) const noexcept { return !operator==(acRhs); }

    operator bool() const noexcept { return *this != CellIdComponent{}; }

    bool IsInInteriorCell() const noexcept { return !WorldSpaceId; }

    bool HasExteriorEstate() const noexcept
    {return bool(WorldSpaceId)&&!(Estate==HomeScope::EstateContext{});}

    bool SamePresence(const CellIdComponent& rhs) const noexcept
    {
        // Interior slots already isolate exact rooms. The estate association
        // is informational there, so a newly registered main room does not
        // invalidate an already admitted basement actor.
        if (HomeScope::Private(Cell.ModId)||HomeScope::Private(rhs.Cell.ModId))
            return !WorldSpaceId&&!rhs.WorldSpaceId&&Cell==rhs.Cell;
        return HomeScope::EstateCompatible(Estate,rhs.Estate);
    }

    bool IsInRange(const CellIdComponent& acRhs, bool aIsDragon) const noexcept
    {
        if (!SamePresence(acRhs)) return false;
        if (HomeScope::Private(Cell.ModId)) return true;
        if (IsInInteriorCell())
            return Cell == acRhs.Cell;

        if (WorldSpaceId != acRhs.WorldSpaceId)
            return false;

        return GridCellCoords::IsCellInGridCell(acRhs.CenterCoords, CenterCoords, aIsDragon);
    }

    GameId Cell{};
    GameId WorldSpaceId{};
    GridCellCoords CenterCoords{};
    HomeScope::EstateContext Estate{};
};
