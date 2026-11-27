#pragma once
#include <cstdint>
struct TESObjectREFR;
namespace Workbench
{
// Native FURN WorkBenchData, not a catalog of FormIDs or ordinary seats.
bool IsStandard(const TESObjectREFR*) noexcept;
bool OrdinaryRoute(const TESObjectREFR*) noexcept;
bool Owns(const TESObjectREFR*) noexcept;
// Called only for a local, non-replayed ordinary activation.
bool Defer(TESObjectREFR*,uint8_t,bool) noexcept;
}
