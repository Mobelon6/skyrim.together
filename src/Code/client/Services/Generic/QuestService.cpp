#include <TiltedOnlinePCH.h>

#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/PartyLeftEvent.h>
#include <Events/UpdateEvent.h>

#include <Services/QuestService.h>
#include <Services/QuestRewardClient.h>
#include <Services/QuestPhysicalStageClient.h>
#include <Services/ScriptedTravelClient.h>
#include <Services/CampaignAccess.h>
#include <deque>
#include <tuple>
#include <Services/ImguiService.h>
#include <Services/OverlayService.h>
#include <Services/TransportService.h>

#include <OverlayApp.hpp>

#include <PlayerCharacter.h>
#include <Games/ActorExtension.h>
#include <Forms/TESQuest.h>
#include <Games/TES.h>
#include <Games/Overrides.h>

#include <Events/EventDispatcher.h>

#include <Messages/RequestQuestUpdate.h>
#include <Messages/NotifyQuestUpdate.h>
#include <Messages/NotifyQuestJournal.h>
#include <Messages/NotifyCampaignRecord.h>
#include <Services/PapyrusService.h>
#include <InventoryEvidence.h>
#include <TradeProperties.h>
#include <map>

static TESQuest* FindQuestByNameId(const String& name)
{
    auto& questRegistry = ModManager::Get()->quests;
    auto it = std::find_if(questRegistry.begin(), questRegistry.end(), [name](auto* it) { return std::strcmp(it->idName.AsAscii(), name.c_str()); });

    return it != questRegistry.end() ? *it : nullptr;
}

using HeldStages = QuestService::HeldStages;

static void ReleaseStagesForWaitingPhases(TESQuest* apQuest, HeldStages& aHeld) noexcept;
static void ReleaseUncheckedStages(TESQuest* apQuest, HeldStages& aHeld) noexcept;

static bool InPlayerJournal(uint32_t form)
{
    auto* player = PlayerCharacter::Get();
    if (player)
        for (const auto& entry : player->objectives)
            if (entry.instance && entry.instance->quest && entry.instance->quest->formID == form) return true;
    return false;
}

// BGSBaseAlias: CommonLibSSE-NG, QType virtual slot 3 and native SE layout.
// Only read engine-owned alias definitions; never allocate or replace an alias.
struct NativeQuestAlias
{
    virtual ~NativeQuestAlias() = default;
    virtual bool Load(void*) = 0;
    virtual void InitItem(TESForm*) = 0;
    virtual const BSFixedString& QType() const = 0;
    BSFixedString Name;
    TESQuest* Owner;
    uint32_t Id, Flags;
    uint16_t FillType, Pad22;
    uint32_t Pad24;
};
static_assert(offsetof(NativeQuestAlias, Id)==0x18);
static_assert(sizeof(NativeQuestAlias)==0x28);

std::string QuestService::CaptureQuestTargets(TESQuest* quest)
{
    if (!quest || (quest->flags&TESQuest::RunOnce)) return {};
    if (quest->aliases.length>quest->aliases.capacity || quest->aliases.length>128 ||
        (quest->aliases.length&&!quest->aliases.data)) return "!";
    std::map<uint32_t,std::string> targets;
    std::vector<InventoryEvidence::PluginSlot> slots;
    if (auto* mods=ModManager::Get())
        for (const auto* mod:mods->mods)
        {
            if (!mod||!mod->IsLoaded()) continue;
            const auto* end=std::find(mod->filename,mod->filename+sizeof(mod->filename),'\0');
            if (end!=mod->filename+sizeof(mod->filename))
                slots.push_back({mod->GetId(),mod->IsLite(),std::string(mod->filename,end)});
        }
    for (auto* raw:quest->aliases)
    {
        auto* alias=static_cast<NativeQuestAlias*>(raw);
        if (!alias||alias->Owner!=quest) return "!";
        const auto* type=alias->QType().AsAscii();
        if (!type) return "!";
        TESForm* form=nullptr;
        char kind{};
        if (std::string_view(type)=="Ref")
        {
            if (alias->FillType==1) continue; // fixed definition in the agreed profile
            kind='r';
            if (!quest->IsStopped()) form=quest->GetAliasedRef(alias->Id);
        }
        else if (std::string_view(type)=="Loc")
        {
            kind='l';
            if (!quest->IsStopped())
            {
                const auto* method=World::Get().ctx().at<PapyrusService>().Get("LocationAlias","GetLocation");
                if (!method) return "!";
                form=PapyrusFunction<TESForm*,NativeQuestAlias>(method)(alias);
            }
        }
        else return "!";
        if (quest->IsStopped()) return "!";
        // Compare proven targets, not optional aliases that may be filled
        // or cleared by the stage fragment itself.
        if (!form)
        {
            if (!(alias->Flags&2u)) return "!";
            continue;
        }
        std::string key;
        if (kind=='r'&&form->formID==0x14) continue; // personal player alias
        const auto stable=InventoryEvidence::Resolve(form->formID,slots);
        if (!stable) return "!";
        const TradeProperties::FormKey formKey{stable->Plugin,stable->Local};
        if (!formKey.Valid()) return "!";
        key=formKey.Encode();
        if (!targets.emplace(alias->Id,std::string(1,kind)+":"+key).second) return "!";
    }
    std::string result;
    for (const auto& [id,value]:targets)
    {
        result+=std::to_string(id)+":"+value+";";
        if (result.size()>2048) return "!";
    }
    return result;
}

uint32_t ExpectedRepeatRun(uint32_t form) noexcept;

bool QuestService::FillQuestTargets(uint32_t form,uint32_t instance,RequestQuestUpdate& update) noexcept
{
    try
    {
        auto* quest=Cast<TESQuest>(TESForm::GetById(form));
        if (!quest||quest->currentInstanceID!=instance) return false;
        // Publish observed results, not a failed/obsolete native stage request.
        if (update.Status==RequestQuestUpdate::StageUpdate&&!quest->IsStageReached(update.Stage)) return false;
        std::string targets;
        if (update.Status==RequestQuestUpdate::Stopped)
        {
            const auto previous=m_lastQuestTargets.find(form);
            if (previous!=m_lastQuestTargets.end()&&previous->second.first==instance)
                targets=previous->second.second;
            else targets=CaptureQuestTargets(quest);
        }
        else targets=CaptureQuestTargets(quest);
        if (targets=="!")
        {
            spdlog::debug("STM quest {:X}: unresolved target, keeping progression local",form);
            return false;
        }
        if (update.Status!=RequestQuestUpdate::Stopped)
        {
            const auto prior=m_lastQuestTargets.find(form);
            if (prior!=m_lastQuestTargets.end()&&prior->second.first==instance&&prior->second.second!=targets)
                return false; // a changed/cleared alias is not a new native start
            m_lastQuestTargets[form]={instance,targets};
        }
        update.Targets=targets.c_str();
        update.BindInstance=!(quest->flags&TESQuest::RunOnce);
        update.Instance=update.BindInstance?instance:0;
        update.ExpectedRun=update.BindInstance&&targets.empty()?ExpectedRepeatRun(form):0;
        return true;
    }
    catch (...) {return false;}
}

static bool MatchesQuestTargets(TESQuest* quest,const NotifyQuestUpdate& update)
{
    if (!quest) return false;
    if (update.CheckInstance&&(quest->IsStopped()||quest->currentInstanceID!=update.ExpectedInstance)) return false;
    if (quest->IsStopped()) return update.Targets.empty();
    if (quest->currentInstanceID!=update.LocalInstance) return false;
    if (quest->flags&TESQuest::RunOnce) return update.Targets.empty();
    try {return QuestService::CaptureQuestTargets(quest)==std::string(update.Targets.data(),update.Targets.size());}
    catch (...) {return false;}
}

void QuestService::ReceiveQuestProgress(const NotifyQuestUpdate& notice) noexcept
{
    if (!m_world.GetPartyService().IsInParty()) return;
    auto update=notice;
    auto form = m_world.GetModSystem().GetGameId(update.Id);
    auto* quest = Cast<TESQuest>(TESForm::GetById(form));
    if (!quest || IsNonSyncableQuest(quest)) return;
    update.LocalInstance=quest->currentInstanceID;
    if (!MatchesQuestTargets(quest,update)) return;
    // The accepted party quest is already shared. Use STR's existing native
    // start/stage/stop handler, including its scene and fragment ordering.
    // Observer/follower consent remains independent of quest progression.
    OnQuestUpdate(update);
}

QuestService::QuestService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_campaignClock = aDispatcher.sink<UpdateEvent>().connect<&QuestService::PollCampaign>(this);
    m_campaignRecords = aDispatcher.sink<NotifyCampaignRecord>().connect<&QuestService::OnCampaignRecord>(this);
    m_journalClock = aDispatcher.sink<UpdateEvent>().connect<&QuestService::PollJournal>(this);
    m_journalRecords = aDispatcher.sink<NotifyQuestJournal>().connect<&QuestService::OnJournalRecord>(this);
    m_joinedConnection = aDispatcher.sink<ConnectedEvent>().connect<&QuestService::OnConnected>(this);
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&QuestService::OnUpdate>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&QuestService::OnDisconnected>(this);
    m_leftConnection = aDispatcher.sink<PartyLeftEvent>().connect<&QuestService::OnPartyLeft>(this);
    m_questUpdateConnection = aDispatcher.sink<NotifyQuestUpdate>().connect<&QuestService::ReceiveQuestProgress>(this);

    // Game events:
    // Starts, stops and stages are sent from TESQuestStartStopEvent and TESQuestStageEvent, which fire while the quest
    // changes. Outside a party they only prompt the overlay.
    // TESQuestStageItemDoneEvent fires once for each of a stage's log entries, after its fragment has run. That's too
    // late to send on, but it tells when an applied held stage is done.
    // The scene, scene action, phase and dialogue line events are only logged, to follow scenes when debugging held
    // stages. OnUpdate explains why they can't trigger the release of held stages.

    auto* pEventList = EventDispatcherManager::Get();
    pEventList->questStartStopEvent.RegisterSink(this);
    pEventList->questStageEvent.RegisterSink(this);
    pEventList->questStageItemDoneEvent.RegisterSink(this);
    pEventList->sceneEvent.RegisterSink(this);
    pEventList->sceneActionEvent.RegisterSink(this);
    pEventList->scenePhaseEvent.RegisterSink(this);
    pEventList->topicInfoEvent.RegisterSink(this);
    pEventList->enterBleedoutEvent.RegisterSink(this);
}

void QuestService::OnConnected(const ConnectedEvent&) noexcept
{
    // TODO: this should be followed with whatever the quest leader selected
    /*
    // deselect any active quests
    auto* pPlayer = PlayerCharacter::Get();
    for (auto& objective : pPlayer->objectives)
    {
        if (auto* pQuest = objective.instance->quest)
            pQuest->SetActive(false);
    }
    */
}

// Main-thread observation queue. The original engine executes all fragments;
// only publication is held. A completed classification never executes a
// script or mutates a quest on either client.
namespace
{
using StageKey=std::tuple<uint32_t,uint32_t,uint16_t>;
struct OutgoingStage
{
    RequestQuestUpdate Message;
    StageKey Key;
    QuestPhysicalStage::Observation Evidence;
    std::chrono::steady_clock::time_point Deadline;
};
std::map<uint32_t,std::deque<OutgoingStage>> outgoingStages;
std::map<StageKey,bool> classifiedStages;
uint64_t physicalEpoch{};
void RefreshPhysicalEpoch()
{
    const auto epoch=ScriptedTravelEpoch();
    if(physicalEpoch!=epoch){physicalEpoch=epoch;outgoingStages.clear();classifiedStages.clear();}
}
bool UsesPhysicalStages()
{return CampaignAccess::Mode()==CampaignReplication::Link::Mode::Ready;}
void ClearPhysicalQuestStages()
{outgoingStages.clear();classifiedStages.clear();}
void FlushPhysicalQuestStages(World& world)
{
    RefreshPhysicalEpoch();
    if(world.IsLoadingSave()||!world.GetPartyService().IsInParty()||!world.GetTransport().IsConnected()) return;
    const auto now=std::chrono::steady_clock::now();
    for(auto it=outgoingStages.begin();it!=outgoingStages.end();)
    {
        auto& queue=it->second;
        while(!queue.empty())
        {
            auto& e=queue.front();
            auto* quest=Cast<TESQuest>(TESForm::GetById(std::get<0>(e.Key)));
            if(!quest||quest->currentInstanceID!=std::get<1>(e.Key))
            {queue.pop_front();continue;}
            if(e.Evidence.Remaining&&now<e.Deadline) break;
            auto& message=e.Message;
            if(message.Status==RequestQuestUpdate::StageUpdate)
            {
                if(e.Evidence.Remaining)
                    spdlog::warn("STM physical stage observation timed out {:X}:{}",std::get<0>(e.Key),message.Stage);
                message.PhysicalStage=e.Evidence.Individual();
            }
            if(!world.GetTransport().Send(message)) break;
            if(message.Status==RequestQuestUpdate::StageUpdate)
                classifiedStages[e.Key]=message.PhysicalStage;
            queue.pop_front();
        }
        if(queue.empty()) it=outgoingStages.erase(it);else ++it;
    }
}
void QueuePhysicalQuestProgress(World& world,uint32_t form,uint32_t instance,
    RequestQuestUpdate message,uint32_t itemCount)
{
    if(!UsesPhysicalStages()){world.GetTransport().Send(message);return;}
    RefreshPhysicalEpoch();
    // Starts carry a native current stage in STR. In campaign mode, don't
    // replay a scripted initial stage as a side effect of a start packet.
    if(message.Status==RequestQuestUpdate::Started&&itemCount) message.Stage=0;
    const StageKey key{form,instance,message.Stage};
    auto& queue=outgoingStages[form];
    if(message.Status==RequestQuestUpdate::StageUpdate)
    {
        if(classifiedStages.contains(key)) message.PhysicalStage=classifiedStages.at(key);
        for(const auto& e:queue)
            if(e.Key==key&&e.Message.Status==RequestQuestUpdate::StageUpdate) return;
    }
    OutgoingStage e{message,key,{message.Status==RequestQuestUpdate::StageUpdate?itemCount:0},
        std::chrono::steady_clock::now()+std::chrono::seconds(60)};
    if(message.PhysicalStage)e.Evidence.Geometry=true;
    queue.push_back(std::move(e));
    FlushPhysicalQuestStages(world);
}
void FinishPhysicalQuestStage(World& world,uint32_t form,uint16_t stage)
{
    RefreshPhysicalEpoch();
    auto it=outgoingStages.find(form);
    if(it==outgoingStages.end()) return;
    auto* quest=Cast<TESQuest>(TESForm::GetById(form));
    if(!quest) return;
    for(auto& e:it->second)
        if(e.Key==StageKey{form,quest->currentInstanceID,stage}&&e.Message.Status==RequestQuestUpdate::StageUpdate&&e.Evidence.Remaining)
        {e.Evidence.Done();break;}
    FlushPhysicalQuestStages(world);
}
}
bool QuestPhysicalStagePending(uint32_t form,uint32_t instance,uint16_t stage) noexcept
{
    RefreshPhysicalEpoch();
    const auto it=outgoingStages.find(form);
    if(it==outgoingStages.end()) return false;
    for(const auto& e:it->second)
        if(e.Key==StageKey{form,instance,stage}&&e.Message.Status==RequestQuestUpdate::StageUpdate&&e.Evidence.Remaining)
            return true;
    return false;
}
void ObserveQuestPhysicalEffect(uint32_t form,uint32_t instance,uint16_t stage,
    QuestPhysicalStage::Effect effect,uint64_t epoch) noexcept
{
    try
    {
        World::Get().GetRunner().Queue([=]{
            RefreshPhysicalEpoch();
            if(epoch!=physicalEpoch||World::Get().IsLoadingSave()) return;
            auto it=outgoingStages.find(form);
            if(it==outgoingStages.end()) return;
            // currentStage alone is ambiguous if multiple stages of the same
            // native quest are executing simultaneously. Do not guess.
            OutgoingStage* only=nullptr;
            for(auto& e:it->second)
                if(std::get<1>(e.Key)==instance&&e.Message.Status==RequestQuestUpdate::StageUpdate&&e.Evidence.Remaining)
                {if(only)return;only=&e;}
            if(only&&only->Key==StageKey{form,instance,stage}) only->Evidence.Seen(effect);
        });
    }
    catch (...) {}
}
void SubmitObservedQuestStage(World& world,TESQuest* quest,const RequestQuestUpdate& message) noexcept
{
    try
    {
        if(!quest) return;
        if(!UsesPhysicalStages()){world.GetTransport().Send(message);return;}
        RefreshPhysicalEpoch();
        // An inventory observation is not proof that a scripted stage
        // finished. It may not bypass a pending or unknown classification.
        const StageKey key{quest->formID,quest->currentInstanceID,message.Stage};
        if(quest->GetStageItemCount(message.Stage)&&!classifiedStages.contains(key)) return;
        QueuePhysicalQuestProgress(world,quest->formID,quest->currentInstanceID,message,0);
    }
    catch (...) {}
}

void QuestService::OnUpdate(const UpdateEvent&) noexcept
{
    FlushPhysicalQuestStages(m_world);
    if (m_heldStages.empty() && m_afterHeldStages.empty())
        return;

    // Checked every frame rather than on scene events: an action's done event fires before its fragment runs, and the
    // action only counts as complete afterwards, with no event of its own
    Vector<uint32_t> questFormIds;
    for (const auto& [questFormId, _] : m_heldStages)
        questFormIds.push_back(questFormId);

    for (uint32_t questFormId : questFormIds)
    {
        TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(questFormId));
        if (HeldStages* pHeld = GetHeldStages(questFormId, pQuest))
        {
            if (pHeld->CheckInstance||!pHeld->Targets.empty())
            {
                bool matches=false;
                try
                {
                    matches=pQuest->currentInstanceID==pHeld->LocalInstance&&
                        (pHeld->Targets.empty()||CaptureQuestTargets(pQuest)==pHeld->Targets);
                }
                catch (...) {}
                if (!matches)
                {
                    auto stale=m_heldStages.find(questFormId);
                    if (stale!=m_heldStages.end()) m_heldStages.erase(stale);
                    auto deferred=m_afterHeldStages.find(questFormId);
                    if (deferred!=m_afterHeldStages.end()) m_afterHeldStages.erase(deferred);
                    auto retry=m_questRetryAt.find(questFormId);
                    if (retry!=m_questRetryAt.end()) m_questRetryAt.erase(retry);
                    continue;
                }
            }
            ReleaseStagesForWaitingPhases(pQuest, *pHeld);
            ReleaseUncheckedStages(pQuest, *pHeld);
            if (pHeld->BranchConflict)
            {
                m_branchConflicts[questFormId]=pQuest->currentInstanceID;
                auto held=m_heldStages.find(questFormId);
                if (held!=m_heldStages.end()) m_heldStages.erase(held);
                auto deferred=m_afterHeldStages.find(questFormId);
                if (deferred!=m_afterHeldStages.end()) m_afterHeldStages.erase(deferred);
                auto retry=m_questRetryAt.find(questFormId);
                if (retry!=m_questRetryAt.end()) m_questRetryAt.erase(retry);
            }
        }
    }

    // Dispatching may start a new run and defer its own stop, so detach each batch before dispatch.
    Vector<uint32_t> ready;
    for (const auto& [formId, events] : m_afterHeldStages)
    {
        const auto retry = m_questRetryAt.find(formId);
        if (m_heldStages.find(formId) == m_heldStages.end() &&
            (retry == m_questRetryAt.end() || std::chrono::steady_clock::now() >= retry.value()))
            ready.push_back(formId);
    }
    for (uint32_t formId : ready)
    {
        auto it = m_afterHeldStages.find(formId);
        auto events = std::move(it.value());
        m_afterHeldStages.erase(it);
        const auto retry = m_questRetryAt.find(formId);
        if (retry != m_questRetryAt.end())
            m_questRetryAt.erase(retry);
        for (const auto& update : events)
            OnQuestUpdate(update);
    }
}

void QuestService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_afterHeldStages.clear();
    m_heldStages.clear();
    m_questRetryAt.clear();
    m_lastQuestTargets.clear();
    m_branchConflicts.clear();
}

void QuestService::OnPartyLeft(const PartyLeftEvent&) noexcept
{
    ClearPhysicalQuestStages();
    m_afterHeldStages.clear();
    m_heldStages.clear();
    m_questRetryAt.clear();
    m_lastQuestTargets.clear();
    m_branchConflicts.clear();
}

BSTEventResult QuestService::OnEvent(const TESQuestStartStopEvent* apEvent, const EventDispatcher<TESQuestStartStopEvent>*)
{
    if (ScopedQuestOverride::IsOverriden())
        return BSTEventResult::kOk;

    // The quest only failed to start here (e.g. a required alias didn't fill), others may be running it
    if (apEvent->started && apEvent->failed)
        return BSTEventResult::kOk;

    if (!m_world.Get().GetPartyService().IsInParty())
    {
        if (apEvent->started)
            NotifyOverlayOfQuestUpdate(apEvent->formId);
        return BSTEventResult::kOk;
    }

    spdlog::info("Quest start/stop event: {:X}", apEvent->formId);

    if (TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(apEvent->formId)))
    {
        if (IsNonSyncableQuest(pQuest))
            return BSTEventResult::kOk;
     
        if (pQuest->type == TESQuest::Type::None || pQuest->type == TESQuest::Type::Miscellaneous)
        {
            // Perhaps redundant, but necessary. We need the logging and
            // the lambda coming up is queued and runs later
            GameId Id;
            auto& modSys = m_world.GetModSystem();
            if (modSys.GetServerModId(pQuest->formID, Id))
            {
                spdlog::info(__FUNCTION__ ": queuing type none/misc quest gameId {:X} questStage {} questStatus {} questType {} formId {:X} name {}",
                             Id.LogFormat(),  pQuest->currentStage, pQuest->IsStopped() ? RequestQuestUpdate::Stopped : RequestQuestUpdate::Started,
                             static_cast<std::underlying_type_t<TESQuest::Type>>(pQuest->type), 
                             pQuest->formID, pQuest->fullName.value.AsAscii());
            }
        }
        
        m_world.GetRunner().Queue(
            [&, formId = pQuest->formID, stageId = pQuest->currentStage, stopped = pQuest->IsStopped(), type = pQuest->type,
                instance = pQuest->currentInstanceID, itemCount=pQuest->GetStageItemCount(pQuest->currentStage), completedRunOnce = pQuest->IsStopped() && (pQuest->flags & TESQuest::RunOnce) &&
                    (pQuest->flags & TESQuest::Completed) && !(pQuest->flags & (TESQuest::Failed | TESQuest::AllowRepeatStages))]()
            {
                GameId Id;
                auto& modSys = m_world.GetModSystem();
                if (modSys.GetServerModId(formId, Id))
                {
                    if (m_world.GetOverlayService().IsPersonalQuest(formId)) return;
                    RequestQuestUpdate update;
                    update.JournalObjective = InPlayerJournal(formId);
                    update.CompletedRunOnce = completedRunOnce;
                    update.Id = Id;
                    update.Stage = stageId;
                    update.Status = stopped ? RequestQuestUpdate::Stopped : RequestQuestUpdate::Started;
                    if (!FillQuestTargets(formId, instance, update)) return;
                    update.ClientQuestType = static_cast<std::underlying_type_t<TESQuest::Type>>(type); 

                    QueuePhysicalQuestProgress(m_world,formId,instance,std::move(update),itemCount);
                }
            });
    }

    return BSTEventResult::kOk;
}

BSTEventResult QuestService::OnEvent(const TESQuestStageEvent* apEvent, const EventDispatcher<TESQuestStageEvent>*)
{
    if (ScopedQuestOverride::IsOverriden())
        return BSTEventResult::kOk;

    if (!m_world.Get().GetPartyService().IsInParty())
    {
        NotifyOverlayOfQuestUpdate(apEvent->formId);
        return BSTEventResult::kOk;
    }

    spdlog::info("Quest stage event: {:X}, stage: {}", apEvent->formId, apEvent->stageId);

    // there is no reason to even fetch the quest object, since the event provides everything already....
    if (TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(apEvent->formId)))
    {
        if (IsNonSyncableQuest(pQuest))
            return BSTEventResult::kOk;

        if (pQuest->type == TESQuest::Type::None || pQuest->type == TESQuest::Type::Miscellaneous)
        {
            // Perhaps redundant, but necessary. We need the logging and
            // the lambda coming up is queued and runs later
            GameId Id;
            auto& modSys = m_world.GetModSystem();
            if (modSys.GetServerModId(pQuest->formID, Id))
            {
                spdlog::info(__FUNCTION__ ": queuing type none/misc quest gameId {:X} questStage {} questStatus {} questType {} formId {:X} name {}",
                             Id.LogFormat(), apEvent->stageId,
                             RequestQuestUpdate::StageUpdate,
                             static_cast<std::underlying_type_t<TESQuest::Type>>(pQuest->type),
                             pQuest->formID, pQuest->fullName.value.AsAscii());
            }
        }

        m_world.GetRunner().Queue(
            [&, formId = apEvent->formId, stageId = apEvent->stageId, type = pQuest->type, instance = pQuest->currentInstanceID,
                itemCount=pQuest->GetStageItemCount(apEvent->stageId)]()
            {
                // Observe the actual opponent picked by the vanilla brawl quest without changing its scripts.
                if (formId == 0x47AE6)
                {
                    auto* pBrawl = Cast<TESQuest>(TESForm::GetById(formId));
                    auto* pOpponent = pBrawl ? Cast<Actor>(pBrawl->GetAliasedRef(0)) : nullptr;
                    if (pOpponent)
                    {
                        spdlog::info("Brawl stage {}, opponent {:X}, dead {}, bleedout {}, essential {}, combat {}",
                                     stageId, pOpponent->formID, pOpponent->IsDead(), pOpponent->actorState.IsBleedingOut(),
                                     pOpponent->IsEssential(), pOpponent->IsInCombat());
                    }
                }
                GameId Id;
                auto& modSys = m_world.GetModSystem();
                if (modSys.GetServerModId(formId, Id))
                {
                    if (m_world.GetOverlayService().IsPersonalQuest(formId)) return;
                    RequestQuestUpdate update;
                    update.JournalObjective = InPlayerJournal(formId);
                    update.Id = Id;
                    update.Stage = stageId;
                    update.Status = RequestQuestUpdate::StageUpdate;
                    if (!FillQuestTargets(formId, instance, update)) return;
                    update.ClientQuestType = static_cast<std::underlying_type_t<TESQuest::Type>>(type);

                    QueuePhysicalQuestProgress(m_world,formId,instance,std::move(update),itemCount);
                }
            });
    }

    return BSTEventResult::kOk;
}

BSTEventResult QuestService::OnEvent(const TESQuestStageItemDoneEvent* apEvent, const EventDispatcher<TESQuestStageItemDoneEvent>*)
{
    // Sent once the fragment of one of the stage's log entries has run, usually from a script thread
    m_world.GetRunner().Queue(
        [this, questFormId = apEvent->formId, stage = apEvent->stageId]()
        {
            FinishPhysicalQuestStage(m_world,questFormId,stage);
            QuestRewardStageDone(questFormId,stage);
            auto it = m_heldStages.find(questFormId);
            if (it == m_heldStages.end())
                return;

            Vector<uint16_t>& pendingItems = it.value().PendingItems;
            auto itemIt = std::find(pendingItems.begin(), pendingItems.end(), stage);
            if (itemIt == pendingItems.end())
                return;

            pendingItems.erase(itemIt);
            spdlog::debug("Fragment of applied member quest stage {} for {:X} done, {} left", stage, questFormId, pendingItems.size());
        });

    return BSTEventResult::kOk;
}

BSTEventResult QuestService::OnEvent(const TESEnterBleedoutEvent* event, const EventDispatcher<TESEnterBleedoutEvent>*)
{
    if (event && event->actor && m_world.GetPartyService().IsInParty())
        m_world.GetRunner().Queue([this, form = event->actor->formID] { m_world.GetOverlayService().BrawlBleedout(form); });
    return BSTEventResult::kOk;
}

BSTEventResult QuestService::OnEvent(const TESSceneEvent* apEvent, const EventDispatcher<TESSceneEvent>*)
{
    spdlog::debug("Scene {:X} {}", apEvent->sceneFormId, apEvent->type == 0 ? "begin" : "end");
    return BSTEventResult::kOk;
}

BSTEventResult QuestService::OnEvent(const TESSceneActionEvent* apEvent, const EventDispatcher<TESSceneActionEvent>*)
{
    spdlog::debug("Scene {:X} action {} done, quest: {:X}, alias: {}", apEvent->sceneFormId, apEvent->actionIndex, apEvent->questFormId, apEvent->aliasId);
    return BSTEventResult::kOk;
}

BSTEventResult QuestService::OnEvent(const TESScenePhaseEvent* apEvent, const EventDispatcher<TESScenePhaseEvent>*)
{
    if (!spdlog::should_log(spdlog::level::debug))
        return BSTEventResult::kOk;

    BGSScene* pScene = Cast<BGSScene>(TESForm::GetById(apEvent->sceneFormId));
    if (!pScene)
        return BSTEventResult::kOk;

    // A phase without completion conditions ends once every action running in it is complete
    String actions;
    for (BGSSceneAction* pAction : pScene->actions)
    {
        if (!pAction || !pAction->IsInPhase(apEvent->phaseIndex))
            continue;

        constexpr std::array cTypeNames{"dialogue", "package", "timer"};
        const uint32_t cType = static_cast<uint32_t>(pAction->GetType());
        actions += spdlog::fmt_lib::format(
            " [{} {} alias {} {}]", pAction->GetIndex(), cType < cTypeNames.size() ? cTypeNames[cType] : "unknown", pAction->actorId,
            pAction->IsComplete() ? "complete" : "running");
    }

    spdlog::debug("Scene {:X} phase {} {}, actions:{}", apEvent->sceneFormId, apEvent->phaseIndex, apEvent->type == 0 ? "begin" : "end", actions);
    return BSTEventResult::kOk;
}

BSTEventResult QuestService::OnEvent(const TESTopicInfoEvent* apEvent, const EventDispatcher<TESTopicInfoEvent>*)
{
    if (apEvent->eType == 0 && apEvent->pSpeakerRef && m_world.GetPartyService().IsInParty())
    {
        m_world.GetRunner().Queue([this, speakerId = apEvent->pSpeakerRef->formID, topic = apEvent->uiTopicInfoFormID]()
        {
            if (!m_world.GetPartyService().IsInParty())
                return;
            Actor* pSpeaker = Cast<Actor>(TESForm::GetById(speakerId));
            if (!pSpeaker || !pSpeaker->GetExtension())
                return;
            // Skyrim.esm DialogueFollower alias 0 is the human follower.
            auto* pQuest = Cast<TESQuest>(TESForm::GetById(0x750BA));
            const auto* pFollower = pQuest ? pQuest->GetAliasedRef(0) : nullptr;
            spdlog::info("Party dialogue topic {:X}, speaker {:X}, follower alias {:X}, remote {}, dead {}, bleedout {}, speaker combat {}, player combat {}",
                         topic, speakerId, pFollower ? pFollower->formID : 0, pSpeaker->GetExtension()->IsRemote(),
                         pSpeaker->IsDead(), pSpeaker->actorState.IsBleedingOut(), pSpeaker->IsInCombat(), PlayerCharacter::Get()->IsInCombat());
        });
    }
    spdlog::debug(
        "Dialogue line {:X} {}, speaker: {:X}", apEvent->uiTopicInfoFormID, apEvent->eType == 0 ? "begin" : "end", apEvent->pSpeakerRef ? apEvent->pSpeakerRef->formID : 0);
    return BSTEventResult::kOk;
}

static bool Compare(TESConditionItem::OpCode aOpCode, float aLhs, float aRhs) noexcept
{
    switch (aOpCode)
    {
    case TESConditionItem::OpCode::kEqualTo: return aLhs == aRhs;
    case TESConditionItem::OpCode::kNotEqualTo: return aLhs != aRhs;
    case TESConditionItem::OpCode::kGreaterThan: return aLhs > aRhs;
    case TESConditionItem::OpCode::kGreaterThanOrEqualTo: return aLhs >= aRhs;
    case TESConditionItem::OpCode::kLessThan: return aLhs < aRhs;
    case TESConditionItem::OpCode::kLessThanOrEqualTo: return aLhs <= aRhs;
    default: return false;
    }
}

// Whether setting the stage turns a GetStage/GetStageDone check on the quest from false to true. With aOrChecksLater,
// also whether it changes the check's result at all, or the check is on the same or a later stage.
static bool IsCheckedByStage(const TESCondition& acConditions, TESQuest* apQuest, uint16_t aStage, bool aOrChecksLater) noexcept
{
    for (const TESConditionItem* pItem = acConditions.pHead; pItem; pItem = pItem->pNext)
    {
        if (pItem->UsesGlobal() || pItem->params[0] != apQuest)
            continue;

        const TESConditionItem::OpCode cOpCode = pItem->GetOpCode();
        float checkedStage = 0.f;
        bool before = false;
        bool after = false;
        if (pItem->function == TESConditionItem::kGetStage)
        {
            // Stages only go up, GetStage returns the highest one set
            checkedStage = pItem->comparisonValue;
            before = Compare(cOpCode, apQuest->currentStage, pItem->comparisonValue);
            after = Compare(cOpCode, std::max(apQuest->currentStage, aStage), pItem->comparisonValue);
        }
        else if (pItem->function == TESConditionItem::kGetStageDone)
        {
            const uint16_t cStage = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(pItem->params[1]));
            checkedStage = cStage;
            if (cStage == aStage)
            {
                before = Compare(cOpCode, 0.f, pItem->comparisonValue);
                after = Compare(cOpCode, 1.f, pItem->comparisonValue);
            }
        }
        else
        {
            continue;
        }

        if (!before && after)
            return true;

        if (aOrChecksLater && (before != after || checkedStage >= aStage))
            return true;
    }

    return false;
}

// Whether the phase's own lines are done, so passing its completion conditions won't cut one off. Timers and packages
// don't hold such a phase back, and packages never complete on clients that don't own the NPC.
static bool IsPhaseWaitingOnConditions(BGSScene* apScene, uint32_t aPhaseIndex) noexcept
{
    for (BGSSceneAction* pAction : apScene->actions)
    {
        if (!pAction || !pAction->IsInPhase(aPhaseIndex))
            continue;

        if (pAction->GetType() != BGSSceneAction::Type::kDialogue || pAction->IsComplete() || pAction->Loops())
            continue;

        // The player's own lines wait for this player to answer, while a member may have answered instead
        if (apScene->owningQuest && apScene->owningQuest->GetAliasedRef(pAction->actorId) == PlayerCharacter::Get())
            continue;

        return false;
    }

    return true;
}

// Whether a phase the playing scenes have yet to finish checks the stage or a later one, or checks differently with it
// (a phase waiting on GetStage == N). Scenes that only end or repeat on it don't count (MQ105's sprint trial).
static bool IsCheckedByPlayingScene(TESQuest* apQuest, uint16_t aStage) noexcept
{
    for (BGSScene* pScene : apQuest->scenes)
    {
        if (!pScene)
            continue;

        for (uint32_t i = pScene->GetNextPhaseIndex(); i < pScene->phases.length; ++i)
        {
            const BGSScenePhase* pPhase = pScene->phases[i];
            if (!pPhase)
                continue;

            // The running phase is past its start conditions
            const bool cIsRunning = pScene->currentPhaseIndex == i;
            if ((!cIsRunning && IsCheckedByStage(pPhase->startConditions, apQuest, aStage, true)) ||
                IsCheckedByStage(pPhase->completionConditions, apQuest, aStage, true))
            {
                return true;
            }
        }
    }

    return false;
}

void QuestService::HoldStage(TESQuest* apQuest, uint16_t aStage) noexcept
{
    Vector<uint16_t>& stages = m_heldStages[apQuest->formID].Stages;
    if (std::find(stages.begin(), stages.end(), aStage) != stages.end())
        return;
    stages.push_back(aStage);

    spdlog::info("Holding member quest stage {} for {:X} until no playing scene of it checks it", aStage, apQuest->formID);
}

// TESQuestStageItem::objConditions is the first member (CommonLibSSE-NG).
// Only prove exclusion from a reached stage in this same native instance.
// Unknown conditions, OR alternatives and unfilled conditions are NOT proof
// of conflict. Never treat a stage number as a branch identifier.
static bool ExcludedStageEntry(TESQuest* quest,uint16_t stage,const TESCondition& conditions) noexcept
{
    bool groupExcluded=true;
    unsigned visited=0;
    for (auto* item=conditions.pHead;item;item=item->pNext)
    {
        if (++visited>1024) return false;
        bool excluded=false;
        if (item->function==TESConditionItem::kGetStageDone&&item->params[0]==quest&&
            !(item->flags&0x1E)&&item->object==0)
        {
            const auto raw=reinterpret_cast<uintptr_t>(item->params[1]);
            if (raw<=UINT16_MAX&&raw!=stage&&quest->IsStageDone(static_cast<uint16_t>(raw)))
            {
                using Op=TESConditionItem::OpCode;
                const auto op=item->GetOpCode();
                excluded=(op==Op::kEqualTo&&item->comparisonValue==0.f)||
                    (op==Op::kNotEqualTo&&item->comparisonValue==1.f)||
                    (op==Op::kLessThan&&item->comparisonValue==1.f)||
                    (op==Op::kLessThanOrEqualTo&&item->comparisonValue==0.f);
            }
        }
        groupExcluded=groupExcluded&&excluded;
        if (!(item->flags&1))
        {
            if (groupExcluded) return true;
            groupExcluded=true;
        }
    }
    return false;
}

static bool SceneBlocksQuestStop(TESQuest* quest) noexcept
{
    for (auto* scene:quest->scenes)
    {
        if (!scene||!scene->isPlaying) continue;
        const auto phase=static_cast<uint32_t>(scene->currentPhaseIndex);
        if (phase>=static_cast<uint32_t>(scene->phases.length)||
            !IsPhaseWaitingOnConditions(scene,phase)) return true;
    }
    return false;
}

static bool StageExcludedByLocalBranch(TESQuest* quest,uint16_t stage) noexcept
{
    if (!quest||quest->IsStopped()||(quest->flags&TESQuest::AllowRepeatStages)) return false;
    for (auto* entry:quest->stages)
    {
        if (!entry||entry->stageIndex!=stage) continue;
        bool hasEntry=false;
        for (auto* raw:entry->items)
        {
            if (!raw) continue;
            hasEntry=true;
            // The engine's entry conditions are a TESCondition at offset zero.
            if (!ExcludedStageEntry(quest,stage,*static_cast<const TESCondition*>(raw))) return false;
        }
        return hasEntry;
    }
    return false;
}

static bool ApplyNextHeldStage(TESQuest* apQuest, HeldStages& aHeld) noexcept
{
    ScopedQuestOverride _;
    const uint16_t stage = aHeld.Stages.front();
    if (apQuest->IsStageReached(stage))
    {
        aHeld.Stages.erase(aHeld.Stages.begin());
        aHeld.RetryAt = {};
        return true;
    }
    if (StageExcludedByLocalBranch(apQuest,stage))
    {
        aHeld.BranchConflict=true;
        spdlog::error("STM quest {:X}: local branch excludes shared stage {}; not executing its fragments",apQuest->formID,stage);
        return false;
    }
    if (!apQuest->ScriptSetStage(stage))
    {
        aHeld.RetryAt = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        spdlog::error("Held quest {:X} stage {} not reached; retaining it for retry", apQuest->formID, stage);
        return false;
    }
    aHeld.Stages.erase(aHeld.Stages.begin());
    aHeld.RetryAt = {};
    aHeld.PendingItems.insert(aHeld.PendingItems.end(), apQuest->GetStageItemCount(stage), stage);
    return true;
}

static void ReleaseStagesForWaitingPhases(TESQuest* apQuest, HeldStages& aHeld) noexcept
{
    if (aHeld.BranchConflict || !aHeld.PendingItems.empty() || std::chrono::steady_clock::now() < aHeld.RetryAt)
        return;

    // A phase whose own actions are done and whose completion conditions want a held stage only moves on with it.
    // Advance the queue in arrival order until the phase can progress.
    for (BGSScene* pScene : apQuest->scenes)
    {
        if (aHeld.Stages.empty())
            break;

        if (!pScene || !pScene->isPlaying || pScene->currentPhaseIndex >= pScene->phases.length)
            continue;

        BGSScenePhase* pPhase = pScene->phases[pScene->currentPhaseIndex];
        if (!pPhase)
            continue;

        // Conditions first, they rarely mention a held stage and are cheaper than walking the phase's actions
        const auto cStageIt = std::find_if(
            aHeld.Stages.begin(), aHeld.Stages.end(),
            [&](uint16_t aStage) { return IsCheckedByStage(pPhase->completionConditions, apQuest, aStage, false); });
        if (cStageIt == aHeld.Stages.end() || !IsPhaseWaitingOnConditions(pScene, pScene->currentPhaseIndex))
            continue;

        spdlog::info(
            "Releasing held quest stage {} for {:X}; scene {:X} phase {} is waiting", aHeld.Stages.front(), apQuest->formID, pScene->formID,
            pScene->currentPhaseIndex);
        ApplyNextHeldStage(apQuest, aHeld);
        return;
    }
}

// Applies held stages in order while no playing scene checks the next one, e.g. once the scenes that did have ended.
// Waits for the fragments of the stages it applied first.
static void ReleaseUncheckedStages(TESQuest* apQuest, HeldStages& aHeld) noexcept
{
    while (!aHeld.BranchConflict && !aHeld.Stages.empty() && aHeld.PendingItems.empty() &&
           std::chrono::steady_clock::now() >= aHeld.RetryAt &&
           !IsCheckedByPlayingScene(apQuest, aHeld.Stages.front()))
    {
        spdlog::info("Applying held member quest stage {} for {:X}, no playing scene checks it", aHeld.Stages.front(), apQuest->formID);
        if (!ApplyNextHeldStage(apQuest, aHeld))
            return;
    }
}

HeldStages* QuestService::GetHeldStages(uint32_t aQuestFormId, TESQuest* apQuest) noexcept
{
    auto it = m_heldStages.find(aQuestFormId);
    if (it == m_heldStages.end())
        return nullptr;

    // Setting a stage on a stopped quest starts it again, and scenes also end because their quest stopped
    HeldStages& held = it.value();
    if (!apQuest || apQuest->IsStopped())
    {
        if (!held.Stages.empty())
            spdlog::info("Dropping held member quest stages for {:X}, quest stopped or missing", aQuestFormId);

        m_heldStages.erase(it);
        return nullptr;
    }

    // Reached by the leader's own scripts in the meantime
    std::erase_if(
        held.Stages,
        [apQuest](uint16_t aStage)
        {
            if (!apQuest->IsStageReached(aStage))
                return false;

            spdlog::info("Dropping held member quest stage {} for {:X}, already reached here", aStage, apQuest->formID);
            return true;
        });

    if (held.Stages.empty() && held.PendingItems.empty())
    {
        m_heldStages.erase(it);
        return nullptr;
    }

    return &held;
}

void QuestService::OnQuestUpdate(const NotifyQuestUpdate& aUpdate) noexcept
{
    ModSystem& modSystem = World::Get().GetModSystem();
    uint32_t formId = modSystem.GetGameId(aUpdate.Id);
    TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(formId));
    if (!pQuest)
    {
        spdlog::error("Failed to find quest, base id: {:X}, mod id: {:X}", aUpdate.Id.BaseId, aUpdate.Id.ModId);
        return;
    }

    auto deferred = m_afterHeldStages.find(formId);
    if (deferred != m_afterHeldStages.end())
    {
        deferred.value().push_back(aUpdate);
        return;
    }

    if (aUpdate.Status == NotifyQuestUpdate::StageUpdate && pQuest->IsStageReached(aUpdate.Stage))
    {
        spdlog::info("Remote quest {:X} already reached stage {}, current stage: {}", formId, aUpdate.Stage, pQuest->currentStage);
        return;
    }

    if (!MatchesQuestTargets(pQuest,aUpdate)) return;
    const auto conflict=m_branchConflicts.find(formId);
    if (conflict!=m_branchConflicts.end())
    {
        if (conflict->second==pQuest->currentInstanceID) return;
        m_branchConflicts.erase(conflict);
    }

    // A replay must not restart a completed one-shot quest, even when an old
    // stage packet arrives rather than an explicit start.
    if (pQuest->IsStopped() && (pQuest->flags & TESQuest::RunOnce) && (pQuest->flags & TESQuest::Completed) &&
        aUpdate.Status != NotifyQuestUpdate::Stopped)
        return;

    // Both clients run native fragments and scenes. In either direction a
    // stage must wait behind its predecessor's pending fragments or a scene.
    const bool stageUpdate = aUpdate.Status == NotifyQuestUpdate::StageUpdate;
    HeldStages* pHeld = stageUpdate || aUpdate.Status == NotifyQuestUpdate::Stopped ? GetHeldStages(formId, pQuest) : nullptr;
    if (stageUpdate && (pHeld || IsCheckedByPlayingScene(pQuest, aUpdate.Stage)))
    {
        auto& held=m_heldStages[formId];
        held.Targets.assign(aUpdate.Targets.data(),aUpdate.Targets.size());
        held.LocalInstance=pQuest->currentInstanceID;
        held.CheckInstance=!pQuest->IsStopped();
        HoldStage(pQuest, aUpdate.Stage);
        return;
    }

    // Let the scene and its running fragments finish before stopping. A restart received
    // meanwhile belongs behind this stop, even if it uses the same form and stage numbers.
    if (aUpdate.Status==NotifyQuestUpdate::Stopped&&pQuest->IsStopped()) return;
    if (aUpdate.Status == NotifyQuestUpdate::Stopped && (pHeld||SceneBlocksQuestStop(pQuest)))
    {
        auto stop=aUpdate;
        stop.CheckInstance=true;
        stop.ExpectedInstance=pQuest->currentInstanceID;
        stop.LocalInstance=pQuest->currentInstanceID;
        m_afterHeldStages[formId].push_back(stop);
        if (!pHeld) m_questRetryAt[formId]=std::chrono::steady_clock::now()+std::chrono::milliseconds(250);
        return;
    }

    if (pQuest->type == TESQuest::Type::None || pQuest->type == TESQuest::Type::Miscellaneous)
    {
        spdlog::info(__FUNCTION__ ": receiving type none/misc quest update gameId {:X} questStage {} questStatus {} questType {} formId {:X} name {}",
                     aUpdate.Id.LogFormat(), aUpdate.Stage, aUpdate.Status,
                     aUpdate.ClientQuestType, formId, pQuest->fullName.value.AsAscii());
    }

    // The quest events fire synchronously while the stage is set, don't send them back
    ScopedQuestOverride _;

    bool bResult = false;
    switch (aUpdate.Status)
    {
    case NotifyQuestUpdate::Started:
    {
        if (!pQuest->IsStageReached(aUpdate.Stage)&&StageExcludedByLocalBranch(pQuest,aUpdate.Stage))
        {
            m_branchConflicts[formId]=pQuest->currentInstanceID;
            spdlog::error("STM quest {:X}: incompatible shared start stage {}",formId,aUpdate.Stage);
            return;
        }
        // Stage zero (or an already executed stage) is not proof of a running quest.
        // Use the existing native unforced start only for an explicit start event.
        // A pending promotion must finish before another native request.
        if (pQuest->IsStopped() && (pQuest->flags & TESQuest::RunOnce) && (pQuest->flags & TESQuest::Completed))
            return;
        if (pQuest->IsStopped() && pQuest->getState() == TESQuest::State::WaitingPromotion)
            break;
        if (pQuest->IsStopped() && (aUpdate.Stage==0||pQuest->IsStageReached(aUpdate.Stage)))
        {
            bool started = false;
            pQuest->EnsureQuestStarted(started, false);
            if (pQuest->IsStopped()) break;
        }
        const bool alreadyReached = aUpdate.Stage==0||pQuest->IsStageReached(aUpdate.Stage);
        bResult = (alreadyReached || pQuest->ScriptSetStage(aUpdate.Stage)) && !pQuest->IsStopped();
        if (bResult)
        {
            if (!alreadyReached)
            {
                const auto pending = pQuest->GetStageItemCount(aUpdate.Stage);
                if (pending)
                {
                    auto& held=m_heldStages[formId];
                    held.PendingItems.insert(held.PendingItems.end(),pending,aUpdate.Stage);
                    held.Targets.assign(aUpdate.Targets.data(),aUpdate.Targets.size());
                    held.LocalInstance=pQuest->currentInstanceID;
                    held.CheckInstance=true;
                }
            }
            pQuest->SetActive(true);
            spdlog::info("Remote quest started: {:X}, stage: {}", formId, aUpdate.Stage);
        }
        break;
    }
    case NotifyQuestUpdate::StageUpdate:
    {
        if (StageExcludedByLocalBranch(pQuest,aUpdate.Stage))
        {
            m_branchConflicts[formId]=pQuest->currentInstanceID;
            spdlog::error("STM quest {:X}: local branch excludes shared stage {}",formId,aUpdate.Stage);
            return;
        }
        const bool alreadyReached = pQuest->IsStageReached(aUpdate.Stage);
        bResult = alreadyReached || pQuest->ScriptSetStage(aUpdate.Stage);
        if (bResult)
        {
            if (!alreadyReached && !pQuest->IsStopped())
            {
                const auto pending = pQuest->GetStageItemCount(aUpdate.Stage);
                if (pending)
                {
                    auto& held=m_heldStages[formId];
                    held.PendingItems.insert(held.PendingItems.end(),pending,aUpdate.Stage);
                    held.Targets.assign(aUpdate.Targets.data(),aUpdate.Targets.size());
                    held.LocalInstance=pQuest->currentInstanceID;
                    held.CheckInstance=true;
                }
            }
            spdlog::info("Remote quest updated: {:X}, stage: {}", formId, aUpdate.Stage);
        }
        break;
    }
    case NotifyQuestUpdate::Stopped:
        bResult = StopQuest(formId);
        spdlog::info("Remote quest stopped: {:X}, stage: {}", formId, aUpdate.Stage);
        break;
    default: break;
    }

    if (!bResult)
    {
        spdlog::error("Failed to update the client quest state, quest: {:X}, stage: {}, status: {}", formId, aUpdate.Stage, aUpdate.Status);
        if (aUpdate.Status == NotifyQuestUpdate::Started || aUpdate.Status == NotifyQuestUpdate::StageUpdate)
        {
            m_afterHeldStages[formId].push_back(aUpdate);
            m_questRetryAt[formId] = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }
    }
}

void QuestService::NotifyOverlayOfQuestUpdate(uint32_t aFormId) noexcept
{
    if (!m_world.GetTransport().IsOnline())
        return;

    // Nobody around to party with, or the invite toast already prompts the player to join one.
    const auto remotePlayerView = m_world.view<PlayerComponent, RemoteComponent>();
    if (m_world.GetPartyService().HasPendingInvitations() || remotePlayerView.begin() == remotePlayerView.end())
        return;

    TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(aFormId));
    if (!pQuest || IsNonSyncableQuest(pQuest))
        return;

    // Hidden background quests and minor misc tasks are too noisy to prompt about
    if (pQuest->type == TESQuest::Type::None || pQuest->type == TESQuest::Type::Miscellaneous)
        return;

    m_world.GetOverlayService().GetOverlayApp()->ExecuteAsync("questUpdated");
}

bool QuestService::StopQuest(uint32_t aformId)
{
    TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(aformId));
    if (pQuest)
    {
        pQuest->SetActive(false);
        pQuest->SetStopped();
        return true;
    }

    return false;
}

static constexpr std::array kNonSyncableQuestIds = std::to_array<uint32_t>({
    0x2BA16,   // Werewolf transformation quest
    0x20071D0, // Vampire transformation quest
    0x3AC44,   // MS13BleakFallsBarrowLeverScene
    // 0xFE014801,  // Unknown dynamic ID, kept as note, maybe lookup correct ID this game?
    0xF2593 // Skill experience quest
});

bool QuestService::IsNonSyncableQuest(TESQuest* apQuest)
{
    // These framework quests own local-player and follower aliases. Replaying their fragments
    // would give a listener a second follower or make the listener the brawl challenger.
    if (apQuest && (apQuest->formID == 0xCA210 || apQuest->formID == 0x750BA || apQuest->formID == 0x47AE6))
        return true;
    // Quests with no quest stages are never synced. Most TESQues::Type:: quests should
    // be synced, including Type::None and Type::Miscellaneous, but there are a few
    // known exceptions that should be excluded that are in the table.
    return    apQuest->stages.Empty() 
           || std::find(kNonSyncableQuestIds.begin(), kNonSyncableQuestIds.end(), apQuest->formID) != kNonSyncableQuestIds.end();
}

void QuestService::DebugDumpQuests()
{
    auto& quests = ModManager::Get()->quests;
    for (TESQuest* pQuest : quests)
        spdlog::info("{:X}|{}|{}|{}", pQuest->formID, (uint8_t)pQuest->type, pQuest->priority, pQuest->idName.AsAscii());
}
