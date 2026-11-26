#pragma once
#include <CampaignCheckpoint.h>
struct World;
namespace CampaignCheckpointClient
{
std::optional<CampaignCheckpoint::Record> Capture(World&) noexcept;
bool Queue(CampaignCheckpoint::Record) noexcept;
// Native load target may execute on a worker. Exchange metadata only.
void ForgetLoaded() noexcept;
void NoteLoadedFile(std::string) noexcept;
void CompleteLoad() noexcept;
// Exact loaded ESS and authenticated replicated record; no teleport/replay.
std::optional<CampaignCheckpoint::Record> Loaded(World&) noexcept;
}
