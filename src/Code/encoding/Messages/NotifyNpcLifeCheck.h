#pragma once

#include "Message.h"

struct NotifyNpcLifeCheck final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyNpcLifeCheck;

    NotifyNpcLifeCheck()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyNpcLifeCheck& acRhs) const noexcept { return Id == acRhs.Id && OwnershipEpoch == acRhs.OwnershipEpoch && GetOpcode() == acRhs.GetOpcode(); }

    uint32_t Id;
    uint32_t OwnershipEpoch{};
};
