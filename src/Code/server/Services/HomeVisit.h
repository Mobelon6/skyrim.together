#pragma once
#include <cstdint>
#include <HomeScope.h>
struct Player;
namespace HomeVisit
{
// Owner or currently authorized visitor, never merely a matching FormID.
bool CanEnter(Player&,uint32_t mod,uint32_t base);
// Derive a logical estate from an admitted original interior; no new rights.
HomeScope::EstateContext EstateFor(Player&,uint32_t mod,uint32_t base) noexcept;
}
