#pragma once
#include "HomeNativeCapabilities.h"
#include "HomePermissions.h"
#include "HomeEstateCatalog.h"
#include <cstdlib>
namespace HomeLinkedAccess
{
using Graph=std::map<std::string,std::set<std::string>>;
inline std::optional<Graph> BuildGraph(const std::set<HomeNativeCapabilities::Row>& rows)
{
    std::map<std::string,std::pair<std::string,std::string>> doors;
    for (const auto& [tag,cell,door,target]:rows)
        if (tag=='L'&&!doors.emplace(door,std::pair{cell,target}).second) return {};
    Graph graph;
    for (const auto& [door,link]:doors)
    {
        const auto other=doors.find(link.second);
        if (other==doors.end()||other->second.second!=door||other->second.first==link.first) return {};
        graph[link.first].insert(other->second.first);
    }
    return graph;
}
inline std::set<std::string> Connected(const Graph& graph,const std::string& root)
{
    std::set<std::string> result{root};std::vector<std::string> pending{root};
    for (size_t i=0;i<pending.size();++i)
    {
        const auto found=graph.find(pending[i]);
        if (found==graph.end()) continue;
        for (const auto& other:found->second)
            if (result.insert(other).second)
            {
                if (result.size()>HomePermissions::MaxGroupRooms) return {};
                pending.push_back(other);
            }
    }
    return result;
}
inline std::vector<uint32_t> RegisteredRooms(const CampaignJournal::Journal& journal,
    const HomeRegistry::Home& home,const Graph& graph)
{
    const auto rooms=Connected(graph,home.Root.Encode());
    const auto homes=HomeRegistry::Read(journal);
    if (!homes||rooms.empty()) return {};
    std::vector<uint32_t> result;
    for (const auto& candidate:*homes)
        if (candidate.Owner==home.Owner&&rooms.count(candidate.Root.Encode())) result.push_back(candidate.Slot);
    if (!std::binary_search(result.begin(),result.end(),home.Slot)) return {};
    return result;
}
inline std::vector<uint32_t> RegisteredRooms(const CampaignJournal::Journal& journal,const HomeRegistry::Home& home)
{
    const auto& catalog=HomeEstateCatalog::Host();
    if (!catalog) return {};
    if (const auto* estate=catalog->ForRoom(home.Root.Encode()))
        return HomeEstateCatalog::RegisteredRooms(*estate,journal,home);
    static const auto graph=[]()->std::optional<Graph>
    {
        const auto* path=std::getenv("STM_CAMPAIGN_ROOT");
        if (!path||!*path) return Graph{};
        const auto file=std::filesystem::path(path)/"CoopNativeHomeObjects.txt";
        if (!std::filesystem::exists(file)) return Graph{};
        std::ifstream in(file);
        const auto rows=HomeNativeCapabilities::Read(in);
        return rows?BuildGraph(*rows):std::nullopt;
    }();
    return graph?RegisteredRooms(journal,home,*graph):std::vector<uint32_t>{};
}
}
