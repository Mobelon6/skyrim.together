#include <GameServer.h>
#include <World.h>
#include <Game/Player.h>
#include <Services/QuestService.h>
#include <Messages/RequestQuestJournal.h>
#include <Messages/NotifyQuestJournal.h>
#include <Messages/NotifyQuestUpdate.h>
#include <QuestJournalDisk.h>
#include <CampaignReplication.h>
#include <CampaignIdentity.h>
#include <CampaignCheckpoint.h>
#include <QuestWordReward.h>
#include <DragonAbsorption.h>
#include <Services/CampaignAccess.h>
#include <Messages/RequestCampaignPull.h>
#include <Messages/NotifyCampaignRecord.h>
#include <sstream>
#include <iomanip>

namespace
{
CampaignReplication::Link campaignLink;
CampaignIdentity::Admission admission;
bool admissionLoaded{};
const std::filesystem::path kJournal{"config/CoopJournal"};

std::string Field(const String& value)
{
    std::string out(value.begin(), value.end());
    for (auto& c : out)
        if (static_cast<unsigned char>(c) < 32)
            c = ' ';
    return out;
}

void Flush(PartyService::Party& party)
{
    if (!party.JournalStream)
        party.JournalStream = QuestJournalDisk::CreateStream(kJournal);
    if (!party.JournalStream)
        return;
    // Limit recovery work per request after a transient disk failure.
    for (unsigned n = 0; n < 8 && party.JournalCommitted < party.JournalRows.size(); ++n)
    {
        const auto& row = party.JournalRows[party.JournalCommitted];
        if (!QuestJournalDisk::Store(kJournal, party.JournalStream, party.JournalCommitted + 1, {row.data(), row.size()}))
        {
            spdlog::error("Cannot commit co-op journal stream {} record {}", party.JournalStream, party.JournalCommitted + 1);
            break;
        }
        ++party.JournalCommitted;
    }
}
}

CampaignReplication::Link& CampaignAccess::Authority() { campaignLink.Start(true); return campaignLink; }
bool CampaignAccess::Admit(World& world, Player& player, const CampaignIdentity::Stamp& stamp, std::string_view profile, std::string_view secret)
{
    if (Authority().State()!=CampaignReplication::Link::Mode::Ready || profile!=campaignLink.Profile || secret.size()!=64)
        return false;
    if (!admissionLoaded)
    {
        admissionLoaded=admission.Load(campaignLink.Root);
        if (!admissionLoaded) { campaignLink.Fail(); return false; }
    }
    std::vector<uint32_t> disconnected;
    for (const auto& entry:admission.Active())
        if (!world.GetPlayerManager().GetById(entry.first)) disconnected.push_back(entry.first);
    for (auto id:disconnected) admission.Release(id);
    return CampaignIdentity::Known(campaignLink.Journal,campaignLink.Campaign,stamp) &&
        admission.Claim(player.GetId(),{stamp.Character,std::string(secret)});
}
void CampaignAccess::Disconnect(uint32_t connection) { admission.Release(connection); }

uint64_t CampaignAccess::Character(const Player& player)
{
    const auto it=admission.Active().find(player.GetId());
    return it==admission.Active().end()?0:it->second;
}

void RecordCampaignDragonLife(World& world,Player& owner,entt::entity entity,bool dead) noexcept
{
    try
    {
        auto& link=CampaignAccess::Authority();
        if (link.State()!=CampaignReplication::Link::Mode::Ready) return;
        const auto* actor=world.try_get<CharacterComponent>(entity);
        const auto* reference=world.try_get<FormIdComponent>(entity);
        const auto* cell=world.try_get<CellIdComponent>(entity);
        if (!actor||!actor->IsDragon()||!reference||!reference->Id||!cell) return;
        std::string key;
        const auto& mods=world.ctx().at<ModsComponent>();
        for (const auto* list:{&mods.GetStandardMods(),&mods.GetLiteMods()})
            for (const auto& [name,entry]:*list)
            {
                if (entry.id!=reference->Id.ModId) continue;
                std::string plugin(name.begin(),name.end());
                for (auto& ch:plugin) if (ch>='A'&&ch<='Z') ch=static_cast<char>(ch-'A'+'a');
                const TradeProperties::FormKey form{plugin,reference->Id.BaseId};
                if (form.Valid()&&(list!=&mods.GetLiteMods()||form.Local<=0xFFF)) key=form.Encode();
            }
        if (key.empty()) return;
        const auto history=DragonAbsorption::ReadShared(link.Journal.Operations());
        if (!history||history->Retired.contains(key)) return;
        std::string kind,payload;
        if (history->Kills.contains(key))
        {
            kind="dragon-retired-v1";payload=key;
        }
        else if (dead)
        {
            const auto* party=world.GetPartyService().GetPlayerParty(&owner);
            uint64_t a{},b{};
            if (party&&party->Members.size()==2)
                for (auto* player:party->Members)
                {
                    if (!player||!player->GetCharacter()||!cell->IsInRange(player->GetCellComponent(),false)) continue;
                    const auto* participant=world.try_get<CharacterComponent>(*player->GetCharacter());
                    const auto character=CampaignAccess::Character(*player);
                    if (!participant||participant->IsDead()||!character) continue;
                    if (!a) a=character;else b=character;
                }
            DragonAbsorption::Kill kill{key,std::min(a,b),std::max(a,b)};
            if (kill.Valid()&&kill.Other(CampaignAccess::Character(owner)))
            {kind="dragon-death-v1";payload=kill.Encode();}
            else
            {kind="dragon-retired-v1";payload=key;}
        }
        else return;
        const auto head=link.Journal.Current();
        if (link.Journal.Append(head,{"dragon-life-"+std::to_string(head.Revision+1),kind,payload})!=CampaignJournal::Result::Committed)
            spdlog::warn("STM could not persist dragon life");
    }
    catch (...) {spdlog::warn("STM dragon life observation failed");}
}

void QuestService::RecordQuest(Player* player, const NotifyQuestUpdate& event) noexcept
{
    auto* party = m_world.GetPartyService().GetPlayerParty(player);
    if (!party)
        return;
    try
    {
        const auto& mods = m_world.ctx().at<ModsComponent>();
        std::string plugin;
        for (const auto* list : {&mods.GetStandardMods(), &mods.GetLiteMods()})
            for (const auto& item : *list)
                if (item.second.id == event.Id.ModId)
                    plugin = Field(item.first);
        const char* phase = event.Status == NotifyQuestUpdate::Started ? "start" :
            event.Status == NotifyQuestUpdate::Stopped ? "stop" : "stage";
        std::ostringstream row;
        row << "quest-v1" << '	' << plugin << '	' << std::hex << event.Id.BaseId << std::dec << '	'
            << phase << '	' << event.Stage << '	' << player->GetId() << '	' << Field(player->GetUsername());
        const auto data = row.str();
        party->JournalRows.emplace_back(data.c_str());
        Flush(*party);
    }
    catch (const std::exception& e) { spdlog::error("Writing co-op quest journal: {}", e.what()); }
}

void QuestService::OnJournalRequest(const PacketEvent<RequestQuestJournal>& event) noexcept
{
    auto* party = m_world.GetPartyService().GetPlayerParty(event.pPlayer);
    if (!party || party->JournalRows.empty())
        return;
    try
    {
        Flush(*party);
        if (!party->JournalStream || !party->JournalCommitted)
            return;
        NotifyQuestJournal reply;
        reply.Stream = party->JournalStream;
        reply.Total = party->JournalCommitted;
        if (event.Packet.Stream != reply.Stream)
        {
            event.pPlayer->Send(reply); // announce stream identity before the client reads its durable cursor
            return;
        }
        for (uint32_t index = event.Packet.Sequence, n = 0; index < reply.Total && n < 8; ++index, ++n)
        {
            reply.Sequence = index + 1;
            reply.Row = party->JournalRows[index];
            event.pPlayer->Send(reply);
        }
    }
    catch (const std::exception& e) { spdlog::error("Serving co-op quest journal: {}", e.what()); }
}

void QuestService::OnCampaignPull(const PacketEvent<RequestCampaignPull>& event) noexcept
{
    try
    {
        const auto& p=event.Packet;
        if (!p.Nonce || p.Profile.size()>128 || p.Credential.size()!=64 || p.Checkpoint.size()>1024 || p.LearnedWord.size()>QuestWordReward::MaxPayload || p.AbsorbedDragon.size()>224 || p.PickupClaim.size()>800 || p.QuestReceipt.size()>800) return;
        CampaignReplication::Pull pull{p.Campaign,p.Nonce,std::string(p.Profile.data(),p.Profile.size()),{p.Revision,p.Tag}};
        CampaignReplication::Reply reply;
        bool pickupPermit=false;
        reply.Campaign=p.Campaign;reply.Nonce=p.Nonce;reply.Base=pull.Base;
        const CampaignIdentity::Stamp stamp{p.Campaign,p.Character,{p.SaveRevision,p.SaveTag}};
        if (CampaignAccess::Admit(m_world,*event.pPlayer,stamp,pull.Profile,{p.Credential.data(),p.Credential.size()}))
        {
            if (!p.QuestReceipt.empty()&&CampaignIdentity::Known(campaignLink.Journal,campaignLink.Campaign,
                {p.Campaign,p.Character,{p.Revision,p.Tag}}))
                CampaignAccess::ReplayQuestReceipt(m_world,*event.pPlayer,{p.QuestReceipt.data(),p.QuestReceipt.size()});
            if (!p.Checkpoint.empty())
            {
                const auto record=CampaignCheckpoint::Decode({p.Checkpoint.data(),p.Checkpoint.size()});
                if (record)
                {
                    const auto result=CampaignCheckpoint::Publish(campaignLink.Journal,campaignLink.Campaign,
                        CampaignAccess::Character(*event.pPlayer),*record);
                    if (result!=CampaignJournal::Result::Committed&&result!=CampaignJournal::Result::Duplicate)
                        spdlog::warn("STM checkpoint refused for character {}",p.Character);
                }
            }
            // A single observed native word, in a stable party/cell sample.
            // Only the nominated peer may acquire a durable entitlement.
            if (!p.LearnedWord.empty()&&p.WordPeer&&p.WordPeer!=event.pPlayer->GetId()&&
                CampaignIdentity::Known(campaignLink.Journal,campaignLink.Campaign,
                    {p.Campaign,p.Character,{p.Revision,p.Tag}}))
            {
                const GameId cell(p.WordCellMod,p.WordCellBase);
                auto* party=m_world.GetPartyService().GetPlayerParty(event.pPlayer);
                if (party&&cell&&event.pPlayer->GetCellComponent().Cell==cell)
                    for (auto* peer:party->Members)
                    {
                        if (!peer||peer==event.pPlayer||peer->GetId()!=p.WordPeer||
                            peer->GetCellComponent().Cell!=cell||
                            !PartyService::WithinSyncRange(m_world,*event.pPlayer,*peer)) continue;
                        const auto batch=QuestWordReward::DecodeBatch({p.LearnedWord.data(),p.LearnedWord.size()});
                        if (!batch) break;
                        const auto source=CampaignAccess::Character(*event.pPlayer);
                        const auto recipient=CampaignAccess::Character(*peer);
                        if (!source||!recipient||source==recipient) break;
                        const auto& mods=m_world.ctx().at<ModsComponent>();
                        auto grants=QuestWordReward::Read(campaignLink.Journal.Operations());
                        if (!grants) break;
                        for (const auto& word:*batch)
                        {
                            QuestWordReward::Grant grant{word,source,recipient};
                            if (!grant.Valid()||QuestWordReward::Entitled(*grants,word,source)) continue;
                            const auto key=TradeProperties::DecodeForm(word);
                            bool found=false;
                            for (const auto* list:{&mods.GetStandardMods(),&mods.GetLiteMods()})
                                for (const auto& [name,entry]:*list)
                                {
                                    std::string plugin(name.begin(),name.end());
                                    for (auto& ch:plugin) if (ch>='A'&&ch<='Z') ch=static_cast<char>(ch-'A'+'a');
                                    if (plugin==key->Plugin&&(list!=&mods.GetLiteMods()||key->Local<=0xFFF)) found=true;
                                }
                            if (!found) continue;
                            const auto head=campaignLink.Journal.Current();
                            if (campaignLink.Journal.Append(head,{"quest-word-"+std::to_string(head.Revision+1),
                                "quest-word-v1",grant.Encode()})!=CampaignJournal::Result::Committed)
                            {spdlog::warn("STM could not persist learned word");break;}
                            grants->push_back(grant);
                        }
                        break;
                    }
            }
            // A witnessed native award can be retried against the durable
            // death cohort even after its transient network actor disappears.
            if (!p.AbsorbedDragon.empty()&&
                CampaignIdentity::Known(campaignLink.Journal,campaignLink.Campaign,
                    {p.Campaign,p.Character,{p.Revision,p.Tag}}))
            {
                const auto entity=static_cast<entt::entity>(p.DragonEntity);
                const auto* actor=m_world.try_get<CharacterComponent>(entity);
                const auto* reference=m_world.try_get<FormIdComponent>(entity);
                const auto* cell=m_world.try_get<CellIdComponent>(entity);
                DragonAbsorption::Receipt receipt{std::string(p.AbsorbedDragon.data(),p.AbsorbedDragon.size()),
                    CampaignAccess::Character(*event.pPlayer)};
                const auto history=DragonAbsorption::ReadShared(campaignLink.Journal.Operations());
                if (receipt.Valid()&&history&&!history->Retired.contains(receipt.Dragon)&&
                    m_world.GetPartyService().GetPlayerParty(event.pPlayer))
                {
                    bool matches=history->Eligible(receipt);
                    if (!matches&&actor&&actor->IsDragon()&&actor->IsDead()&&reference&&reference->Id&&cell&&
                        cell->IsInRange(event.pPlayer->GetCellComponent(),true))
                    {
                        const auto key=TradeProperties::DecodeForm(receipt.Dragon);
                        const auto& mods=m_world.ctx().at<ModsComponent>();
                        for (const auto* list:{&mods.GetStandardMods(),&mods.GetLiteMods()})
                            for (const auto& [name,entry]:*list)
                            {
                                if (entry.id!=reference->Id.ModId||reference->Id.BaseId!=key->Local) continue;
                                std::string plugin(name.begin(),name.end());
                                for (auto& ch:plugin) if (ch>='A'&&ch<='Z') ch=static_cast<char>(ch-'A'+'a');
                                if (plugin==key->Plugin&&(list!=&mods.GetLiteMods()||key->Local<=0xFFF)) matches=true;
                            }
                    }
                    const auto receipts=DragonAbsorption::Read(campaignLink.Journal.Operations());
                    if (matches&&receipts&&!receipts->contains(receipt.Encode()))
                    {
                        const auto head=campaignLink.Journal.Current();
                        if (campaignLink.Journal.Append(head,{"dragon-absorbed-"+std::to_string(head.Revision+1),
                            "dragon-absorbed-v1",receipt.Encode()})!=CampaignJournal::Result::Committed)
                            spdlog::warn("STM native dragon receipt could not be committed");
                    }
                }
            }
            // A crash between the receipt and grant append must not strand
            // the other eligible player. Reconcile the existing journal on pull.
            if (const auto history=DragonAbsorption::ReadShared(campaignLink.Journal.Operations()))
                for (const auto& grant:DragonAbsorption::MissingGrants(*history,campaignLink.Journal.Operations()))
                {
                    const auto head=campaignLink.Journal.Current();
                    if (campaignLink.Journal.Append(head,{"dragon-shared-"+std::to_string(head.Revision+1),
                        "dragon-shared-v1",grant.Encode()})!=CampaignJournal::Result::Committed)
                    {spdlog::warn("STM shared dragon grant could not be committed");break;}
                }
            reply=CampaignReplication::Serve(campaignLink.Journal,campaignLink.Campaign,campaignLink.Profile,pull);
            if (!p.PickupClaim.empty()&&(reply.State==CampaignReplication::Status::Current||reply.State==CampaignReplication::Status::Record))
                pickupPermit=CampaignAccess::ReserveQuestPickup(m_world,*event.pPlayer,{p.PickupClaim.data(),p.PickupClaim.size()},p.Nonce);
        }
        NotifyCampaignRecord out;
        out.PickupPermit=pickupPermit;
        out.Campaign=reply.Campaign;out.Nonce=reply.Nonce;out.BaseRevision=reply.Base.Revision;out.BaseTag=reply.Base.Tag;
        out.TargetRevision=reply.Target.Revision;out.TargetTag=reply.Target.Tag;out.Total=reply.Total;
        out.State=static_cast<uint32_t>(reply.State);out.Row=reply.Row.c_str();
        event.pPlayer->Send(out);
    }
    catch (...) {spdlog::error("Campaign replication request failed");}
}
