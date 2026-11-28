#pragma once
struct Actor;
struct TESObjectREFR;
namespace HomeInn {struct Room;}
namespace InnRentalClient
{
// Only the rental-specific NPC actor value is kept local in campaign mode.
bool LocalVariable(const Actor*) noexcept;
// Native bed ownership in the current personal ESS; no synthetic rent timer.
const HomeInn::Room* RentedRoom(const TESObjectREFR*) noexcept;
}
