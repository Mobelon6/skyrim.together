#pragma once
#include <HomeSnapshot.h>
struct TESObjectREFR;
namespace HomeInventory
{
std::optional<std::vector<HomeSnapshot::Item>> Capture(TESObjectREFR&);
std::optional<std::vector<HomeSnapshot::Item>> CaptureLoose(TESObjectREFR&);
}
