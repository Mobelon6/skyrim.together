#pragma once

#include "Message.h"
#include <Structs/GameId.h>

struct RequestQuestUpdate final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestQuestUpdate;

    RequestQuestUpdate()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestQuestUpdate& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Id == acRhs.Id && Stage == acRhs.Stage && Status == acRhs.Status && ClientQuestType == acRhs.ClientQuestType && JournalObjective == acRhs.JournalObjective && CompletedRunOnce == acRhs.CompletedRunOnce && Targets == acRhs.Targets && BindInstance == acRhs.BindInstance && Instance == acRhs.Instance && ExpectedRun == acRhs.ExpectedRun && PhysicalStage == acRhs.PhysicalStage; }

    enum StatusCode : uint8_t
    {
        StageUpdate,
        Started,
        Stopped
    };

    bool JournalObjective{};
    bool CompletedRunOnce{};
    bool PhysicalStage{}; // actual local fragment observed a geometry effect
    String Targets{};
    bool BindInstance{};
    uint32_t Instance{};
    uint32_t ExpectedRun{}; // nonzero: join/progress only in this already accepted generation
    GameId Id;
    uint16_t Stage;
    uint8_t Status;
    uint8_t ClientQuestType;
};
