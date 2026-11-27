#pragma once
#include <map>
#include <deque>
#include <functional>
#include <tuple>
#include <string>

#include <include/internal/cef_ptr.h>
#include <mutex>
#include <Messages/NotifyPartyTravel.h>
#include <Messages/NotifyPlayerDialogue.h>

namespace TiltedPhoques
{
struct OverlayApp;
}

struct TESCameraState;
struct OverlayClient;
struct RenderSystemD3D11;
struct D3D11RenderProvider;
struct FormIdComponent;
struct World;
struct NotifyPartyHome;
struct NotifyPartyTrade;
struct Actor;
struct UpdateEvent;
struct ConnectedEvent;
struct DisconnectedEvent;
struct TransportService;
struct NotifyChatMessageBroadcast;
struct NotifyPlayerList;
struct NotifyPlayerJoined;
struct NotifyPlayerLeft;
struct NotifyPlayerDialogue;
struct ConnectionErrorEvent;
struct NotifyPlayerLevel;
struct NotifyPlayerCellChanged;
struct NotifyTeleport;
struct ActivateEvent;
struct NotifyPlayerHealthUpdate;
enum ChatMessageTypes;
struct PartyJoinedEvent;
struct PartyLeftEvent;

using TiltedPhoques::OverlayApp;

/**
 * @brief Controls the UI from the client.
 */
struct OverlayService
{
    OverlayService(World& aWorld, TransportService& transport, entt::dispatcher& aDispatcher);
    ~OverlayService() noexcept;

    TP_NOCOPYMOVE(OverlayService);

    void Create(RenderSystemD3D11* apRenderSystem) noexcept;

    void Render() noexcept;
    void Reset() const noexcept;
    void Reload() noexcept;

    void Initialize() noexcept;

    void SetActive(bool aActive) noexcept;
    [[nodiscard]] bool GetActive() const noexcept;

    void SetInGame(bool aInGame) noexcept;
    [[nodiscard]] bool GetInGame() const noexcept;

    void SetVersion(const std::string& acVersion);

    OverlayApp* GetOverlayApp() const noexcept { return m_pOverlay.get(); }

    void SendSystemMessage(const std::string& acMessage);

    void NoteActivation(uint32_t form) noexcept;
    uint32_t TradePeer(uint32_t form) const noexcept;
    void SpendRescue(uint32_t peerId,uint32_t fall) noexcept;
    void CloseRescue() noexcept;
    void DisplayRescue() noexcept;
    void ShowRescueReturn(bool visible) noexcept;
    bool TryTrade(uint32_t form) noexcept;
    void TradeAction(uint32_t session, int action, uint32_t row, uint32_t count) noexcept;
    bool TryHouseDoor(uint32_t form) noexcept;
    void ChooseHome(uint32_t token, int choice) noexcept;
    void UpdateHomes() noexcept;
    void ClearHomes() noexcept;
    void UpdateTrade() noexcept;
    void ClearTrade() noexcept;
    void RecheckTradeAfterLoad() noexcept;
    bool WasActivated(uint32_t form) const noexcept;
    static void RequestObserverExit() noexcept;
    static bool ObservingDialogue() noexcept;
    bool CaptionForObserver(uint32_t npc, const char* name, const char* text) noexcept;
    bool BeginHire(uint32_t npc, uint32_t info, int32_t index) noexcept;
    static void InstallReplicaLifeGuard(Actor* actor) noexcept;
    static bool FollowerBusy(uint32_t npc) noexcept;
    static void ObservePartyCombat(uint32_t form) noexcept;
    static void BeginCombatSettlement(uint32_t form) noexcept;
    static void CancelCombatSettlement(uint32_t form) noexcept;
    static void UpdateCombatSettlements(World& world) noexcept;
    static void ClearCombatSettlements() noexcept;
    static void CheckRevivedReplica(uint32_t form) noexcept;
    void BrawlBleedout(uint32_t form) noexcept;
    static bool NonlethalOpponent(uint32_t npc) noexcept;
    bool TryJoinConversation(uint32_t form) noexcept;
    void ConfirmMarkerTravel() noexcept;
    void SetTravelPreferences(bool npc, bool markers) noexcept { m_shareNpc = npc; m_shareMap = markers; }
    void QueueProgress(bool quest, uint32_t form, uint32_t speaker, uint32_t session, const char* label, std::function<void()> apply,
                       std::function<bool()> needed = {}, uint32_t subject = 0, uint32_t encounter = 0) noexcept;
    void QueueRewardChoice(uint32_t,const char*,std::string,std::string,std::function<void(bool)>,std::function<bool()>) noexcept;
    bool IsPersonalQuest(uint32_t form) const noexcept;
    void ResetSessionProgress() noexcept;
    void AnswerProgress(uint32_t token, bool accept) noexcept;
    void AnswerCarriage(uint32_t rider, uint32_t trip, bool yes) noexcept;
    void SetPlayerHealthPercentage(uint32_t aFormId) const noexcept;

protected:
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnConnectedEvent(const ConnectedEvent&) noexcept;
    void OnDisconnectedEvent(const DisconnectedEvent&) noexcept;
    void OnWaitingFor3DRemoved(entt::registry& aRegistry, entt::entity aEntity) const noexcept;
    void OnPlayerComponentRemoved(entt::registry& aRegistry, entt::entity aEntity) const noexcept;
    void OnConnectionError(const ConnectionErrorEvent& acConnectedEvent) const noexcept;
    void OnChatMessageReceived(const NotifyChatMessageBroadcast&) noexcept;
    void OnPlayerDialogue(const NotifyPlayerDialogue&) noexcept;
    void OnPlayerJoined(const NotifyPlayerJoined&) noexcept;
    void OnPlayerLeft(const NotifyPlayerLeft&) noexcept;
    void OnPlayerLevel(const NotifyPlayerLevel&) noexcept;
    void OnPlayerCellChanged(const NotifyPlayerCellChanged& acMessage) const noexcept;
    void OnNotifyTeleport(const NotifyTeleport& acMessage) noexcept;
    void OnNotifyPlayerHealthUpdate(const NotifyPlayerHealthUpdate& acMessage) noexcept;
    void OnPartyJoinedEvent(const PartyJoinedEvent& acEvent) noexcept;
    void OnPartyLeftEvent(const PartyLeftEvent& acEvent) noexcept;

private:
    void OnTrade(const NotifyPartyTrade&) noexcept;
    void OnHome(const NotifyPartyHome&) noexcept;
    entt::scoped_connection m_homeConnection;
    entt::scoped_connection m_tradeConnection;
    void OnInteraction(const struct NotifyCoopInteraction&) noexcept;
    void UpdateInteraction() noexcept;
    using ProgressKey = std::tuple<bool,uint32_t,uint32_t,uint32_t>;
    struct PendingProgress {
        ProgressKey Key;
        uint32_t Token;
        std::string Label;
        struct Change { std::function<void()> Apply; std::function<bool()> Needed; };
        std::vector<Change> Changes;
        std::chrono::steady_clock::time_point Ready{};
        std::string SelfName,PeerName;
        std::function<void(bool)> RewardChoice;
    };
    std::deque<PendingProgress> m_progress;
    std::map<ProgressKey,bool> m_progressAnswers;
    std::map<uint32_t,ProgressKey> m_currentQuest;
    uint32_t m_progressToken{};
    bool m_showingProgress{};
    struct FavorSettlement {
        uint32_t Npc{}, Stages{};
        bool Sent20{}, Sent200{};
        std::chrono::steady_clock::time_point Expires{};
    };
    std::map<uint32_t,FavorSettlement> m_favorSettlements;
    void ApplyFavorSettlements() noexcept;
    void ClearProgress() noexcept;
    void UpdateProgress() noexcept;
    void SetPeerRange(uint32_t peer, bool withinRange) noexcept;
    void ClearInteraction() noexcept;
    entt::scoped_connection m_interactionConnection;
    uint32_t m_activated{}, m_pendingForm{}, m_pendingInfo{}, m_nonce{}, m_pendingNonce{}, m_pendingSession{};
    std::chrono::steady_clock::time_point m_followerPoll{};
    int32_t m_pendingIndex{};
    uint32_t m_prepareForm{}, m_prepareSession{}, m_prepareNonce{}, m_dispatchedNonce{};
    bool m_hireReady{};
    std::chrono::steady_clock::time_point m_prepareAt{}, m_prepareSent{};
    std::map<uint32_t,uint64_t> m_peaceSession;
    std::chrono::steady_clock::time_point m_rejoinUntil{};
    void UpdateHireOffer() noexcept;
    uint64_t m_pendingJoin{};
    std::chrono::steady_clock::time_point m_activatedUntil{}, m_pollAt{}, m_pendingUntil{};
    struct OutcomeWatch
    {
        uint32_t Npc{}, Session{}, Rank{}, State{UINT32_MAX};
        bool Brawl{}, Settlement{}, FavorParticipant{};
        uint32_t FavorSent{};
        bool FactionReported{};
        std::chrono::steady_clock::time_point KneelingUntil{};
        uint32_t ReportedRank{};
        std::chrono::steady_clock::time_point Expires{};
    } m_watch;
    void LeaveConversation(uint64_t key) noexcept;
    void OnBoardCarriage(const ActivateEvent&) noexcept;
    void OnTravelNotice(const NotifyPartyTravel&) noexcept;
    void UpdateCarriage() noexcept;
    void UpdateMapTravel() noexcept;
    void ClearCarriage() noexcept;
    void CloseTravelPrompt() noexcept;
    uint32_t m_tripCounter{}, m_boardedTrip{}, m_mapTrip{};
    bool m_shareNpc{true}, m_shareMap{true}, m_mapConfirmed{}, m_mapWasOpen{};
    std::chrono::steady_clock::time_point m_mapExpires{}, m_mapRefresh{};
    glm::vec3 m_mapOrigin{};
    glm::vec3 m_departure{};
    std::chrono::steady_clock::time_point m_boardDeadline{}, m_offerDeadline{};
    std::optional<NotifyPartyTravel> m_offer;
    ::OverlayClient* m_gameOverlayClient{}; // owned by m_pOverlay
    bool m_promptOpen{}, m_ownsTravelOverlay{}, m_acceptedRide{};
    entt::scoped_connection m_boardConnection, m_noticeConnection;
    void RunDebugDataUpdates() noexcept;
    void UpdateDialogueHud() noexcept;
    void ClearDialogueHud() noexcept;
    void HideDialogueText() noexcept;
    void DrawDialogueMarker() noexcept;
    struct DialogueMarker
    {
        glm::vec3 Anchor{}, Eye{}, Npc{}, Speaker{};
        bool Active{}, HasNpc{}, HasSpeaker{};
    };
    void UpdateObserver(const DialogueMarker&, uint32_t speaker, uint32_t session, float distance) noexcept;
    void EndObserver() noexcept;
    uint64_t m_observedTalk{}, m_dismissedTalk{}, m_requestedTalk{}, m_invitedTalk{};
    uint32_t m_invitedNpc{}, m_invitedSpeaker{};
    void PublishInvitation(uint32_t speaker, uint32_t session, uint32_t npcForm, uint32_t speakerForm) noexcept;
    uint64_t m_rejoinKey{};
    bool ObserverInCombat(uint32_t speaker) const noexcept;
    void ObserverHud(bool active) noexcept;
    bool m_restoreThird{}, m_replyLeft{}, m_lastReplyLeft{};
    TESCameraState* m_observerCamera{};
    glm::vec3 m_observerEye{};
    float m_appliedPitch{}, m_appliedYaw{}, m_offsetPitch{}, m_offsetYaw{};
    // Publish plain world coordinates from the game update. Only the render thread reads the live camera.
    std::mutex m_markerMutex;
    DialogueMarker m_marker{};
    entt::scoped_connection m_drawDialogueConnection;
    struct PartyConversation
    {
        NotifyPlayerDialogue Last{};
        uint32_t LocalNpcForm{};
        std::chrono::steady_clock::time_point Expires{};
    };
    Map<uint32_t, PartyConversation> m_conversations;
    uint32_t m_lastSpeaker{}, m_lastSession{}, m_lastChoice{};
    int m_lastDialogueMode{-1};
    String m_lastDialogueText, m_sampledCaption;
    bool m_dialogueHudVisible{};

    void RunPlayerHealthUpdates() noexcept;

    CefRefPtr<OverlayApp> m_pOverlay{nullptr};
    TiltedPhoques::UniquePtr<D3D11RenderProvider> m_pProvider;

    World& m_world;
    TransportService& m_transport;

    std::optional<bool> m_gameMenuOpen;
    bool m_active = false;
    bool m_inGame = false;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_connectionErrorConnection;
    entt::scoped_connection m_cellChangeEventConnection;
    entt::scoped_connection m_chatMessageConnection;
    entt::scoped_connection m_playerDialogueConnection;
    entt::scoped_connection m_playerJoinedConnection;
    entt::scoped_connection m_playerLeftConnection;
    entt::scoped_connection m_playerAddedConnection;
    entt::scoped_connection m_playerRemovedConnection;
    entt::scoped_connection m_playerLevelConnection;
    entt::scoped_connection m_cellChangedConnection;
    entt::scoped_connection m_teleportConnection;
    entt::scoped_connection m_playerHealthConnection;
    entt::scoped_connection m_partyJoinedConnection;
    entt::scoped_connection m_partyLeftConnection;
};
