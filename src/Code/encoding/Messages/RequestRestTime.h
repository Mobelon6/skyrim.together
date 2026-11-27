#pragma once

#include "Message.h"
#include "Structs/TimeModel.h"

struct RequestRestTime final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestRestTime;
    RequestRestTime() : ClientMessage(Opcode) {}

    TimeModel Arrival;
    void SerializeRaw(Buffer::Writer& writer) const noexcept override { Arrival.Serialize(writer); }
    void DeserializeRaw(Buffer::Reader& reader) noexcept override
    {
        ClientMessage::DeserializeRaw(reader);
        Arrival.Deserialize(reader);
    }
};
