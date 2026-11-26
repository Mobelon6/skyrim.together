#pragma once
#include "CampaignJournal.h"
#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace CampaignIdentity
{
// Values are stored in nonconstant Skyrim GLOB records (exact 22-bit integer chunks).
// A checkpoint represents APPLIED game effects, never merely a downloaded journal.
inline constexpr unsigned FieldCount=12;
using Fields=std::array<float,FieldCount>;
struct Stamp
{
    uint64_t Campaign{}, Character{};
    CampaignJournal::Head Applied{};
    bool operator==(const Stamp&) const=default;
};
enum class SaveState { Empty, Valid, Invalid };
SaveState Decode(const Fields&, Stamp&);
std::optional<Fields> Encode(const Stamp&);
bool Known(const CampaignJournal::Journal&, uint64_t campaign, const Stamp&);

struct Credential
{
    uint64_t Character{};
    std::string Secret;
};
std::optional<Credential> ReadCredential(const std::filesystem::path& root);
class Admission
{
public:
    bool Load(const std::filesystem::path& root);
    bool Claim(uint32_t connection, const Credential&);
    void Release(uint32_t connection);
    const std::map<uint32_t,uint64_t>& Active() const {return m_active;}
private:
    std::map<uint64_t,std::string> m_credentials;
    std::map<uint32_t,uint64_t> m_active;
};
}
