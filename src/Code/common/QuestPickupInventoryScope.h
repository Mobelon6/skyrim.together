#pragma once
#include <cstdint>

// A native pickup can enter AddInventoryItem before the outer pickup hook
// observes its final count. Publish that acquisition once, not twice.
namespace QuestPickupInventory
{
inline thread_local uint32_t Owner{}, Item{};
struct Scope
{
    uint32_t PreviousOwner, PreviousItem;
    Scope(uint32_t owner,uint32_t item) noexcept : PreviousOwner(Owner),PreviousItem(Item)
    {Owner=owner;Item=item;}
    ~Scope(){Owner=PreviousOwner;Item=PreviousItem;}
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
};
inline bool IsNestedAdd(uint32_t owner,uint32_t item) noexcept
{return owner&&item&&Owner==owner&&Item==item;}
}
