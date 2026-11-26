#pragma once

#include "Message.h"

struct RequestQuestJournal final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestQuestJournal;
    RequestQuestJournal() : ClientMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& writer) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& reader) noexcept override;
    bool operator==(const RequestQuestJournal& other) const noexcept { return Stream == other.Stream && Sequence == other.Sequence; }
    uint64_t Stream{};
    uint32_t Sequence{};
};
