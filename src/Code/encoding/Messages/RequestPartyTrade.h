#pragma once
#include "Message.h"
#include <Structs/PartyTrade.h>
struct RequestPartyTrade final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestPartyTrade;
    RequestPartyTrade() : ClientMessage(Opcode) {}
    PartyTrade Event{};
    void SerializeRaw(TiltedPhoques::Buffer::Writer& w) const noexcept override { Event.Serialize(w); }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& r) noexcept override { ClientMessage::DeserializeRaw(r); Event.Deserialize(r); }
    bool operator==(const RequestPartyTrade& other) const noexcept { return Event == other.Event; }
};
