#include <Messages/NotifyPlayerDialogue.h>

void NotifyPlayerDialogue::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteString(aWriter, Name);
    Anchor.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, SpeakerId);
    Serialization::WriteString(aWriter, Text);
    Serialization::WriteString(aWriter, NpcLine);
}

void NotifyPlayerDialogue::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    Name = Serialization::ReadString(aReader);
    Anchor.Deserialize(aReader);
    SpeakerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Text = Serialization::ReadString(aReader);
    NpcLine = Serialization::ReadString(aReader);
}
