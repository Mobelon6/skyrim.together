#include <Messages/RequestCampaignPull.h>
void RequestCampaignPull::SerializeRaw(TiltedPhoques::Buffer::Writer& w) const noexcept
{
    Serialization::WriteVarInt(w,Campaign); Serialization::WriteVarInt(w,Nonce);
    Serialization::WriteVarInt(w,Tag); Serialization::WriteVarInt(w,Revision);
    Serialization::WriteString(w,Profile);
    Serialization::WriteVarInt(w,Character);Serialization::WriteVarInt(w,SaveTag);
    Serialization::WriteVarInt(w,SaveRevision);Serialization::WriteString(w,Credential);
    Serialization::WriteString(w,Checkpoint);
    Serialization::WriteString(w,LearnedWord);Serialization::WriteVarInt(w,WordPeer);
    Serialization::WriteVarInt(w,WordCellMod);Serialization::WriteVarInt(w,WordCellBase);
    Serialization::WriteString(w,AbsorbedDragon);Serialization::WriteVarInt(w,DragonEntity);
    Serialization::WriteString(w,PickupClaim);
    Serialization::WriteString(w,QuestReceipt);
}
void RequestCampaignPull::DeserializeRaw(TiltedPhoques::Buffer::Reader& r) noexcept
{
    ClientMessage::DeserializeRaw(r);
    Campaign=Serialization::ReadVarInt(r);Nonce=Serialization::ReadVarInt(r);
    Tag=Serialization::ReadVarInt(r);Revision=Serialization::ReadVarInt(r)&0xffffffff;
    Profile=Serialization::ReadString(r);
    Character=Serialization::ReadVarInt(r);SaveTag=Serialization::ReadVarInt(r);
    SaveRevision=Serialization::ReadVarInt(r)&0xffffffff;Credential=Serialization::ReadString(r);
    Checkpoint=Serialization::ReadString(r);
    LearnedWord=Serialization::ReadString(r);WordPeer=Serialization::ReadVarInt(r)&0xffffffff;
    WordCellMod=Serialization::ReadVarInt(r)&0xffffffff;WordCellBase=Serialization::ReadVarInt(r)&0xffffffff;
    AbsorbedDragon=Serialization::ReadString(r);DragonEntity=Serialization::ReadVarInt(r)&0xffffffff;
    PickupClaim=Serialization::ReadString(r);
    QuestReceipt=Serialization::ReadString(r);
}
