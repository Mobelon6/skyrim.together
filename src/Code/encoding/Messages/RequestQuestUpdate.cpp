
#include <Messages/RequestQuestUpdate.h>
#include <TiltedCore/Serialization.hpp>

void RequestQuestUpdate::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Id.Serialize(aWriter);
    aWriter.WriteBits(Stage, 16);
    aWriter.WriteBits(Status, 8);
    aWriter.WriteBits(ClientQuestType, 8);
    aWriter.WriteBits(JournalObjective, 1);
    aWriter.WriteBits(CompletedRunOnce, 1);
    TiltedPhoques::Serialization::WriteString(aWriter, Targets);
    aWriter.WriteBits(BindInstance,1);
    TiltedPhoques::Serialization::WriteVarInt(aWriter,Instance);
    TiltedPhoques::Serialization::WriteVarInt(aWriter,ExpectedRun);
}

void RequestQuestUpdate::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);
    Id.Deserialize(aReader);

    uint64_t tmp;
    aReader.ReadBits(tmp, 16);
    Stage = tmp & 0xFFFF;

    aReader.ReadBits(tmp, 8);
    Status = tmp & 0xFF;

    aReader.ReadBits(tmp, 8);
    ClientQuestType = tmp & 0xFF;
    aReader.ReadBits(tmp, 1);
    JournalObjective = tmp != 0;
    aReader.ReadBits(tmp, 1);
    CompletedRunOnce = tmp != 0;
    Targets = TiltedPhoques::Serialization::ReadString(aReader);
    aReader.ReadBits(tmp,1);
    BindInstance=tmp!=0;
    Instance=static_cast<uint32_t>(TiltedPhoques::Serialization::ReadVarInt(aReader));
    ExpectedRun=static_cast<uint32_t>(TiltedPhoques::Serialization::ReadVarInt(aReader));
}
