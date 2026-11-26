#include <Messages/NotifyCampaignRecord.h>
void NotifyCampaignRecord::SerializeRaw(TiltedPhoques::Buffer::Writer& w) const noexcept
{
    Serialization::WriteVarInt(w,Campaign);Serialization::WriteVarInt(w,Nonce);
    Serialization::WriteVarInt(w,BaseTag);Serialization::WriteVarInt(w,TargetTag);
    Serialization::WriteVarInt(w,BaseRevision);Serialization::WriteVarInt(w,TargetRevision);
    Serialization::WriteVarInt(w,Total);Serialization::WriteVarInt(w,State);
    Serialization::WriteString(w,Row);w.WriteBits(PickupPermit,1);
}
void NotifyCampaignRecord::DeserializeRaw(TiltedPhoques::Buffer::Reader& r) noexcept
{
    ServerMessage::DeserializeRaw(r);
    Campaign=Serialization::ReadVarInt(r);Nonce=Serialization::ReadVarInt(r);
    BaseTag=Serialization::ReadVarInt(r);TargetTag=Serialization::ReadVarInt(r);
    BaseRevision=Serialization::ReadVarInt(r)&0xffffffff;TargetRevision=Serialization::ReadVarInt(r)&0xffffffff;
    Total=Serialization::ReadVarInt(r)&0xffffffff;State=Serialization::ReadVarInt(r)&0xffffffff;
    Row=Serialization::ReadString(r);uint64_t permit{};r.ReadBits(permit,1);PickupPermit=permit!=0;
}
