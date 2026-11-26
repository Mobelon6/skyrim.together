#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/Inventory.h>
#include <chrono>

struct RemoteComponent
{
    RemoteComponent(uint32_t aId, uint32_t aRefId, uint32_t aOwnershipEpoch) noexcept
        : Id(aId)
        , CachedRefId(aRefId)
        , OwnershipEpoch(aOwnershipEpoch)
    {
    }

    uint32_t Id;
    uint32_t CachedRefId;
    uint32_t ConfirmedCorpseEpoch{};
    std::chrono::steady_clock::time_point NextLifeCheck{};
    uint64_t LastCombatTick{};
    uint8_t CombatMode{};
    bool ObservedDead{};
    bool ObservedBleedout{};
    uint32_t OwnershipEpoch;
};
