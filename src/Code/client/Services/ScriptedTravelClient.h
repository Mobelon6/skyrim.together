#pragma once
#include <cstdint>
#include <string>
struct TESObjectREFR;
struct ScriptedMoveObservation
{
    uint32_t Quest{},Target{},Instance{};
    uint16_t Stage{};
    uint64_t Epoch{};
    std::string Script;
    float Start{},End{};
    bool Sleep{},WasAtTarget{};
};
uint64_t ScriptedTravelEpoch() noexcept;
bool AtScriptedTravelTarget(TESObjectREFR*) noexcept;
void ObserveScriptedTravel(ScriptedMoveObservation) noexcept;
bool DispatchScriptedSleep(uint32_t quest,const std::string& script,float start,float end) noexcept;
bool DispatchPersonalSleepBonus() noexcept;
