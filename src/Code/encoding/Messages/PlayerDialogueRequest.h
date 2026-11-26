#pragma once

#include "Message.h"
#include <Structs/DialogueAnchor.h>

using TiltedPhoques::String;

struct PlayerDialogueRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kPlayerDialogueRequest;

    PlayerDialogueRequest()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const PlayerDialogueRequest& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Anchor == acRhs.Anchor && Text == acRhs.Text && NpcLine == acRhs.NpcLine; }

    DialogueAnchor Anchor{};
    TiltedPhoques::String Text{}, NpcLine{};
};
