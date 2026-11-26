#pragma once

#include "Message.h"

struct NotifyQuestJournal final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyQuestJournal;
    NotifyQuestJournal() : ServerMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& writer) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& reader) noexcept override;
    bool operator==(const NotifyQuestJournal& other) const noexcept { return Stream == other.Stream && Sequence == other.Sequence && Total == other.Total && Row == other.Row; }
    uint64_t Stream{};
    uint32_t Sequence{};
    uint32_t Total{};
    String Row{};
};
