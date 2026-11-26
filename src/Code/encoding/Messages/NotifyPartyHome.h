#pragma once
#include "Message.h"
#include <Structs/PartyHome.h>
struct NotifyPartyHome final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyPartyHome;
    NotifyPartyHome() : ServerMessage(Opcode) {}
    PartyHome Event{};
    void SerializeRaw(TiltedPhoques::Buffer::Writer& w) const noexcept override { Event.Serialize(w); }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& r) noexcept override { ServerMessage::DeserializeRaw(r); Event.Deserialize(r); }
    bool operator==(const NotifyPartyHome& other) const noexcept { return Event == other.Event; }
};
