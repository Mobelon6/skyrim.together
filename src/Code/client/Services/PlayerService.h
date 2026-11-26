#pragma once

struct World;
struct TransportService;

struct UpdateEvent;
struct ConnectedEvent;
struct DisconnectedEvent;
struct ServerSettings;
struct GridCellChangeEvent;
struct CellChangeEvent;
struct PlayerDialogueEvent;
struct PlayerLevelEvent;
struct PartyJoinedEvent;
struct PartyLeftEvent;

struct NotifyPlayerRespawn;

/**
 * @brief Handles logic related to the local player.
 */
struct PlayerService
{
    PlayerService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~PlayerService() noexcept = default;

    TP_NOCOPYMOVE(PlayerService);
    void ResetWorldTracking() noexcept
    {
        m_respawnTimerStarted=m_knockdownStart=m_godmodeStart=false;
        m_respawnDeadline=m_knockdownDeadline=m_godmodeDeadline={};
        m_cachedMainSpellId=m_cachedSecondarySpellId=m_cachedPowerId=0;
        m_dialogueInfo=m_dialogueQuest=m_closedSpeaker=m_dialogueHandle=0;
        m_dialogueSession=m_dialogueChoice=0;
        m_gameInitiated=false;
        m_dialogueText.clear();
        m_npcSubtitle.clear();
        m_closedUntil=m_nextDialogueUpdate=m_dialogueTextUntil={};
    }

    uint32_t ActiveDialogueSession(uint32_t form) noexcept;

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnConnected(const ConnectedEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;
    void OnServerSettingsReceived(const ServerSettings& acSettings) noexcept;
    void OnNotifyPlayerRespawn(const NotifyPlayerRespawn& acMessage) const noexcept;
    void OnGridCellChangeEvent(const GridCellChangeEvent& acEvent) const noexcept;
    void OnCellChangeEvent(const CellChangeEvent& acEvent) const noexcept;
    void OnPlayerDialogueEvent(const PlayerDialogueEvent& acEvent) noexcept;
    void UpdateDialogue(const PlayerDialogueEvent* apChoice = nullptr) noexcept;
    void OnPlayerLevelEvent(const PlayerLevelEvent& acEvent) const noexcept;
    void OnPartyJoinedEvent(const PartyJoinedEvent& acEvent) noexcept;
    void OnPartyLeftEvent(const PartyLeftEvent& acEvent) noexcept;

private:
    uint32_t m_dialogueInfo{}, m_dialogueQuest{}, m_closedSpeaker{};
    std::chrono::steady_clock::time_point m_closedUntil{};
    bool m_gameInitiated{};
    uint32_t m_dialogueHandle{};
    uint32_t m_dialogueSession{};
    uint32_t m_dialogueChoice{};
    String m_dialogueText, m_npcSubtitle;
    std::chrono::steady_clock::time_point m_nextDialogueUpdate{};
    std::chrono::steady_clock::time_point m_dialogueTextUntil{};

    /**
     * @brief Run the respawn timer, and if it hits 0, respawn the player.
     */
    void RunRespawnUpdates() noexcept;
    void RunPostDeathUpdates() noexcept;
    /**
     * @brief Make sure difficulty doesn't get changed while connected
     */
    void RunDifficultyUpdates() const noexcept;
    void RunLevelUpdates() const noexcept;
    void RunBeastFormDetection() const noexcept;

    void ToggleDeathSystem(bool aSet) noexcept;

    World& m_world;
    entt::dispatcher& m_dispatcher;
    TransportService& m_transport;

    std::chrono::steady_clock::time_point m_respawnDeadline;
    bool m_respawnTimerStarted=false;
    int32_t m_serverDifficulty = 6;
    int32_t m_previousDifficulty = 6;

    bool m_isDeathSystemEnabled = false;

    bool m_knockdownStart = false;
    std::chrono::steady_clock::time_point m_knockdownDeadline;

    bool m_godmodeStart = false;
    std::chrono::steady_clock::time_point m_godmodeDeadline;

    uint32_t m_cachedMainSpellId = 0;
    uint32_t m_cachedSecondarySpellId = 0;
    uint32_t m_cachedPowerId = 0;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_settingsConnection;
    entt::scoped_connection m_notifyRespawnConnection;
    entt::scoped_connection m_gridCellChangeConnection;
    entt::scoped_connection m_cellChangeConnection;
    entt::scoped_connection m_playerDialogueConnection;
    entt::scoped_connection m_playerLevelConnection;
    entt::scoped_connection m_partyJoinedConnection;
    entt::scoped_connection m_partyLeftConnection;
};
