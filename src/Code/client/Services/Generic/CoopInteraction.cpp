#include <TiltedOnlinePCH.h>
#include <World.h>
#include <Games/ActorExtension.h>
#include <Services/PapyrusService.h>
#include <Services/PlayerService.h>
#include <Forms/TESQuest.h>
#include <Forms/TESFaction.h>
#include <Games/Misc/MenuTopicManager.h>
#include <PlayerCharacter.h>
#include <NetImmerse/NiNode.h>
#include <cmath>
#include <Utils.h>
#include <Messages/RequestCoopInteraction.h>
#include <Messages/NotifyCoopInteraction.h>
#include <Messages/RequestNpcLifeCheck.h>
#include <Components.h>
#include <Games/Overrides.h>
#include <atomic>
#include <Windows.h>
#include <map>
#include <mutex>

extern thread_local Actor* applyingCoopDeath;

namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
std::mutex rightsMutex;
std::map<uint32_t, Clock::time_point> occupied;
std::atomic_uint32_t occupiedCount{};
uint32_t opponent{}, lastPose{UINT32_MAX};
struct PeaceSnapshot
{
    uint32_t speaker{};
    Clock::time_point expires{}, started{}, nextAttempt{}, retryStop{}, stableSince{};
    bool encounter{}, initiated{}, rebuilt{}, sawAlive{}, stopDelivered{};
    uint32_t session{};
};
std::map<uint32_t, PeaceSnapshot> recoveries;

bool IsPartyTarget(World& world, Actor* target, uint32_t speaker)
{
    if (!target) return false;
    if (target == PlayerCharacter::Get()) return true;
    const auto players = world.view<PlayerComponent, FormIdComponent>();
    for (const auto ent : players)
        if (players.get<PlayerComponent>(ent).Id == speaker && players.get<FormIdComponent>(ent).Id == target->formID)
            return true;
    return false;
}

bool HorizontalTorso(Actor* npc)
{
    // The graph can report "alive / normal" while the rendered biped is still lying flat.
    static BSFixedString headName("NPC Head [Head]"), hipName("NPC Pelvis [Pelv]");
    auto* root = npc->GetNiNode();
    auto* head = root->GetByName(headName);
    auto* hips = root->GetByName(hipName);
    if (!head || !hips) return false;
    const glm::vec3 delta = static_cast<const glm::vec3&>(head->worldPosition) -
                            static_cast<const glm::vec3&>(hips->worldPosition);
    const float length = glm::length(delta);
    return length > 8.f && delta.z < 0.42f * length;
}

// Called on the game thread; an authoritative end-of-fight is distinct from personal quest consent.
bool VerifyStanding(World& world, Actor* npc, PeaceSnapshot& state, Clock::time_point now)
{
    if (now >= state.expires)
    {
        if (state.encounter) spdlog::info("Co-op encounter {}: pose verification expired (actor {:X}, model loaded {}, rebuilt {})",
            state.session, npc ? npc->formID : 0u, npc && npc->GetNiNode(), state.rebuilt);
        return true;
    }
    if (!npc || !npc->GetNiNode()) return false;
    if (npc->IsDisabled() || npc == PlayerCharacter::Get() || (npc->GetExtension() && npc->GetExtension()->IsPlayer()))
        return true;
    if (!state.encounter && (!npc->GetExtension() || !npc->GetExtension()->IsRemote())) return true;
    if (npc->IsDead())
    {
        // A fresh death after observing an alive episode belongs to the next world state.
        if (!state.encounter || state.sawAlive) return true;
        if (now < state.nextAttempt) return false;
        state.nextAttempt = now + 700ms;
        npc->RestoreRemoteLife();
        if (npc->IsDead() || !npc->GetNiNode()) return false;
        state.nextAttempt = now;
    }
    state.sawAlive = true;
    if (!state.initiated && state.encounter && npc->GetExtension())
        npc->GetExtension()->Pose.Observe(ReplicaPose::Standing, world.GetTick());
    auto* target = npc->GetCombatTarget();
    if (npc->IsInCombat() || (!state.initiated && target))
    {
        if (!state.encounter || (target && !IsPartyTarget(world, target, state.speaker))) return true;
        // Native combat and pose are verified separately. An animation can finish first.
    }
    // Seated, mounted, sleeping and flying actors have legitimate non-standing poses.
    if (((npc->actorState.flags1 >> 14) & 0x7f) != 0) return true;
    const unsigned knock = (npc->actorState.flags1 >> 25) & 7;
    if (!npc->animationGraphHolder.IsReady()) return false;
    ScopedReplicaTransition transition(npc);
    static BSFixedString endKneel("bleedOutStop"), bleedVariable("IsBleedingOut");
    // A kneeling biped can have a vertical torso and an already-normal ActorState.
    // Give the graph its matching stop event even when neither flag detects a ragdoll.
    if (state.encounter && !state.stopDelivered && now >= state.retryStop)
    {
        state.stopDelivered = npc->SendAnimationEvent(&endKneel);
        state.retryStop = now + 850ms;
    }
    bool graphDown = false;
    npc->animationGraphHolder.GetVariableBool(&bleedVariable, &graphDown);
    const bool ragdoll = npc->IsInRagdollState();
    const bool stuck = graphDown || knock || ragdoll || npc->actorState.IsBleedingOut() || HorizontalTorso(npc);
    if (!state.initiated)
    {
        state.initiated = true;
        state.started = now;
        if (state.nextAttempt < now) state.nextAttempt = now;
    }
    if (!stuck)
    {
        if (state.stableSince == Clock::time_point{}) state.stableSince = now;
        if (now - state.stableSince < 900ms) return false;
        if (state.encounter) spdlog::info("Co-op encounter {}: actor {:X} standing; graph stop accepted {}, rebuilt {}",
            state.session, npc->formID, state.stopDelivered, state.rebuilt);
        return true;
    }
    state.stableSince = {};
    if (now < state.nextAttempt) return false;
    state.nextAttempt = now + 850ms;
    // Give the native get-up transition time to finish before rebuilding a still-prone skeleton.
    if (!state.rebuilt && now - state.started >= 3500ms && (knock != 6 || now - state.started >= 6s))
    {
        state.rebuilt = true;
        npc->RefreshReplicaModel();
        spdlog::info("Rebuilt stuck co-op actor model {:X} after confirmed recovery", npc->formID);
    }
    else if (knock != 6)
    {
        if (ragdoll) npc->PotentiallyFixRagdollState();
        if (knock || ragdoll) npc->InitiateGetUpPackage();
        if (graphDown && now >= state.retryStop)
        {
            state.stopDelivered = npc->SendAnimationEvent(&endKneel);
            state.retryStop = now + 850ms;
        }
    }
    return false;
}

void ApplyPeace(World& world)
{
    const auto now = Clock::now();
    for (auto it = recoveries.begin(); it != recoveries.end();)
    {
        if (VerifyStanding(world, Cast<Actor>(TESForm::GetById(it->first)), it->second, now))
            it = recoveries.erase(it);
        else ++it;
    }
}
Clock::time_point opponentExpires{};
std::atomic_uint32_t confirmedOpponent{}, localOpponent{};
std::atomic_int64_t confirmedUntil{}, localUntil{};
// Skyrim may call the NPC death virtual directly, without going through STR Actor::Kill.
using DeathFunction = void (*)(Actor*, Actor*, float, bool, bool);
DeathFunction originalDeath{};

void ReplicaDeath(Actor* npc, Actor* attacker, float damage, bool sendEvent, bool ragdoll)
{
    auto* role = npc->GetExtension();
    const bool protectedBrawl = npc->formID == confirmedOpponent.load() && Clock::now().time_since_epoch().count() < confirmedUntil.load();
    if (role && !role->IsPlayer() && (protectedBrawl || (role->IsRemote() && applyingCoopDeath != npc)))
        return;
    originalDeath(npc, attacker, damage, sendEvent, ragdoll);
}
void GuardReplica(Actor* npc)
{
    if (originalDeath || !npc->GetExtension() || npc->GetExtension()->IsPlayer())
        return;
    auto** slot = *reinterpret_cast<void***>(npc) + 0x10E; // RE::Actor::KillImpl, SE/AE
    DWORD prior{};
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &prior))
        return;
    originalDeath = reinterpret_cast<DeathFunction>(*slot);
    InterlockedExchangePointer(slot, reinterpret_cast<void*>(&ReplicaDeath));
    DWORD unused{};
    VirtualProtect(slot, sizeof(*slot), prior, &unused);
}
void PublishAlive(std::atomic_uint32_t& slot, std::atomic_int64_t& until, uint32_t npc)
{
    until.store((Clock::now() + 4s).time_since_epoch().count());
    slot.store(npc);
}

void Transmit(Interaction kind, uint32_t speaker, uint32_t session, GameId npc = {}, uint32_t value = 0)
{
    RequestCoopInteraction message;
    message.Event = {kind, speaker, session, value, npc};
    World::Get().GetTransport().Send(message);
}
GameId NetworkForm(uint32_t id)
{
    GameId result{};
    if (id && (id >> 24) != 0xFF)
        World::Get().GetModSystem().GetServerModId(id, result);
    return result;
}
TESObjectREFR* Alias(uint32_t questId)
{
    auto* quest = Cast<TESQuest>(TESForm::GetById(questId));
    return quest && !quest->IsStopped() ? quest->GetAliasedRef(0) : nullptr;
}
std::optional<int32_t> BrawlFactionRank(Actor* npc, TESFaction* faction)
{
    auto* method = World::Get().ctx().at<PapyrusService>().Get("Actor", "GetFactionRank");
    if (!method || !npc || !faction) return {};
    PapyrusFunction<int32_t, Actor, TESFaction*> get(method);
    return get(npc, faction);
}
std::optional<int32_t> Relationship(Actor* npc)
{
    auto* method = World::Get().ctx().at<PapyrusService>().Get("Actor", "GetRelationshipRank");
    if (!method || !npc || !PlayerCharacter::Get())
        return {};
    PapyrusFunction<int32_t, Actor, Actor*> get(method);
    return get(npc, PlayerCharacter::Get());
}
}

void OverlayService::CheckRevivedReplica(uint32_t form) noexcept
{
    World::Get().GetRunner().Queue([form] {
        if (!World::Get().GetPartyService().IsInParty() || OverlayService::NonlethalOpponent(form)) return;
        PeaceSnapshot state{};
        state.expires = Clock::now() + 12s;
        recoveries.try_emplace(form, state);
    });
}

void OverlayService::InstallReplicaLifeGuard(Actor* npc) noexcept
{
    GuardReplica(npc);
}

void OverlayService::LeaveConversation(uint64_t key) noexcept
{
    if (key && m_transport.IsConnected())
        Transmit(Interaction::Leave, static_cast<uint32_t>(key >> 32), static_cast<uint32_t>(key));
}
void OverlayService::NoteActivation(uint32_t form) noexcept
{
    m_activated = form;
    m_activatedUntil = Clock::now() + 4s;
}
bool OverlayService::WasActivated(uint32_t form) const noexcept
{
    return form && m_activated == form && Clock::now() < m_activatedUntil;
}

bool OverlayService::FollowerBusy(uint32_t npc) noexcept
{
    if (!occupiedCount.load(std::memory_order_relaxed))
        return false;
    std::lock_guard guard(rightsMutex);
    const auto i = occupied.find(npc);
    return i != occupied.end() && i->second > Clock::now();
}
bool OverlayService::NonlethalOpponent(uint32_t npc) noexcept
{
    const auto now = Clock::now().time_since_epoch().count();
    return npc && ((confirmedOpponent.load() == npc && now < confirmedUntil.load()) ||
        (localOpponent.load() == npc && now < localUntil.load()));
}

bool OverlayService::BeginHire(uint32_t npc, uint32_t info, int32_t index) noexcept
{
    if (!m_transport.IsOnline())
        return false;
    if (FollowerBusy(npc))
    {
        Utils::ShowHudMessage("Этот спутник уже сопровождает другого игрока.");
        return true;
    }
    const GameId key = NetworkForm(npc);
    if (!key)
        return false;
    if (m_pendingForm && Clock::now() < m_pendingUntil)
        return true;
    m_pendingSession = m_world.ctx().at<PlayerService>().ActiveDialogueSession(npc);
    if (!m_pendingSession)
    {
        Utils::ShowHudMessage("Разговор ещё открывается. Выберите реплику снова.");
        return true;
    }
    if (m_hireReady && m_prepareForm == npc && m_prepareSession == m_pendingSession && Clock::now() < m_prepareSent + 1100ms)
    {
        Transmit(Interaction::CommitHire, 0, m_prepareNonce, key, m_pendingSession);
        m_dispatchedNonce = m_prepareNonce;
        m_prepareForm = 0;
        m_hireReady = false;
        m_prepareAt = Clock::now() + 2s;
        return false; // pass the original click to Skyrim, on its original call stack
    }
    m_pendingForm = npc;
    m_pendingInfo = info;
    m_pendingIndex = index;
    m_pendingNonce = ++m_nonce;
    m_pendingUntil = Clock::now() + 4s;
    Transmit(Interaction::Hire, 0, m_nonce, key, m_pendingSession);
    return true;
}

void OverlayService::ClearInteraction() noexcept
{
    if (!m_transport.IsOnline())
    { std::lock_guard guard(rightsMutex); occupied.clear(); occupiedCount.store(0); }
    m_activated = m_pendingForm = m_prepareForm = m_dispatchedNonce = opponent = 0;
    m_hireReady = false;
    confirmedOpponent.store(0);
    localOpponent.store(0);
    m_pendingJoin = m_requestedTalk = 0;
    recoveries.clear();
    ClearCombatSettlements();
    ClearTrade();
    ClearHomes();
    m_rejoinKey = 0;
    ClearProgress();
    m_watch = {};
    lastPose = UINT32_MAX;
    m_pollAt = {};
}

void OverlayService::OnInteraction(const NotifyCoopInteraction& packet) noexcept
{
    const auto& e = packet.Event;
    const bool hireNotice = e.Kind == Interaction::FollowerReserved || e.Kind == Interaction::FollowerReleased ||
        e.Kind == Interaction::HireReady || e.Kind == Interaction::Granted || e.Kind == Interaction::Denied;
    if (!hireNotice && !m_world.GetPartyService().IsInParty())
        return;
    const auto now = Clock::now();
    uint32_t form = e.Npc ? m_world.GetModSystem().GetGameId(e.Npc) : 0;
    const uint64_t key = (uint64_t(e.Speaker) << 32) | e.Session;
    if (e.Kind == Interaction::Joined || e.Kind == Interaction::Rejoin)
    {
        if ((e.Kind == Interaction::Joined && m_pendingJoin != key) ||
            (!m_conversations.count(e.Speaker)) || m_conversations.at(e.Speaker).Last.Anchor.Session != e.Session)
            return;
        if (m_dismissedTalk == key && e.Kind == Interaction::Rejoin)
            return;
        m_pendingJoin = 0;
        m_requestedTalk = key;
        m_rejoinKey = e.Kind == Interaction::Rejoin ? key : 0;
        m_rejoinUntil = e.Kind == Interaction::Rejoin ? now + 8s : Clock::time_point{};
        m_dismissedTalk = 0;
        return;
    }
    if (e.Kind == Interaction::SyncRange)
    {
        SetPeerRange(e.Speaker, e.Value != 0);
        return;
    }
    if (!form)
        return;
    if (e.Kind == Interaction::OpponentState && e.Value == 2)
    {
        // Repeated settlement messages must not restart recovery or override a later assault.
        if (!m_peaceSession.count(form) || m_peaceSession[form] != key)
        {
            m_peaceSession[form] = key;
            PeaceSnapshot state{};
            state.speaker = e.Speaker;
            state.session = e.Session;
            spdlog::info("Co-op encounter {}: received peaceful outcome for actor {:X} from {}", e.Session, form, e.Speaker);
            state.expires = now + 15s;
            state.encounter = true;
            BeginCombatSettlement(form);
            recoveries.insert_or_assign(form, state);
            opponent = form;
            opponentExpires = now + 3s;
            PublishAlive(confirmedOpponent, confirmedUntil, form);
            lastPose = UINT32_MAX;
            ApplyPeace(m_world);
        }
        return;
    }
    if (e.Kind == Interaction::OpponentState && m_peaceSession.count(form) &&
        m_peaceSession.at(form) == key) return;
    if (e.Kind == Interaction::OpponentState && e.Value != 2)
    {
        recoveries.erase(form);
        CancelCombatSettlement(form);
    }
    if (e.Kind == Interaction::FollowerReserved || e.Kind == Interaction::FollowerReleased)
    {
        bool newlyReserved = false;
        {
            std::lock_guard guard(rightsMutex);
            if (e.Kind == Interaction::FollowerReserved)
            {
                auto& until = occupied[form];
                newlyReserved = until <= now;
                until = now + 10s;
            }
            else
                occupied.erase(form);
            occupiedCount.store(static_cast<uint32_t>(occupied.size()), std::memory_order_relaxed);
        }
        if (newlyReserved)
        {
            // Repair a rank copied by previous STR builds, while preserving a pre-existing personal follower.
            auto* quest = Cast<TESQuest>(TESForm::GetById(0x750BA));
            auto* actor = Cast<Actor>(TESForm::GetById(form));
            auto* faction = Cast<TESFaction>(TESForm::GetById(0x5C84E));
            auto* method = m_world.ctx().at<PapyrusService>().Get("Actor", "GetFactionRank");
            if (actor && quest && quest->GetAliasedRef(0) != actor && faction && method)
            {
                PapyrusFunction<int32_t, Actor, TESFaction*> rank(method);
                if (rank(actor, faction) >= 0)
                    actor->SetFactionRank(faction, -1);
            }
        }
        return;
    }
    if (e.Kind == Interaction::HireReady)
    {
        if (form == m_prepareForm && e.Session == m_prepareNonce && e.Value == m_prepareSession)
            m_hireReady = now < m_prepareSent + 1100ms;
        return;
    }
    if (e.Kind == Interaction::Granted || e.Kind == Interaction::Denied)
    {
        if (form != m_pendingForm || e.Session != m_pendingNonce || now >= m_pendingUntil)
        {
            if (e.Kind == Interaction::Granted && e.Session != m_dispatchedNonce)
                Transmit(Interaction::CancelHire, 0, e.Session, e.Npc);
            return;
        }
        m_pendingForm = 0;
        if (e.Kind == Interaction::Granted)
        {
            m_dispatchedNonce = e.Session;
            auto* menu = MenuTopicManager::Get();
            if (m_world.ctx().at<PlayerService>().ActiveDialogueSession(form) != m_pendingSession || !menu ||
                !menu->ResumeHire(m_pendingIndex, form, m_pendingInfo))
            {
                Transmit(Interaction::CancelHire, 0, e.Session, e.Npc);
                Utils::ShowHudMessage("Реплика найма больше недоступна.");
            }
        }
        else
            Utils::ShowHudMessage("Нанять спутника сейчас не удалось.");
        return;
    }
    auto* actor = Cast<Actor>(TESForm::GetById(form));
    if (!actor)
        return;
    if (e.Kind == Interaction::WatchOutcomes)
    {
        if (m_watch.Npc == form && m_watch.Session == e.Session)
            return;
        auto rank = Relationship(actor);
        bool settlement = e.Value == 1;
        // Retain the subject captured before the engine clears the radiant quest aliases.
        auto* favor = Cast<TESQuest>(TESForm::GetById(0xCA210));
        bool matchingFavor = (favor && !favor->IsStopped() && favor->GetAliasedRef(17) == actor) ||
                             (settlement && m_watch.Npc == form && m_watch.FavorParticipant);
        m_watch = {};
        m_watch.Settlement = settlement;
        m_watch.FavorParticipant = matchingFavor;
        m_watch.Npc = form;
        m_watch.Session = e.Session;
        m_watch.Rank = rank ? static_cast<uint32_t>(*rank + 4) : UINT32_MAX;
        m_watch.Expires = now + 6s;
    }
    else if (e.Kind == Interaction::BrawlFactionEnded)
    {
        auto* faction = Cast<TESFaction>(TESForm::GetById(0x4CFA6));
        const auto rank = BrawlFactionRank(actor, faction);
        if (rank && *rank >= 0) actor->SetFactionRank(faction, -1);
    }
    else if (e.Kind == Interaction::FavorResult && e.Value && !(e.Value & ~3u))
    {
        QueueProgress(false, form, e.Speaker, e.Session, actor->baseForm ? actor->baseForm->GetName() : "", [this, form, stages = e.Value] {
            auto& pending = m_favorSettlements[form];
            pending.Npc = form;
            pending.Stages |= stages;
            pending.Expires = Clock::now() + 20s;
        }, [form, stages = e.Value] {
            auto* quest = Cast<TESQuest>(TESForm::GetById(0xCA210));
            if (!quest || quest->IsStopped()) return false;
            auto* subject = quest->GetAliasedRef(17);
            return subject && subject->formID == form && (((stages & 1) && !quest->IsStageReached(20)) ||
                ((stages & 2) && !quest->IsStageReached(200)));
        });
    }
    else if (e.Kind == Interaction::Outcome && e.Value >= 1 && e.Value <= 4)
    {
        QueueProgress(false, form, e.Speaker, e.Session, actor->baseForm ? actor->baseForm->GetName() : "", [this, form, rank = e.Value] {
            auto* npc = Cast<Actor>(TESForm::GetById(form));
            const auto current = Relationship(npc);
            auto* method = m_world.ctx().at<PapyrusService>().Get("Actor", "SetRelationshipRank");
            if (current && *current < static_cast<int32_t>(rank) && method)
            {
                PapyrusFunction<void, Actor, Actor*, int32_t> set(method);
                set(npc, PlayerCharacter::Get(), static_cast<int32_t>(rank));
            }
        }, [form, rank = e.Value] {
            const auto current = Relationship(Cast<Actor>(TESForm::GetById(form)));
            return current && *current < static_cast<int32_t>(rank);
        });
    }
    else if (e.Kind == Interaction::OpponentState && e.Value <= 3)
    {
        if (e.Value == 3)
        {
            if (opponent == form)
            {
                opponent = 0;
                confirmedOpponent.store(0);
                lastPose = UINT32_MAX;
            }
            // End the nonlethal contract. Only the owner/epoch-checked life channel can confirm death.
            if (auto* extension = actor->GetExtension()) extension->Pose = {};
            auto replicas = m_world.view<FormIdComponent, RemoteComponent>();
            for (auto entity : replicas)
                if (replicas.get<FormIdComponent>(entity).Id == form)
                {
                    const auto& remote = replicas.get<RemoteComponent>(entity);
                    RequestNpcLifeCheck check;
                    check.Id = remote.Id;
                    check.OwnershipEpoch = remote.OwnershipEpoch;
                    m_transport.Send(check);
                    break;
                }
            return;
        }
        if (opponent != form || opponentExpires < now)
            lastPose = UINT32_MAX;
        opponent = form;
        PublishAlive(confirmedOpponent, confirmedUntil, form);
        GuardReplica(actor);
        if (auto* extension = actor->GetExtension())
            extension->Pose.Observe(e.Value == 1 ? ReplicaPose::Kneeling : ReplicaPose::Fighting, m_world.GetTick());
        if (lastPose != e.Value)
            spdlog::debug("Brawl replica {:X}: session {}, state {}, local death {}", form, e.Session, e.Value, actor->IsDead());
        opponentExpires = now + (e.Value == 2 ? 8s : 4s);
        confirmedUntil.store(opponentExpires.time_since_epoch().count());
        if (!actor->GetNiNode())
            return;
        if (actor->IsDead())
            actor->RestoreRemoteLife();
        if (actor->IsDead())
            return;
        const uint32_t pose = e.Value == 2 ? 0 : e.Value;
        if (pose != lastPose)
        {
            static BSFixedString down("bleedOutStart"), up("bleedOutStop");
            ScopedReplicaTransition transition(actor);
            const bool change = pose == 1 || lastPose == 1 || actor->actorState.IsBleedingOut();
            if (!change || (actor->animationGraphHolder.IsReady() && actor->SendAnimationEvent(pose == 1 ? &down : &up)))
                lastPose = pose;
        }
        ObservePartyCombat(form);
        if (actor->GetCombatTarget() == PlayerCharacter::Get())
        {
            auto players = m_world.view<PlayerComponent, FormIdComponent>();
            for (auto entity : players)
                if (players.get<PlayerComponent>(entity).Id == e.Speaker)
                    if (auto* challenger = Cast<Actor>(TESForm::GetById(players.get<FormIdComponent>(entity).Id)))
                        actor->SetCombatTargetEx(challenger);
        }
    }
}

void OverlayService::UpdateHireOffer() noexcept
{
    const auto now = Clock::now();
    if (m_pendingForm && now < m_pendingUntil)
        return;
    auto* menu = MenuTopicManager::Get();
    auto* npc = menu && menu->speaker && menu->OffersHire() ? TESObjectREFR::GetByHandle(menu->speaker.handle.iBits) : nullptr;
    const uint32_t form = npc ? npc->formID : 0;
    if (m_prepareForm && form != m_prepareForm)
    {
        Transmit(Interaction::CancelHire, 0, m_prepareNonce, NetworkForm(m_prepareForm));
        m_prepareForm = 0;
        m_hireReady = false;
        m_prepareAt = {};
    }
    if (!form || now < m_prepareAt)
        return;
    auto session = m_world.ctx().at<PlayerService>().ActiveDialogueSession(form);
    auto key = NetworkForm(form);
    if (!key || !session)
        return;
    if (m_prepareForm != form || session != m_prepareSession)
    {
        m_prepareForm = form;
        m_prepareSession = session;
        m_prepareNonce = ++m_nonce;
    }
    // Until the matching reply arrives, a click goes through the normal explicit request.
    m_hireReady = false;
    m_prepareSent = now;
    m_prepareAt = now + 800ms;
    Transmit(Interaction::PrepareHire, 0, m_prepareNonce, key, session);
}

void OverlayService::BrawlBleedout(uint32_t form) noexcept
{
    if (!form || m_watch.Npc != form || m_watch.Settlement || !m_world.GetPartyService().IsInParty()) return;
    auto* actor = Cast<Actor>(TESForm::GetById(form));
    if (!actor || (Alias(0x47AE6) != actor && (!m_watch.Brawl || m_watch.State >= 2))) return;
    const auto npc = NetworkForm(form);
    if (!npc) return;
    if (!m_watch.Brawl) Transmit(Interaction::Brawl, 0, m_watch.Session, npc);
    m_watch.Brawl = true;
    spdlog::info("Co-op encounter {}: Skyrim reported opponent {:X} entering bleedout", m_watch.Session, form);
    m_watch.KneelingUntil = Clock::now() + 300ms;
    m_watch.State = 1;
    Transmit(Interaction::OpponentState, 0, m_watch.Session, npc, 1);
}

void OverlayService::UpdateInteraction() noexcept
{
    if (!m_transport.IsOnline())
        return;
    const auto now = Clock::now();
    if (now < m_pollAt)
        return;
    m_pollAt = now + 100ms;
    auto* local = PlayerCharacter::Get();
    if (!local || !local->parentCell || !local->GetNiNode())
        return; // a load screen cannot confirm dismissal
    auto* followerQuest = Cast<TESQuest>(TESForm::GetById(0x750BA)); // DialogueFollower, human follower alias
    if (followerQuest && now >= m_followerPoll)
    {
        m_followerPoll = now + 500ms;
        auto* follower = followerQuest->GetAliasedRef(0);
        Transmit(Interaction::FollowerReport, 0, 0, follower ? NetworkForm(follower->formID) : GameId{});
    }
    UpdateHireOffer();
    if (!m_world.GetPartyService().IsInParty())
        return;
    UpdateCombatSettlements(m_world);
    ApplyPeace(m_world);
    ApplyFavorSettlements();
    if (!m_watch.Npc)
        return;
    auto* actor = Cast<Actor>(TESForm::GetById(m_watch.Npc));
    const auto* menu = MenuTopicManager::Get();
    if (actor && menu && menu->IsCurrentSpeaker(actor->GetHandle()))
        m_watch.Expires = now + 6s;
    if (!actor || now >= m_watch.Expires)
    {
        m_watch = {};
        return;
    }
    auto key = NetworkForm(m_watch.Npc);
    if (!key)
        return;
    const bool fighting = Alias(0x47AE6) == actor; // DGIntimidateQuest's current opponent, irrespective of NPC
    PublishAlive(localOpponent, localUntil, fighting && !actor->IsDead() ? m_watch.Npc : 0);
    if (fighting && !m_watch.Brawl)
    {
        Transmit(Interaction::Brawl, 0, m_watch.Session, key);
        m_watch.Brawl = true;
    }
    if (m_watch.Brawl)
    {
        if (fighting)
            m_watch.Expires = now + 15s;
        const uint32_t pose = actor->IsDead() ? 3u : !fighting ? 2u : (actor->actorState.IsBleedingOut() || now < m_watch.KneelingUntil) ? 1u : 0u;
        if (pose <= 1 || m_watch.State != pose)
            Transmit(Interaction::OpponentState, 0, m_watch.Session, key, pose);
        m_watch.State = pose;
    }
    auto* favor = Cast<TESQuest>(TESForm::GetById(0xCA210));
    if (favor && !favor->IsStopped() && favor->GetAliasedRef(17) == actor)
        m_watch.FavorParticipant = true;
    if ((m_watch.Settlement || (m_watch.Brawl && m_watch.State == 2)) && !m_watch.FactionReported && !actor->IsInCombat())
    {
        const auto rank = BrawlFactionRank(actor, Cast<TESFaction>(TESForm::GetById(0x4CFA6)));
        if (rank && *rank < 0)
        {
            Transmit(Interaction::BrawlFactionEnded, 0, m_watch.Session, key);
            m_watch.FactionReported = true;
        }
    }
    if (m_watch.Settlement && m_watch.FavorParticipant && favor &&
        (favor->IsStopped() || favor->GetAliasedRef(17) == actor))
    {
        const uint32_t stages = (favor->IsStageReached(20) ? 1u : 0u) | (favor->IsStageReached(200) ? 2u : 0u);
        if ((stages & 1) && stages != m_watch.FavorSent)
        {
            Transmit(Interaction::FavorResult, 0, m_watch.Session, key, stages);
            m_watch.FavorSent = stages;
        }
    }
    const auto rank = Relationship(actor);
    if (rank && *rank >= -4 && *rank <= 4)
    {
        const auto observed = static_cast<uint32_t>(*rank + 4);
        if (*rank > 0 && ((m_watch.Settlement && m_watch.ReportedRank != observed) ||
            (m_watch.Rank != UINT32_MAX && observed > m_watch.Rank)))
        {
            m_watch.ReportedRank = observed;
            Transmit(Interaction::Outcome, 0, m_watch.Session, key, static_cast<uint32_t>(*rank));
        }
        m_watch.Rank = observed;
    }
}

void OverlayService::ApplyFavorSettlements() noexcept
{
    auto* favor = Cast<TESQuest>(TESForm::GetById(0xCA210));
    if (!favor) return;
    for (auto it = m_favorSettlements.begin(); it != m_favorSettlements.end();)
    {
        auto& result = it->second;
        auto* subject = !favor->IsStopped() ? favor->GetAliasedRef(17) : nullptr;
        if (Clock::now() >= result.Expires || (subject && subject->formID != result.Npc) ||
            (favor->IsStopped() && favor->IsStageReached(200)))
        {
            it = m_favorSettlements.erase(it);
            continue;
        }
        if (subject)
        {
            ScopedQuestOverride scope;
            // Each fragment gets a game update before its successor. A receipt cannot replay a payment.
            if ((result.Stages & 1) && !favor->IsStageReached(20) && !result.Sent20)
            {
                result.Sent20 = true;
                favor->ScriptSetStage(20);
            }
            else if ((result.Stages & 2) && favor->IsStageReached(20) && !favor->IsStageReached(200) && !result.Sent200)
            {
                result.Sent200 = true;
                favor->ScriptSetStage(200);
            }
        }
        ++it;
    }
}
