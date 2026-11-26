#include <Messages/RequestQuestJournal.h>

void RequestQuestJournal::SerializeRaw(TiltedPhoques::Buffer::Writer& w) const noexcept
{
    Serialization::WriteVarInt(w, Stream);
    Serialization::WriteVarInt(w, Sequence);
}

void RequestQuestJournal::DeserializeRaw(TiltedPhoques::Buffer::Reader& r) noexcept
{
    ClientMessage::DeserializeRaw(r);
    Stream = Serialization::ReadVarInt(r);
    Sequence = Serialization::ReadVarInt(r) & 0xFFFFFFFF;
}
