#pragma once
#include <CampaignIdentity.h>
#include <CampaignReplication.h>
struct World;
struct Player;
struct RequestInventoryChanges;
struct GameId;
namespace CampaignAccess
{
CampaignReplication::Link& Authority();
bool Admit(World&, Player&, const CampaignIdentity::Stamp&, std::string_view profile, std::string_view secret);
uint64_t Character(const Player&);
bool RestoreFollowers(World&);
bool RecordFollower(GameId,uint64_t character,bool hire,bool reserved=false);
void Disconnect(uint32_t connection);
bool RecordQuestItemChange(World&,Player&,const RequestInventoryChanges&) noexcept;
void ReplayQuestReceipt(World&,Player&,std::string_view) noexcept;
bool ReserveQuestPickup(World&,Player&,std::string_view,uint64_t nonce) noexcept;
}
