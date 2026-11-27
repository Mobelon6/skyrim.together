#pragma once
#include <cstdint>
struct TESObjectREFR;
struct TESBoundObject;
struct ExtraDataList;

// Observe, never suppress, a native direct player AddItem. The outer
// Actor/REFR hook owns the observation when the engine nests them.
struct QuestRewardAddObservation
{
    QuestRewardAddObservation(TESObjectREFR*,TESBoundObject*,ExtraDataList*,int32_t,TESObjectREFR*) noexcept;
    ~QuestRewardAddObservation() noexcept;
    QuestRewardAddObservation(const QuestRewardAddObservation&)=delete;
    QuestRewardAddObservation& operator=(const QuestRewardAddObservation&)=delete;
private:
    TESObjectREFR* Owner{};
    TESBoundObject* Item{};
    uint32_t Quest{},Instance{};
    uint16_t Stage{};
    uint64_t Epoch{},Version{};
    bool Local{};
};
void QuestRewardInventoryTouched(TESObjectREFR*,uint32_t) noexcept;
void QuestRewardStageDone(uint32_t,uint16_t) noexcept;
