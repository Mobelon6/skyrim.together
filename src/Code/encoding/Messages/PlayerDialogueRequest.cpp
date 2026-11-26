#include <Messages/PlayerDialogueRequest.h>

void PlayerDialogueRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Anchor.Serialize(aWriter);
    Serialization::WriteString(aWriter, Text);
    Serialization::WriteString(aWriter, NpcLine);
}

void PlayerDialogueRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    Anchor.Deserialize(aReader);
    Text = Serialization::ReadString(aReader);
    NpcLine = Serialization::ReadString(aReader);
}
