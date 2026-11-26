#pragma once
#include "HomeRegistry.h"
#include <map>
#include <set>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <cmath>
#include <vector>

namespace HomeEstateCatalog
{
// Derived from the same frozen winning plugin profile as native capabilities.
// Optional B rows are provisional anchor polygons, never access grants.
struct Point {int32_t X{},Y{};};
using Polygon=std::vector<Point>;
inline double Cross(const Point& a,const Point& b,double x,double y) noexcept
{return (double(b.X)-a.X)*(y-a.Y)-(double(b.Y)-a.Y)*(x-a.X);}
inline bool ValidPolygon(const Polygon& p) noexcept
{
    if (p.size()<3||p.size()>64) return false;
    for (size_t i=0;i<p.size();++i)
        for (size_t j=0;j<p.size();++j)
            if (j!=i&&j!=(i+1)%p.size()&&Cross(p[i],p[(i+1)%p.size()],p[j].X,p[j].Y)<=0) return false;
    return true;
}
inline bool Contains(const Polygon& p,double x,double y) noexcept
{
    if (p.size()<3||!std::isfinite(x)||!std::isfinite(y)) return false;
    for (size_t i=0;i<p.size();++i)
        if (Cross(p[i],p[(i+1)%p.size()],x,y)<0) return false;
    return true;
}
struct Estate
{
    std::string Quest,Main,World,Table,Marker;
    std::set<std::string> Rooms;
    std::map<std::string,std::string> Construction;
    std::map<std::string,Polygon> CandidateRegions;
    bool ContainsCandidate(const std::string& world,double x,double y) const noexcept
    {
        if (World!=world) return false;
        for (const auto& [_,p]:CandidateRegions) if (Contains(p,x,y)) return true;
        return false;
    }
};
struct Catalog
{
    std::map<std::string,Estate> Estates;
    std::map<std::string,std::string> RoomEstate;
    const Estate* ForRoom(const std::string& room) const
    {
        const auto it=RoomEstate.find(room);
        if (it==RoomEstate.end()) return nullptr;
        const auto found=Estates.find(it->second);
        return found==Estates.end()?nullptr:&found->second;
    }
};
inline bool Key(const std::string& s)
{
    const auto k=TradeProperties::DecodeForm(s);
    return k&&k->Valid()&&k->Encode()==s;
}
inline std::optional<Catalog> Read(std::istream& input)
{
    Catalog result;std::string line;
    if (!std::getline(input,line)||line!="STM-ESTATE-INDEX 1") return {};
    std::set<std::string> tables,markers;
    while (std::getline(input,line))
    {
        if (line.size()>1400) return {};
        std::istringstream row(line);char tag{};std::string extra;
        if (!(row>>tag)) return {};
        if (tag=='E')
        {
            Estate e;
            if (!(row>>e.Quest>>e.Main>>e.World>>e.Table>>e.Marker)||row>>extra||
                !Key(e.Quest)||!Key(e.Main)||!Key(e.World)||!Key(e.Table)||!Key(e.Marker)||
                e.Table==e.Marker||!tables.insert(e.Table).second||!markers.insert(e.Marker).second||
                result.Estates.size()>=128) return {};
            const auto key=e.Quest;
            if (!result.Estates.emplace(key,std::move(e)).second) return {};
        }
        else if (tag=='I')
        {
            std::string quest,room;
            if (!(row>>quest>>room)||row>>extra||!Key(room)) return {};
            auto found=result.Estates.find(quest);
            if (found==result.Estates.end()||found->second.Rooms.size()>=128||
                !result.RoomEstate.emplace(room,quest).second) return {};
            found->second.Rooms.insert(room);
        }
        else if (tag=='C')
        {
            std::string quest,ref,base;
            if (!(row>>quest>>ref>>base)||row>>extra||!Key(ref)||!Key(base)) return {};
            auto found=result.Estates.find(quest);
            if (found==result.Estates.end()||found->second.Construction.size()>=1024||
                !found->second.Construction.emplace(ref,base).second) return {};
        }
        else if (tag=='B')
        {
            std::string quest,region;size_t count{};
            if (!(row>>quest>>region>>count)||count<3||count>64||(region!="main"&&region!="hatchery")) return {};
            auto found=result.Estates.find(quest);
            if (found==result.Estates.end()) return {};
            Polygon polygon;
            for (size_t i=0;i<count;++i)
            {
                Point p;
                if (!(row>>p.X>>p.Y)||p.X < -10000000||p.X > 10000000||p.Y < -10000000||p.Y > 10000000) return {};
                polygon.push_back(p);
            }
            if (row>>extra||!ValidPolygon(polygon)||!found->second.CandidateRegions.emplace(region,std::move(polygon)).second) return {};
        }
        else return {};
    }
    if (input.bad()||result.Estates.empty()) return {};
    std::set<std::string> construction;
    for (const auto& [_,e]:result.Estates)
    {
        if (!e.Rooms.count(e.Main)||(!e.CandidateRegions.empty()&&!e.CandidateRegions.count("main"))) return {};
        if (!e.Construction.empty()&&(!e.Construction.count(e.Marker)||!e.Construction.count(e.Table))) return {};
        for (const auto& [ref,base]:e.Construction)
            if (!construction.insert(ref).second||result.RoomEstate.count(ref)) return {};
    }
    return result;
}
inline const std::optional<Catalog>& Host()
{
    static const auto catalog=[]()->std::optional<Catalog>
    {
        const auto* root=std::getenv("STM_CAMPAIGN_ROOT");
        if (!root||!*root) return Catalog{};
        const auto file=std::filesystem::path(root)/"CoopEstateIndex.txt";
        std::error_code ec;
        const bool exists=std::filesystem::exists(file,ec);
        if (ec) return {};
        if (!exists) return Catalog{};
        std::ifstream input(file);
        return Read(input);
    }();
    return catalog;
}
// Presence identity is not a property registration or an access grant.
// Stable quest key + character + campaign exists even before land purchase.
// The caller must authenticate the character and independently check access.
inline std::optional<HomeScope::EstateContext> Identity(const Catalog& catalog,
    uint64_t campaign,uint64_t owner,const std::string& quest)
{
    if (!campaign||!owner) return {};
    const auto found=catalog.Estates.find(quest);
    if (found==catalog.Estates.end()) return {};
    return HomeScope::EstateContext{campaign,owner,found->first};
}
inline std::optional<HomeScope::EstateContext> Identity(const Catalog& catalog,
    const CampaignJournal::Journal& journal,uint64_t campaign,const HomeRegistry::Home& room)
{
    const auto* e=catalog.ForRoom(room.Root.Encode());
    if (!e) return {};
    const auto registered=HomeRegistry::BySlot(journal,room.Slot);
    if (!registered||!registered->Same(room.Owner,room.Root)) return {};
    return Identity(catalog,campaign,room.Owner,e->Quest);
}
inline std::vector<uint32_t> RegisteredRooms(const Estate& estate,
    const CampaignJournal::Journal& journal,const HomeRegistry::Home& room)
{
    const auto homes=HomeRegistry::Read(journal);
    if (!homes||!estate.Rooms.count(room.Root.Encode())) return {};
    std::vector<uint32_t> slots;
    for (const auto& h:*homes)
        if (h.Owner==room.Owner&&estate.Rooms.count(h.Root.Encode())) slots.push_back(h.Slot);
    if (!std::binary_search(slots.begin(),slots.end(),room.Slot)) return {};
    return slots;
}
}
