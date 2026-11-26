#include "QuestJournalDisk.h"
#include <atomic>
#include <cstdio>
#include <fstream>
#include <random>
#include <iterator>
#include <charconv>
#include <limits>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif

namespace QuestJournalDisk
{
namespace fs = std::filesystem;
namespace
{
fs::path Directory(const fs::path& root, uint64_t stream)
{
    return root / std::to_string(stream);
}

fs::path Record(const fs::path& directory, uint32_t sequence)
{
    return directory / (std::to_string(sequence) + ".tsv");
}

bool Same(const fs::path& path, const std::string& data)
{
    std::ifstream file(path, std::ios::binary);
    if (!file || fs::file_size(path) != data.size())
        return false;
    return data == std::string(std::istreambuf_iterator<char>(file), {});
}
}

uint64_t CreateStream(const fs::path& root)
{
    fs::create_directories(root);
    std::random_device random;
    for (int attempt = 0; attempt < 16; ++attempt)
    {
        uint64_t id = (uint64_t(random()) << 32) | random();
        if (id && fs::create_directory(Directory(root, id)))
            return id;
    }
    return 0;
}

bool Read(const fs::path& root, uint64_t stream, uint32_t sequence, std::string& row)
{
    row.clear();
    if (!stream || !sequence)
        return false;
    const auto path = Record(Directory(root, stream), sequence);
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec || fs::file_size(path, ec) > 4110 || ec)
        return false;
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::string data;
    char c;
    while (file.get(c))
    {
        if (data.size() == 4110) return false;
        data.push_back(c);
    }
    const auto prefix = std::to_string(sequence) + "\t";
    if (file.bad() || !data.starts_with(prefix) || data.size() <= prefix.size() + 1 || data.back() != '\n')
        return false;
    auto value = data.substr(prefix.size(), data.size() - prefix.size() - 1);
    if (value.size() > 4096 || value.find_first_of("\r\n") != std::string::npos)
        return false;
    row = std::move(value);
    return true;
}

bool Inspect(const fs::path& root, uint64_t stream, uint32_t& total)
{
    total = 0;
    if (!stream) return false;
    const auto dir = Directory(root, stream);
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || ec) return false;
    uint32_t count{}, largest{};
    for (const auto& entry : fs::directory_iterator(dir))
    {
        if (entry.path().extension() != ".tsv") continue;
        const auto stem = entry.path().stem().string();
        uint32_t seq{};
        const auto parsed = std::from_chars(stem.data(), stem.data()+stem.size(), seq);
        std::string row;
        if (parsed.ec != std::errc{} || parsed.ptr != stem.data()+stem.size() || !seq ||
            stem != std::to_string(seq) || count == UINT32_MAX || !Read(root, stream, seq, row))
            return false;
        ++count;
        if (seq > largest) largest = seq;
    }
    if (count != largest) return false;
    total = count;
    return true;
}

uint32_t Contiguous(const fs::path& root, uint64_t stream)
{
    uint32_t last{};
    std::string row;
    while (last < UINT32_MAX && Read(root, stream, last + 1, row))
        ++last;
    return last;
}

bool Store(const fs::path& root, uint64_t stream, uint32_t sequence, std::string_view row)
{
    if (!stream || !sequence || row.empty() || row.size() > 4096 || row.find_first_of("\r\n") != std::string_view::npos)
        return false;
    const auto dir = Directory(root, stream);
    fs::create_directories(dir);
    const auto dest = Record(dir, sequence);
    const auto data = std::to_string(sequence) + "\t" + std::string(row) + "\n";
    if (fs::exists(dest))
        return Same(dest, data);
#ifdef _WIN32
    auto pid = GetCurrentProcessId();
#else
    auto pid = getpid();
#endif
    static std::atomic_uint64_t counter{0};
    fs::path temp = dest;
    temp += "." + std::to_string(pid) + "." + std::to_string(++counter) + ".tmp";
#ifdef _WIN32
    FILE* file = _wfopen(temp.c_str(), L"wb");
#else
    FILE* file = std::fopen(temp.c_str(), "wb");
#endif
    if (!file)
        return false;
    bool ok = std::fwrite(data.data(), 1, data.size(), file) == data.size() && !std::fflush(file);
#ifdef _WIN32
    ok = ok && !_commit(_fileno(file));
#else
    ok = ok && !fsync(fileno(file));
#endif
    if (std::fclose(file) != 0)
        ok = false;
    if (ok)
    {
#ifdef _WIN32
        ok = MoveFileExW(temp.c_str(), dest.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
#else
        ok = link(temp.c_str(), dest.c_str()) == 0;
        if (ok)
        {
            const int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY);
            ok = fd >= 0 && fsync(fd) == 0;
            if (fd >= 0)
                close(fd);
        }
#endif
        if (!ok && fs::exists(dest))
            ok = Same(dest, data);
    }
    std::error_code ignored;
    fs::remove(temp, ignored);
    return ok;
}
}
