#pragma once
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>
#include <string>

struct World;
struct TESObjectCELL;

namespace HomeOriginalOverlay
{
enum class Result { Ready, Pending, Unavailable };
struct Fields
{
    bool Disabled{};
    uint32_t LockState{},LockLevel{},Open{};
    std::array<uint32_t,6> Pose{};
    bool Valid() const noexcept
    {
        if (LockState>2||LockLevel>255||(!LockState&&LockLevel)||
            Open>4) return false;
        for (auto v:Pose) if (!std::isfinite(std::bit_cast<float>(v))) return false;
        return true;
    }
    bool Same(const Fields& o) const noexcept
    {
        if (Disabled!=o.Disabled||LockState!=o.LockState||LockLevel!=o.LockLevel||Open!=o.Open) return false;
        for (size_t i=0;i<Pose.size();++i)
        {
            auto d=double(std::bit_cast<float>(Pose[i]))-std::bit_cast<float>(o.Pose[i]);
            if (i>=3) d=std::remainder(d,6.283185307179586);
            if (std::abs(d)>(i<3?0.001:0.0001)) return false;
        }
        return true;
    }
};
struct Delta
{
    uint32_t Ref{},Base{},Type{},Scale{};
    Fields Personal,Guest;
    bool ParentDriven{},Loose{},LiveLoose{},LiveDoor{};
};
// Retain the personal copy until readback proves restoration.
// A pending native operation is never submitted again on each frame.
class Transaction
{
public:
    bool Stage(std::vector<Delta> rows)
    {
        if (m_staged) return false;
        for (const auto& row:rows)
            if (!row.Ref||!row.Base||!row.Personal.Valid()||!row.Guest.Valid()||
                row.Personal.Open==2||row.Personal.Open==4||row.Guest.Open==2||row.Guest.Open==4||
                (row.Personal.Same(row.Guest)&&!row.Loose&&!row.LiveDoor) ||
                (row.LiveLoose&&!row.Loose) ||
                (row.LiveDoor&&(row.Type!=29||row.Loose||row.Personal.Disabled||row.Guest.Disabled||
                               !row.Personal.Open||!row.Guest.Open))) return false;
        m_rows=std::move(rows);m_staged=true;return true;
    }
    bool Dirty() const noexcept {return m_dirty.load(std::memory_order_acquire);}
    bool Staged() const noexcept {return m_staged;}
    template<class Read,class Write>
    Result Switch(bool guest,Read&& read,Write&& write)
    {
        if (!m_staged||m_failed) return Result::Unavailable;
        if (m_pending&&m_target!=guest)
        {
            const auto result=Switch(m_target,read,write);
            return result==Result::Ready?Result::Pending:result;
        }
        const auto fields=[](const Delta& row,bool target)->const Fields&
        {return target?row.Guest:row.Personal;};
        if (!m_pending&&m_guest!=guest)
        {
            for (const auto& row:m_rows)
            {
                const auto current=read(row);
                if (!current||!current->Valid()||!current->Same(fields(row,m_guest)))
                    return Result::Unavailable;
            }
            m_target=guest;m_pending=true;m_dirty.store(true,std::memory_order_release);
            for (const auto& row:m_rows)
                if (!row.Personal.Same(row.Guest)&&!write(row,fields(row,guest)))
                {m_failed=true;return Result::Unavailable;}
        }
        bool complete=true;
        for (const auto& row:m_rows)
        {
            const auto current=read(row);
            if (!current||!current->Valid())
            {m_failed=true;return Result::Unavailable;}
            if (!current->Same(fields(row,guest))) complete=false;
        }
        if (!complete)
        {
            if (!m_pending) {m_failed=true;return Result::Unavailable;}
            return Result::Pending;
        }
        m_guest=guest;m_pending=false;
        // Even a guest visit with zero field differences needs a save boundary.
        m_dirty.store(guest,std::memory_order_release);
        return Result::Ready;
    }
    // Stable original loose references are retained even when both worlds
    // initially agree. Only confirmed, irreversible ledger consumption may
    // change their guest target. Personal fields remain untouched.
    bool CanTake(uint32_t ref) const noexcept
    {
        if (!m_staged||!m_guest||m_pending||m_failed) return false;
        for (const auto& row:m_rows)
            if (row.Ref==ref) return row.LiveLoose&&!row.Guest.Disabled;
        return false;
    }
    template<class Read,class Write>
    Result RefreshHidden(const std::vector<std::pair<uint32_t,bool>>& balances,Read&& read,Write&& write)
    {
        if (!m_staged||!m_guest||m_failed||m_pending) return Result::Unavailable;
        std::vector<Delta*> changes;
        size_t looseCount=0;
        for (const auto& row:m_rows) if (row.Loose) ++looseCount;
        if (looseCount!=balances.size()) return Result::Unavailable;
        for (const auto& [ref,hidden]:balances)
        {
            Delta* row=nullptr;
            for (auto& item:m_rows) if (item.Ref==ref&&item.Loose) {row=&item;break;}
            if (!row) return Result::Unavailable;
            if (row->Guest.Disabled==hidden) continue;
            if (!hidden||!row->LiveLoose) return Result::Unavailable;
            const auto current=read(*row);
            if (!current||!current->Valid()||!current->Same(row->Guest)) return Result::Unavailable;
            for (auto* existing:changes) if (existing==row) return Result::Unavailable;
            changes.push_back(row);
        }
        if (changes.empty()) return Result::Ready;
        m_target=true;m_pending=true;
        for (auto* row:changes) row->Guest.Disabled=true;
        for (auto* row:changes)
            if (!write(*row,row->Guest)) {m_failed=true;return Result::Unavailable;}
        return Switch(true,read,write);
    }
    uint32_t DoorState(uint32_t ref) const noexcept
    {
        if (!m_staged||!m_guest||m_pending||m_failed) return 0;
        for (const auto& row:m_rows)
            if (row.Ref==ref && row.LiveDoor) return row.Guest.Open;
        return 0;
    }
    template<class Read,class Write>
    Result RefreshDoors(const std::vector<std::pair<uint32_t,uint32_t>>& states,Read&& read,Write&& write)
    {
        if (!m_staged||!m_guest||m_failed||m_pending) return Result::Unavailable;
        size_t count=0;
        for (const auto& row:m_rows) if (row.LiveDoor) ++count;
        if (count!=states.size()) return Result::Unavailable;
        std::vector<std::pair<Delta*,uint32_t>> changes;
        std::vector<uint32_t> seen;
        for (const auto& [ref,open]:states)
        {
            if (open!=1&&open!=3) return Result::Unavailable;
            for (auto id:seen) if (id==ref) return Result::Unavailable;
            seen.push_back(ref);
            Delta* row=nullptr;
            for (auto& item:m_rows) if (item.Ref==ref&&item.LiveDoor) {row=&item;break;}
            if (!row) return Result::Unavailable;
            const auto current=read(*row);
            if (!current||!current->Valid()||!current->Same(row->Guest)) return Result::Unavailable;
            if (row->Guest.Open!=open) changes.emplace_back(row,open);
        }
        if (changes.empty()) return Result::Ready;
        m_target=true;m_pending=true;
        for (auto [row,open]:changes) row->Guest.Open=open;
        for (auto [row,open]:changes)
        {
            (void)open;
            if (!write(*row,row->Guest)) {m_failed=true;return Result::Unavailable;}
        }
        return Switch(true,read,write);
    }
    bool Release() noexcept
    {
        if (Dirty()||m_pending||m_failed||m_guest) return false;
        ForgetReplacedWorld();return true;
    }
    void ForgetReplacedWorld() noexcept
    {
        m_rows.clear();m_staged=m_pending=m_target=m_guest=m_failed=false;
        m_dirty.store(false,std::memory_order_release);
    }
private:
    std::vector<Delta> m_rows;
    std::atomic_bool m_dirty{};
    bool m_staged{},m_pending{},m_target{},m_guest{},m_failed{};
};
// Game thread; attached original interior, before publishing guest scope.
bool Prepare(World&,TESObjectCELL&,uint64_t campaign,uint32_t slot,const std::string& root) noexcept;
Result Switch(World&,bool guest) noexcept;
Result RefreshLoose(World&,uint64_t campaign,uint32_t slot) noexcept;
bool CanTake(uint32_t ref) noexcept;
uint32_t DoorState(uint32_t ref) noexcept;
Result RefreshDoors(World&,uint64_t campaign,uint32_t slot) noexcept;
bool Dirty() noexcept;
bool Release() noexcept;
void ForgetReplacedWorld() noexcept;
}
