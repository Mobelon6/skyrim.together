#pragma once
#include "HomeSnapshot.h"
#include <array>
#include <charconv>
#include <cmath>
#include <bit>
#include <sstream>

namespace HomePlacement
{
// A placed stack has a receipt-derived identity, not a runtime FF form ID.
// Its creation pose is immutable; later poses are separate CAS observations.
using Pose=std::array<uint32_t,6>;
inline bool Valid(const Pose& p)
{
    for (size_t i=0;i<p.size();++i)
    {
        const auto f=std::bit_cast<float>(p[i]);
        if (!std::isfinite(f) || std::abs(f)>(i<3?10000000.f:1000.f)) return false;
    }
    return true;
}
inline bool Different(const Pose& a,const Pose& b)
{
    for (size_t i=0;i<a.size();++i)
        if (std::abs(std::bit_cast<float>(a[i])-std::bit_cast<float>(b[i]))>(i<3?2.f:0.03f)) return true;
    return false;
}
inline std::string EncodePose(const Pose& p)
{
    std::string s;
    for (auto n:p) {if (!s.empty()) s+='/';s+=std::to_string(n);}
    return s;
}
inline std::optional<Pose> DecodePose(std::string_view s)
{
    Pose p{};
    for (size_t i=0;i<p.size();++i)
    {
        const auto sep=s.find('/');
        if ((i+1==p.size())!=(sep==std::string_view::npos) ||
            !TradeProperties::Number(s.substr(0,sep),p[i])) return {};
        if (sep!=std::string_view::npos) s.remove_prefix(sep+1);
    }
    if (!Valid(p)) return {};
    return p;
}
struct Key
{
    uint32_t Slot{};
    uint64_t Creator{};
    uint32_t Receipt{};
    Pose Initial{};
    uint32_t LegacySource{};
    std::string Encode() const
    {
        return "home/"+std::to_string(Slot)+(LegacySource?"/legacy/":"/drop/")+std::to_string(Creator)+"/"+
            std::to_string(LegacySource?LegacySource:Receipt)+"/"+EncodePose(Initial);
    }
};
inline std::optional<Key> Decode(std::string_view s)
{
    if (!s.starts_with("home/") || s.size()>180) return {};
    const auto original=s;
    s.remove_prefix(5);
    auto sep=s.find('/');
    Key k;
    if (sep==std::string_view::npos || !TradeProperties::Number(s.substr(0,sep),k.Slot) ||
        !k.Slot || k.Slot>HomeRegistry::MaxSlot) return {};
    s.remove_prefix(sep+1);
    const bool legacy=s.starts_with("legacy/");
    if (!legacy && !s.starts_with("drop/")) return {};
    s.remove_prefix(legacy?7:5);
    sep=s.find('/');
    if (sep==std::string_view::npos) return {};
    const auto owner=s.substr(0,sep);
    const auto parsed=std::from_chars(owner.data(),owner.data()+owner.size(),k.Creator);
    if (!k.Creator || parsed.ec!=std::errc{} || parsed.ptr!=owner.data()+owner.size()) return {};
    s.remove_prefix(sep+1);
    sep=s.find('/');
    uint32_t token{};
    if (sep==std::string_view::npos || !TradeProperties::Number(s.substr(0,sep),token)) return {};
    if (legacy)
    {
        if ((token>>24)!=0xFF || !(token&0xFFFFFFu)) return {};
        k.LegacySource=token;
    }
    else
    {
        if (!token || token>0xFFFFFE) return {};
        k.Receipt=token;
    }
    auto pose=DecodePose(s.substr(sep+1));
    if (!pose) return {};
    k.Initial=*pose;
    return k.Encode()==original?std::optional<Key>(k):std::nullopt;
}
struct Stack
{
    Key Id;
    HomeSnapshot::Item Item;
    Pose Position;
    uint32_t Revision{};
};
struct Move
{
    std::string Account;
    uint32_t Revision{};
    Pose Position{};
    std::string Encode() const {return Account+" "+std::to_string(Revision)+" "+EncodePose(Position);}
};
inline std::optional<Move> DecodeMove(std::string_view s)
{
    Move m;std::string pose,extra;
    std::istringstream in{std::string(s)};
    if (!(in>>m.Account>>m.Revision>>pose) || in>>extra || !m.Revision || !Decode(m.Account)) return {};
    auto p=DecodePose(pose);if (!p) return {};
    m.Position=*p;
    return m.Encode()==s?std::optional<Move>(m):std::nullopt;
}
}
