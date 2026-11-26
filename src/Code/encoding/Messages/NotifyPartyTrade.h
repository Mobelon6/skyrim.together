#pragma once
#include "Message.h"
#include <Structs/PartyTrade.h>
struct NotifyPartyTrade final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyPartyTrade;
    NotifyPartyTrade() : ServerMessage(Opcode) {}
    PartyTrade Event{};
    void SerializeRaw(TiltedPhoques::Buffer::Writer& w) const noexcept override { Event.Serialize(w); }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& r) noexcept override { ServerMessage::DeserializeRaw(r); Event.Deserialize(r); }
    bool operator==(const NotifyPartyTrade& other) const noexcept { return Event == other.Event; }
};
