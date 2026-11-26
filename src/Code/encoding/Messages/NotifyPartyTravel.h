#pragma once

#include "Message.h"
#include <Structs/GameId.h>
#include <Structs/Vector3_NetQuantize.h>

enum TravelNotice : uint32_t { InvitePassenger, CancelPassenger, TransportPassenger, InviteMarkerPassenger };

struct NotifyPartyTravel final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyPartyTravel;
    NotifyPartyTravel() : ServerMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& writer) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& reader) noexcept override;
    bool operator==(const NotifyPartyTravel& other) const noexcept { return Rider == other.Rider && Trip == other.Trip && Kind == other.Kind && Cell == other.Cell && World == other.World && Position == other.Position && Name == other.Name; }

    uint32_t Rider{};
    uint32_t Trip{};
    uint32_t Kind{};
    GameId Cell{}, World{};
    Vector3_NetQuantize Position{};
    String Name{};
};
