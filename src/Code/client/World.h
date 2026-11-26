#pragma once

#include <Services/RunnerService.h>
#include <Services/TransportService.h>
#include <Services/PartyService.h>
#include <Services/CharacterService.h>
#include <Services/OverlayService.h>
#include <Services/CharacterService.h>
#include <Services/MagicService.h>
#include <Services/DebugService.h>

#include <Systems/ModSystem.h>

#include <Structs/ServerSettings.h>

struct World : entt::registry
{
    World();
    ~World();

    void Update() noexcept;

    bool BeginCheckpointWrite() noexcept;
    void EndCheckpointWrite() noexcept;
    // Native save-load entry points. Only the game-update thread can tear
    // down an authenticated session; an offline load may run on a worker.
    bool BeginSaveLoad() noexcept;
    void EndSaveLoad(bool success,bool actualWorldLoad) noexcept;
    bool IsLoadingSave() const noexcept { return m_saveLoadDepth.load(std::memory_order_acquire)!=0; }
    static bool Exists() noexcept;

    RunnerService& GetRunner() noexcept;
    TransportService& GetTransport() noexcept;
    ModSystem& GetModSystem() noexcept;

    PartyService& GetPartyService() noexcept { return ctx().at<PartyService>(); }
    const PartyService& GetPartyService() const noexcept { return ctx().at<const PartyService>(); }
    CharacterService& GetCharacterService() noexcept { return ctx().at<CharacterService>(); }
    const CharacterService& GetCharacterService() const noexcept { return ctx().at<const CharacterService>(); }
    OverlayService& GetOverlayService() noexcept { return ctx().at<OverlayService>(); }
    const OverlayService& GetOverlayService() const noexcept { return ctx().at<const OverlayService>(); }
    DebugService& GetDebugService() noexcept { return ctx().at<DebugService>(); }
    const DebugService& GetDebugService() const noexcept { return ctx().at<const DebugService>(); }
    MagicService& GetMagicService() noexcept { return ctx().at<MagicService>(); }
    const MagicService& GetMagicService() const noexcept { return ctx().at<const MagicService>(); }

    auto& GetDispatcher() noexcept { return m_dispatcher; }

    const ServerSettings& GetServerSettings() const noexcept { return m_serverSettings; }
    void SetServerSettings(ServerSettings aServerSettings) noexcept { m_serverSettings = aServerSettings; }

    [[nodiscard]] uint64_t GetTick() const noexcept;

    static void Create() noexcept;
    [[nodiscard]] static World& Get() noexcept;

private:
    void ResetWorldTracking() noexcept;
    std::recursive_mutex m_saveLoadMutex;
    std::atomic<unsigned> m_saveLoadDepth{};
    std::atomic<bool> m_disconnectBeforeLoad{};
    uint32_t m_gameThreadId{};
    bool m_inWorldFrame{};
    bool m_preparingLoad{};
    bool m_resetAfterLoad{};
    bool m_loadedWorld{},m_loadFailed{},m_successfulLoadPending{};
    entt::dispatcher m_dispatcher;
    RunnerService m_runner;
    TransportService m_transport;
    ModSystem m_modSystem;
    ServerSettings m_serverSettings{};

    std::chrono::high_resolution_clock::time_point m_lastFrameTime;
};
