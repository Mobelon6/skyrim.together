#pragma once
#include <Structs/GameId.h>
#include <TiltedCore/Serialization.hpp>
struct PartyHome
{
    uint32_t House{}, Owner{};
    TiltedPhoques::String Visible;
    uint64_t Campaign{}, Character{}, SaveTag{};
    uint32_t SaveRevision{};
    uint64_t SnapshotDigest{};
    uint32_t SnapshotPart{}, SnapshotParts{};
    // AccessOp 11: ephemeral ordinary workbench lease. Root/Visible are
    // canonical CELL/REFR keys; AccessGuest nonce, AccessRight action/result,
    // AccessRevision echoes the action. No campaign or snapshot fields.
    uint32_t AccessOp{}, AccessRevision{}, AccessRight{};
    uint64_t AccessGuest{};
    TiltedPhoques::String Profile{}, Credential{}, Root{};
    bool operator==(const PartyHome&) const noexcept=default;
    void Serialize(TiltedPhoques::Buffer::Writer& w) const noexcept
    {
        using TiltedPhoques::Serialization;
        Serialization::WriteVarInt(w,House); Serialization::WriteVarInt(w,Owner); Serialization::WriteString(w,Visible);
        Serialization::WriteVarInt(w,Campaign);Serialization::WriteVarInt(w,Character);
        Serialization::WriteVarInt(w,SaveTag);Serialization::WriteVarInt(w,SaveRevision);
        Serialization::WriteString(w,Profile);Serialization::WriteString(w,Credential);Serialization::WriteString(w,Root);
        Serialization::WriteVarInt(w,SnapshotDigest);Serialization::WriteVarInt(w,SnapshotPart);Serialization::WriteVarInt(w,SnapshotParts);
        Serialization::WriteVarInt(w,AccessOp);Serialization::WriteVarInt(w,AccessRevision);
        Serialization::WriteVarInt(w,AccessRight);Serialization::WriteVarInt(w,AccessGuest);
    }
    void Deserialize(TiltedPhoques::Buffer::Reader& r) noexcept
    {
        using TiltedPhoques::Serialization;
        House=static_cast<uint32_t>(Serialization::ReadVarInt(r));
        Owner=static_cast<uint32_t>(Serialization::ReadVarInt(r)); Visible=Serialization::ReadString(r);
        Campaign=Serialization::ReadVarInt(r);Character=Serialization::ReadVarInt(r);
        SaveTag=Serialization::ReadVarInt(r);SaveRevision=static_cast<uint32_t>(Serialization::ReadVarInt(r));
        Profile=Serialization::ReadString(r);Credential=Serialization::ReadString(r);Root=Serialization::ReadString(r);
        SnapshotDigest=Serialization::ReadVarInt(r);SnapshotPart=static_cast<uint32_t>(Serialization::ReadVarInt(r));
        SnapshotParts=static_cast<uint32_t>(Serialization::ReadVarInt(r));
        AccessOp=static_cast<uint32_t>(Serialization::ReadVarInt(r));AccessRevision=static_cast<uint32_t>(Serialization::ReadVarInt(r));
        AccessRight=static_cast<uint32_t>(Serialization::ReadVarInt(r));AccessGuest=Serialization::ReadVarInt(r);
    }
};
