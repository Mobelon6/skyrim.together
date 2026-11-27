#include "CampaignJournal.h"
#include "QuestJournalDisk.h"
#include <charconv>
#include <array>
#include <limits>
#include <stdexcept>

namespace CampaignJournal
{
namespace
{
// Noncryptographic chained checksum for corruption/divergence detection, NOT authentication.
uint64_t Tag(std::string_view text)
{
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : text) { hash ^= c; hash *= 1099511628211ull; }
    return hash;
}
bool Token(std::string_view value, size_t limit)
{
    if (value.empty() || value.size() > limit) return false;
    for (unsigned char c : value)
        if (!(c >= 'A' && c <= 'Z') && !(c >= 'a' && c <= 'z') &&
            !(c >= '0' && c <= '9') && c != '-' && c != '_' && c != '.') return false;
    return true;
}
std::string Hex(std::string_view value)
{
    constexpr char digits[]="0123456789abcdef";
    std::string out;
    out.reserve(value.size()*2);
    for (unsigned char c : value) { out+=digits[c>>4]; out+=digits[c&15]; }
    return out;
}
int Digit(char c) { return c>='0'&&c<='9' ? c-'0' : c>='a'&&c<='f' ? c-'a'+10 : -1; }
bool Unhex(std::string_view v, std::string& out)
{
    if (v.size()%2 || v.size()>3600) return false;
    out.clear();
    for (size_t i=0;i<v.size();i+=2)
    {
        int a=Digit(v[i]), b=Digit(v[i+1]);
        if (a<0||b<0) return false;
        out+=static_cast<char>(a*16+b);
    }
    return true;
}
bool Valid(const Operation& op)
{
    return Token(op.Id,128) && Token(op.Kind,48) && op.Payload.size()<=1800;
}
std::string Encode(const Operation& op, Head parent)
{
    auto body = "op-v1\t"+std::to_string(parent.Revision)+"\t"+std::to_string(parent.Tag)+"\t"+
        op.Id+"\t"+op.Kind+"\t"+Hex(op.Payload);
    return body+"\t"+std::to_string(Tag(body));
}
bool Decode(std::string_view row, Head parent, Operation& op)
{
    std::array<std::string_view,7> f{};
    for (size_t i=0;i+1<f.size();++i)
    {
        auto pos=row.find('\t');
        if (pos==row.npos) return false;
        f[i]=row.substr(0,pos); row.remove_prefix(pos+1);
    }
    f.back()=row;
    if (f[0]!="op-v1" || f[1]!=std::to_string(parent.Revision) || f[2]!=std::to_string(parent.Tag)) return false;
    op.Id=f[3];op.Kind=f[4];
    return Unhex(f[5],op.Payload) && Valid(op);
}
}
bool Journal::Open(const std::filesystem::path& root, uint64_t campaign, std::string_view profile, bool create)
{
    *this=Journal{};
    if (!campaign || !Token(profile,128)) return false;
    try
    {
        m_root=root; m_campaign=campaign;
        const auto manifest="campaign-v1\t"+std::to_string(campaign)+"\t"+std::string(profile);
        const auto dir=root/std::to_string(campaign);
        if (create)
        {
            // Never initialize over an existing directory, even one left incomplete by a crash.
            std::filesystem::create_directories(root);
            if (!std::filesystem::create_directory(dir) || !QuestJournalDisk::Store(root,campaign,1,manifest))
                return false;
        }
        uint32_t total{};
        std::string row;
        if (!QuestJournalDisk::Inspect(root,campaign,total) || !total ||
            !QuestJournalDisk::Read(root,campaign,1,row) || row!=manifest) return false;
        m_heads.push_back({0,Tag(manifest)});
        for (uint64_t seq=2;seq<=total;++seq)
        {
            if (!QuestJournalDisk::Read(root,campaign,static_cast<uint32_t>(seq),row) ||
                Accept(static_cast<uint32_t>(seq-1),row,false)!=Result::Committed)
            {
                *this=Journal{};
                return false;
            }
        }
        m_ready=true;
        return true;
    }
    catch (...) { *this=Journal{};return false; }
}
const Operation* Journal::Find(const std::string& id) const
{
    const auto it=m_ids.find(id);
    return m_ready && it!=m_ids.end() ? &m_operations[it->second-1] : nullptr;
}
Head Journal::Current() const { return m_ready ? m_heads.back() : Head{}; }
std::optional<Head> Journal::At(uint32_t revision) const
{
    if (!m_ready || revision>=m_heads.size()) return {};
    return m_heads[revision];
}
std::optional<std::string> Journal::Export(uint32_t revision) const
{
    if (!m_ready || !revision || revision>m_records.size()) return {};
    return m_records[revision-1];
}
Result Journal::Append(Head expected, const Operation& op)
{
    if (!m_ready || !Valid(op)) return Result::Invalid;
    auto it=m_ids.find(op.Id);
    if (it!=m_ids.end()) return m_operations[it->second-1]==op ? Result::Duplicate : Result::Conflict;
    if (expected!=Current() || expected.Revision>=UINT32_MAX-1) return Result::Conflict;
    return Accept(expected.Revision+1,Encode(op,expected),true);
}
Result Journal::Import(uint32_t revision, std::string_view record)
{
    if (!m_ready) return Result::Invalid;
    return Accept(revision,record,true);
}
Result Journal::ImportExpected(Head previous, Head target, std::string_view record)
{
    if (!m_ready || previous != Current() || previous.Revision >= UINT32_MAX-1 ||
        target.Revision != previous.Revision+1 || record.size()>4096 || target.Tag != Tag(record))
        return Result::Conflict;
    return Import(target.Revision,record);
}
Result Journal::Accept(uint32_t revision, std::string_view record, bool persist)
{
    if (!revision || revision==UINT32_MAX || record.size()>4096 || m_heads.empty()) return Result::Invalid;
    if (revision<=m_records.size())
        return m_records[revision-1]==record ? Result::Duplicate : Result::Conflict;
    if (revision!=m_heads.size()) return Result::Conflict;
    try
    {
        const auto parent=m_heads.back();
        Operation op;
        // Re-encoding checks canonical fields AND the checksum, including the parent tag.
        if (!Decode(record,parent,op) || Encode(op,parent)!=record) return Result::Invalid;
        if (m_ids.contains(op.Id)) return Result::Conflict;
        if (persist && !QuestJournalDisk::Store(m_root,m_campaign,revision+1,record))
            return Result::StorageError;
        m_ids.emplace(op.Id,revision);
        m_operations.push_back(std::move(op));
        m_records.emplace_back(record);
        m_heads.push_back({revision,Tag(record)});
        return Result::Committed;
    }
    catch (...) { m_ready=false;return Result::StorageError; } // reopen, never use partially updated RAM
}
}
