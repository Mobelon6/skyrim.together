#pragma once
#include <Services/HomeInstance.h>
#include <Services/NativeTrade.h>
#include <Games/References.h>
#include <Forms/TESObjectCELL.h>

// A guest NPC is a projection, never a second source of possessions.
namespace HomeActorProtection
{
inline bool Resident(const TESObjectREFR* object) noexcept
{
    if (!object || object->formID==0x14) return false;
    auto* actor=Cast<Actor>(const_cast<TESObjectREFR*>(object));
    if (!actor) return false;
    auto* ext=actor->GetExtension();
    const auto* cell=actor->GetParentCellEx();
    return ext && !ext->IsPlayer() && cell &&
        (HomeInstance::ProjectionForm(cell->formID)||HomeInstance::NativeGuestCell(cell->formID));
}
inline bool PickupBlocked(const TESObjectREFR* object) noexcept
{
    return object && (HomeInstance::ProjectionForm(object->formID) || Resident(object) ||
        NativeTrade::BlockAdd(nullptr,object));
}
}
