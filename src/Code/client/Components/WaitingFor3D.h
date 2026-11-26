#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Messages/CharacterSpawnRequest.h>
#include <Messages/NotifyEquipmentChanges.h>
#include <Messages/NotifyDeathStateChange.h>

#include <optional>

struct WaitingFor3D
{
    WaitingFor3D(const CharacterSpawnRequest& aSpawnRequest)
        : SpawnRequest(aSpawnRequest)
    {
    }

    std::optional<bool> GetDeathState(const uint32_t aOwnershipEpoch) const noexcept
    {
        if (aOwnershipEpoch == 0)
            return std::nullopt;

        if (PendingDeathState && PendingDeathState->OwnershipEpoch == aOwnershipEpoch)
            return PendingDeathState->IsDead;

        if (SpawnRequest.OwnershipEpoch == aOwnershipEpoch)
            return SpawnRequest.IsDead;

        return std::nullopt;
    }

    CharacterSpawnRequest SpawnRequest{};
    // A life-state notification is newer than the spawn data already queued
    // for this actor. It does not implicitly update the other spawn fields.
    std::optional<NotifyDeathStateChange> PendingDeathState{};
    // Equipment changes relayed before the 3D loaded, replayed after the spawn request's inventory is applied
    Vector<NotifyEquipmentChanges> PendingEquipmentChanges{};
};
