#pragma once
#include <HomeSnapshot.h>
#include <cstdint>
struct World;
struct TESObjectREFR;
struct TESBoundObject;
struct ExtraDataList;
struct NiPoint3;
namespace HomePlacementClient
{
bool Tracked(const TESObjectREFR*) noexcept;
bool LegacyBlocked(const TESObjectREFR*) noexcept;
std::optional<HomeSnapshot::LegacyStack> CaptureLegacy(TESObjectREFR&);
bool Scoped(const TESObjectREFR*) noexcept;
bool InterceptDrop(const TESObjectREFR*,const TESBoundObject*,ExtraDataList*,int32_t,
                   const NiPoint3*,const NiPoint3*) noexcept;
void Update(World&) noexcept;
void Clear() noexcept;
void Forget() noexcept;
}
