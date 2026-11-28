#pragma once
#include <QuestPhysicalStage.h>
struct World;
struct TESQuest;
struct RequestQuestUpdate;
void ObserveQuestPhysicalEffect(uint32_t form,uint32_t instance,uint16_t stage,
    QuestPhysicalStage::Effect effect,uint64_t epoch) noexcept;
bool QuestPhysicalStagePending(uint32_t form,uint32_t instance,uint16_t stage) noexcept;
void SubmitObservedQuestStage(World&,TESQuest*,const RequestQuestUpdate&) noexcept;
