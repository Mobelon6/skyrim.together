#pragma once

#include <Structs/GameId.h>

#include <Structs/Vector3_NetQuantize.h>
#include <Structs/Rotator2_NetQuantize.h>
#include <Structs/AnimationVariables.h>

using TiltedPhoques::Buffer;

struct Movement
{
    Movement() = default;
    ~Movement() = default;

    bool operator==(const Movement& acRhs) const noexcept;
    bool operator!=(const Movement& acRhs) const noexcept;

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

    // Current native STR ownership generation, including player actors.
    uint32_t OwnershipEpoch{};
    GameId CellId{};
    GameId WorldSpaceId{};
    Vector3_NetQuantize Position{};
    Rotator2_NetQuantize Rotation{};
    AnimationVariables Variables{};
    float Direction{};
    // Epoch 0 means no NPC combat sample. Mode 1 is idle, 2 fights a player, 3 other combat.
    uint32_t CombatEpoch{};
    uint8_t CombatMode{};
    bool HasLookAt{};
    Vector3_NetQuantize LookOffset{}; // From the head to the look-at target
};
