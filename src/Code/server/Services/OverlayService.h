#pragma once

#include <Messages/SendChatMessageRequest.h>

#include <Events/PacketEvent.h>
#include <Events/PlayerEnterWorldEvent.h>

struct World;
struct RequestPartyHome;
struct RequestPartyTrade;

#include <Messages/NotifyPartyTravel.h>
#include <chrono>
#include <map>

struct RequestCoopInteraction;
struct PartyTravelRequest;
struct UpdateEvent;
struct PlayerDialogueRequest;
struct TeleportRequest;
struct RequestPlayerHealthUpdate;

/**
 * @brief Dispatches UI events that modify the UI view of other cients.
 */
class OverlayService
{
public:
    OverlayService(World& aWorld, entt::dispatcher& aDispatcher);

protected:
    /**
     * @brief Applies regex on chat message and relays it to other clients.
     */
    void HandleChatMessage(const PacketEvent<SendChatMessageRequest>& acMessage) const noexcept;
    void HandlePlayerJoin(const PlayerEnterWorldEvent& acEvent) const noexcept;
    void OnPlayerDialogue(const PacketEvent<PlayerDialogueRequest>& acMessage) const noexcept;
    void OnTeleport(const PacketEvent<TeleportRequest>& acMessage) const noexcept;
    void OnPlayerHealthUpdate(const PacketEvent<RequestPlayerHealthUpdate>& acMessage) const noexcept;

private:
    void OnTrade(const PacketEvent<RequestPartyTrade>&) noexcept;
    void OnHome(const PacketEvent<RequestPartyHome>&) noexcept;
    entt::scoped_connection m_homeConnection;
    entt::scoped_connection m_tradeConnection;
    void OnInteraction(const PacketEvent<RequestCoopInteraction>&) noexcept;
    entt::scoped_connection m_interactionConnection;
    struct Ride
    {
        uint32_t party{}, number{};
        bool marker{};
        GameId originWorld{};
        glm::vec3 origin{};
        std::chrono::steady_clock::time_point expires{};
        std::map<uint32_t, bool> passengers; // false: invited, true: accepted
        NotifyPartyTravel destination{};
    };
    std::map<uint32_t, Ride> m_rides;
    void OnPartyTravel(const PacketEvent<PartyTravelRequest>& request) noexcept;
    void UpdateRides(const UpdateEvent&) noexcept;
    entt::scoped_connection m_rideConnection, m_tickRides;
    World& m_world;

    entt::scoped_connection m_chatMessageConnection;
    entt::scoped_connection m_playerDialogueConnection;
    entt::scoped_connection m_teleportConnection;
    entt::scoped_connection m_playerHealthConnection;
};
