#pragma once
#include "Message.h"
struct RequestCampaignPull final : ClientMessage
{
    static constexpr ClientOpcode Opcode=kRequestCampaignPull;
    RequestCampaignPull() : ClientMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer&) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader&) noexcept override;
    uint64_t Campaign{}, Nonce{}, Tag{};
    uint32_t Revision{};
    String Profile{};
    uint64_t Character{}, SaveTag{};
    uint32_t SaveRevision{};
    String Credential{}, Checkpoint{}, LearnedWord{}, AbsorbedDragon{}, PickupClaim{}, QuestReceipt{};
    uint32_t DragonEntity{};
    uint32_t WordPeer{}, WordCellMod{}, WordCellBase{};
    bool operator==(const RequestCampaignPull& o) const noexcept
    { return Campaign==o.Campaign && Nonce==o.Nonce && Tag==o.Tag && Revision==o.Revision && Profile==o.Profile && Character==o.Character && SaveTag==o.SaveTag && SaveRevision==o.SaveRevision && Credential==o.Credential && Checkpoint==o.Checkpoint && LearnedWord==o.LearnedWord && WordPeer==o.WordPeer && WordCellMod==o.WordCellMod && WordCellBase==o.WordCellBase && AbsorbedDragon==o.AbsorbedDragon && DragonEntity==o.DragonEntity && PickupClaim==o.PickupClaim && QuestReceipt==o.QuestReceipt; }
};
