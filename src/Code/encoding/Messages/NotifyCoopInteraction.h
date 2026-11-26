#pragma once
#include "Message.h"
#include <Structs/CoopInteraction.h>
struct NotifyCoopInteraction final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyCoopInteraction;
    NotifyCoopInteraction() : ServerMessage(Opcode) {}
    CoopInteraction Event{};
    void SerializeRaw(TiltedPhoques::Buffer::Writer& w) const noexcept override { Event.Serialize(w); }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& r) noexcept override { ServerMessage::DeserializeRaw(r); Event.Deserialize(r); }
    bool operator==(const NotifyCoopInteraction& other) const noexcept { return Event == other.Event; }
};
