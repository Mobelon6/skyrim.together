#pragma once
#include <HomeResidents.h>
struct World;
struct TESObjectCELL;
namespace HomeResidentProjection
{
struct Entry
{
    uint32_t Source{},Base{};
    HomeResidents::Record Record;
};
bool Sync(World&,TESObjectCELL&,uint32_t slot,const std::vector<Entry>&);
void Clear() noexcept;
}
