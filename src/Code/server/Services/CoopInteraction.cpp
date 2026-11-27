#include <GameServer.h>
#include <Services/CampaignAccess.h>
#include <World.h>
#include <Game/Player.h>
#include <Services/OverlayService.h>
#include <Messages/RequestCoopInteraction.h>
#include <Messages/NotifyCoopInteraction.h>

namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
void Send(Player& player, Interaction kind, uint32_t speaker, uint32_t session, GameId npc = {}, uint32_t value = 0)
{
    NotifyCoopInteraction message;
    message.Event = {kind, speaker, session, value, npc};
    player.Send(message);
}
bool InScene(World& world, Player& player, const DialogueAnchor& anchor)
{
    if (!player.GetCharacter())
        return false;
    const auto& cell = player.GetCellComponent();
    if (cell.WorldSpaceId != anchor.WorldSpace || (!anchor.WorldSpace && cell.Cell != anchor.Cell))
        return false;
    const auto* movement = world.try_get<MovementComponent>(*player.GetCharacter());
    return movement && glm::distance(movement->Position, static_cast<const glm::vec3&>(anchor.Position)) <= 420.f;
}
}

void OverlayService::OnInteraction(const PacketEvent<RequestCoopInteraction>& packet) noexcept
{
    using namespace std::chrono_literals;
    const auto& e = packet.Packet.Event;
    auto& player = *packet.pPlayer;
    auto* party = m_world.GetPartyService().GetPlayerParty(&player);
    auto& followers = m_world.GetPartyService().Followers();
    auto& hireTalks = m_world.GetPartyService().HireTalks();
    const auto now = Clock::now();
    const auto id = player.GetId();
    const auto member = [&](uint32_t candidate) -> Player* {
        auto* p = m_world.GetPlayerManager().GetById(candidate);
        return p && p->GetParty().JoinedPartyId == player.GetParty().JoinedPartyId ? p : nullptr;
    };
    const auto announce = [&](Interaction k, uint32_t holder, GameId npc) {
        m_world.GetPlayerManager().ForEach([&](Player* peer) {
            if (peer->GetId() != holder)
                Send(*peer, k, holder, 0, npc);
        });
    };
    if (e.Kind == Interaction::CancelHire && e.Npc)
    {
        auto i = followers.find(e.Npc.LogFormat());
        if (i != followers.end() && !i->second.Confirmed && i->second.Owner == id && i->second.Nonce == e.Session)
        {
            if (!i->second.Prepared) announce(Interaction::FollowerReleased, id, e.Npc);
            followers.erase(i);
        }
        return;
    }
    const bool prepare = e.Kind == Interaction::PrepareHire;
    if (e.Kind == Interaction::CommitHire && e.Npc)
    {
        auto i = followers.find(e.Npc.LogFormat());
        if (i != followers.end() && i->second.Prepared && i->second.Owner == id &&
            i->second.Session == e.Value && i->second.Nonce == e.Session && now < i->second.Expires)
        {
            i->second.Prepared = false;
            i->second.Expires = now + 8s;
            announce(Interaction::FollowerReserved, id, e.Npc);
        }
        return;
    }
    if (e.Kind == Interaction::FollowerReport || e.Kind == Interaction::Hire || prepare)
    {
        const auto character = CampaignAccess::Character(player);
        for (auto i = followers.begin(); i != followers.end(); )
        {
            auto& f = i->second;
            if (f.Detached)
            {
                if (e.Kind == Interaction::FollowerReport && character == f.Character && e.Npc == f.Npc)
                {
                    f.Owner = id;
                    f.Detached = false;
                    f.Expires = now;
                    Send(player, Interaction::FollowerReleased, id, 0, f.Npc);
                }
                ++i;
                continue;
            }
            if (!m_world.GetPlayerManager().GetById(f.Owner) || (!f.Confirmed && now >= f.Expires) ||
                (e.Kind == Interaction::FollowerReport && f.Owner == id && f.Confirmed && e.Npc != f.Npc))
            {
                if (!f.Prepared) announce(Interaction::FollowerReleased, f.Owner, f.Npc);
                i = followers.erase(i);
            }
            else
                ++i;
        }
        if (e.Npc)
        {
            auto i = followers.find(e.Npc.LogFormat());
            bool free = i == followers.end();
            if (e.Kind == Interaction::Hire || prepare)
            {
                const auto scene = hireTalks.find(id);
                const bool valid = scene != hireTalks.end() && !scene->second.Closed && now < scene->second.Expires &&
                    scene->second.Anchor.Session == e.Value && scene->second.Anchor.TargetForm == e.Npc &&
                    InScene(m_world, player, scene->second.Anchor);
                bool competing = false;
                if (prepare)
                    for (const auto& [who, talk] : hireTalks)
                        if (who != id && !talk.Closed && talk.Expires > now && talk.Anchor.TargetForm == e.Npc)
                            competing = true;
                bool own = !free && i->second.Owner == id && i->second.Session == e.Value && !i->second.Confirmed;
                if (valid && !competing && (free || own))
                {
                    for (const auto& [key, f] : followers)
                        if ((f.Detached ? character && f.Character == character : f.Owner == id) && f.Npc != e.Npc)
                            free = own = false;
                    if (free || (own && (i->second.Prepared || (!prepare && i->second.Nonce == e.Session))))
                    {
                        auto& f = followers[e.Npc.LogFormat()];
                        bool announceClaim = !prepare && (free || f.Prepared);
                        f = {e.Npc, id, now + (prepare ? 2s : 8s), false, e.Session, prepare, e.Value};
                        if (announceClaim) announce(Interaction::FollowerReserved, id, e.Npc);
                        Send(player, prepare ? Interaction::HireReady : Interaction::Granted, id, e.Session, e.Npc, e.Value);
                        return;
                    }
                }
                if (!prepare)
                    Send(player, Interaction::Denied, id, e.Session, e.Npc);
                return;
            }
            if (free || (!i->second.Detached && i->second.Owner == id))
            {
                if (free || i->second.Prepared)
                    announce(Interaction::FollowerReserved, id, e.Npc);
                auto& entry = followers[e.Npc.LogFormat()];
                const bool checkSimulation = !entry.Confirmed || now >= entry.Expires;
                entry.Npc = e.Npc;
                entry.Owner = id;
                if (character) entry.Character = character;
                entry.Confirmed = true;
                entry.Prepared = false;
                if (checkSimulation)
                {
                    entry.Expires = now + 4s;
                    m_world.GetCharacterService().AssignInteractionOwner(&player, e.Npc);
                }
            }
        }
        for (const auto& [key, f] : followers)
            if ((f.Detached ? character && f.Character != character : f.Owner != id) && !f.Prepared)
                Send(player, Interaction::FollowerReserved, f.Owner, 0, f.Npc);
        return;
    }
    if (!party)
        return;
    if (e.Kind == Interaction::Join || e.Kind == Interaction::Leave)
    {
        auto it = party->Talks.find(e.Speaker);
        auto* speaker = member(e.Speaker);
        if (!speaker || speaker == &player || it == party->Talks.end() || it->second.Anchor.Session != e.Session)
            return;
        auto& talk = it->second;
        if (e.Kind == Interaction::Leave)
        {
            talk.Listeners.erase(id);
            auto fight = party->Brawls.find(e.Speaker);
            if (fight != party->Brawls.end() && fight->second.Anchor.Session == e.Session)
                fight->second.Listeners.erase(id);
            return;
        }
        if (talk.Closed || talk.Expires <= now || !InScene(m_world, player, talk.Anchor))
            return;
        talk.Listeners.insert(id);
        Send(player, Interaction::Joined, e.Speaker, e.Session, talk.Anchor.TargetForm);
        Send(*speaker, Interaction::WatchOutcomes, e.Speaker, e.Session, talk.Anchor.TargetForm);
        return;
    }
    // Only the player conducting the original conversation can originate these outcomes.
    auto it = party->Talks.find(id);
    auto encounter = party->Brawls.find(id);
    if (encounter != party->Brawls.end() && encounter->second.Expires <= now)
    {
        party->Brawls.erase(encounter);
        encounter = party->Brawls.end();
    }
    if (e.Kind == Interaction::Brawl && it != party->Talks.end() && it->second.Expires > now &&
        it->second.Anchor.Session == e.Session && e.Npc && it->second.Anchor.TargetForm == e.Npc)
    {
        if (encounter != party->Brawls.end() && encounter->second.Anchor.Session == e.Session &&
            encounter->second.Anchor.TargetForm == e.Npc) return;
        auto& fight = party->Brawls[id] = it->second;
        fight.Expires = now + 5min;
        for (auto* peer : party->Members)
            if (peer != &player && PartyService::WithinSyncRange(m_world, player, *peer))
                Send(*peer, Interaction::OpponentState, id, e.Session, e.Npc, 0);
        m_world.GetCharacterService().AssignInteractionOwner(&player, e.Npc);
        return;
    }
    PartyService::Party::Conversation* source = nullptr;
    if (encounter != party->Brawls.end() && encounter->second.Anchor.Session == e.Session && encounter->second.Anchor.TargetForm == e.Npc)
        source = &encounter->second;
    else if (it != party->Talks.end() && it->second.Anchor.Session == e.Session && it->second.Anchor.TargetForm == e.Npc && it->second.Expires > now)
        source = &it->second;
    if (!source || !e.Npc)
        return;
    if (e.Kind == Interaction::OpponentState && e.Value <= 3 && encounter != party->Brawls.end() && source == &encounter->second)
    {
        if (source->OpponentFinished) return;
        source->OpponentFinished = e.Value >= 2;
        source->MayResume = e.Value != 3 && (source->MayResume || e.Value > 0);
        source->Expires = now + (e.Value >= 2 ? 20s : 5min);
        // An NPC's life state is visible to bystanders. Eligibility is personal to the enrolled listeners.
        for (auto* peer : party->Members)
            if (peer != &player && PartyService::WithinSyncRange(m_world, player, *peer))
                Send(*peer, e.Kind, id, e.Session, e.Npc, e.Value);
    }
    else if (e.Kind == Interaction::BrawlFactionEnded && (source->Settlement ||
        (encounter != party->Brawls.end() && source == &encounter->second && source->MayResume)))
    {
        for (auto* peer : party->Members)
            if (peer != &player && PartyService::WithinSyncRange(m_world, player, *peer))
                Send(*peer, e.Kind, id, e.Session, e.Npc);
    }
    else if ((e.Kind == Interaction::Outcome && e.Value >= 1 && e.Value <= 4) ||
             (e.Kind == Interaction::FavorResult && source->Settlement && e.Value && !(e.Value & ~3u)))
    {
        for (auto* guest : party->Members)
            if (guest != &player && PartyService::WithinSyncRange(m_world, player, *guest) &&
                (source->Listeners.count(guest->GetId()) || (source->Settlement && party->LeaderPlayerId == id)))
                Send(*guest, e.Kind, id, source->Encounter, e.Npc, e.Value);
    }
}
