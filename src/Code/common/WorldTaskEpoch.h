#pragma once
#include <atomic>
#include <cstdint>

// The low bit blocks submission and execution during title-screen teardown.
// A generation changes on both sides of the boundary, so an old queued task
// cannot become valid again when a new world is entered.
class WorldTaskEpoch
{
public:
    uint64_t Ticket() const noexcept {return m_epoch.load(std::memory_order_acquire);}
    bool Allows(uint64_t ticket) const noexcept {return !(ticket&1) && Ticket()==ticket;}
    bool Suspend() noexcept
    {
        auto value=Ticket();
        while (!(value&1))
            if (m_epoch.compare_exchange_weak(value,value+1,std::memory_order_acq_rel)) return true;
        return false;
    }
    bool Resume() noexcept
    {
        auto value=Ticket();
        while (value&1)
            if (m_epoch.compare_exchange_weak(value,value+1,std::memory_order_acq_rel)) return true;
        return false;
    }
    void Invalidate() noexcept {m_epoch.fetch_add(2,std::memory_order_acq_rel);}
private:
    std::atomic<uint64_t> m_epoch{};
};
