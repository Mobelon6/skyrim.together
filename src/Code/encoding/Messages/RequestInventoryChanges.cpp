#include <Messages/RequestInventoryChanges.h>
#include <TiltedCore/Serialization.hpp>

void RequestInventoryChanges::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ServerId);
    Serialization::WriteVarInt(aWriter, OwnershipEpoch);
    Item.Serialize(aWriter);
    Serialization::WriteBool(aWriter, Drop);
    Serialization::WriteBool(aWriter, UpdateClients);
    QuestId.Serialize(aWriter);
    QuestRef.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, QuestAlias);
    Serialization::WriteBool(aWriter, QuestInitialEvidence);
}

void RequestInventoryChanges::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    ServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    OwnershipEpoch = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Item.Deserialize(aReader);
    Drop = Serialization::ReadBool(aReader);
    UpdateClients = Serialization::ReadBool(aReader);
    QuestId.Deserialize(aReader);
    QuestRef.Deserialize(aReader);
    QuestAlias = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    QuestInitialEvidence = Serialization::ReadBool(aReader);
}
