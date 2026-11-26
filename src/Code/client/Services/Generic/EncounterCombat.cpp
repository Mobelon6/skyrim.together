#include <TiltedOnlinePCH.h>
#include <World.h>
#include <Services/OverlayService.h>
#include <Services/PartyService.h>
#include <PlayerCharacter.h>
#include <Games/ActorExtension.h>
#include <Combat/CombatGroup.h>
#include <map>
#include <Games/TES.h>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
struct Settlement
{
    Clock::time_point deadline{}, next{}, quiet{};
    bool associated{}, localInvolved{};
    unsigned attempts{};
};
std::map<uint32_t, Settlement> settling;
std::map<uint32_t,Clock::time_point> guestInvolved;

struct Threats { bool participant{}, other{}; };
Threats Classify(Actor* actor, const std::vector<uint32_t>& allowed)
{
    Threats result;
    const auto classify = [&](uint32_t handle) {
        if (!handle) return;
        if (std::find(allowed.begin(), allowed.end(), handle) != allowed.end()) result.participant = true;
        else result.other = true;
    };
    if (auto* current = actor->GetCombatTarget()) classify(current->GetHandle().handle.iBits);
    if (auto* group = actor->GetCombatGroup())
    {
        group->lock.LockForRead();
        for (uint32_t i = 0; i < group->targets.length; ++i)
            classify(group->targets[i].targetHandle);
        group->lock.UnlockRead();
    }
    return result;
}

bool Settle(World& world, Actor* npc, Settlement& state, Clock::time_point now)
{
    if (!npc || npc->IsDead() || npc->IsDisabled()) return true;
    auto* me = PlayerCharacter::Get();
    if (!me || !npc->GetNiNode() || !me->GetNiNode()) return false;
    std::vector<Actor*> actors{me};
    std::vector<uint32_t> handles{me->GetHandle().handle.iBits};
    const auto& members = world.GetPartyService().GetPartyMembers();
    auto view = world.view<FormIdComponent, PlayerComponent>();
    for (auto entity : view)
    {
        const auto id = view.get<PlayerComponent>(entity).Id;
        if (std::find(members.begin(), members.end(), id) == members.end()) continue;
        auto* replica = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
        if (replica && replica != me && replica->GetNiNode())
        {
            actors.push_back(replica);
            handles.push_back(replica->GetHandle().handle.iBits);
        }
    }
    const auto theirs = Classify(npc, handles);
    const std::vector<uint32_t> subject{npc->GetHandle().handle.iBits};
    bool involved = theirs.participant;
    for (auto* actor : actors) involved |= Classify(actor, subject).participant;
    state.associated |= involved;
    state.localInvolved |= Classify(me,subject).participant || npc->GetCombatTarget()==me;
    bool otherAttacker=false;
    if (state.localInvolved && me->IsInCombat())
        if (auto* list=ProcessLists::Get())
            for (auto h:list->highActorHandleArray)
            {
                auto* a=Cast<Actor>(TESObjectREFR::GetByHandle(h));
                if (a && a!=npc && a!=me && a->IsInCombat() && a->GetCombatTarget()==me)
                {otherAttacker=true;break;}
            }
    bool outstanding = false;
    const auto stop = [&](Actor* actor, Threats threats, bool allowTargetless) {
        if (threats.other || (!threats.participant && !(allowTargetless && actor->IsInCombat()))) return;
        outstanding = true;
        if (now >= state.next)
        {
            actor->StopCombat();
            ++state.attempts;
        }
    };
    // Stop each side independently. Never stop an actor fighting another threat.
    stop(npc, theirs, true);
    for (auto* actor : actors)
    {
        const auto theirTargets = Classify(actor, subject);
        // Only clear a targetless combat flag on the local player if this encounter implicated them.
        if (actor!=me || !otherAttacker) stop(actor, theirTargets, actor == me && state.localInvolved && !theirs.other);
    }
    if (outstanding)
    {
        state.quiet = {};
        if (now >= state.next) state.next = now + 350ms;
        return false;
    }
    if (state.quiet == Clock::time_point{}) state.quiet = now;
    return now - state.quiet >= 2500ms;
}
}
void OverlayService::ObservePartyCombat(uint32_t form) noexcept
{
    auto* player=PlayerCharacter::Get();
    auto* npc=Cast<Actor>(TESForm::GetById(form));
    if (!player || !npc || !npc->GetNiNode()) return;
    if (Classify(player,{npc->GetHandle().handle.iBits}).participant ||
        Classify(npc,{player->GetHandle().handle.iBits}).participant)
        guestInvolved[form]=Clock::now()+10s;
}
void OverlayService::BeginCombatSettlement(uint32_t form) noexcept
{
    if (!form) return;
    auto [it, inserted] = settling.try_emplace(form, Settlement{Clock::now() + 15s});
    const auto earlier=guestInvolved.find(form);
    if (earlier!=guestInvolved.end() && earlier->second>Clock::now()) it->second.localInvolved=true;
}
void OverlayService::CancelCombatSettlement(uint32_t form) noexcept { settling.erase(form); }
void OverlayService::ClearCombatSettlements() noexcept { settling.clear(); guestInvolved.clear(); }
void OverlayService::UpdateCombatSettlements(World& world) noexcept
{
    const auto now = Clock::now();
    std::erase_if(guestInvolved,[now](const auto& entry){return entry.second<=now;});
    for (auto it = settling.begin(); it != settling.end();)
    {
        const bool expired = now >= it->second.deadline;
        if (expired || Settle(world, Cast<Actor>(TESForm::GetById(it->first)), it->second, now))
        {
            spdlog::info("Co-op combat actor {:X}: {}, native stops {}, party threat observed {}", it->first,
                expired ? "timed out" : "settled", it->second.attempts, it->second.associated);
            it = settling.erase(it);
        }
        else ++it;
    }
}
