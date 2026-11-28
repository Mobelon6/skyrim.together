#pragma once
#include <cstdint>
struct TESObjectREFR;
struct TESBoundObject;
struct ExtraDataList;
namespace NativeTrade
{
// The display chest is never an authoritative inventory.
bool IsProxy(const TESObjectREFR*) noexcept;
// Durable shared stock uses the actual reference and existing trade receipts.
bool IsSharedContainer(const TESObjectREFR*) noexcept;
bool BlockAdd(const TESObjectREFR* to,const TESObjectREFR* from) noexcept;
bool BlockRemove(const TESObjectREFR* from,const TESObjectREFR* to,
                 const TESBoundObject* item,int32_t count,ExtraDataList* extra) noexcept;
}
