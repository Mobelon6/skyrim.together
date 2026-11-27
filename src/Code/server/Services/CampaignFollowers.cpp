#include <GameServer.h>
#include <World.h>
#include <Services/CampaignAccess.h>
#include <Services/PartyService.h>
#include <CampaignFollower.h>
#include <InventoryEvidence.h>

namespace
{
// This server process serves a single configured campaign.
std::optional<CampaignFollower::Ownership> owners;
size_t indexedMods=SIZE_MAX;
std::map<uint32_t,std::string> plugins;
}

// Resolve saved plugin names only after the joining player's mods are known.
// Network mod numbers and connection IDs can both change on restart.
bool CampaignAccess::RestoreFollowers(World& world)
{
    auto& link=Authority();
    if (link.State()==CampaignReplication::Link::Mode::Disabled) return true;
    if (link.State()!=CampaignReplication::Link::Mode::Ready) return false;
    if (!owners) owners=CampaignFollower::Read(link.Journal);
    if (!owners) return false;
    const auto& mods=world.ctx().at<ModsComponent>();
    const auto count=mods.GetStandardMods().size()+mods.GetLiteMods().size();
    if (count==indexedMods) return true;
    for (const auto* list:{&mods.GetStandardMods(),&mods.GetLiteMods()})
        for (const auto& [name,entry]:*list)
            plugins[entry.id]=InventoryEvidence::CanonicalPlugin({name.data(),name.size()});
    auto& live=world.GetPartyService().Followers();
    for (const auto& [key,character]:*owners)
    {
        if (!character) continue;
        const auto npc=TradeProperties::DecodeForm(key);
        for (const auto& [index,name]:plugins)
        {
            if (name!=npc->Plugin) continue;
            GameId id(index,npc->Local);
            auto [it,inserted]=live.try_emplace(id.LogFormat());
            if (!inserted) continue;
            it->second.Npc=id;
            it->second.Character=character;
            it->second.Confirmed=true;
            it->second.Detached=true;
        }
    }
    indexedMods=count;
    return true;
}

bool CampaignAccess::RecordFollower(GameId id,uint64_t character,bool hire,bool reserved)
{
    auto& link=Authority();
    if (link.State()==CampaignReplication::Link::Mode::Disabled) return true;
    if (!owners||!character) return false;
    const auto it=plugins.find(id.ModId);
    // Temporary runtime forms have no persistent plugin key.
    if (it==plugins.end()) return true;
    return CampaignFollower::Write(link.Journal,*owners,{it->second,id.BaseId},character,hire,reserved);
}
