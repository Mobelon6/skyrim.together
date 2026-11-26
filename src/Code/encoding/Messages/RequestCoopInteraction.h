#pragma once
#include "Message.h"
#include <Structs/CoopInteraction.h>
struct RequestCoopInteraction final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestCoopInteraction;
    RequestCoopInteraction() : ClientMessage(Opcode) {}
    CoopInteraction Event{};
    void SerializeRaw(TiltedPhoques::Buffer::Writer& w) const noexcept override { Event.Serialize(w); }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& r) noexcept override { ClientMessage::DeserializeRaw(r); Event.Deserialize(r); }
    bool operator==(const RequestCoopInteraction& other) const noexcept { return Event == other.Event; }
};
