#include <TiltedOnlinePCH.h>

#include <Services/RunnerService.h>

#include <Events/UpdateEvent.h>

RunnerService::RunnerService(entt::dispatcher& aDispatcher) noexcept
    : m_dispatcher(aDispatcher)
{
}

void RunnerService::Queue(std::function<void()> aFunctor) noexcept
{
    const auto ticket=m_epoch.Ticket();
    if (!m_epoch.Allows(ticket)) return;
    m_runner.Add([this,ticket,fn=std::move(aFunctor)]() {
        if (m_epoch.Allows(ticket)) fn();
    });
}

void RunnerService::OnUpdate(const UpdateEvent& acUpdateEvent) noexcept
{
    m_runner.Drain();
}
