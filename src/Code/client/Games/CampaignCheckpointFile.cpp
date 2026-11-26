#include <TiltedOnlinePCH.h>
#include <Games/CampaignCheckpointFile.h>
#include <Games/TES.h>
#include <cryptopp/sha.h>
#include <shlobj.h>
#include <array>
#include <fstream>
#include <cstring>

namespace CampaignCheckpointFile
{
std::filesystem::path Path(const uint64_t* data) noexcept
{
    try
    {
        // SKSE 2.2.8 Hooks_SaveLoad.cpp: the resolved native save name.
        if (!data) return {};
        const auto* name=reinterpret_cast<const char*>(data[0xBB0/8]);
        if (!name||!*name) return {};
        std::filesystem::path file(name);
        if (file.has_root_path()||file.has_parent_path()||file.filename()!=file||file=="."||file=="..") return {};
        if (_stricmp(file.extension().string().c_str(),".ess")!=0) file+=".ess";
        char documents[MAX_PATH]{};
        if (FAILED(SHGetFolderPathA(nullptr,CSIDL_MYDOCUMENTS,nullptr,SHGFP_TYPE_CURRENT,documents))) return {};
        std::filesystem::path local="Saves";
        if (auto* settings=INISettingCollection::Get())
            if (auto* value=settings->GetSetting("sLocalSavePath:General");value&&value->data)
                local=reinterpret_cast<const char*>(value->data);
        if (local.empty()||local.has_root_path()) return {};
        return std::filesystem::path(documents)/"My Games"/"Skyrim Special Edition"/local/file;
    }
    catch (...) {return {};}
}
std::optional<std::string> Digest(const std::filesystem::path& path) noexcept
{
    try
    {
        if (path.empty()||!std::filesystem::is_regular_file(path)) return {};
        const auto size=std::filesystem::file_size(path);
        const auto stamp=std::filesystem::last_write_time(path);
        if (size<13) return {};
        std::ifstream in(path,std::ios::binary);
        std::array<char,65536> data{};
        CryptoPP::SHA256 hash;
        uintmax_t read{};
        while (in)
        {
            in.read(data.data(),data.size());
            const auto n=in.gcount();
            if (n<=0) break;
            if (!read&&(n<13||std::memcmp(data.data(),"TESV_SAVEGAME",13)!=0)) return {};
            hash.Update(reinterpret_cast<const CryptoPP::byte*>(data.data()),static_cast<size_t>(n));
            read+=static_cast<uintmax_t>(n);
        }
        if (in.bad()||!in.eof()||read!=size||size!=std::filesystem::file_size(path)||
            stamp!=std::filesystem::last_write_time(path)) return {};
        std::array<CryptoPP::byte,CryptoPP::SHA256::DIGESTSIZE> digest{};
        hash.Final(digest.data());
        std::string out;constexpr char hex[]="0123456789abcdef";
        for (auto c:digest) {out+=hex[c>>4];out+=hex[c&15];}
        return out;
    }
    catch (...) {return {};}
}
}
