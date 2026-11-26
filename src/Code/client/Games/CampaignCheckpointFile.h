#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
namespace CampaignCheckpointFile
{
std::filesystem::path Path(const uint64_t*) noexcept;
std::optional<std::string> Digest(const std::filesystem::path&) noexcept;
}
