#include <Messages/PartyTravelRequest.h>

void PartyTravelRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& writer) const noexcept
{
    Serialization::WriteVarInt(writer, Action);
    Serialization::WriteVarInt(writer, Rider);
    Serialization::WriteVarInt(writer, Trip);
    Cell.Serialize(writer);
    World.Serialize(writer);
    Position.Serialize(writer);
}

void PartyTravelRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& reader) noexcept
{
    ClientMessage::DeserializeRaw(reader);
    Action = Serialization::ReadVarInt(reader);
    Rider = Serialization::ReadVarInt(reader);
    Trip = Serialization::ReadVarInt(reader);
    Cell.Deserialize(reader);
    World.Deserialize(reader);
    Position.Deserialize(reader);
}
