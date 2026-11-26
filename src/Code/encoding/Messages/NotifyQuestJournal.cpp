#include <Messages/NotifyQuestJournal.h>

void NotifyQuestJournal::SerializeRaw(TiltedPhoques::Buffer::Writer& w) const noexcept
{
    Serialization::WriteVarInt(w, Stream);
    Serialization::WriteVarInt(w, Sequence);
    Serialization::WriteVarInt(w, Total);
    Serialization::WriteString(w, Row);
}

void NotifyQuestJournal::DeserializeRaw(TiltedPhoques::Buffer::Reader& r) noexcept
{
    ServerMessage::DeserializeRaw(r);
    Stream = Serialization::ReadVarInt(r);
    Sequence = Serialization::ReadVarInt(r) & 0xFFFFFFFF;
    Total = Serialization::ReadVarInt(r) & 0xFFFFFFFF;
    Row = Serialization::ReadString(r);
}
