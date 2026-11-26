#pragma once
#include <CampaignCheckpoint.h>
struct World;

namespace HomeGuestSave
{
struct Context
{
    CampaignCheckpoint::Record Checkpoint;
    uint32_t ReturnCell{};
    std::array<uint32_t,6> ReturnPose{};
};
// Game-thread capture and exit; no engine objects retained across frames.
std::optional<Context> CaptureAndLeave(World&) noexcept;
bool AtReturn(const Context&) noexcept;
bool Resume(World&,const CampaignCheckpoint::Record&) noexcept;
bool InventoryReady(World&) noexcept;
// Native save coordinator. Pump runs outside the STR world frame.
bool HooksReady() noexcept;
bool Busy() noexcept;
void Pump(World&) noexcept;
void Cancel() noexcept;
}
