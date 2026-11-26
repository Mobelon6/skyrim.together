#pragma once
#include "Message.h"
struct NotifyCampaignRecord final : ServerMessage
{
    static constexpr ServerOpcode Opcode=kNotifyCampaignRecord;
    NotifyCampaignRecord() : ServerMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer&) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader&) noexcept override;
    uint64_t Campaign{}, Nonce{}, BaseTag{}, TargetTag{};
    uint32_t BaseRevision{}, TargetRevision{}, Total{}, State{};
    String Row{};
    bool PickupPermit{};
    bool operator==(const NotifyCampaignRecord& o) const noexcept
    { return Campaign==o.Campaign && Nonce==o.Nonce && BaseTag==o.BaseTag && TargetTag==o.TargetTag &&
        BaseRevision==o.BaseRevision && TargetRevision==o.TargetRevision && Total==o.Total && State==o.State && Row==o.Row && PickupPermit==o.PickupPermit; }
};
