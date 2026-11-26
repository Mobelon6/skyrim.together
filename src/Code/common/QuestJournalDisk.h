#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace QuestJournalDisk
{
// Each party session gets an immutable stream. A record is published only after flushing its contents.
uint64_t CreateStream(const std::filesystem::path& root);
// Reads and validates the exact sequence prefix and complete bounded row.
bool Read(const std::filesystem::path& root, uint64_t stream, uint32_t sequence, std::string& row);
// Strictly reject gaps, malformed published rows, and noncanonical sequence filenames.
bool Inspect(const std::filesystem::path& root, uint64_t stream, uint32_t& total);
uint32_t Contiguous(const std::filesystem::path& root, uint64_t stream);
bool Store(const std::filesystem::path& root, uint64_t stream, uint32_t sequence, std::string_view row);
}
