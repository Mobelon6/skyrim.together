#pragma once

#include "Message.h"
#include "Structs/TimeModel.h"
#include "Structs/GameId.h"

struct ServerTimeSettings final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kServerTimeSettings;

    ServerTimeSettings()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const ServerTimeSettings& achRhs) const noexcept
    {
        return GetOpcode() == achRhs.GetOpcode() && timeModel == achRhs.timeModel && RestCompleted == achRhs.RestCompleted &&
            Generation==achRhs.Generation && SleepPhase==achRhs.SleepPhase && Source==achRhs.Source && Bed==achRhs.Bed;
    }

    bool operator!=(const ServerTimeSettings& achRhs) const noexcept
    {
        return !this->operator==(achRhs);
    }

    TimeModel timeModel;
    bool RestCompleted{};
    uint32_t Generation{},Source{};
    // 0 = clock update; 1 = snapshot local eligibility; 2 = grant personal sleep effects.
    uint8_t SleepPhase{};
    GameId Bed{0,0};
};
