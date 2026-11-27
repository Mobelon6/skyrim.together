#include <Services/WeatherService.h>

#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/PartyJoinedEvent.h>
#include <Events/PartyLeftEvent.h>

#include <Messages/RequestWeatherChange.h>
#include <Messages/NotifyWeatherChange.h>
#include <Messages/RequestCurrentWeather.h>

#include <Sky/Sky.h>
#include <Forms/TESWeather.h>

WeatherService::WeatherService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&WeatherService::OnUpdate>(this);
    m_disconnectConnection = aDispatcher.sink<DisconnectedEvent>().connect<&WeatherService::OnDisconnected>(this);
    m_partyJoinedConnection = aDispatcher.sink<PartyJoinedEvent>().connect<&WeatherService::OnPartyJoinedEvent>(this);
    m_partyLeftConnection = aDispatcher.sink<PartyLeftEvent>().connect<&WeatherService::OnPartyLeftEvent>(this);
    m_playerAddedConnection = m_world.on_destroy<WaitingFor3D>().connect<&WeatherService::OnWaitingFor3DRemoved>(this);
    m_playerRemovedConnection = m_world.on_destroy<PlayerComponent>().connect<&WeatherService::OnPlayerComponentRemoved>(this);
    m_weatherChangeConnection = aDispatcher.sink<NotifyWeatherChange>().connect<&WeatherService::OnWeatherChange>(this);
}

void WeatherService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    RunWeatherUpdates(acEvent.Delta);
}

void WeatherService::OnDisconnected(const DisconnectedEvent& acEvent) noexcept
{
    ToggleGameWeatherSystem(true);
}

void WeatherService::RefreshPartyWeather() noexcept
{
    if (!m_transport.IsOnline() || !m_world.GetPartyService().IsInParty())
    {
        ToggleGameWeatherSystem(true);
        return;
    }
    OnPartyJoinedEvent(PartyJoinedEvent(m_world.GetPartyService().IsLeader()));
}

void WeatherService::OnPartyJoinedEvent(const PartyJoinedEvent& acEvent) noexcept
{
    // Do not retain an override from the previous leader or party.
    ToggleGameWeatherSystem(true);
    if (!acEvent.IsLeader)
    {
        // TODO: why is this loop here? Party should always have a leader.
        auto view = m_world.view<PlayerComponent>();
        const auto& partyService = m_world.GetPartyService();

        for (auto entity : view)
        {
            const auto& playerComponent = view.get<PlayerComponent>(entity);
            if (playerComponent.Id == partyService.GetLeaderPlayerId())
            {
                ToggleGameWeatherSystem(false);
                break;
            }
        }
    }
    else
    {
        Sky* pSky = Sky::Get();
        if (!pSky)
            return;

        TESWeather* pWeather = pSky->GetWeather();
        if (!pWeather)
        {
            m_cachedWeatherId = 0;
            return;
        }

        // Potentially sets cached weather to map weather.
        // When the player closes the map, it'll send out the proper weather on the next update.
        m_cachedWeatherId = pWeather->formID;

        // This is the map weather, should not be synced.
        if (pWeather->formID == 0xA6858)
            return;

        RequestWeatherChange request{};

        auto& modSystem = m_world.GetModSystem();
        if (!modSystem.GetServerModId(pWeather->formID, request.Id))
        {
            spdlog::error("{}: weather server ID not found, form id: {:X}", __FUNCTION__, pWeather->formID);
            return;
        }

        m_transport.Send(request);
    }
}

void WeatherService::OnPartyLeftEvent(const PartyLeftEvent& acEvent) noexcept
{
    ToggleGameWeatherSystem(true);
}

// TODO: OnPlayerComponentAdded() instead? Does PlayerComponent exist already by then?
void WeatherService::OnWaitingFor3DRemoved(entt::registry& aRegistry, entt::entity aEntity) noexcept
{
    if (!m_transport.IsOnline()) return;
    const auto* pPlayerComponent = m_world.try_get<PlayerComponent>(aEntity);
    if (!pPlayerComponent)
        return;

    const auto& partyService = m_world.GetPartyService();
    if (!partyService.IsInParty() || partyService.IsLeader())
        return;

    if (partyService.GetLeaderPlayerId() == pPlayerComponent->Id)
        ToggleGameWeatherSystem(false);
}

void WeatherService::OnPlayerComponentRemoved(entt::registry& aRegistry, entt::entity aEntity) noexcept
{
    if (!m_transport.IsOnline()) return;
    const auto& playerComponent = m_world.get<PlayerComponent>(aEntity);

    const auto& partyService = m_world.GetPartyService();
    if (!partyService.IsInParty() || partyService.IsLeader())
        return;

    if (partyService.GetLeaderPlayerId() == playerComponent.Id)
        ToggleGameWeatherSystem(true);
}

void WeatherService::OnWeatherChange(const NotifyWeatherChange& acMessage) noexcept
{
    // A reply already in flight must not override regional weather after the
    // leader has left our loaded area, or after we become the leader.
    if (!m_followingWeather || !m_transport.IsOnline() ||
        !m_world.GetPartyService().IsInParty() || m_world.GetPartyService().IsLeader())
        return;
    auto& modSystem = m_world.GetModSystem();
    const uint32_t weatherId = modSystem.GetGameId(acMessage.Id);
    TESWeather* pWeather = Cast<TESWeather>(TESForm::GetById(weatherId));

    if (!pWeather)
    {
        spdlog::error("{}: weather not found, form id: {:X}", __FUNCTION__, acMessage.Id.ModId + acMessage.Id.BaseId);
        return;
    }

    if (auto* sky=Sky::Get())
    {
        sky->ForceWeather(pWeather);
        m_cachedWeatherId = weatherId;
    }
}

void WeatherService::RunWeatherUpdates(const double acDelta) noexcept
{
    if (!m_transport.IsOnline() || !m_world.GetPartyService().IsInParty()) return;
    Sky* pSky = Sky::Get();
    if (!pSky)
        return;

    TESWeather* pWeather = pSky->GetWeather();
    if (!pWeather)
    {
        if (m_world.GetPartyService().IsLeader())
            m_cachedWeatherId = 0;
        else
            SetCachedWeather();

        return;
    }

    // This is the map weather, should not be synced.
    if (pWeather->formID == 0xA6858)
        return;

    // Have to manually check each frame because there's no singular SetWeather being used in-game.
    if (pWeather->formID == m_cachedWeatherId)
        return;

    if (m_world.GetPartyService().IsLeader())
    {
        m_cachedWeatherId = pWeather->formID;

        RequestWeatherChange request{};

        auto& modSystem = m_world.GetModSystem();
        if (!modSystem.GetServerModId(pWeather->formID, request.Id))
        {
            spdlog::error("{}: weather server ID not found, form id: {:X}", __FUNCTION__, pWeather->formID);
            return;
        }

        m_transport.Send(request);
    }
    else
    {
        SetCachedWeather();
    }
}

void WeatherService::ToggleGameWeatherSystem(bool aToggle) noexcept
{
    m_followingWeather = !aToggle;
    m_cachedWeatherId = 0;
    if (aToggle)
    {
        if (auto* sky=Sky::Get()) sky->ReleaseWeatherOverride();
    }
    else
        m_transport.Send(RequestCurrentWeather());
}

void WeatherService::SetCachedWeather() noexcept
{
    if (!m_followingWeather || m_cachedWeatherId == 0)
        return;

    TESWeather* pWeather = Cast<TESWeather>(TESForm::GetById(m_cachedWeatherId));

    if (!pWeather)
    {
        spdlog::error("{}: weather not found, form id: {:X}", __FUNCTION__, m_cachedWeatherId);
        return;
    }

    if (auto* sky=Sky::Get()) sky->ForceWeather(pWeather);
}
