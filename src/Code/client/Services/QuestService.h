#pragma once
#include <chrono>
struct NotifyQuestJournal;
struct NotifyCampaignRecord;

#include <World.h>
#include <map>
#include <string>
#include <Events/EventDispatcher.h>
#include <Games/Events.h>
#include <Messages/NotifyQuestUpdate.h>

struct DisconnectedEvent;
struct PartyLeftEvent;
struct UpdateEvent;

struct TESQuest;
struct RequestQuestUpdate;

/**
 * @brief Syncs quest starts, stops and stages across the party
 *
 * Every client runs the party's quests itself (scripts, scenes, dialogue), and only starts, stops and stages are
 * shared. Anything else a quest holds (aliases, script properties) only matches when the stage fragments produce it.
 *
 * Sending: TESQuestStartStopEvent and TESQuestStageEvent go out as RequestQuestUpdate, only in a party (outside one, an
 * update may prompt the overlay to suggest one). Failed starts, quests without stages and those in
 * kNonSyncableQuestIds are never sent, and the server drops None/Miscellaneous ones unless bEnableMiscQuestSync is set.
 *
 * Relaying: the server sends updates to the rest of the party as NotifyQuestUpdate. Members' scripts often reach the
 * same stage on their own, so it relays each once, from whoever reaches it first, until the quest next starts or stops.
 *
 * Applying: stages go through ScriptSetStage so their fragments run (and a stopped quest restarts), skipping those
 * already reached. Stops skip scripts. ScopedQuestOverride keeps the events this fires from being sent back.
 *
 * Scenes: setting a stage a playing scene waits on can end its phase early, skipping its lines and scripts and leaving
 * the quest stuck. So the leader, whose world the party follows, holds member stages that an upcoming phase checks and
 * queues later ones behind them. Members apply every stage right away.
 */
class QuestService final : public BSTEventSink<TESQuestStartStopEvent>,
                           BSTEventSink<TESQuestStageEvent>,
                           BSTEventSink<TESQuestStageItemDoneEvent>,
                           BSTEventSink<TESSceneEvent>,
                           BSTEventSink<TESSceneActionEvent>,
                           BSTEventSink<TESScenePhaseEvent>,
                           BSTEventSink<TESTopicInfoEvent>,
                           BSTEventSink<TESEnterBleedoutEvent>
{
public:
    QuestService(World&, entt::dispatcher&);
    ~QuestService() = default;

    static bool IsNonSyncableQuest(TESQuest* apQuest);
    static void DebugDumpQuests();
    static bool StopQuest(uint32_t aformId);
    static std::string CaptureQuestTargets(TESQuest* quest);

    struct HeldStages
    {
        // Member stages in the order accepted by the server
        Vector<uint16_t> Stages{};
        // A stage per log entry of the applied stages whose fragment hasn't run yet
        Vector<uint16_t> PendingItems{};
        std::chrono::steady_clock::time_point RetryAt{};
        std::string Targets;
        uint32_t LocalInstance{};
        bool CheckInstance{};
        bool BranchConflict{};
    };

private:
    bool FillQuestTargets(uint32_t form, uint32_t instance, RequestQuestUpdate& update) noexcept;
    std::map<uint32_t,std::pair<uint32_t,std::string>> m_lastQuestTargets;
    std::map<uint32_t,uint32_t> m_branchConflicts; // form -> current native instance
    void PollCampaign(const UpdateEvent&) noexcept;
    void OnCampaignRecord(const NotifyCampaignRecord&) noexcept;
    entt::scoped_connection m_campaignClock, m_campaignRecords;
    void PollJournal(const UpdateEvent&) noexcept;
    void OnJournalRecord(const NotifyQuestJournal&) noexcept;
    uint64_t m_journalStream{};
    uint32_t m_journalConfirmed{};
    std::chrono::steady_clock::time_point m_nextJournalPoll{};
    entt::scoped_connection m_journalClock, m_journalRecords;
    void OnConnected(const ConnectedEvent&) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void OnPartyLeft(const PartyLeftEvent&) noexcept;

    BSTEventResult OnEvent(const TESQuestStartStopEvent*, const EventDispatcher<TESQuestStartStopEvent>*) override;
    BSTEventResult OnEvent(const TESQuestStageEvent*, const EventDispatcher<TESQuestStageEvent>*) override;
    BSTEventResult OnEvent(const TESQuestStageItemDoneEvent*, const EventDispatcher<TESQuestStageItemDoneEvent>*) override;
    BSTEventResult OnEvent(const TESSceneEvent*, const EventDispatcher<TESSceneEvent>*) override;
    BSTEventResult OnEvent(const TESSceneActionEvent*, const EventDispatcher<TESSceneActionEvent>*) override;
    BSTEventResult OnEvent(const TESScenePhaseEvent*, const EventDispatcher<TESScenePhaseEvent>*) override;
    BSTEventResult OnEvent(const TESTopicInfoEvent*, const EventDispatcher<TESTopicInfoEvent>*) override;

    BSTEventResult OnEvent(const TESEnterBleedoutEvent*, const EventDispatcher<TESEnterBleedoutEvent>*) override;

    void ReceiveQuestProgress(const NotifyQuestUpdate&) noexcept;
    void OnQuestUpdate(const NotifyQuestUpdate&) noexcept;
    void HoldStage(TESQuest* apQuest, uint16_t aStage) noexcept;
    // The quest's held stages, while it has stages to apply or fragments to wait for. Drops the stages that can't be
    // applied anymore.
    HeldStages* GetHeldStages(uint32_t aQuestFormId, TESQuest* apQuest) noexcept;
    void NotifyOverlayOfQuestUpdate(uint32_t aFormId) noexcept;

    World& m_world;
    // Keyed by quest form ID
    Map<uint32_t, HeldStages> m_heldStages{};
    // From the first deferred stop through any events belonging to the next run of this quest.
    Map<uint32_t, Vector<NotifyQuestUpdate>> m_afterHeldStages{};
    Map<uint32_t, std::chrono::steady_clock::time_point> m_questRetryAt{};

    entt::scoped_connection m_joinedConnection;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_leftConnection;
    entt::scoped_connection m_questUpdateConnection;
};
