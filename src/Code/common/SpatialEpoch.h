#pragma once
#include <cstdint>
#include <limits>
#include <cmath>

namespace SpatialEpoch
{
// Per transport connection. It is a freshness fence, not an access grant.
// A reliable, independently authorized location report installs the epoch.
class Counter
{
public:
    uint64_t Value() const noexcept {return m_value;}
    bool Matches(uint64_t value) const noexcept {return value==m_value;}
    bool Advance(uint64_t value) noexcept
    {
        if (!value||value<=m_value) return false;
        m_value=value;return true;
    }
    bool Next() noexcept
    {
        if (m_value==std::numeric_limits<uint64_t>::max()) return false;
        return Advance(m_value+1);
    }
    void Reset() noexcept {m_value=0;}
private:
    uint64_t m_value{};
};
enum class Order { Newest, Historical, Duplicate };
struct Sample
{
    uint32_t Owner{};
    uint64_t Location{},Tick{};
    bool Seen{};
    // The caller checks the actual ownership and connection epochs first.
    Order Observe(uint32_t owner,uint64_t location,uint64_t tick) noexcept
    {
        if (Seen&&Owner==owner&&Location==location)
        {
            if (tick==Tick) return Order::Duplicate;
            if (tick<Tick) return Order::Historical;
        }
        Owner=owner;Location=location;Tick=tick;Seen=true;
        return Order::Newest;
    }
};
inline bool Finite(double x,double y,double z) noexcept
{return std::isfinite(x)&&std::isfinite(y)&&std::isfinite(z);}
}
