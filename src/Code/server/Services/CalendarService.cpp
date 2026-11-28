#include <Services/CalendarService.h>

#include <GameServer.h>
#include <World.h>

#include <Events/PlayerJoinEvent.h>
#include <Events/UpdateEvent.h>

#include <Messages/ServerTimeSettings.h>
#include <Messages/RequestRestTime.h>
#include <cmath>

#include "Game/Player.h"

CalendarService::CalendarService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&CalendarService::OnUpdate>(this);
    m_restConnection = aDispatcher.sink<PacketEvent<RequestRestTime>>().connect<&CalendarService::OnRest>(this);
    m_joinConnection = aDispatcher.sink<PlayerJoinEvent>().connect<&CalendarService::OnPlayerJoin>(this);
}

void CalendarService::OnRest(const PacketEvent<RequestRestTime>& event) noexcept
{
    auto* player=event.pPlayer;
    auto* party=m_world.GetPartyService().GetPlayerParty(player);
    if (!player->GetCharacter()||!party) return;
    const auto& request=event.Packet;
    if(request.Generation!=m_restGeneration)
    {
        SendTimeResync(false);
        return;
    }
    const auto& cell=player->GetCellComponent();
    if(request.StartSleep)
    {
        if(request.Slept||!request.Bed||!cell.Cell||cell.WorldSpaceId) return;
        const auto* movement=m_world.try_get<MovementComponent>(*player->GetCharacter());
        if(!movement||m_sleepCohorts.count(player->GetId()))return;
        SleepCohort cohort;
        cohort.Connection=uint64_t(player->GetConnectionId());
        cohort.Bed=request.Bed;cohort.Cell=cell.Cell;
        ServerTimeSettings notice;
        notice.timeModel=m_dateTime.m_timeModel;notice.Generation=m_restGeneration;
        notice.SleepPhase=1;notice.Source=player->GetId();notice.Bed=request.Bed;
        for(auto* peer:party->Members)
        {
            if(peer==player||!peer->GetCharacter())continue;
            const auto& other=peer->GetCellComponent();
            if(other.Cell!=cell.Cell||other.WorldSpaceId)continue;
            const auto* pos=m_world.try_get<MovementComponent>(*peer->GetCharacter());
            if(!pos||glm::distance(pos->Position,movement->Position)>420.f||
                std::abs(pos->Position.z-movement->Position.z)>128.f)continue;
            cohort.Peers.push_back({peer->GetId(),uint64_t(peer->GetConnectionId()),peer->LocationEpoch()});
            peer->Send(notice);
        }
        m_sleepCohorts.emplace(player->GetId(),std::move(cohort));
        return;
    }
    const auto& target = request.Arrival;
    if (!std::isfinite(target.Time) || target.Time < 0 || target.Time >= 24 ||
        target.Month >= 12 || !target.Day || target.Day > DateTime::GetNumberOfDaysByMonthIndex(target.Month) ||
        target.Year > 9999)
        return;
    const auto& current = m_dateTime.m_timeModel;
    const auto dayIndex = [](const TimeModel& date) {
        int64_t days = int64_t(date.Year) * 365 + date.Day;
        for (unsigned m = 0; m < date.Month; ++m)
            days += DateTime::GetNumberOfDaysByMonthIndex(m);
        return days;
    };
    const double hours = (dayIndex(target) - dayIndex(current)) * 24.0 + target.Time - current.Time;
    if (hours > 0 && hours <= 24.1)
    {
        const float scale = current.TimeScale;
        m_dateTime.m_timeModel = target;
        m_dateTime.m_timeModel.TimeScale = scale;
        const auto completed=m_restGeneration++;
        const auto it=m_sleepCohorts.find(player->GetId());
        SleepCohort cohort;
        if(request.Slept&&it!=m_sleepCohorts.end()&&it->second.Connection==uint64_t(player->GetConnectionId())&&it->second.Bed==request.Bed)
            cohort=std::move(it->second);
        m_sleepCohorts.clear();
        SendTimeResync(true);
        // Only a peer present in the same room at native sleep start, with
        // unchanged connection/location, may consume the local eligibility snapshot.
        for(auto* peer:party->Members)
        {
            for(const auto& candidate:cohort.Peers)
            {
                if(peer->GetId()!=candidate.Id||uint64_t(peer->GetConnectionId())!=candidate.Connection||
                    peer->LocationEpoch()!=candidate.Location||peer->GetCellComponent().Cell!=cohort.Cell||
                    peer->GetCellComponent().WorldSpaceId)continue;
                ServerTimeSettings notice;
                notice.timeModel=m_dateTime.m_timeModel;notice.Generation=completed;
                notice.SleepPhase=2;notice.Source=player->GetId();notice.Bed=cohort.Bed;
                peer->Send(notice);
            }
        }
        return;
    }
    m_sleepCohorts.erase(player->GetId());
    SendTimeResync(false);
}

void CalendarService::OnUpdate(const UpdateEvent&) noexcept
{
    if (!m_lastTick)
        m_lastTick = GameServer::Get()->GetTick();

    auto now = GameServer::Get()->GetTick();

    // client got ahead, we wait
    if (now < m_lastTick)
        return;

    auto delta = now - m_lastTick;
    m_lastTick = now;
    m_dateTime.Update(delta);
}

void CalendarService::OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept
{
    ServerTimeSettings timeMsg;
    // the player with the furthest date is used
    bool playerHasFurthestTime = acEvent.PlayerTime.GetTimeInDays() > m_dateTime.GetTimeInDays();

    if (playerHasFurthestTime)
    {
        ++m_restGeneration;m_sleepCohorts.clear();
        // Note that this doesn't set timescale because the server config should set that.
        if (!m_timeSetFromFirstPlayer)
        {
            m_dateTime.m_timeModel.Time = acEvent.PlayerTime.m_timeModel.Time;
            m_timeSetFromFirstPlayer = true;
        }
        m_dateTime.m_timeModel.Day = acEvent.PlayerTime.m_timeModel.Day;
        m_dateTime.m_timeModel.Month = acEvent.PlayerTime.m_timeModel.Month;
        m_dateTime.m_timeModel.Year = acEvent.PlayerTime.m_timeModel.Year;
    }
    timeMsg.timeModel.TimeScale = m_dateTime.m_timeModel.TimeScale;
    timeMsg.timeModel.Time = m_dateTime.m_timeModel.Time;
    timeMsg.timeModel.Day = m_dateTime.m_timeModel.Day;
    timeMsg.timeModel.Month = m_dateTime.m_timeModel.Month;
    timeMsg.timeModel.Year = m_dateTime.m_timeModel.Year;

    timeMsg.Generation=m_restGeneration;
    if (playerHasFurthestTime)
        GameServer::Get()->SendToPlayers(timeMsg);
    else
        acEvent.pPlayer->Send(timeMsg);
}

bool CalendarService::SetTime(int aHours, int aMinutes, float aScale) noexcept
{
    m_dateTime.m_timeModel.TimeScale = aScale;

    if (aHours >= 0 && aHours <= 23 && aMinutes >= 0 && aMinutes <= 59)
    {
        // encode time as skyrim time
        auto minutes = static_cast<float>(aMinutes) * 0.17f;
        minutes = floor(minutes * 100) / 1000;
        m_dateTime.m_timeModel.Time = static_cast<float>(aHours) + minutes;

        ++m_restGeneration;m_sleepCohorts.clear();
        SendTimeResync();

        GameServer::Get()->GetWorld().GetScriptService().HandleSetTime(aHours, aMinutes, aScale);
        return true;
    }
    return false;
}

bool CalendarService::SetDate(int aDay, int aMonth, float aYear) noexcept
{
    if (aMonth >= 0 && aMonth < 12 && aYear >= 0 && aYear <= 999)
    {
        auto maxDays = m_dateTime.GetNumberOfDaysByMonthIndex(aMonth);
        if (aDay >= 0 && aDay < maxDays)
        {
            m_dateTime.m_timeModel.Day = aDay;
            m_dateTime.m_timeModel.Month = aMonth;
            m_dateTime.m_timeModel.Year = aYear;
            ++m_restGeneration;m_sleepCohorts.clear();
        SendTimeResync();
            return true;
        }
    }
    return false;
}

void CalendarService::SendTimeResync(bool restCompleted) noexcept
{
    ServerTimeSettings timeMsg;
    timeMsg.RestCompleted = restCompleted;
    timeMsg.Generation=m_restGeneration;
    timeMsg.timeModel.TimeScale = m_dateTime.m_timeModel.TimeScale;
    timeMsg.timeModel.Time = m_dateTime.m_timeModel.Time;
    timeMsg.timeModel.Day = m_dateTime.m_timeModel.Day;
    timeMsg.timeModel.Month = m_dateTime.m_timeModel.Month;
    timeMsg.timeModel.Year = m_dateTime.m_timeModel.Year;
    GameServer::Get()->SendToLoaded(timeMsg);
}

CalendarService::TTime CalendarService::GetTime() const noexcept
{
    const auto hour = floor(m_dateTime.m_timeModel.Time);
    const auto minutes = (m_dateTime.m_timeModel.Time - hour) / 17.f;

    const auto flatMinutes = static_cast<int>(ceil((minutes * 100.f) * 10.f));
    return {static_cast<int>(hour), flatMinutes};
}

CalendarService::TTime CalendarService::GetRealTime() noexcept
{
    const auto t = std::time(nullptr);
    int h = (t / 3600) % 24;
    int m = (t / 60) % 60;
    return {h, m};
}

CalendarService::TDate CalendarService::GetDate() const noexcept
{
    return {m_dateTime.m_timeModel.Day, m_dateTime.m_timeModel.Month, m_dateTime.m_timeModel.Year};
}

bool CalendarService::SetTimeScale(float aScale) noexcept
{
    if (aScale >= 0.f && aScale <= 1000.f)
    {
        m_dateTime.m_timeModel.TimeScale = aScale;

        ServerTimeSettings timeMsg;
        timeMsg.Generation=m_restGeneration;
        timeMsg.timeModel.TimeScale = m_dateTime.m_timeModel.TimeScale;
        timeMsg.timeModel.Time = m_dateTime.m_timeModel.Time;
        timeMsg.timeModel.Day = m_dateTime.m_timeModel.Day;
        timeMsg.timeModel.Month = m_dateTime.m_timeModel.Month;
        timeMsg.timeModel.Year = m_dateTime.m_timeModel.Year;
        GameServer::Get()->SendToPlayers(timeMsg);
        return true;
    }

    return false;
}
