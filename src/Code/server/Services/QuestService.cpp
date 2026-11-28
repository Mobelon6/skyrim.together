#include <GameServer.h>
#include <Components.h>

#include <World.h>
#include <Services/QuestService.h>
#include <Services/CampaignAccess.h>
#include <TradeProperties.h>
#include <QuestPickupPermit.h>
#include <UniqueQuestReward.h>
#include <KeywordQuestReward.h>
#include <ScriptedTravel.h>
#include <QuestPhysicalStage.h>
#include <QuestRunMember.h>
#include <sstream>
#include <Messages/RequestInventoryChanges.h>

#include <Messages/RequestQuestUpdate.h>
#include <Messages/RequestQuestJournal.h>
#include <Messages/RequestCampaignPull.h>
#include <Messages/NotifyQuestUpdate.h>

#include <Setting.h>
namespace
{
Console::Setting bEnableMiscQuestSync{"Gameplay:bEnableMiscQuestSync", "(Experimental) Syncs miscellaneous quests when possible", false};

using QuestRun = PartyService::Party::QuestRun;

bool ValidQuestTargets(std::string_view value)
{
    if (value.empty()) return true;
    if (value.size()>2048) return false;
    uint32_t previous{};
    bool first=true;
    while (!value.empty())
    {
        const auto end=value.find(';');
        if (end==std::string_view::npos) return false;
        const auto entry=value.substr(0,end);
        const auto colon=entry.find(':');
        uint32_t id{};
        if (colon==std::string_view::npos || !TradeProperties::Number(entry.substr(0,colon),id) ||
            (!first&&id<=previous) || colon+3>=entry.size() || entry[colon+2]!=':') return false;
        const char kind=entry[colon+1];
        const auto key=entry.substr(colon+3);
        if (kind!='r'&&kind!='l') return false;
        const auto form=TradeProperties::DecodeForm(key);
        if (!form||!form->Valid()||form->Encode()!=key) return false;
        first=false;previous=id;value.remove_prefix(end+1);
    }
    return !first;
}

bool AdvanceQuestRun(QuestRun& run, NotifyQuestUpdate& notify, bool completedRunOnce=false)
{
    if (run.CompletedRunOnce) return false;
    const std::string targets(notify.Targets.data(),notify.Targets.size());
    if (!ValidQuestTargets(targets)) return false;
    const bool newRun=run.Stopped&&notify.Status==NotifyQuestUpdate::Started;
    const bool hasRun=run.Started||!run.OrderedStages.empty()||run.Stopped;
    if (hasRun&&!newRun&&run.Targets!=targets) return false;
    if (completedRunOnce && (notify.Status!=NotifyQuestUpdate::Stopped || run.Generation!=1 ||
        (!run.Started && run.Stages.empty()))) return false;
    switch (notify.Status)
    {
    case NotifyQuestUpdate::Started:
        if (run.Started && !run.Stopped)
            return false;
        if (run.Stopped)
        {
            if (run.Generation == UINT32_MAX) return false;
            ++run.Generation;
        }
        run.Targets=targets;
        run.Started = true;
        run.Stopped = false;
        return true;
    case NotifyQuestUpdate::StageUpdate:
        if (run.Stopped || !run.Stages.insert(notify.Stage).second)
            return false;
        run.Targets=targets;
        run.OrderedStages.push_back(notify.Stage);
        return true;
    case NotifyQuestUpdate::Stopped:
        if (run.Stopped) return false;
        run.Stopped = true;
        run.Started = false;
        run.CompletedRunOnce = completedRunOnce;
        if (!completedRunOnce)
        {
            run.Stages.clear();
            run.OrderedStages.clear();
        }
        return true;
    default:
        return false;
    }
}

std::string QuestKey(World& world, const GameId& id)
{
    const auto& mods = world.ctx().at<ModsComponent>();
    for (const auto* list : {&mods.GetStandardMods(), &mods.GetLiteMods()})
        for (const auto& [name, entry] : *list)
            if (entry.id == id.ModId)
            {
                std::string plugin(name.begin(), name.end());
                for (auto& c : plugin)
                    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                const TradeProperties::FormKey key{plugin, id.BaseId};
                return key.Valid() ? key.Encode() : std::string{};
            }
    return {};
}

bool RestoreQuestRun(const CampaignJournal::Journal& journal, const std::string& key, QuestRun& run,
    std::string_view scope={})
{
    if (key.empty() || !journal.Ready()) return false;
    QuestRun restored;
    for (const auto& op : journal.Operations())
    {
        const bool completed=op.Kind=="quest-completed-v1";
        const bool binding=op.Kind=="quest-binding-v1";
        if (!completed && !binding && op.Kind != "quest-progress-v1") continue;
        std::istringstream row(op.Payload);
        std::string form, extra;
        if (!(row >> form)) return false;
        if (form != QuestRunMember::RecordKey(key,scope)) continue;
        uint32_t generation{}, status{}, stage{}, type{};
        uint64_t character{};
        if (!(row >> generation >> status >> stage >> type >> character) ||
            !character || status > NotifyQuestUpdate::Stopped || stage > UINT16_MAX || type > UINT8_MAX)
            return false;
        std::string targets;
        std::optional<uint32_t> instance;
        if (!QuestRunMember::ReadTail(row,targets,instance)||!ValidQuestTargets(targets)) return false;
        if (targets!=scope) continue;
        if (binding)
        {
            if (!instance||restored.Stopped||restored.Generation!=generation||
                (!restored.Started&&restored.OrderedStages.empty())||restored.Targets!=targets||
                !QuestRunMember::Remember(restored.Members,character,generation,*instance)) return false;
            continue;
        }
        NotifyQuestUpdate event{};
        event.Targets=targets.c_str();
        event.Status = static_cast<uint8_t>(status);
        event.Stage = static_cast<uint16_t>(stage);
        event.ClientQuestType = static_cast<uint8_t>(type);
        if (!AdvanceQuestRun(restored, event, completed) || restored.Generation != generation)
            return false;
        if (instance&&!QuestRunMember::Remember(restored.Members,character,generation,*instance)) return false;
    }
    run = std::move(restored);
    return true;
}

bool StoreQuestRun(CampaignJournal::Journal& journal, const std::string& key,
                   uint64_t character, const QuestRun& run, const NotifyQuestUpdate& event,
                   std::optional<uint32_t> instance,bool bindingOnly=false)
{
    if (!character || key.empty()) return false;
    const auto head = journal.Current();
    auto payload = QuestRunMember::RecordKey(key,run.Targets) + " " + std::to_string(run.Generation) + " " +
        std::to_string(event.Status) + " " + std::to_string(event.Stage) + " " +
        std::to_string(event.ClientQuestType) + " " + std::to_string(character);
    if (!run.Targets.empty()) payload+=" "+run.Targets;
    if (instance) payload+=" @"+std::to_string(*instance);
    return journal.Append(head, {"quest-progress-" + std::to_string(head.Revision + 1),
        bindingOnly?"quest-binding-v1":run.CompletedRunOnce?"quest-completed-v1":"quest-progress-v1", payload}) == CampaignJournal::Result::Committed;
}

bool CanRecordQuestPickup(const QuestRun& run,bool initialEvidence,int32_t count)
{
    if (run.Generation!=1||run.Stopped||run.CompletedRunOnce||count!=1) return false;
    return run.Started||!run.OrderedStages.empty()||initialEvidence;
}

enum class QuestItemDecision { Record, Duplicate, Reject, Copy, ConsumeCopy };

// The original pickup fixes the shared reference and its authoritative holder.
// A second native pickup is evidence of an engine-side replica conflict, not
// another shared item or a grant. Only an identified actual holder may spend it.
QuestItemDecision QuestItemChange(const std::vector<CampaignJournal::Operation>& ops,
    const std::string& prefix,const std::string& payload,bool consumed)
{
    const auto separator=payload.rfind(' ');
    if (separator==std::string::npos || separator<prefix.size()) return QuestItemDecision::Reject;
    const auto identity=payload.substr(0,separator+1);
    std::string original;
    bool copy=false,spent=false;
    for (const auto& op:ops)
    {
        if (!op.Payload.starts_with(prefix)) continue;
        if (op.Kind=="quest-pickup-v1")
        {
            if (!op.Payload.starts_with(identity) || (!original.empty() && original!=op.Payload))
                return QuestItemDecision::Reject;
            original=op.Payload;
        }
        else if (op.Kind=="quest-copy-v1")
        {
            if (original.empty() || !op.Payload.starts_with(identity) || op.Payload==original)
                return QuestItemDecision::Reject;
            if (op.Payload==payload) copy=true;
        }
        else if (op.Kind=="quest-consumed-v1")
        {
            if (original.empty() || op.Payload!=original) return QuestItemDecision::Reject;
            spent=true;
        }
        else if (op.Kind=="quest-consumed-v2")
        {
            if (original.empty() || !op.Payload.starts_with(identity) || op.Payload==original)
                return QuestItemDecision::Reject;
            const auto recorded=std::any_of(ops.begin(),ops.end(),[&](const auto& candidate) {
                return candidate.Kind=="quest-copy-v1" && candidate.Payload==op.Payload;
            });
            if (!recorded) return QuestItemDecision::Reject;
            spent=true;
        }
    }
    if (original.empty()) return consumed?QuestItemDecision::Reject:QuestItemDecision::Record;
    if (payload==original) return consumed&&!spent?QuestItemDecision::Record:QuestItemDecision::Duplicate;
    if (copy) return consumed&&!spent?QuestItemDecision::ConsumeCopy:QuestItemDecision::Duplicate;
    if (consumed) return QuestItemDecision::Reject;
    return QuestItemDecision::Copy;
}

// A player who has independently accepted this quest may have missed earlier
// shared stages (e.g. because the unique pickup happened before acceptance).
// Offer them through the existing native quest path; never replay another run
// or force a completed quest to start on the basis of old history alone.
void OfferKnownStages(Player& player, const PartyService::Party& party,
                      const QuestRun& run, const NotifyQuestUpdate& notice)
{
    if ((run.Stopped && !run.CompletedRunOnce) || run.OrderedStages.empty()) return;
    auto speaker = party.LeaderPlayerId;
    if (speaker == player.GetId())
        for (auto* member : party.Members)
            if (member != &player) { speaker = member->GetId(); break; }
    if (speaker == player.GetId()) return;
    NotifyQuestUpdate update{};
    update.Id = notice.Id;
    update.ClientQuestType = notice.ClientQuestType;
    update.Status = NotifyQuestUpdate::StageUpdate;
    update.Speaker = speaker;
    update.SharedRun = run.Generation;
    update.Targets=run.Targets.c_str();
    update.CheckInstance=notice.CheckInstance;
    update.ExpectedInstance=notice.ExpectedInstance;
    for (auto stage : run.OrderedStages)
    {
        update.Stage = stage;
        player.Send(update);
    }
}

}

static QuestPickupPermit::TurnInLeases sharedTurnIns;

bool CampaignAccess::ReserveQuestPickup(World& world,Player& player,std::string_view value,uint64_t nonce) noexcept
{
    try
    {
        auto& authority=Authority();
        const auto character=Character(player);
        if (!character||authority.State()!=CampaignReplication::Link::Mode::Ready||
            !world.GetPartyService().GetPlayerParty(&player)) return false;
        const bool turnIn=value.starts_with("turnin ");
        if (turnIn) value.remove_prefix(7);
        const auto claim=QuestPickupPermit::Decode(std::string(value));
        if (!claim) return false;
        // Validate every plugin key against the negotiated profile.
        const auto& mods=world.ctx().at<ModsComponent>();
        for (const auto* encoded:{&claim->Quest,&claim->Reference,&claim->Item})
        {
            const auto key=TradeProperties::DecodeForm(*encoded);
            bool found=false;
            for (const auto* list:{&mods.GetStandardMods(),&mods.GetLiteMods()})
                for (const auto& [name,entry]:*list)
                {
                    std::string plugin(name.begin(),name.end());
                    for (auto& ch:plugin) if (ch>='A'&&ch<='Z') ch=static_cast<char>(ch-'A'+'a');
                    if (plugin==key->Plugin&&(list!=&mods.GetLiteMods()||key->Local<=0xFFF)) found=true;
                }
            if (!found) return false;
        }
        QuestRun run;
        if (!RestoreQuestRun(authority.Journal,claim->Quest,run)||!CanRecordQuestPickup(run,true,1)) return false;
        if (turnIn)
        {
            if (!QuestPickupPermit::HasAvailableHolder(authority.Journal.Operations(),*claim,character)) return false;
            return sharedTurnIns.Acquire(*claim,character,player.GetId(),nonce,QuestPickupPermit::Leases::Clock::now());
        }
        if (QuestPickupPermit::AlreadyTaken(authority.Journal.Operations(),*claim)) return false;
        static QuestPickupPermit::Leases leases;
        return leases.Acquire(*claim,character,QuestPickupPermit::Leases::Clock::now());
    }
    catch (...) {return false;}
}

void ApplyUniqueRewardCommand(World& world,Player& player,std::string_view value)
{
    const auto command=UniqueQuestReward::Decode(value);
    if (!command) return;
    auto& authority=CampaignAccess::Authority();
    const auto character=CampaignAccess::Character(player);
    auto* party=world.GetPartyService().GetPlayerParty(&player);
    if (!character||!party||authority.State()!=CampaignReplication::Link::Mode::Ready) return;
    const auto* entry=UniqueQuestReward::Find(command->Item);
    const auto state=UniqueQuestReward::Read(authority.Journal.Operations(),entry->Quest);
    if (!state) return;
    uint64_t recipient{};
    if (command->Choice)
    {
        if (state->Item!=entry->Item||state->Source!=character||state->Recipient) return;
        for (auto* member:party->Members)
            if (member&&member->GetId()==command->Peer) recipient=CampaignAccess::Character(*member);
        if (!recipient) return;
    }
    else if (state->Item||UniqueQuestReward::StageSource(authority.Journal.Operations(),*entry)!=character) return;
    const auto head=authority.Journal.Current();
    auto payload=std::to_string(entry->Item)+" "+std::to_string(character);
    if (command->Choice) payload+=" "+std::to_string(recipient);
    if (authority.Journal.Append(head,{"quest-unique-"+std::to_string(head.Revision+1),
        command->Choice?"quest-unique-choice-v1":"quest-unique-offer-v1",payload})!=CampaignJournal::Result::Committed)
        spdlog::error("STM unique quest reward choice could not be committed");
}

void ApplyScriptedTravelCommand(World& world,Player& player,std::string_view value)
{
    const bool arrival=value.starts_with("script-sleep-arrived ");
    if (!arrival&&!value.starts_with("script-sleep-move ")) return;
    auto move=ScriptedTravel::Decode(value.substr(arrival?21:18));
    if (!move) return;
    auto& authority=CampaignAccess::Authority();
    const auto who=CampaignAccess::Character(player);
    auto* party=world.GetPartyService().GetPlayerParty(&player);
    if (!who||!party||authority.State()!=CampaignReplication::Link::Mode::Ready) return;
    // Validate the stable keys against the negotiated mod profile.
    const auto& mods=world.ctx().at<ModsComponent>();
    for (const auto* encoded:{&move->Quest,&move->Target})
    {
        const auto key=TradeProperties::DecodeForm(*encoded);
        bool found=false;
        for (const auto* list:{&mods.GetStandardMods(),&mods.GetLiteMods()})
            for (const auto& [name,entry]:*list)
            {
                std::string plugin(name.begin(),name.end());
                for (auto& c:plugin) if (c>='A'&&c<='Z') c=static_cast<char>(c-'A'+'a');
                if (plugin==key->Plugin&&(list!=&mods.GetLiteMods()||key->Local<=0xFFF)) found=true;
            }
        if (!found) return;
    }
    const auto records=ScriptedTravel::Read(authority.Journal.Operations());
    if (!records) return;
    const auto* existing=ScriptedTravel::Find(*records,*move);
    uint64_t source=who,recipient{};
    if (arrival)
    {
        if (!ScriptedTravel::MayAck(existing,who,*move)) return;
        source=existing->Source;recipient=who;
    }
    else
    {
        for (auto* member:party->Members)
            if (member&&member!=&player)
            {
                const auto other=CampaignAccess::Character(*member);
                if (!other||recipient) return;
                recipient=other;
            }
        if (!ScriptedTravel::MayPublish(party->LeaderPlayerId==player.GetId(),who,recipient,existing,
            ScriptedTravel::CurrentStage(authority.Journal.Operations(),*move))) return;
    }
    const auto head=authority.Journal.Current();
    if (authority.Journal.Append(head,{"script-sleep-"+std::to_string(head.Revision+1),
        arrival?"quest-sleep-arrived-v1":"quest-sleep-move-v1",
        ScriptedTravel::Payload(*move,source,recipient)})!=CampaignJournal::Result::Committed)
        spdlog::error("STM scripted travel record could not be committed");
}

void ApplyKeywordRewardCommand(World& world,Player& player,std::string_view value)
{
    namespace KR=KeywordQuestReward;
    auto cmd=KR::Decode(value);
    if (!cmd) return;
    auto& authority=CampaignAccess::Authority();
    const auto who=CampaignAccess::Character(player);
    auto* party=world.GetPartyService().GetPlayerParty(&player);
    if (!who||!party||authority.State()!=CampaignReplication::Link::Mode::Ready) return;
    std::map<uint32_t,uint64_t> peers{{player.GetId(),who}};
    for (auto* member:party->Members)
        if (member)
            if (auto character=CampaignAccess::Character(*member)) peers[member->GetId()]=character;
    if (peers.size()!=2) return;
    std::set<uint64_t> active;
    for (const auto& [id,character]:peers) active.insert(character);
    auto state=KR::Read(authority.Journal.Operations(),cmd->Reward);
    if (!state||active.size()!=2||!active.contains(state->Source)) return;
    for (auto ready:state->Ready) if (!active.contains(ready)) return;
    uint64_t recipient{};
    if (cmd->Type==KR::Action::Choice)
    {
        const auto target=peers.find(cmd->Peer);
        if (target==peers.end()) return;
        recipient=target->second;
    }
    const auto head=authority.Journal.Current();
    if (!KR::Advance(*state,cmd->Type,who,recipient,head.Revision+1)) return;
    auto payload=cmd->Reward.Encode()+" "+std::to_string(who);
    if (recipient) payload+=" "+std::to_string(recipient);
    if (authority.Journal.Append(head,{"quest-keyword-"+std::to_string(head.Revision+1),KR::Kind(cmd->Type),payload})!=CampaignJournal::Result::Committed)
        spdlog::error("STM keyword reward could not be committed");
}

void CampaignAccess::ReplayQuestReceipt(World& world,Player& player,std::string_view value) noexcept
{
    try
    {
        if (value.starts_with("script-sleep-"))
        {ApplyScriptedTravelCommand(world,player,value);return;}
        if (value.starts_with("reward-keyword-"))
        {ApplyKeywordRewardCommand(world,player,value);return;}
        if (value.starts_with("reward-offer ")||value.starts_with("reward-choice "))
        {ApplyUniqueRewardCommand(world,player,value);return;}
        const auto receipt=QuestPickupPermit::DecodeReceipt(std::string(value));
        if (!receipt) return;
        // Old virtual turn-in commands remain decodable for compatibility,
        // but cannot change the campaign under the physical-holder rule.
        if (receipt->Shared) return;
        const auto& mods=world.ctx().at<ModsComponent>();
        const auto resolve=[&](const std::string& encoded)->GameId {
            const auto key=TradeProperties::DecodeForm(encoded);
            if (!key) return {};
            for (const auto* list:{&mods.GetStandardMods(),&mods.GetLiteMods()})
                for (const auto& [name,entry]:*list)
                {
                    std::string plugin(name.begin(),name.end());
                    for (auto& ch:plugin) if (ch>='A'&&ch<='Z') ch=static_cast<char>(ch-'A'+'a');
                    if (plugin==key->Plugin&&(list!=&mods.GetLiteMods()||key->Local<=0xFFF))
                        return GameId(entry.id,key->Local);
                }
            return {};
        };
        RequestInventoryChanges message{};
        message.QuestId=resolve(receipt->Object.Quest);
        message.QuestRef=resolve(receipt->Object.Reference);
        message.Item.BaseId=resolve(receipt->Object.Item);
        if (!message.QuestId||!message.QuestRef||!message.Item.BaseId) return;
        message.QuestAlias=receipt->Object.Alias;
        message.Item.Count=receipt->Count;
        message.Item.IsQuestItem=true;
        message.QuestInitialEvidence=receipt->Initial;
        RecordQuestItemChange(world,player,message);
    }
    catch (...) {spdlog::warn("STM quest receipt replay failed");}
}

bool CampaignAccess::RecordQuestItemChange(World& world,Player& player,const RequestInventoryChanges& message) noexcept
{
    try
    {
        auto& authority=Authority();
        if (authority.State()!=CampaignReplication::Link::Mode::Ready||
            !world.GetPartyService().GetPlayerParty(&player)) return false;
        const auto character=Character(player);
        if (!character||!message.QuestId||!message.QuestRef||!message.Item.IsQuestItem||
            (message.Item.Count<=0&&message.Item.Count!=-1)||message.Drop) return false;
        const auto key=QuestKey(world,message.QuestId);
        const auto reference=QuestKey(world,message.QuestRef);
        const auto item=QuestKey(world,message.Item.BaseId);
        if (key.empty()||reference.empty()||item.empty()) return false;
        QuestRun run;
        if (!RestoreQuestRun(authority.Journal,key,run)||run.Generation!=1) return false;
        const bool consumed=message.Item.Count<0;
        if (!consumed&&message.Item.Count!=1) return false;
        const auto prefix=key+" "+std::to_string(run.Generation)+" "+std::to_string(message.QuestAlias)+" ";
        const auto payload=prefix+reference+" "+item+" "+
            std::to_string(consumed?1:message.Item.Count)+" "+std::to_string(character);
        const auto decision=QuestItemChange(authority.Journal.Operations(),prefix,payload,consumed);
        if (decision==QuestItemDecision::Duplicate) return true;
        if (decision==QuestItemDecision::Record && !consumed &&
            (!CanRecordQuestPickup(run,message.QuestInitialEvidence,message.Item.Count)||
             QuestPickupPermit::AlreadyTaken(authority.Journal.Operations(),
                 {key,reference,item,message.QuestAlias}))) return false;
        // A later pickup is never a new shared item. Record only an actual
        // displayed RunOnce quest pickup, not an ambiguous background alias.
        if (decision==QuestItemDecision::Copy && !message.QuestInitialEvidence) return false;
        if (decision==QuestItemDecision::Reject)
        {
            spdlog::warn("Unmatched quest item change for {:X}, alias {}",message.QuestId.LogFormat(),message.QuestAlias);
            return false;
        }
        const auto head=authority.Journal.Current();
        const char* kind=decision==QuestItemDecision::Copy?"quest-copy-v1":
            decision==QuestItemDecision::ConsumeCopy?"quest-consumed-v2":
            consumed?"quest-consumed-v1":"quest-pickup-v1";
        if (authority.Journal.Append(head,{std::string("quest-item-")+std::to_string(head.Revision+1),
            kind,payload})!=CampaignJournal::Result::Committed)
        {
            spdlog::error("STM could not persist quest item change");
            return false;
        }
        return true;
    }
    catch (...) {spdlog::error("STM quest item change failed");return false;}
}

QuestService::QuestService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_campaignRequests = aDispatcher.sink<PacketEvent<RequestCampaignPull>>().connect<&QuestService::OnCampaignPull>(this);
    m_journalRequests = aDispatcher.sink<PacketEvent<RequestQuestJournal>>().connect<&QuestService::OnJournalRequest>(this);
    m_questUpdateConnection = aDispatcher.sink<PacketEvent<RequestQuestUpdate>>().connect<&QuestService::OnQuestChanges>(this);
}

// The marker is committed BEFORE the ordinary progress record. If the
// second append fails, retry can finish it without ever publishing an
// unclassified stage. A second member records their own native attainment.
bool RecordPhysicalStage(CampaignJournal::Journal& journal,const std::string& key,uint32_t run,
    uint16_t stage,uint64_t who)
{
    const auto markers=QuestPhysicalStage::Read(journal.Operations());
    if(!markers||key.empty()||!run||!who) return false;
    if(QuestPhysicalStage::Reached(*markers,key,run,stage,who)) return true;
    const auto head=journal.Current();
    return journal.Append(head,{"quest-physical-"+std::to_string(head.Revision+1),
        "quest-physical-stage-v1",QuestPhysicalStage::Payload(key,run,stage,who)})==CampaignJournal::Result::Committed;
}
bool HasPhysicalStages(const CampaignJournal::Journal& journal,const std::string& key,uint32_t run)
{
    const auto markers=QuestPhysicalStage::Read(journal.Operations());
    return !markers||QuestPhysicalStage::Any(*markers,key,run);
}

void QuestService::OnQuestChanges(const PacketEvent<RequestQuestUpdate>& acMessage) noexcept
{
    const auto& message = acMessage.Packet;

    // Do not admit a city's paid HousePurchase stage into the ordinary
    // campaign quest journal, including messages from an older client.
    // The local number alone is not enough: another plugin can reuse it.
    if (message.Id.BaseId==0xA7B33)
    {
        try
        {
            if (CampaignAccess::Authority().State()!=CampaignReplication::Link::Mode::Disabled &&
                QuestKey(m_world,message.Id)==TradeProperties::FormKey{"skyrim.esm",0xA7B33}.Encode())
                return;
        }
        catch (...) {return;}
    }

    auto* pPlayer = acMessage.pPlayer;

    auto& questComponent = pPlayer->GetQuestLogComponent();
    auto& entries = questComponent.QuestContent.Entries;

    auto questIt = std::find_if(entries.begin(), entries.end(), [&message](const auto& e) { return e.Id == message.Id; });

    NotifyQuestUpdate notify{};
    notify.Id = message.Id;
    notify.Stage = message.Stage;
    notify.Status = message.Status;
    notify.ClientQuestType = message.ClientQuestType;

    PartyService::Party* pParty = m_world.GetPartyService().GetPlayerParty(pPlayer);
    const PartyService::Party::Conversation* participants = nullptr;
    const std::string targets(message.Targets.data(),message.Targets.size());
    if (!ValidQuestTargets(targets)|| (message.PhysicalStage&&message.Status!=RequestQuestUpdate::StageUpdate)) return;
    notify.Targets=message.Targets;
    // A replay-created native instance can attach only to the generation it
    // was started for. A stale attachment must never create a new run.
    if (message.ExpectedRun&&message.Status==RequestQuestUpdate::Started)
    {
        try
        {
            auto& authority=CampaignAccess::Authority();
            const auto character=CampaignAccess::Character(*pPlayer);
            if (!pParty||!message.BindInstance||message.CompletedRunOnce||!character||
                authority.State()!=CampaignReplication::Link::Mode::Ready) return;
            const auto key=QuestKey(m_world,message.Id);
            QuestRun run;
            if (!RestoreQuestRun(authority.Journal,key,run,targets)||run.Targets!=targets||
                !QuestRunMember::CanAttach(run.Members,character,run.Generation,message.ExpectedRun,
                    message.Instance,run.Started,run.Stopped)||run.CompletedRunOnce) return;
            if (QuestRunMember::Bound(run.Members,character,run.Generation,message.Instance)) return;
            if (!QuestRunMember::Remember(run.Members,character,run.Generation,message.Instance)) return;
            notify.Status=NotifyQuestUpdate::Started;
            if (!StoreQuestRun(authority.Journal,key,character,run,notify,message.Instance,true))
                spdlog::warn("STM could not attach native repeat quest instance");
        }
        catch (...) {spdlog::warn("STM repeat quest attachment failed");}
        return;
    }
    bool knownSharedQuest = false, knownRepeatStart=false;
    if (pParty && (message.JournalObjective || message.CompletedRunOnce ||
        message.Status == RequestQuestUpdate::Stopped ||
        (message.BindInstance&&message.Status==RequestQuestUpdate::Started)))
    {
        try
        {
            auto& authority = CampaignAccess::Authority();
            using Mode = CampaignReplication::Link::Mode;
            QuestRun run;
            bool known = false;
            if (authority.State() == Mode::Ready && CampaignAccess::Character(*pPlayer))
                known = RestoreQuestRun(authority.Journal, QuestKey(m_world, message.Id), run,targets);
            else if (authority.State() == Mode::Disabled)
            {
                if (targets.empty())
                {
                    const auto found=pParty->QuestRuns.find(message.Id);
                    if (found!=pParty->QuestRuns.end()) {run=found.value();known=true;}
                }
                else
                {
                    const auto key=QuestRunMember::ScopeKey(QuestKey(m_world,message.Id),targets);
                    const auto found=pParty->TargetQuestRuns.find(key);
                    if (found!=pParty->TargetQuestRuns.end()) {run=found->second;known=true;}
                }
            }
            if (known && (run.Started||!run.OrderedStages.empty()||run.Stopped) &&
                !(run.Stopped&&message.Status==RequestQuestUpdate::Started)&&run.Targets!=targets) return;
            if (known && run.CompletedRunOnce)
            {
                // Late acceptance may happen while the rest of the party is
                // elsewhere. Offer the recorded stages, never open a new run
                // or force Stop on a client that has not finished the fragments.
                const auto key=QuestKey(m_world,message.Id);
                const auto physical=QuestPhysicalStage::Read(authority.Journal.Operations());
                if(physical&&message.Status==RequestQuestUpdate::StageUpdate&&run.Stages.contains(message.Stage)&&
                    QuestPhysicalStage::Has(*physical,key,run.Generation,message.Stage))
                    RecordPhysicalStage(authority.Journal,key,run.Generation,message.Stage,CampaignAccess::Character(*pPlayer));
                if(!HasPhysicalStages(authority.Journal,key,run.Generation)&&
                    (message.Status == RequestQuestUpdate::Started ||
                    (message.Status == RequestQuestUpdate::StageUpdate && run.Stages.contains(message.Stage))))
                    OfferKnownStages(*pPlayer, *pParty, run, notify);
                return;
            }
            knownSharedQuest = known && !run.Stopped && (run.Started || !run.OrderedStages.empty());
            knownRepeatStart=known&&message.BindInstance&&message.Status==RequestQuestUpdate::Started&&
                (run.Started||run.Stopped||!run.OrderedStages.empty());
            if (knownSharedQuest && authority.State()==Mode::Disabled && run.Generation==1 &&
                (message.Status == RequestQuestUpdate::Started || run.Stages.contains(message.Stage)))
                OfferKnownStages(*pPlayer, *pParty, run, notify);
        }
        catch (const std::exception& error)
        {
            spdlog::error("STM quest catch-up: {}", error.what());
        }
    }
    if (!pParty) return;
    const auto now = std::chrono::steady_clock::now();
    const auto talk = pParty->Talks.find(pPlayer->GetId());
    const auto fight = pParty->Brawls.find(pPlayer->GetId());
    const bool inTalk = talk != pParty->Talks.end() && talk->second.Expires > now;
    const bool inFight = fight != pParty->Brawls.end() && fight->second.Expires > now;
    if (inTalk && talk->second.SharesQuest(message.Id, now) && talk->second.Anchor.TargetForm)
    {
        participants = &talk->second;
        notify.Subject = participants->Anchor.TargetForm;
        notify.Encounter = participants->Encounter;
    }
    else if (notify.ClientQuestType == 0 || notify.ClientQuestType == 6)
    {
        // An enabled objective in the plugin says nothing about what caused this event.
        // During an interaction, only the quest of the witnessed topic has provenance.
        if (inTalk || inFight) return;
        if (!bEnableMiscQuestSync &&
            !(message.JournalObjective && (message.Status == RequestQuestUpdate::StageUpdate ||
                message.Status == RequestQuestUpdate::Started)) && !knownRepeatStart &&
            !(knownSharedQuest && message.Status == RequestQuestUpdate::Stopped)) return;
    }

    if (message.Status == RequestQuestUpdate::Started || message.Status == RequestQuestUpdate::StageUpdate)
    {
        // in order to prevent bugs when a quest is in progress
        // and being updated we add it as a new quest record to
        // maintain a proper remote questlog state.
        if (questIt == entries.end())
        {
            auto& newQuest = entries.emplace_back();
            newQuest.Id = message.Id;
            newQuest.Stage = message.Stage;

            if (message.Status == RequestQuestUpdate::Started)
            {
                spdlog::debug("Started quest: {:X} stage: {}", message.Id.LogFormat(), message.Stage);

                notify.Status = NotifyQuestUpdate::Started;
            }
            else
            {
                notify.Status = NotifyQuestUpdate::StageUpdate;
            }
        }
        else
        {
            spdlog::debug("Updated quest: {:X}, stage: {}", message.Id.LogFormat(), message.Stage);

            auto& record = *questIt;
            record.Id = message.Id;
            record.Stage = message.Stage;

            notify.Status = NotifyQuestUpdate::StageUpdate;
        }
    }
    else if (message.Status == RequestQuestUpdate::Stopped)
    {
        spdlog::debug("Stopped quest: {:X}, stage: {}", message.Id.LogFormat(), message.Stage);

        if (questIt != entries.end())
            entries.erase(questIt);
        else
            spdlog::warn("Unable to delete quest object {:X}", message.Id.LogFormat());

        notify.Status = NotifyQuestUpdate::Stopped;
    }

    if (!pParty)
        return;

    try
    {
        const auto formKey=QuestKey(m_world,message.Id);
        if (!targets.empty()&&formKey.empty()) return;
        auto& run=targets.empty()?pParty->QuestRuns[message.Id]:
            pParty->TargetQuestRuns[QuestRunMember::ScopeKey(formKey,targets)];
        auto& authority = CampaignAccess::Authority();
        using Mode = CampaignReplication::Link::Mode;
        const bool durable = authority.State() == Mode::Ready;
        const auto character = durable ? CampaignAccess::Character(*pPlayer) : uint64_t{};
        std::string key;
        if (durable)
        {
            key = QuestKey(m_world, message.Id);
            if (!character || !RestoreQuestRun(authority.Journal, key, run,targets)) return;
        }
        else if (authority.State() != Mode::Disabled)
            return;

        const bool start=message.Status==RequestQuestUpdate::Started;
        const bool stage=message.Status==RequestQuestUpdate::StageUpdate;
        const bool bind=durable&&message.BindInstance;
        if (durable&&!bind&&(!run.Members.empty()||run.Generation>1)) return;
        if (message.ExpectedRun&&(!bind||run.Stopped||run.Generation!=message.ExpectedRun||
            !QuestRunMember::Bound(run.Members,character,run.Generation,message.Instance))) return;
        if (bind&&!QuestRunMember::Admit(run.Members,character,run.Generation,message.Instance,
            run.Stopped,start,stage,!targets.empty()&&run.Targets!=targets,!targets.empty())) return;
        // Do not mutate the accepted run until its authoritative row is stored.
        auto next = run;
        notify.Status = message.Status;
        const bool completed=durable && !bind && message.CompletedRunOnce && message.Status==RequestQuestUpdate::Stopped &&
            next.Generation==1 && (next.Started||!next.Stages.empty());
        if (!AdvanceQuestRun(next, notify, completed))
        {
            if (durable&&!run.Stopped&&run.Targets==targets&&
                ((start&&run.Started)||(stage&&run.Stages.contains(message.Stage))))
            {
                if (bind&&!QuestRunMember::Bound(run.Members,character,run.Generation,message.Instance))
                {
                    auto attached=run;
                    if (!QuestRunMember::Remember(attached.Members,character,run.Generation,message.Instance)||
                        !StoreQuestRun(authority.Journal,key,character,attached,notify,message.Instance,true)) return;
                    run=std::move(attached);
                }
                if(stage)
                {
                    const auto physical=QuestPhysicalStage::Read(authority.Journal.Operations());
                    if(!physical) return;
                    if(targets.empty()&&(message.PhysicalStage||QuestPhysicalStage::Has(*physical,key,run.Generation,message.Stage))&&
                        !RecordPhysicalStage(authority.Journal,key,run.Generation,message.Stage,character)) return;
                }
                notify.CheckInstance=bind;notify.ExpectedInstance=bind?message.Instance:0;
                if(!HasPhysicalStages(authority.Journal,key,run.Generation)) OfferKnownStages(*pPlayer,*pParty,run,notify);
            }
            return;
        }
        if (bind&&!QuestRunMember::Remember(next.Members,character,next.Generation,message.Instance)) return;
        const auto instance=bind?std::optional<uint32_t>(message.Instance):std::nullopt;
        if(durable&&stage&&message.PhysicalStage&&targets.empty()&&
            !RecordPhysicalStage(authority.Journal,key,next.Generation,message.Stage,character)) return;
        if (durable && !StoreQuestRun(authority.Journal, key, character, next, notify,instance))
        {
            spdlog::error("STM could not persist quest progress {:X}", message.Id.LogFormat());
            return;
        }
        run = std::move(next);
        // Do not force-stop a listener while its native completion fragments are pending.
        // Confirmed history is replayed through the existing quest consent handler.
        if (completed) return;
        // A run containing a physical barrier is delivered via the existing
        // durable pull/catch-up path, which checks the local ESS before
        // applying any later stage or Stop. Never bypass it with live offers.
        if(durable&&HasPhysicalStages(authority.Journal,key,run.Generation)) return;
        if (!durable)
            RecordQuest(pPlayer, notify);
        notify.Speaker = pPlayer->GetId();
        if (durable || notify.Speaker == pParty->LeaderPlayerId)
            notify.SharedRun = run.Generation;
        for (auto* guest : pParty->Members)
            if (guest != pPlayer &&
                (durable || (PartyService::WithinSyncRange(m_world, *pPlayer, *guest) &&
                    (!participants || participants->ChoiceListeners.count(guest->GetId())))))
            {
                auto outgoing=notify;
                if (bind)
                {
                    const auto instance=QuestRunMember::Instance(run.Members,CampaignAccess::Character(*guest),run.Generation);
                    if (instance)
                    {
                        outgoing.CheckInstance=true;
                        outgoing.ExpectedInstance=*instance;
                    }
                    else continue; // wait for the journal-confirmed native instance, even in generation one
                }
                guest->Send(outgoing);
            }
    }
    catch (const std::exception& error)
    {
        spdlog::error("STM quest progression: {}", error.what());
    }
}
