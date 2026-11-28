#pragma once

#include <Events/PacketEvent.h>
#include <DateTime.h>
#include <Structs/GameId.h>
#include <map>
#include <vector>

struct World;
struct UpdateEvent;
struct PlayerJoinEvent;
struct RequestRestTime;

/**
 * @brief Manages time and date of the world.
 */
class CalendarService
{
public:
    CalendarService(World& aWorld, entt::dispatcher& aDispatcher);

    // we use these types for SOL
    // this is done this way because SOL
    // provides direct support for these
    using TTime = std::pair<int, int>;
    using TDate = std::tuple<int, int, int>;

    bool SetTime(int aHour, int aMinutes, float aScale) noexcept;
    bool SetDate(int aDay, int aMonth, float aYear) noexcept;

    // returns hours, minutes
    TTime GetTime() const noexcept;
    static TTime GetRealTime() noexcept;

    // returns dd/mm/yy
    TDate GetDate() const noexcept;

    float GetTimeScale() const noexcept { return m_dateTime.m_timeModel.TimeScale; }
    bool SetTimeScale(float aScale) noexcept;

private:
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnPlayerJoin(const PlayerJoinEvent&) noexcept;
    void OnRest(const PacketEvent<RequestRestTime>&) noexcept;
    entt::scoped_connection m_restConnection;
    void SendTimeResync(bool restCompleted = false) noexcept;

    struct SleepPeer {uint32_t Id{};uint64_t Connection{},Location{};};
    struct SleepCohort
    {
        uint64_t Connection{};
        GameId Bed{0,0},Cell{0,0};
        std::vector<SleepPeer> Peers;
    };
    uint32_t m_restGeneration{};
    std::map<uint32_t,SleepCohort> m_sleepCohorts;
    DateTime m_dateTime;
    uint64_t m_lastTick = 0;
    bool m_timeSetFromFirstPlayer = false;

    World& m_world;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_joinConnection;
};
