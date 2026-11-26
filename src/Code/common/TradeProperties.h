#pragma once
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

// Canonical values for supported extra-data. Never persist a load-order slot
// or a dynamic engine form ID. V1 records remain byte-for-byte valid.
namespace TradeProperties
{
inline bool Number(std::string_view s,uint32_t& v)
{
    if (s.empty() || (s.size()>1 && s.front()=='0')) return false;
    auto parsed=std::from_chars(s.data(),s.data()+s.size(),v);
    return parsed.ec==std::errc{} && parsed.ptr==s.data()+s.size();
}
struct FormKey
{
    std::string Plugin{};
    uint32_t Local{};
    bool Empty() const {return Plugin.empty() && !Local;}
    bool Valid() const
    {
        if (Plugin.empty() || Plugin.size()>103 || !Local || Local>0xFFFFFF) return false;
        for (unsigned char ch:Plugin)
            if (ch<32 || ch==127 || (ch>='A' && ch<='Z') || ch=='/' || ch=='\\' || ch==':') return false;
        return true;
    }
    std::string Encode() const
    {
        if (Empty()) return "-";
        std::string out;
        constexpr char digits[]="0123456789abcdef";
        for (unsigned char ch:Plugin)
        {
            out+=digits[ch>>4];out+=digits[ch&15];
        }
        return out+"."+std::to_string(Local);
    }
};
inline std::optional<FormKey> DecodeForm(std::string_view s)
{
    if (s=="-") return FormKey{};
    auto dot=s.find('.');
    if (dot==std::string_view::npos || !dot || dot>206 || dot%2) return {};
    FormKey key;
    auto digit=[](char c)->int
    {
        if (c>='0'&&c<='9') return c-'0';
        if (c>='a'&&c<='f') return c-'a'+10;
        return -1;
    };
    for (size_t i=0;i<dot;i+=2)
    {
        auto hi=digit(s[i]),lo=digit(s[i+1]);
        if (hi<0||lo<0) return {};
        key.Plugin+=static_cast<char>(hi*16+lo);
    }
    if (!Number(s.substr(dot+1),key.Local)||!key.Valid()) return {};
    return key;
}
// Preserve the original engine bytes (including non-ASCII names), not
// the localized/tempered suffix. Hex prevents delimiter and log injection.
inline constexpr size_t MaxNameBytes=255;
inline bool ValidName(std::string_view name)
{
    if (name.empty() || name.size()>MaxNameBytes) return false;
    for (unsigned char ch:name)
        if (ch<32 || ch==127) return false;
    return true;
}
inline std::string EncodeName(std::string_view name)
{
    constexpr char digits[]="0123456789abcdef";
    std::string out;
    for (unsigned char ch:name) {out+=digits[ch>>4];out+=digits[ch&15];}
    return out;
}
inline std::optional<std::string> DecodeName(std::string_view hex)
{
    if (hex.empty() || hex.size()%2 || hex.size()>2*MaxNameBytes) return {};
    std::string out;
    const auto digit=[](char c)->int
    {
        if (c>='0'&&c<='9') return c-'0';
        if (c>='a'&&c<='f') return c-'a'+10;
        return -1;
    };
    for (size_t i=0;i<hex.size();i+=2)
    {
        const auto hi=digit(hex[i]),lo=digit(hex[i+1]);
        if (hi<0||lo<0) return {};
        out+=static_cast<char>(hi*16+lo);
    }
    if (!ValidName(out)) return {};
    return out;
}
// Created enchantments use ordered effects, never a save-local FF form ID.
struct Effect
{
    FormKey Setting;
    uint32_t Magnitude{},Area{},Duration{},Cost{};
    bool Valid() const
    {
        return Setting.Valid() && std::isfinite(std::bit_cast<float>(Magnitude)) &&
            std::isfinite(std::bit_cast<float>(Cost)) && Area<=INT32_MAX && Duration<=INT32_MAX;
    }
};
struct CreatedEnchant
{
    bool Weapon{};
    // Cost override, flags, casting, charge override, delivery, spell type, charge time.
    std::array<uint32_t,7> Metadata{};
    FormKey Base{},Restrictions{};
    std::string Name;
    std::vector<Effect> Effects;
    bool Valid() const
    {
        if (Effects.empty() || Effects.size()>8 || Metadata[2]>2 || Metadata[4]>4 ||
            Metadata[5]>13 || !std::isfinite(std::bit_cast<float>(Metadata[6])) ||
            (!Base.Empty()&&!Base.Valid()) || (!Restrictions.Empty()&&!Restrictions.Valid()) ||
            (!Name.empty()&&!ValidName(Name))) return false;
        for (const auto& e:Effects) if (!e.Valid()) return false;
        return true;
    }
    std::string Encode() const
    {
        std::string out=Weapon?"1":"0";
        for (auto v:Metadata) out+=","+std::to_string(v);
        out+=","+Base.Encode()+","+Restrictions.Encode()+","+(Name.empty()?"-":EncodeName(Name));
        for (const auto& e:Effects)
            out+=","+e.Setting.Encode()+","+std::to_string(e.Magnitude)+","+std::to_string(e.Area)+
                 ","+std::to_string(e.Duration)+","+std::to_string(e.Cost);
        return out;
    }
};
inline std::optional<CreatedEnchant> DecodeCreated(std::string_view input)
{
    if (input.empty() || input.size()>3000) return {};
    std::vector<std::string_view> f;
    auto rest=input;
    for (;;)
    {
        auto end=rest.find(',');
        f.push_back(rest.substr(0,end));
        if (f.back().empty() || f.size()>51) return {};
        if (end==std::string_view::npos) break;
        rest.remove_prefix(end+1);
    }
    if (f.size()<16 || (f.size()-11)%5 || (f[0]!="0"&&f[0]!="1")) return {};
    CreatedEnchant result;result.Weapon=f[0]=="1";
    for (unsigned i=0;i<7;++i) if (!Number(f[i+1],result.Metadata[i])) return {};
    auto base=DecodeForm(f[8]),restrictions=DecodeForm(f[9]);
    if (!base||!restrictions) return {};
    result.Base=*base;result.Restrictions=*restrictions;
    if (f[10]!="-")
    {
        auto name=DecodeName(f[10]);
        if (!name) return {};
        result.Name=std::move(*name);
    }
    for (size_t i=11;i<f.size();i+=5)
    {
        Effect e;
        auto key=DecodeForm(f[i]);
        if (!key||!Number(f[i+1],e.Magnitude)||!Number(f[i+2],e.Area)||
            !Number(f[i+3],e.Duration)||!Number(f[i+4],e.Cost)) return {};
        e.Setting=*key;result.Effects.push_back(std::move(e));
    }
    if (!result.Valid()||result.Encode()!=input) return {};
    return result;
}
struct Values
{
    uint32_t Mask{},Health{},Charge{},Soul{};
    enum : uint32_t { HasHealth=1, HasCharge=2, HasSoul=4, HasEnchant=8, HasPoison=16, HasName=32, HasCreated=64, HasOwnership=128 };
    FormKey Enchant{},Poison{},Owner{};
    uint32_t EnchantCharge{},RemoveOnUnequip{},PoisonCount{};
    std::string Name{};
    std::optional<CreatedEnchant> Created;
    bool Valid() const
    {
        if (Mask&~255u) return false;
        if (Mask&HasOwnership)
        {
            // Player base 7 is local to each player; no stable campaign
            // character identity can be inferred from this engine marker.
            if (!Owner.Valid() || (Owner.Plugin=="skyrim.esm" && Owner.Local==7)) return false;
        }
        else if (!Owner.Empty()) return false;
        if (Mask&HasCreated)
        {
            if (!(Mask&HasEnchant) || !Created || !Created->Valid() || !Enchant.Empty()) return false;
        }
        else if (Created) return false;
        if (Mask&HasName)
        {
            if (!ValidName(Name)) return false;
        }
        else if (!Name.empty()) return false;
        if (Mask&HasHealth)
        {
            const auto v=std::bit_cast<float>(Health);
            if (!std::isfinite(v)||v<=0) return false;
        }
        else if (Health) return false;
        if (Mask&HasCharge)
        {
            const auto v=std::bit_cast<float>(Charge);
            if (!std::isfinite(v)||v<0) return false;
        }
        else if (Charge) return false;
        if (Mask&HasSoul) {if (Soul>5) return false;}
        else if (Soul) return false;
        if (Mask&HasEnchant)
        {
            if ((!(Mask&HasCreated)&&!Enchant.Valid()) || EnchantCharge>UINT16_MAX || RemoveOnUnequip>1) return false;
        }
        else if (!Enchant.Empty() || EnchantCharge || RemoveOnUnequip) return false;
        if (Mask&HasPoison)
        {
            if (!Poison.Valid() || !PoisonCount) return false;
        }
        else if (!Poison.Empty() || PoisonCount) return false;
        return true;
    }
    std::string Encode() const
    {
        if (!Mask) return {};
        if (Mask&HasOwnership)
        {
            auto inner=*this;
            inner.Mask&=~HasOwnership;inner.Owner={};
            auto encoded=inner.Encode();
            return "5:"+Owner.Encode()+":"+(encoded.empty()?"-":encoded);
        }
        const auto fields=std::to_string(Mask)+":"+std::to_string(Health)+":"+
            std::to_string(Charge)+":"+std::to_string(Soul);
        if (!(Mask&(HasEnchant|HasPoison|HasName))) return "1:"+fields;
        const auto extra=fields+":"+Enchant.Encode()+":"+std::to_string(EnchantCharge)+":"+
            std::to_string(RemoveOnUnequip)+":"+Poison.Encode()+":"+std::to_string(PoisonCount);
        if (Mask&HasCreated) return "4:"+extra+":"+(Name.empty()?"-":EncodeName(Name))+":"+(Created?Created->Encode():"-");
        if (Mask&HasName) return "3:"+extra+":"+EncodeName(Name);
        return "2:"+extra;
    }
};
inline std::optional<Values> Decode(std::string_view input)
{
    if (input.empty()) return Values{};
    if (input.size()>3000) return {};
    if (input.starts_with("5:"))
    {
        const auto end=input.find(':',2);
        if (end==std::string_view::npos) return {};
        auto owner=DecodeForm(input.substr(2,end-2));
        auto inner=input.substr(end+1);
        if (!owner || !owner->Valid() || inner.empty() || inner.starts_with("5:")) return {};
        auto v=Decode(inner=="-"?std::string_view{}:inner);
        if (!v || (v->Mask&Values::HasOwnership)) return {};
        v->Mask|=Values::HasOwnership;v->Owner=*owner;
        if (!v->Valid() || v->Encode()!=input) return {};
        return v;
    }
    if (!input.starts_with("1:")&&!input.starts_with("2:")&&!input.starts_with("3:")&&!input.starts_with("4:")) return {};
    const bool created=input.front()=='4',named=input.front()=='3'||created,extended=input.front()!='1';
    auto rest=input.substr(2);
    std::array<std::string_view,11> fields{};
    const unsigned n=created?11:named?10:extended?9:4;
    for (unsigned i=0;i<n;++i)
    {
        const auto end=rest.find(':');
        if ((i==n-1)!=(end==std::string_view::npos)) return {};
        fields[i]=rest.substr(0,end);
        if (fields[i].empty()) return {};
        if (i!=n-1) rest.remove_prefix(end+1);
    }
    Values v;
    if (!Number(fields[0],v.Mask)||!Number(fields[1],v.Health)||
        !Number(fields[2],v.Charge)||!Number(fields[3],v.Soul)) return {};
    if (extended)
    {
        auto enchant=DecodeForm(fields[4]),poison=DecodeForm(fields[7]);
        if (!enchant||!poison||!Number(fields[5],v.EnchantCharge)||
            !Number(fields[6],v.RemoveOnUnequip)||!Number(fields[8],v.PoisonCount)) return {};
        v.Enchant=*enchant;v.Poison=*poison;
    }
    if (named && (!created || fields[9]!="-"))
    {
        auto name=DecodeName(fields[9]);
        if (!name) return {};
        v.Name=std::move(*name);
    }
    if (created)
    {
        auto enchant=DecodeCreated(fields[10]);
        if (!enchant) return {};
        v.Created=std::move(*enchant);
    }
    if (!v.Mask||!v.Valid()||v.Encode()!=input) return {};
    return v;
}
}
