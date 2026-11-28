#pragma once
#include <cstdint>
#include <optional>
struct World;
namespace CityHomeClient
{
// Verified native HousePurchaseScript/quest readback; no world pointers
// retained by the VM observer. Unknown/partial state must not be replayed.
enum class Local:uint8_t { Unknown, Unowned, Owned, Partial };
Local ReadNative(uint32_t cell) noexcept;
bool DispatchNative(uint32_t cell) noexcept;
// Direct native TIF is preferred. The legacy known-effect fallback is only
// allowed if no VM dispatch was attempted, never after a rejected invocation.
enum class TifDispatch:uint8_t { Unavailable, Dispatched, Rejected };
TifDispatch TryTif(uint32_t operation) noexcept;
uint32_t RuntimeInfo(uint32_t operation) noexcept;
bool NativeReady() noexcept;
// Resolve the exact runtime INFO through its original plugin, never a fixed DLC slot.
uint32_t OperationForInfo(uint32_t info) noexcept;
uint32_t ChargeContext() noexcept;
bool SkipCharge(uint32_t cell) noexcept;
void ObservedDebit(uint32_t cell,uint32_t amount,uint64_t epoch) noexcept;
bool DeferPurchase(uint32_t quest,uint32_t info,uint32_t target) noexcept;
bool Busy() noexcept;
}
