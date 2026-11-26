#pragma once

#include <Events/PacketEvent.h>
#include <Structs/GameId.h>

struct World;
struct UpdateEvent;
struct RequestQuestUpdate;
struct RequestQuestJournal;
struct RequestCampaignPull;
struct NotifyQuestUpdate;
struct Player;

/**
 * @brief Dispatch quest sync messages.
 *
 * This service is currently not in use.
 */
class QuestService
{
public:
    QuestService(World& aWorld, entt::dispatcher& aDispatcher);

private:
    void RecordQuest(Player*, const NotifyQuestUpdate&) noexcept;
    void OnCampaignPull(const PacketEvent<RequestCampaignPull>&) noexcept;
    entt::scoped_connection m_campaignRequests;
    void OnJournalRequest(const PacketEvent<RequestQuestJournal>&) noexcept;
    entt::scoped_connection m_journalRequests;
    void OnQuestChanges(const PacketEvent<RequestQuestUpdate>& aChanges) noexcept;

    World& m_world;

    entt::scoped_connection m_questUpdateConnection;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_joinConnection;
};
