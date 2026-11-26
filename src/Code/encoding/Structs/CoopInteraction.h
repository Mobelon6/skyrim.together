#pragma once
#include <Structs/GameId.h>
#include <TiltedCore/Serialization.hpp>
enum class Interaction : uint32_t
{
    Join, Leave, Joined, WatchOutcomes, FollowerReport, FollowerReserved, FollowerReleased,
    Hire, Granted, Denied, Brawl, OpponentState, Outcome, Rejoin, CancelHire, PrepareHire, HireReady, CommitHire, SyncRange, FavorResult, BrawlFactionEnded
};
struct CoopInteraction
{
    Interaction Kind{};
    uint32_t Speaker{}, Session{}, Value{};
    GameId Npc{};
    void Serialize(TiltedPhoques::Buffer::Writer& w) const noexcept
    {
        using TiltedPhoques::Serialization;
        Serialization::WriteVarInt(w, static_cast<uint32_t>(Kind));
        Serialization::WriteVarInt(w, Speaker);
        Serialization::WriteVarInt(w, Session);
        Serialization::WriteVarInt(w, Value);
        Npc.Serialize(w);
    }
    void Deserialize(TiltedPhoques::Buffer::Reader& r) noexcept
    {
        using TiltedPhoques::Serialization;
        Kind = static_cast<Interaction>(Serialization::ReadVarInt(r));
        Speaker = static_cast<uint32_t>(Serialization::ReadVarInt(r));
        Session = static_cast<uint32_t>(Serialization::ReadVarInt(r));
        Value = static_cast<uint32_t>(Serialization::ReadVarInt(r));
        Npc.Deserialize(r);
    }
    bool operator==(const CoopInteraction&) const noexcept = default;
};
