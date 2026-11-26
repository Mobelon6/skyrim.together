#include <Messages/NotifyPartyTravel.h>

void NotifyPartyTravel::SerializeRaw(TiltedPhoques::Buffer::Writer& writer) const noexcept
{
    Serialization::WriteVarInt(writer, Rider);
    Serialization::WriteVarInt(writer, Trip);
    Serialization::WriteVarInt(writer, Kind);
    if (Kind == TransportPassenger)
    {
        Cell.Serialize(writer);
        World.Serialize(writer);
        Position.Serialize(writer);
    }
    Serialization::WriteString(writer, Name);
}

void NotifyPartyTravel::DeserializeRaw(TiltedPhoques::Buffer::Reader& reader) noexcept
{
    ServerMessage::DeserializeRaw(reader);
    Rider = Serialization::ReadVarInt(reader);
    Trip = Serialization::ReadVarInt(reader);
    Kind = Serialization::ReadVarInt(reader);
    if (Kind == TransportPassenger)
    {
        Cell.Deserialize(reader);
        World.Deserialize(reader);
        Position.Deserialize(reader);
    }
    Name = Serialization::ReadString(reader);
}
