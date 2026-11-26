#pragma once

#include "Message.h"
#include <Structs/GameId.h>
#include <Structs/Vector3_NetQuantize.h>

enum TravelAction : uint32_t { BoardCarriage, ArriveByCarriage, AcceptCarriage, DeclineCarriage, CancelCarriage, OpenTravelMap, ArriveByMarker };

struct PartyTravelRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kPartyTravelRequest;
    PartyTravelRequest() : ClientMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& writer) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& reader) noexcept override;
    bool operator==(const PartyTravelRequest& other) const noexcept { return Action == other.Action && Rider == other.Rider && Trip == other.Trip && Cell == other.Cell && World == other.World && Position == other.Position; }

    uint32_t Action{};
    uint32_t Rider{};
    uint32_t Trip{};
    GameId Cell{};
    GameId World{};
    Vector3_NetQuantize Position{};
};
