#pragma once
#include <cstdint>
struct World;
struct TESObjectCELL;
namespace HomeInstance
{
// Called on the game thread before discovery encodes the current cell.
void Prepare(World&,TESObjectCELL*) noexcept;
bool Encode(uint32_t local,uint32_t& mod,uint32_t& base) noexcept;
bool Tagged(uint32_t mod) noexcept;
bool Foreign(uint32_t mod) noexcept;
uint32_t Decode(uint32_t mod,uint32_t base) noexcept;
bool PrivateCell(uint32_t local) noexcept;
bool ProjectionForm(uint32_t local) noexcept;
uint32_t GuestReference(uint32_t local,uint32_t& source) noexcept;
bool ManagedCell(uint32_t local) noexcept;
bool NativeGuestCell(uint32_t local) noexcept;
bool NativeGuestActive() noexcept;
bool NativeResidentBinding(uint32_t source,uint32_t base) noexcept;
bool NativeGuestResident(uint32_t source,uint32_t base) noexcept;
void ForgetOriginalVisit() noexcept;
void ObserveLock(uint32_t reference) noexcept;
}
