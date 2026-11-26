#include <TiltedOnlinePCH.h>

#include "World.h"

#include <Services/DiscoveryService.h>
#include <Services/InputService.h>
#include <Services/TransportService.h>
#include <Services/RunnerService.h>
#include <Services/ImguiService.h>
#include <Services/PapyrusService.h>
#include <Services/DiscordService.h>
#include <Services/ObjectService.h>
#include <Services/QuestService.h>
#include <Services/ActorValueService.h>
#include <Services/InventoryService.h>
#include <Services/MagicService.h>
#include <Services/CommandService.h>
#include <Services/CalendarService.h>
#include <Services/StringCacheService.h>
#include <Services/PlayerService.h>
#include <Services/CombatService.h>
#include <Services/WeatherService.h>
#include <Services/MapService.h>

#include <Events/PreUpdateEvent.h>
#include <Events/UpdateEvent.h>

#include <ModCompat/BehaviorVar.h>  

World::World()
    : m_runner(m_dispatcher)
    , m_transport(*this, m_dispatcher)
    , m_modSystem(m_dispatcher)
    , m_lastFrameTime{std::chrono::high_resolution_clock::now()}
{
    m_gameThreadId=GetCurrentThreadId();
    ctx().emplace<ImguiService>();
    ctx().emplace<DiscoveryService>(*this, m_dispatcher);
    ctx().emplace<OverlayService>(*this, m_transport, m_dispatcher);
    ctx().emplace<InputService>(ctx().at<OverlayService>());
    ctx().emplace<CharacterService>(*this, m_dispatcher, m_transport);
    ctx().emplace<DebugService>(m_dispatcher, *this, m_transport, ctx().at<ImguiService>());
    ctx().emplace<PapyrusService>(m_dispatcher);
    ctx().emplace<DiscordService>(m_dispatcher);
    ctx().emplace<ObjectService>(*this, m_dispatcher, m_transport);
    ctx().emplace<CalendarService>(*this, m_dispatcher, m_transport);
    ctx().emplace<QuestService>(*this, m_dispatcher);
    ctx().emplace<PartyService>(*this, m_dispatcher, m_transport);
    ctx().emplace<ActorValueService>(*this, m_dispatcher, m_transport);
    ctx().emplace<InventoryService>(*this, m_dispatcher, m_transport);
    ctx().emplace<MagicService>(*this, m_dispatcher, m_transport);
    ctx().emplace<CommandService>(*this, m_transport, m_dispatcher);
    ctx().emplace<PlayerService>(*this, m_dispatcher, m_transport);
    ctx().emplace<StringCacheService>(m_dispatcher);
    ctx().emplace<CombatService>(*this, m_transport, m_dispatcher);
    ctx().emplace<WeatherService>(*this, m_transport, m_dispatcher);
    ctx().emplace<MapService>(*this, m_dispatcher, m_transport);

    BehaviorVar::Get()->Init();
}

#include <Services/CampaignCheckpointClient.h>
#include <Services/HomeOriginalOverlay.h>
#include <Services/HomeGuestSave.h>

World::~World() = default;

bool World::BeginCheckpointWrite() noexcept
{
    if (!m_saveLoadMutex.try_lock()) return false;
    if (GetCurrentThreadId()!=m_gameThreadId||m_inWorldFrame||m_preparingLoad||IsLoadingSave())
    {m_saveLoadMutex.unlock();return false;}
    m_inWorldFrame=true;return true;
}
void World::EndCheckpointWrite() noexcept
{
    m_inWorldFrame=false;m_saveLoadMutex.unlock();
}
bool World::BeginSaveLoad() noexcept
{
    // Never wait on a frame that might itself be waiting for the loading thread.
    std::unique_lock lock(m_saveLoadMutex,std::try_to_lock);
    if (!lock.owns_lock() || m_inWorldFrame || m_preparingLoad)
    {
        m_disconnectBeforeLoad.store(true,std::memory_order_release);
        spdlog::warn("STM save load deferred: world update/teardown in progress; retry after disconnect");
        return false;
    }
    if (IsLoadingSave())
    {
        // Load_Impl can invoke the lower load routine, including on a worker.
        m_saveLoadDepth.fetch_add(1,std::memory_order_acq_rel);
        return true;
    }
    const bool gameThread=GetCurrentThreadId()==m_gameThreadId;
    if (!gameThread && m_transport.HasWorldSession())
    {
        m_disconnectBeforeLoad.store(true,std::memory_order_release);
        spdlog::warn("STM save load deferred: active session must disconnect on game thread; retry load");
        return false;
    }
    HomeGuestSave::Cancel();
    CampaignCheckpointClient::ForgetLoaded();
    m_loadedWorld=m_loadFailed=m_successfulLoadPending=false;
    m_saveLoadDepth.store(1,std::memory_order_release);
    m_transport.SuspendForSaveLoad();
    if (gameThread)
    {
        m_preparingLoad=true;
        // Client::Close synchronously delivers the authenticated disconnect
        // while the old forms still exist. DNS cancellation is asynchronous.
        m_transport.CloseWorld();
        ResetWorldTracking();
        m_preparingLoad=false;
    }
    else
    {
        // No authenticated session: do not touch the registry/engine on a worker.
        // The next game frame discards only tracking state, before discovery.
        m_resetAfterLoad=true;
    }
    spdlog::info("STM save load begin (game thread={})",gameThread);
    return true;
}

void World::EndSaveLoad(bool success,bool actualWorldLoad) noexcept
{
    // Serialize the zero-depth transition with BeginSaveLoad.
    std::lock_guard lock(m_saveLoadMutex);
    if (!success) m_loadFailed=true;
    if (success && actualWorldLoad) m_loadedWorld=true;
    const auto previous=m_saveLoadDepth.fetch_sub(1,std::memory_order_acq_rel);
    if (previous==1)
    {
        m_successfulLoadPending=m_loadedWorld && !m_loadFailed;
        spdlog::info("STM save load returned; completed world load={}",m_successfulLoadPending);
    }
}

void World::ResetWorldTracking() noexcept
{
    // This is tracking state, not a request to resolve/delete TESForms.
    // AtTitle suppresses component-removal observers that query live actors.
    m_dispatcher.clear();
    clear<LocalComponent>();
    clear();
    ctx().at<DiscoveryService>().ResetWorldTracking();
    ctx().at<CharacterService>().ResetWorldTracking();
    ctx().at<MagicService>().ResetWorldTracking();
    ctx().at<PlayerService>().ResetWorldTracking();
    m_dispatcher.clear();
}

void World::Update() noexcept
{
    HomeGuestSave::Pump(*this);
    std::unique_lock lock(m_saveLoadMutex,std::try_to_lock);
    if (!lock.owns_lock() || IsLoadingSave() || m_inWorldFrame) return;
    m_inWorldFrame=true;
    struct FrameGuard { bool& Active; ~FrameGuard(){ Active=false; } } frame{m_inWorldFrame};
    m_gameThreadId=GetCurrentThreadId();
    if (m_disconnectBeforeLoad.exchange(false,std::memory_order_acq_rel) && m_transport.HasWorldSession())
        m_transport.CloseWorld();
    if (m_resetAfterLoad)
    {
        ResetWorldTracking();
        m_resetAfterLoad=false;
    }
    const auto cNow = std::chrono::high_resolution_clock::now();
    const auto cDelta = cNow - m_lastFrameTime;
    m_lastFrameTime = cNow;

    const auto cDeltaSeconds = std::chrono::duration_cast<std::chrono::duration<double>>(cDelta).count();

    // Check the title boundary BEFORE discovery or queued game operations.
    // Keep pumping transport so a cancelled DNS/connection can finish closing.
    const bool wasSuspended=m_transport.AtTitle();
    if (!m_transport.PrepareWorldFrame())
    {
        if (!wasSuspended && m_transport.AtTitle())
        {HomeGuestSave::Cancel();CampaignCheckpointClient::ForgetLoaded();ResetWorldTracking();}
        m_runner.OnUpdate(UpdateEvent(cDeltaSeconds)); // suspended tickets are discarded
        m_transport.Update();
        return;
    }
    if (m_successfulLoadPending)
    {
        HomeOriginalOverlay::ForgetReplacedWorld();
        CampaignCheckpointClient::CompleteLoad();
        GetOverlayService().RecheckTradeAfterLoad();
        m_successfulLoadPending=false;
    }
    m_dispatcher.trigger(PreUpdateEvent(cDeltaSeconds));

    // Force run this before so we get the tasks scheduled to run
    m_runner.OnUpdate(UpdateEvent(cDeltaSeconds));
    m_dispatcher.trigger(UpdateEvent(cDeltaSeconds));
}

RunnerService& World::GetRunner() noexcept
{
    return m_runner;
}

TransportService& World::GetTransport() noexcept
{
    return m_transport;
}

ModSystem& World::GetModSystem() noexcept
{
    return m_modSystem;
}

uint64_t World::GetTick() const noexcept
{
    return m_transport.GetClock().GetCurrentTick();
}

void World::Create() noexcept
{
    if (!entt::locator<World>::has_value())
    {
        entt::locator<World>::emplace();
    }
}

bool World::Exists() noexcept
{
    return entt::locator<World>::has_value();
}

World& World::Get() noexcept
{
    return entt::locator<World>::value();
}
