#include "TESContainer.h"

int64_t TESContainer::GetItemCount(TESForm* apItem) const noexcept
{
    int64_t total=0;
    if (!apItem || !entries) return total;
    for (uint32_t i=0;i<count;++i)
        if (const auto* entry=entries[i]; entry && entry->form==apItem)
            total+=static_cast<int32_t>(entry->count);
    return total;
}
