#pragma once

#include "Message.h"
#include "Structs/TimeModel.h"
#include "Structs/GameId.h"

struct RequestRestTime final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestRestTime;
    RequestRestTime() : ClientMessage(Opcode) {}

    TimeModel Arrival;
    uint32_t Generation{};
    GameId Bed{0,0};
    bool StartSleep{}, Slept{};
    void SerializeRaw(Buffer::Writer& writer) const noexcept override
    {
        Arrival.Serialize(writer);
        writer.WriteBits(Generation,32);
        Bed.Serialize(writer);
        writer.WriteBits(StartSleep,1);
        writer.WriteBits(Slept,1);
    }
    void DeserializeRaw(Buffer::Reader& reader) noexcept override
    {
        ClientMessage::DeserializeRaw(reader);
        Arrival.Deserialize(reader);
        uint64_t generation{},start{},slept{};
        reader.ReadBits(generation,32);
        Generation=static_cast<uint32_t>(generation);
        Bed.Deserialize(reader);
        reader.ReadBits(start,1);reader.ReadBits(slept,1);
        StartSleep=start!=0;Slept=slept!=0;
    }
};
