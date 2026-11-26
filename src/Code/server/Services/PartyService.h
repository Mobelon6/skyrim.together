#pragma once

#include <Events/PacketEvent.h>
#include <QuestRunMember.h>

#include <Structs/DialogueAnchor.h>
#include <chrono>
#include <map>
#include <set>
struct World;
struct UpdateEvent;
struct PlayerJoinEvent;
struct PlayerLeaveEvent;
struct PartyInviteRequest;
struct PartyAcceptInviteRequest;
struct PartyLeaveRequest;
struct NotifyPartyInfo;
struct PartyCreateRequest;
struct PartyChangeLeaderRequest;
struct PartyKickRequest;

/**
 * @brief Manages every party in the server.
 */
struct PartyService
{
    struct Party
    {
        uint32_t LeaderPlayerId;
        Vector<Player*> Members;
        GameId CachedWeather{};
        struct QuestRun
        {
            TiltedPhoques::Set<uint16_t> Stages;
            Vector<uint16_t> OrderedStages;
            uint32_t Generation{1};
            bool Stopped = false;
            bool Started = false;
            bool CompletedRunOnce = false;
            std::string Targets;
            QuestRunMember::Members Members;
        };
        std::map<std::string,QuestRun> TargetQuestRuns; // same QUST, different actual targets

        struct Conversation
        {
            DialogueAnchor Anchor{};
            uint32_t Encounter{}; // retained by a verified continuation with the same participant
            std::set<uint32_t> Listeners;
            std::chrono::steady_clock::time_point Expires{};
            bool Closed{}, MayResume{}, Settlement{}, OpponentFinished{};
            std::chrono::steady_clock::time_point ChoiceExpires{};
            std::set<uint32_t> ChoiceListeners; // capture who had joined when that actual option was selected
            bool SharesQuest(const GameId& quest, std::chrono::steady_clock::time_point now) const noexcept
            {
                return Anchor.Choice && Anchor.TopicInfo && Anchor.TopicQuest == quest &&
                    now < ChoiceExpires && !ChoiceListeners.empty();
            }
        };
        struct Follower
        {
            GameId Npc{};
            uint32_t Owner{};
            std::chrono::steady_clock::time_point Expires{};
            bool Confirmed{};
            uint32_t Nonce{};
            bool Prepared{};
            uint32_t Session{};
        };
        std::map<uint32_t, Conversation> Talks;
        std::map<uint32_t, Conversation> Brawls;
        Map<GameId, QuestRun> QuestRuns{};
        uint64_t JournalStream{};
        uint32_t JournalCommitted{};
        Vector<String> JournalRows;
    };

    auto& Followers() noexcept { return m_followers; }
    auto& HireTalks() noexcept { return m_hireTalks; }

    PartyService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~PartyService() noexcept = default;

    TP_NOCOPYMOVE(PartyService);

    static bool WithinSyncRange(World&, Player&, Player&) noexcept;
    const Party* GetById(uint32_t aId) const noexcept;
    bool IsPlayerInParty(Player* const apPlayer) const noexcept;
    bool IsPlayerLeader(const Player* const apPlayer) const noexcept;
    Party* GetPlayerParty(Player* const apPlayer) noexcept;

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept;
    void OnPartyInvite(const PacketEvent<PartyInviteRequest>& acPacket) noexcept;
    void OnPartyAcceptInvite(const PacketEvent<PartyAcceptInviteRequest>& acPacket) noexcept;
    void OnPartyLeave(const PacketEvent<PartyLeaveRequest>& acPacket) noexcept;
    void OnPartyCreate(const PacketEvent<PartyCreateRequest>& acPacket) noexcept;
    void OnPartyChangeLeader(const PacketEvent<PartyChangeLeaderRequest>& acPacket) noexcept;
    void OnPartyKick(const PacketEvent<PartyKickRequest>& acPacket) noexcept;
    void RemovePlayerFromParty(Player* apPlayer) noexcept;

    void BroadcastPlayerList(Player* apPlayer = nullptr) const noexcept;
    void BroadcastPartyInfo(uint32_t aPartyId) const noexcept;

private:
    // Session-scoped hire leases are independent of party membership and simulation ownership.
    std::map<uint64_t, Party::Follower> m_followers;
    std::map<uint32_t, Party::Conversation> m_hireTalks;
    World& m_world;

    TiltedPhoques::Map<uint32_t, Party> m_parties;
    uint32_t m_nextId{0};
    std::map<std::pair<uint32_t, uint32_t>, bool> m_peerRange;
    uint64_t m_rangeTick{};
    uint64_t m_nextInvitationExpire{0};

    entt::scoped_connection m_updateEvent;
    entt::scoped_connection m_playerJoinConnection;
    entt::scoped_connection m_playerLeaveConnection;
    entt::scoped_connection m_partyInviteConnection;
    entt::scoped_connection m_partyAcceptInviteConnection;
    entt::scoped_connection m_partyLeaveConnection;
    entt::scoped_connection m_partyCreateConnection;
    entt::scoped_connection m_partyChangeLeaderConnection;
    entt::scoped_connection m_partyKickConnection;

    void SendPartyJoinedEvent(Party& aParty, Player* aPlayer) noexcept;
};
