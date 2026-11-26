#pragma once
#include <CampaignIdentity.h>
#include <CampaignReplication.h>
struct World;
struct Player;
struct RequestInventoryChanges;
namespace CampaignAccess
{
CampaignReplication::Link& Authority();
bool Admit(World&, Player&, const CampaignIdentity::Stamp&, std::string_view profile, std::string_view secret);
uint64_t Character(const Player&);
void RecordQuestItemChange(World&,Player&,const RequestInventoryChanges&) noexcept;
void ReplayQuestReceipt(World&,Player&,std::string_view) noexcept;
bool ReserveQuestPickup(World&,Player&,std::string_view,uint64_t nonce) noexcept;
}
