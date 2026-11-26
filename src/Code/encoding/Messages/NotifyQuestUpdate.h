#pragma once

#include "Message.h"
#include <Structs/GameId.h>

struct NotifyQuestUpdate final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyQuestUpdate;

    NotifyQuestUpdate()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyQuestUpdate& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Id == acRhs.Id && Stage == acRhs.Stage && Status == acRhs.Status && ClientQuestType == acRhs.ClientQuestType && SharedRun == acRhs.SharedRun && Speaker == acRhs.Speaker && Encounter == acRhs.Encounter && Subject == acRhs.Subject && Targets == acRhs.Targets && CheckInstance == acRhs.CheckInstance && ExpectedInstance == acRhs.ExpectedInstance; }

    enum StatusCode : uint8_t
    {
        StageUpdate,
        Started,
        Stopped
    };

    uint32_t SharedRun{}, Speaker{}, Encounter{};
    GameId Subject{};
    String Targets{};
    uint32_t LocalInstance{}; // client only; never serialized
    bool CheckInstance{};
    uint32_t ExpectedInstance{};
    GameId Id;
    uint16_t Stage;
    uint8_t Status;
    uint8_t ClientQuestType;
};
