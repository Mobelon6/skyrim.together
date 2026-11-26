#pragma once

#include <Structs/Inventory.h>
#include <ExtraData/ExtraDataList.h>

/**
 * @brief Dispatched when the contents of an object or actor inventory changes locally.
 *
 * The event has a Drop member variable, since dropped items need to be handled differently.
 */
struct InventoryChangeEvent
{
    InventoryChangeEvent(const uint32_t aFormId, Inventory::Entry arItem)
        : FormId(aFormId)
        , Item(std::move(arItem))
    {
    }

    InventoryChangeEvent(const uint32_t aFormId, Inventory::Entry arItem, bool aDrop)
        : FormId(aFormId)
        , Item(std::move(arItem))
        , Drop(aDrop)
    {
    }

    InventoryChangeEvent(const uint32_t aFormId, Inventory::Entry arItem, bool aDrop, bool aUpdateClients)
        : FormId(aFormId)
        , Item(std::move(arItem))
        , Drop(aDrop)
        , UpdateClients(aUpdateClients)
    {
    }

    uint32_t FormId{};
    Inventory::Entry Item{};
    GameId QuestId{}, QuestRef{};
    uint32_t QuestAlias{};
    bool Drop = false;
    bool UpdateClients = true;
    // The item moved to or from the local player, e.g. looting or pickpocketing an actor owned by someone else
    bool IsPlayerTransfer = false;
};

// Copy a stable quest/alias/reference identity before the native operation.
// An absent or ambiguous alias leaves all identity fields empty.
// allowStopped is only for reconciling an already confirmed consumption.
void CaptureQuestItem(ExtraDataList*,TESObjectREFR*,TESForm*,InventoryChangeEvent&,bool allowStopped=false);
