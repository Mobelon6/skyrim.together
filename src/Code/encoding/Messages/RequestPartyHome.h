#pragma once
#include "Message.h"
#include <Structs/PartyHome.h>
struct RequestPartyHome final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestPartyHome;
    RequestPartyHome() : ClientMessage(Opcode) {}
    PartyHome Event{};
    void SerializeRaw(TiltedPhoques::Buffer::Writer& w) const noexcept override { Event.Serialize(w); }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& r) noexcept override { ClientMessage::DeserializeRaw(r); Event.Deserialize(r); }
    bool operator==(const RequestPartyHome& other) const noexcept { return Event == other.Event; }
};
