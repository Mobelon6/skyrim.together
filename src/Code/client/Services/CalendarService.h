#pragma once

#include <DateTime.h>
#include <Events/EventDispatcher.h>
#include <Games/Events.h>
#include <Structs/GameId.h>

struct ServerTimeSettings;
struct DisconnectedEvent;
struct World;
struct UpdateEvent;
struct TransportService;

/**
 * @brief Handles time sync.
 */
class CalendarService final : public BSTEventSink<TESActivateEvent>, BSTEventSink<TESSleepStartEvent>, BSTEventSink<TESSleepStopEvent>
{
public:
    CalendarService(World&, entt::dispatcher&, TransportService&);
    ~CalendarService();

    static bool AllowGameTick() noexcept;

private:
    BSTEventResult OnEvent(const TESActivateEvent*,const EventDispatcher<TESActivateEvent>*) override;
    BSTEventResult OnEvent(const TESSleepStartEvent*,const EventDispatcher<TESSleepStartEvent>*) override;
    BSTEventResult OnEvent(const TESSleepStopEvent*,const EventDispatcher<TESSleepStopEvent>*) override;
    void OnTimeUpdate(const ServerTimeSettings&) noexcept;
    void HandleUpdate(const UpdateEvent&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;

    void ToggleGameClock(bool aEnable);
    float TimeInterpolate(const TimeModel& aFrom, TimeModel& aTo) const;

    entt::scoped_connection m_timeUpdateConnection;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectedConnection;

    DateTime m_onlineTime;
    DateTime m_offlineTime;
    float m_fadeTimer = 0.f;
    static bool s_gameClockLocked;
    static bool s_restMenuOpen;
    float m_restStartDays{};
    TimeModel m_restStartTime;
    uint32_t m_generation{},m_restStartGeneration{},m_lastLocalSleepGeneration=UINT32_MAX;
    uint32_t m_activatedBed{};
    uint64_t m_bedTick{};
    GameId m_sleepBed{0,0};
    bool m_nativeSleep{},m_nativeSleepCompleted{};
    struct SleepGrant
    {
        uint32_t Generation{},Source{},Cell{};
        GameId Bed{0,0};
        bool Eligible{},Ready{};
    };
    SleepGrant m_sleepGrant;
    void ResetRestTracking() noexcept;

    uint64_t m_lastTick = 0;
    uint64_t m_lastLogTick = 0;
    World& m_world;
    TransportService& m_transport;
};
