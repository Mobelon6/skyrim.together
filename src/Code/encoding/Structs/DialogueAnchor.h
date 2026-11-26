#pragma once

#include <Structs/GameId.h>
#include <Structs/Vector3_NetQuantize.h>
#include <TiltedCore/Serialization.hpp>

// Session and choice identify a presentation, never a Papyrus action.
// An empty Cell closes the current session. Heartbeats allow late joiners to see it.
struct DialogueAnchor
{
    bool GameInitiated{};
    uint32_t Session{};
    uint32_t Choice{};
    // Runtime-created references use the shared entity ID. Placed references also carry a plugin-relative ID.
    uint32_t TargetEntity{UINT32_MAX};
    GameId TargetForm{};
    GameId TopicInfo{}; // selected INFO record in the speaker's load order
    GameId TopicQuest{};
    GameId Cell{};
    GameId WorldSpace{};
    Vector3_NetQuantize Position{};

    bool operator==(const DialogueAnchor&) const noexcept = default;

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
    {
        TiltedPhoques::Serialization::WriteVarInt(aWriter, Session);
        TiltedPhoques::Serialization::WriteVarInt(aWriter, Choice);
        TiltedPhoques::Serialization::WriteVarInt(aWriter, TargetEntity);
        TiltedPhoques::Serialization::WriteBool(aWriter, GameInitiated);
        TargetForm.Serialize(aWriter);
        TopicInfo.Serialize(aWriter);
        TopicQuest.Serialize(aWriter);
        Cell.Serialize(aWriter);
        WorldSpace.Serialize(aWriter);
        Position.Serialize(aWriter);
    }

    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
    {
        Session = TiltedPhoques::Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        Choice = TiltedPhoques::Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        TargetEntity = TiltedPhoques::Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        GameInitiated = TiltedPhoques::Serialization::ReadBool(aReader);
        TargetForm.Deserialize(aReader);
        TopicInfo.Deserialize(aReader);
        TopicQuest.Deserialize(aReader);
        Cell.Deserialize(aReader);
        WorldSpace.Deserialize(aReader);
        Position.Deserialize(aReader);
    }
};
