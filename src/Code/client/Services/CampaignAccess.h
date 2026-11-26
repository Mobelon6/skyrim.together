#pragma once
#include <CampaignIdentity.h>
#include <CampaignReplication.h>
#include <array>
#include <map>
#include <HomeResidents.h>
#include <HomeObjects.h>
#include <HomePlacement.h>
#include <HomeEstateConstruction.h>
struct World;
namespace CampaignAccess
{
struct Identity
{
    CampaignIdentity::Stamp Save;
    CampaignIdentity::Credential Credential;
    std::string Profile;
};
CampaignReplication::Link::Mode Mode();
std::optional<Identity> Current(World&);
std::optional<Identity> LocalIdentity(World&);
bool GameReady();
bool TradeEnabled();
uint32_t HomeScope(uint64_t campaign,uint64_t character,std::string_view root);
// 0 missing, 1 complete, 2 invalid/unavailable; no game effects applied.
uint32_t HomeBaselineState(uint64_t campaign,uint32_t slot);
uint32_t HomeContainerState(uint64_t campaign,uint32_t slot,std::string_view reference);
// Catalog offers only; does not authorize entry into a foreign scope.
std::vector<std::pair<uint32_t,uint64_t>> HomeOffers(uint64_t campaign,uint64_t character,std::string_view root);
std::optional<std::string> HomeBaseline(uint64_t campaign,uint32_t slot);
// Canonical ref -> state, level and per-reference revision.
std::optional<std::map<std::string,std::array<uint32_t,3>>> HomeLockView(uint64_t campaign,uint32_t slot);
std::optional<HomeResidents::Residents> HomeResidentView(uint64_t campaign,uint32_t slot);
std::optional<HomeObjects::Records> HomeObjectView(uint64_t campaign,uint32_t slot);
std::optional<std::vector<HomePlacement::Stack>> HomePlacements(uint64_t campaign,uint32_t slot);
std::optional<HomeEstateConstruction::Record> EstateConstruction(uint64_t campaign,uint64_t owner,const std::string& plot);
std::optional<HomeEstateConstruction::Record> SharedEstateConstruction(uint64_t campaign,const std::string& plot);
std::optional<std::string> EstateRoomShape(uint64_t campaign,uint32_t slot,const std::string& root);
bool HomeCanModify(uint64_t campaign,uint32_t slot,uint64_t character);
bool ConfirmReceipt(World&, uint64_t stream, uint64_t savedStream, uint32_t cursor, uint32_t receipt,
                    std::string_view plugin, uint32_t base, int count,int64_t before,std::string_view variant={});
}
