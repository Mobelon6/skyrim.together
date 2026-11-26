#pragma once

#include <SpatialEpoch.h>
#include "Events/ConnectedEvent.h"
#include "Events/DisconnectedEvent.h"

#include <atomic>
#include <Client.hpp>

struct ImguiService;
struct UpdateEvent;
struct ClientMessage;
struct AuthenticationResponse;
struct NotifySettingsChange;

struct World;

using TiltedPhoques::Client;

/**
 * @brief Handles communication with the server.
 */
struct TransportService : Client
{
    TransportService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~TransportService() noexcept = default;

    TP_NOCOPYMOVE(TransportService);

    bool Send(const ClientMessage& acMessage, TiltedPhoques::EPacketFlags aPacketFlags = TiltedPhoques::kReliable) const noexcept;

    void OnConsume(const void* apData, uint32_t aSize) override;
    void OnConnected() override;
    void OnDisconnected(EDisconnectReason aReason) override;
    void OnUpdate() override;

    // Main-thread entry points for world and connection lifecycle.
    bool ConnectWorld(const std::string& endpoint,const std::string& password) noexcept;
    void CloseWorld() noexcept;
    // Called under World's load/frame exclusion; only atomic gates are changed.
    void SuspendForSaveLoad() noexcept;
    bool HasWorldSession() const noexcept { return m_connected || m_connectionRequested; }
    bool PrepareWorldFrame() noexcept;
    bool AtTitle() const noexcept {return m_title.load(std::memory_order_acquire);}
    [[nodiscard]] bool IsOnline() const noexcept { return m_connected && !AtTitle(); }
    void SetServerPassword(const std::string& acPassword) noexcept { m_serverPassword = acPassword; }
    const uint32_t& GetLocalPlayerId() const noexcept { return m_localPlayerId; }

    // Times below are in ms on the networking library's local clock, which keeps running while the game thread is stalled

    // When the message being handled arrived from the network, it may have waited out a long frame since.
    // Only meaningful while a received message is being handled.
    [[nodiscard]] double GetMessageReceiveTime() const noexcept;
    // When received messages were last picked up, everything that arrived before then has been handled
    [[nodiscard]] double GetLastPollTime() const noexcept { return m_lastPollTime; }

protected:
    // Event handlers
    void HandleUpdate(const UpdateEvent& acEvent) noexcept;
    void HandleConnected(const ConnectedEvent& acEvent) noexcept;
    void HandleDisconnected(const DisconnectedEvent& acEvent) noexcept;

    // Packet handlers
    void HandleAuthenticationResponse(const AuthenticationResponse& acMessage) noexcept;
    void HandleNotifySettingsChange(const NotifySettingsChange& acMessage) noexcept;

private:
    World& m_world;
    entt::dispatcher& m_dispatcher;
    bool m_connected;
    mutable SpatialEpoch::Counter m_spatialEpoch;
    std::atomic<bool> m_title{};
    bool m_connectionRequested{};
    String m_serverPassword{};
    uint32_t m_localPlayerId;
    double m_lastPollTime{};

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_sendServerMessageConnection;
    entt::scoped_connection m_settingsChangeConnection;
    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
    std::function<void(UniquePtr<ServerMessage>&)> m_messageHandlers[kServerOpcodeMax];
};
