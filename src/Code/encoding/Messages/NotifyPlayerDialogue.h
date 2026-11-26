#pragma once

#include "Message.h"
#include <Structs/DialogueAnchor.h>

struct NotifyPlayerDialogue final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyPlayerDialogue;

    NotifyPlayerDialogue()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyPlayerDialogue& achRhs) const noexcept { return GetOpcode() == achRhs.GetOpcode() && Name == achRhs.Name && SpeakerId == achRhs.SpeakerId && Anchor == achRhs.Anchor && Text == achRhs.Text && NpcLine == achRhs.NpcLine; }

    TiltedPhoques::String Name{};
    DialogueAnchor Anchor{};
    uint32_t SpeakerId{};
    TiltedPhoques::String Text{}, NpcLine{};
};
