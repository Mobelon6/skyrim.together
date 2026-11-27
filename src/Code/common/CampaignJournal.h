#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// One authoritative writer on the server; replicas accept only its ordered records.
// The storage layer does not apply effects to a Skyrim save. The caller must bind
// campaign and character identities, authenticate peers, and reconcile save receipts.
namespace CampaignJournal
{
struct Head
{
    uint32_t Revision{};
    uint64_t Tag{};
    bool operator==(const Head&) const = default;
};
struct Operation
{
    std::string Id;
    std::string Kind;
    std::string Payload;
    bool operator==(const Operation&) const = default;
};
enum class Result { Committed, Duplicate, Conflict, Invalid, StorageError };
class Journal
{
public:
    // Explicit creation only. Opening a missing or corrupted existing history fails.
    bool Open(const std::filesystem::path& root, uint64_t campaign, std::string_view profile, bool create);
    Head Current() const;
    const Operation* Find(const std::string& id) const;
    std::optional<Head> At(uint32_t revision) const;
    Result Append(Head expected, const Operation& op);
    // Caller authenticates source and verifies the campaign/profile handshake first.
    Result Import(uint32_t revision, std::string_view record);
    std::optional<std::string> Export(uint32_t revision) const;
    // Verify the advertised revision and tag before touching the replica's disk.
    Result ImportExpected(Head previous, Head target, std::string_view record);
    const std::vector<Operation>& Operations() const { return m_operations; }
    bool Ready() const { return m_ready; }
private:
    Result Accept(uint32_t revision, std::string_view record, bool persist);
    std::filesystem::path m_root;
    uint64_t m_campaign{};
    std::vector<Operation> m_operations;
    std::vector<std::string> m_records;
    std::vector<Head> m_heads;
    std::map<std::string, uint32_t> m_ids;
    bool m_ready{};
};
}
