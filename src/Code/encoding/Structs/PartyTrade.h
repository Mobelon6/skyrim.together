#pragma once
#include <Structs/GameId.h>
#include <TiltedCore/Serialization.hpp>

enum class TradeOp : uint8_t { Poll, Invite, Offer, Accept, Open, Close, Deposit, Take, Entry, Reset, Receipt, Error, Ready, RecoveryConflict, ReturnOffer, Return, HomeOpen, HomePlace, HomeTake, HomeMove, HomeStockNeeded, HomeStockPart, HomePersonal };
struct PartyTrade
{
    TradeOp Op{};
    uint32_t Peer{}, Session{}, Cursor{}, Count{};
    uint64_t Stream{};
    GameId Item{};
    TiltedPhoques::String Text{};
    uint64_t Campaign{}, Character{}, SaveTag{};
    uint32_t SaveRevision{}, BeforePlusOne{}; // zero = no checkpoint
    TiltedPhoques::String Profile{}, Credential{}, Variant{};
    bool operator==(const PartyTrade&) const noexcept = default;
    void Serialize(TiltedPhoques::Buffer::Writer& w) const noexcept
    {
        using S = TiltedPhoques::Serialization;
        w.WriteBits(static_cast<uint8_t>(Op),5);
        for (uint32_t x : {Peer,Session,Cursor,Count}) S::WriteVarInt(w,x);
        S::WriteVarInt(w,Stream);
        Item.Serialize(w);
        S::WriteString(w,Text);
        S::WriteVarInt(w,Campaign);S::WriteVarInt(w,Character);S::WriteVarInt(w,SaveTag);S::WriteVarInt(w,SaveRevision);
        S::WriteString(w,Profile);S::WriteString(w,Credential);S::WriteVarInt(w,BeforePlusOne);S::WriteString(w,Variant);
    }
    void Deserialize(TiltedPhoques::Buffer::Reader& r) noexcept
    {
        using S = TiltedPhoques::Serialization;
        uint64_t raw{}; r.ReadBits(raw,5); Op=static_cast<TradeOp>(raw);
        for (auto* x : {&Peer,&Session,&Cursor,&Count}) *x=static_cast<uint32_t>(S::ReadVarInt(r));
        Stream=S::ReadVarInt(r); Item.Deserialize(r); Text=S::ReadString(r);
        Campaign=S::ReadVarInt(r);Character=S::ReadVarInt(r);SaveTag=S::ReadVarInt(r);SaveRevision=static_cast<uint32_t>(S::ReadVarInt(r));
        Profile=S::ReadString(r);Credential=S::ReadString(r);BeforePlusOne=static_cast<uint32_t>(S::ReadVarInt(r));Variant=S::ReadString(r);
    }
};
