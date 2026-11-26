#pragma once
#include "CampaignJournal.h"
#include <filesystem>
#include <string>

namespace CampaignReplication
{
using CampaignJournal::Head;
enum class Status : uint8_t { Current, Record, Conflict, Unavailable };
struct Pull
{
    uint64_t Campaign{}, Nonce{};
    std::string Profile;
    Head Base{};
};
struct Reply
{
    uint64_t Campaign{}, Nonce{};
    Status State{Status::Unavailable};
    Head Base{}, Target{};
    uint32_t Total{};
    std::string Row;
};

// Server exposes a read-only, single-record pull. It never accepts client writes.
Reply Serve(const CampaignJournal::Journal&, uint64_t campaign, std::string_view profile, const Pull&);
CampaignJournal::Result Receive(CampaignJournal::Journal&, const Pull&, const Reply&);

// Explicit physical path, outside the replaceable mod. Disabled unless configured.
// On the host the authority and local replica use separate directories.
class Link
{
public:
    enum class Mode { Disabled, Failed, Ready };
    Mode Start(bool authority);
    Mode State() const { return m_mode; }
    void Fail() { m_mode=Mode::Failed; }
    uint64_t Campaign{};
    std::string Profile;
    std::filesystem::path Root;
    CampaignJournal::Journal Journal;
private:
    Mode m_mode{Mode::Disabled};
    bool m_started{};
};
}
