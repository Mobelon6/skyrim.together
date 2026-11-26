#pragma once

struct TESObjectREFR;

/**
 * @brief Dispatched when a local or remote actor's health changes locally.
 *
 * The exact hueristics for what hit is registered by who can be found in
 * the Actor::HookDamageActor() hook.
 */
struct HealthChangeEvent
{
    HealthChangeEvent(uint32_t aHitteeId, float aDeltaHealth, bool aSampleOwnerHealth = false, bool aKeepAlive = false)
        : HitteeId(aHitteeId)
        , DeltaHealth(aDeltaHealth)
        , SampleOwnerHealth(aSampleOwnerHealth)
        , KeepAlive(aKeepAlive)
    {
    }

    uint32_t HitteeId;
    float DeltaHealth;
    bool SampleOwnerHealth;
    bool KeepAlive;
};
