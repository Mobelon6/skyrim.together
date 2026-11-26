#pragma once
#include <cstdint>

struct Actor;

// Only the actor life service may change engine life state. Action history is
// sampled before animation playback and can arrive after a newer world event.
struct ReplicaPose
{
    enum Phase : uint8_t { None, Fighting, Kneeling, Standing };
    static constexpr uint32_t LifeMask = 0xFu << 21;
    static constexpr uint32_t KnockMask = 7u << 25;
    // Skyrim.esm AACT records, shared by all actors using these actions.
    static constexpr uint32_t Death = 0x489ED, DeathWait = 0x5DD59, Ragdoll = 0x9BB4E;
    static constexpr uint32_t KnockDown = 0xD1FDC, GetUp = 0xD1FDD;
    static constexpr uint32_t BleedStart = 0x13EC9, BleedStop = 0x13ECA;

    uint64_t barrier{}, expires{};
    Phase phase{None};

    void Observe(Phase next, uint64_t tick)
    {
        if (next != phase || tick >= expires) barrier = tick;
        phase = next;
        expires = tick + (next == Standing ? 15000 : 4000);
    }
    Phase At(uint64_t now) const { return now < expires ? phase : None; }
    bool Old(uint64_t tick) const { return barrier && tick <= barrier; }
    bool AllowsVariables(uint64_t tick, uint64_t now) const { return !Old(tick) && At(now) != Kneeling; }
    uint32_t MergeState(uint32_t local, uint32_t sample, uint64_t now) const
    {
        const auto native = LifeMask | (At(now) != None ? KnockMask : 0u);
        return (local & native) | (sample & ~native);
    }
    bool AllowsAction(uint32_t action, uint64_t tick, uint64_t now, bool dead) const
    {
        if (action == Death || action == DeathWait) return dead;
        const auto current = At(now);
        if (action == Ragdoll || action == KnockDown) return !Old(tick) && current == None;
        if (action == BleedStart) return !dead && !Old(tick) && current == None;
        if (action == BleedStop || action == GetUp) return !dead && !Old(tick) && current != Kneeling;
        return true;
    }
};

// Native actions started by this actor's own recovery must pass the remote-AI filter.
struct ScopedReplicaTransition
{
    inline static thread_local const Actor* Current{};
    const Actor* previous;
    explicit ScopedReplicaTransition(const Actor* actor) : previous(Current) { Current = actor; }
    ~ScopedReplicaTransition() { Current = previous; }
    ScopedReplicaTransition(const ScopedReplicaTransition&) = delete;
};
