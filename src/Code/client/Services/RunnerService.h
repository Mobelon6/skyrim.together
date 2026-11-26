#pragma once

#include <TiltedCore/TaskQueue.hpp>
#include <WorldTaskEpoch.h>

struct UpdateEvent;

/**
 * @brief Dispatches events.
 */
struct RunnerService
{
    RunnerService(entt::dispatcher& aDispatcher) noexcept;
    ~RunnerService() noexcept = default;

    TP_NOCOPYMOVE(RunnerService);

    /**
     * @brief Executes all queued events every frame.
     */
    void OnUpdate(const UpdateEvent& acUpdateEvent) noexcept;

    /**
     * @brief Queues an event for OnUpdate() to execute.
     */
    template <class T> void Trigger(T acEvent)
    {
        const auto ticket=m_epoch.Ticket();
        if (!m_epoch.Allows(ticket)) return;
        m_runner.Add([event = std::move(acEvent), this, ticket]() {
            if (m_epoch.Allows(ticket)) m_dispatcher.trigger(std::move(event));
        });
    }

    /**
     * @brief Queues a lambda for OnUpdate() to execute.
     */
    void Queue(std::function<void()> aFunctor) noexcept;
    bool SuspendWorld() noexcept {return m_epoch.Suspend();}
    bool ResumeWorld() noexcept {return m_epoch.Resume();}
    void InvalidateWorld() noexcept {m_epoch.Invalidate();}

private:
    entt::dispatcher& m_dispatcher;
    TiltedPhoques::TaskQueue m_runner;
    WorldTaskEpoch m_epoch;
};
