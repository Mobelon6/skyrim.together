
#include <Messages/NotifyQuestUpdate.h>
#include <TiltedCore/Serialization.hpp>

void NotifyQuestUpdate::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Id.Serialize(aWriter);
    aWriter.WriteBits(Stage, 16);
    aWriter.WriteBits(Status, 8);
    aWriter.WriteBits(ClientQuestType, 8);
    TiltedPhoques::Serialization::WriteVarInt(aWriter, SharedRun);
    TiltedPhoques::Serialization::WriteVarInt(aWriter, Speaker);
    TiltedPhoques::Serialization::WriteVarInt(aWriter, Encounter);
    if (Encounter) Subject.Serialize(aWriter);
    TiltedPhoques::Serialization::WriteString(aWriter, Targets);
    aWriter.WriteBits(CheckInstance,1);
    TiltedPhoques::Serialization::WriteVarInt(aWriter,ExpectedInstance);
}

void NotifyQuestUpdate::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);
    Id.Deserialize(aReader);

    uint64_t tmp;
    aReader.ReadBits(tmp, 16);
    Stage = tmp & 0xFFFF;

    aReader.ReadBits(tmp, 8);
    Status = tmp & 0xFF;

    aReader.ReadBits(tmp, 8);
    ClientQuestType = tmp & 0xFF;
    SharedRun = static_cast<uint32_t>(TiltedPhoques::Serialization::ReadVarInt(aReader));
    Speaker = static_cast<uint32_t>(TiltedPhoques::Serialization::ReadVarInt(aReader));
    Encounter = static_cast<uint32_t>(TiltedPhoques::Serialization::ReadVarInt(aReader));
    if (Encounter) Subject.Deserialize(aReader);
    Targets = TiltedPhoques::Serialization::ReadString(aReader);
    aReader.ReadBits(tmp,1);
    CheckInstance=tmp!=0;
    ExpectedInstance=static_cast<uint32_t>(TiltedPhoques::Serialization::ReadVarInt(aReader));
}
