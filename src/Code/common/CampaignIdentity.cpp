#include "CampaignIdentity.h"
#include <cmath>
#include <charconv>
#include <fstream>
#include <algorithm>
#include <limits>

namespace CampaignIdentity
{
namespace
{
constexpr uint32_t Mask=(1u<<22)-1;
bool Part(float f,uint32_t limit,uint64_t& v)
{
    if (!std::isfinite(f)||f<0.f||f>static_cast<float>(limit)||std::floor(f)!=f) return false;
    v=static_cast<uint64_t>(f);return true;
}
bool Read64(const Fields& f,unsigned offset,uint64_t& out)
{
    uint64_t a{},b{},c{};
    if (!Part(f[offset],Mask,a)||!Part(f[offset+1],Mask,b)||!Part(f[offset+2],(1u<<20)-1,c)) return false;
    out=a|(b<<22)|(c<<44);return true;
}
void Write64(Fields& f,unsigned offset,uint64_t value)
{
    for(unsigned i=0;i<3;++i) f[offset+i]=static_cast<float>((value>>(22*i))&Mask);
}
bool SecretValid(std::string_view value)
{
    return value.size()==64 && std::all_of(value.begin(),value.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
bool ParseId(std::string_view text,uint64_t& value)
{
    const auto r=std::from_chars(text.data(),text.data()+text.size(),value);
    return r.ec==std::errc{} && r.ptr==text.data()+text.size() && value && text==std::to_string(value);
}
bool ReadEntry(std::istream& in,Credential& value)
{
    std::string id;
    return (in>>id>>value.Secret) && ParseId(id,value.Character) && SecretValid(value.Secret);
}
bool EqualSecret(std::string_view a,std::string_view b)
{
    if(a.size()!=64||b.size()!=64) return false;
    unsigned diff{};
    for(unsigned i=0;i<64;++i) diff|=static_cast<unsigned char>(a[i])^static_cast<unsigned char>(b[i]);
    return diff==0;
}
}
SaveState Decode(const Fields& f, Stamp& out)
{
    out={};
    if(std::all_of(f.begin(),f.end(),[](float x){return x==0.f;})) return SaveState::Empty;
    Stamp value;
    uint64_t low{},high{};
    if(f[11]!=1.f||!Read64(f,0,value.Campaign)||!Read64(f,3,value.Character)||
        !Part(f[6],Mask,low)||!Part(f[7],1023,high)||!Read64(f,8,value.Applied.Tag)||
        !value.Campaign||!value.Character||!value.Applied.Tag) return SaveState::Invalid;
    value.Applied.Revision=static_cast<uint32_t>(low|(high<<22));
    if(value.Applied.Revision==UINT32_MAX) return SaveState::Invalid;
    out=value;return SaveState::Valid;
}
std::optional<Fields> Encode(const Stamp& value)
{
    if(!value.Campaign||!value.Character||!value.Applied.Tag||value.Applied.Revision==UINT32_MAX) return {};
    Fields f{};
    Write64(f,0,value.Campaign);Write64(f,3,value.Character);
    f[6]=static_cast<float>(value.Applied.Revision&Mask);f[7]=static_cast<float>(value.Applied.Revision>>22);
    Write64(f,8,value.Applied.Tag);f[11]=1.f;
    return f;
}
bool Known(const CampaignJournal::Journal& journal,uint64_t campaign,const Stamp& stamp)
{
    const auto head=journal.At(stamp.Applied.Revision);
    return campaign && stamp.Campaign==campaign && stamp.Character && head && *head==stamp.Applied;
}
std::optional<Credential> ReadCredential(const std::filesystem::path& root)
{
    try
    {
        const auto path=root/"CharacterLink.txt";
        if(!std::filesystem::is_regular_file(path)||std::filesystem::file_size(path)>512) return {};
        std::ifstream in(path);
        Credential value;
        std::string extra;
        if(!ReadEntry(in,value)||in>>extra||in.bad()) return {};
        return value;
    }
    catch(...) {return {};}
}
bool Admission::Load(const std::filesystem::path& root)
{
    m_credentials.clear();m_active.clear();
    try
    {
        const auto path=root/"Participants.txt";
        if(!std::filesystem::is_regular_file(path)||std::filesystem::file_size(path)>8192) return false;
        std::ifstream in(path);
        std::map<uint64_t,std::string> next;
        for(;;)
        {
            in>>std::ws;
            if(in.eof()) break;
            Credential c;
            if(!ReadEntry(in,c)||next.size()>=32||next.contains(c.Character)) return false;
            for(const auto& entry:next) if(EqualSecret(entry.second,c.Secret)) return false;
            next.emplace(c.Character,std::move(c.Secret));
        }
        if(in.bad()||next.empty()) return false;
        m_credentials=std::move(next);return true;
    }
    catch(...) {return false;}
}
bool Admission::Claim(uint32_t connection,const Credential& c)
{
    if(!connection) return false;
    const auto entry=m_credentials.find(c.Character);
    if(entry==m_credentials.end()||!EqualSecret(entry->second,c.Secret)) return false;
    for(const auto& [who,character]:m_active)
        if((who==connection&&character!=c.Character)||(who!=connection&&character==c.Character)) return false;
    m_active.insert_or_assign(connection,c.Character);return true;
}
void Admission::Release(uint32_t connection){m_active.erase(connection);}
}
