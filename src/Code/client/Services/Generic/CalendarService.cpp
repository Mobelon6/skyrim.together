#include <Services/CalendarService.h>

#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>
#include <Messages/ServerTimeSettings.h>
#include <World.h>

#include <Forms/TESObjectCELL.h>
#include <PlayerCharacter.h>
#include <TimeManager.h>
#include <Interface/UI.h>
#include <Messages/RequestRestTime.h>
#include <Services/TransportService.h>
#include <cmath>

constexpr float kTransitionSpeed = 5.f;

bool CalendarService::s_gameClockLocked = false;
bool CalendarService::s_restMenuOpen = false;

bool CalendarService::AllowGameTick() noexcept
{
    return !s_gameClockLocked || s_restMenuOpen;
}

CalendarService::CalendarService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport)
    : m_world(aWorld), m_transport(aTransport)
{
    m_timeUpdateConnection = aDispatcher.sink<ServerTimeSettings>().connect<&CalendarService::OnTimeUpdate>(this);
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&CalendarService::HandleUpdate>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&CalendarService::OnDisconnected>(this);
}

void CalendarService::OnTimeUpdate(const ServerTimeSettings& acMessage) noexcept
{
    if (acMessage.RestCompleted)
    {
        // Advance elapsed days only by the part the local engine has not already applied.
        auto* clock = TimeData::Get();
        if (clock && clock->GameDaysPassed && clock->GameHour && clock->GameDay &&
            clock->GameMonth && clock->GameYear && !s_restMenuOpen)
        {
            auto local = acMessage.timeModel;
            local.Day = static_cast<uint32_t>(clock->GameDay->f);
            local.Month = static_cast<uint32_t>(clock->GameMonth->f);
            local.Year = static_cast<uint32_t>(clock->GameYear->f);
            local.Time = clock->GameHour->f;
            if (local.Month < 12 && local.Day && local.Day <= 31)
            {
                const float days = DateTime(acMessage.timeModel).GetTimeInDays() - DateTime(local).GetTimeInDays();
                if (days > 0 && days <= 1.01f)
                    clock->GameDaysPassed->f += days;
            }
        }
        m_fadeTimer = kTransitionSpeed;
        m_lastTick = m_world.GetTick();
    }
    // disable the game clock
    ToggleGameClock(false);
    m_onlineTime.m_timeModel.TimeScale = acMessage.timeModel.TimeScale;
    m_onlineTime.m_timeModel.Time = acMessage.timeModel.Time;

    if (m_world.GetServerSettings().SyncPlayerCalendar || m_world.GetPartyService().IsInParty())
    {
        m_onlineTime.m_timeModel.Day = acMessage.timeModel.Day;
        m_onlineTime.m_timeModel.Month = acMessage.timeModel.Month;
        m_onlineTime.m_timeModel.Year = acMessage.timeModel.Year;
    }
    else
    {
        m_onlineTime.m_timeModel.Day = m_offlineTime.m_timeModel.Day;
        m_onlineTime.m_timeModel.Month = m_offlineTime.m_timeModel.Month;
        m_onlineTime.m_timeModel.Year = m_offlineTime.m_timeModel.Year;
    }
}

void CalendarService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    // Never read old world globals while the engine is tearing them down.
    s_gameClockLocked = false;
    s_restMenuOpen = false;
    m_fadeTimer = 0.f;
    m_lastTick = 0;
    m_lastLogTick = 0;
}

float CalendarService::TimeInterpolate(const TimeModel& aFrom, TimeModel& aTo) const
{
    const auto t = aTo.Time - aFrom.Time;
    if (t < 0.f)
    {
        const auto v = t + 24.f;
        // interpolate on the time difference, not the time
        const auto x = TiltedPhoques::Lerp(0.f, v, m_fadeTimer / kTransitionSpeed) + aFrom.Time;

        return TiltedPhoques::Mod(x, 24.f);
    }

    return TiltedPhoques::Lerp(aFrom.Time, aTo.Time, m_fadeTimer / kTransitionSpeed);
}

void CalendarService::ToggleGameClock(bool aEnable)
{
    auto* pGameTime = TimeData::Get();
    if (!pGameTime || !pGameTime->GameDay || !pGameTime->GameMonth ||
        !pGameTime->GameYear || !pGameTime->GameHour || !pGameTime->TimeScale)
    {
        s_gameClockLocked = false;
        return;
    }
    m_offlineTime.m_timeModel.Day = pGameTime->GameDay->f;
    m_offlineTime.m_timeModel.Month = pGameTime->GameMonth->f;
    m_offlineTime.m_timeModel.Year = pGameTime->GameYear->f;
    m_offlineTime.m_timeModel.Time = pGameTime->GameHour->f;
    m_offlineTime.m_timeModel.TimeScale = pGameTime->TimeScale->f;

    s_gameClockLocked = !aEnable;
}

void CalendarService::HandleUpdate(const UpdateEvent& aEvent) noexcept
{
    if (m_world.IsLoadingSave())
    {
        s_restMenuOpen = false;
        m_lastTick = 0;
        return;
    }
    if (s_gameClockLocked)
    {
        const auto updateDelta = static_cast<float>(aEvent.Delta);
        auto* pGameTime = TimeData::Get();
        if (!pGameTime || !pGameTime->GameDay || !pGameTime->GameMonth ||
            !pGameTime->GameYear || !pGameTime->GameHour || !pGameTime->GameDaysPassed ||
            !pGameTime->TimeScale)
        {
            m_lastTick=0;
            return;
        }

        auto* ui = UI::Get();
        const bool resting = ui && ui->GetMenuOpen("Sleep/Wait Menu") &&
            !m_world.IsLoadingSave() && m_world.GetPartyService().IsInParty();
        if (resting && !s_restMenuOpen)
        {
            m_restStartTime = m_onlineTime.m_timeModel;
            m_restStartDays = pGameTime->GameDaysPassed->f;
        }
        if (resting)
        {
            s_restMenuOpen = true;
            return;
        }
        if (s_restMenuOpen)
        {
            s_restMenuOpen = false;
            const float hours = (pGameTime->GameDaysPassed->f - m_restStartDays) * 24.f;
            if (!m_world.IsLoadingSave() && m_world.GetPartyService().IsInParty() &&
                std::isfinite(hours) && hours >= 0.5f && hours <= 24.5f)
            {
                DateTime arrival(m_restStartTime);
                arrival.m_timeModel.Time += std::round(hours);
                arrival.Update(0);
                RequestRestTime request;
                m_onlineTime = arrival;
                m_lastTick = m_world.GetTick();
                m_fadeTimer = kTransitionSpeed;
                request.Arrival = arrival.m_timeModel;
                m_transport.Send(request);
            }
        }

        if (!m_lastTick)
            m_lastTick = m_world.GetTick();

        const auto now = m_world.GetTick();

        // we got disconnected or the client got ahead of us
        if (now < m_lastTick)
        {
            m_lastTick=now;
            return;
        }

        const auto delta = now - m_lastTick;
        m_lastTick = now;

        m_onlineTime.Update(delta);
        pGameTime->TimeScale->f = m_onlineTime.m_timeModel.TimeScale;
        pGameTime->GameDay->f = m_onlineTime.m_timeModel.Day;
        pGameTime->GameMonth->f = m_onlineTime.m_timeModel.Month;
        pGameTime->GameYear->f = m_onlineTime.m_timeModel.Year;
        pGameTime->GameDaysPassed->f += m_onlineTime.GetDeltaTime(delta) * (1.f / 24.f);

        // time transition in
        if (m_fadeTimer < kTransitionSpeed)
        {
            pGameTime->GameHour->f = TimeInterpolate(m_offlineTime.m_timeModel, m_onlineTime.m_timeModel);
            m_fadeTimer += updateDelta;
        }
        else
            pGameTime->GameHour->f = m_onlineTime.m_timeModel.Time;
    }
}
