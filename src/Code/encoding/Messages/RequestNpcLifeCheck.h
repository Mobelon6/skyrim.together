#pragma once

#include "Message.h"

struct RequestNpcLifeCheck final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestNpcLifeCheck;

    RequestNpcLifeCheck()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestNpcLifeCheck& acRhs) const noexcept { return Id == acRhs.Id && OwnershipEpoch == acRhs.OwnershipEpoch && GetOpcode() == acRhs.GetOpcode(); }

    uint32_t Id;
    uint32_t OwnershipEpoch{};
};
