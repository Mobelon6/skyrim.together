#include <GameServer.h>
#include <Services/OverlayService.h>
#include <Messages/PartyTravelRequest.h>
#include <Events/UpdateEvent.h>
#include <Game/Player.h>
#include <cmath>

namespace
{
using Clock = std::chrono::steady_clock;
bool Within(const glm::vec3& a, const glm::vec3& b, float radius)
{
    const auto delta = a - b;
    return glm::dot(delta, delta) <= radius * radius;
}
}

void OverlayService::OnPartyTravel(const PacketEvent<PartyTravelRequest>& event) noexcept
{
    auto& sender = *event.pPlayer;
    const auto& packet = event.Packet;
    const auto party = sender.GetParty().JoinedPartyId;
    if (!party || !packet.Trip || !sender.GetCharacter())
        return;

    if (packet.Action == BoardCarriage || packet.Action == OpenTravelMap)
    {
        auto prior = m_rides.find(sender.GetId());
        if (prior != m_rides.end())
        {
            if (prior->second.marker && !prior->second.destination.Cell)
            {
                if (prior->second.number == packet.Trip && packet.Action == OpenTravelMap)
                {
                    prior->second.expires = Clock::now() + std::chrono::minutes(3);
                    return;
                }
                m_rides.erase(prior);
            }
            else return;
        }
        const auto* origin = m_world.try_get<MovementComponent>(*sender.GetCharacter());
        const auto& cell = sender.GetCellComponent();
        if (!origin || !cell.WorldSpaceId)
            return;
        Ride ride;
        ride.marker = packet.Action == OpenTravelMap;
        ride.number = packet.Trip;
        ride.party = *party;
        ride.origin = origin->Position;
        ride.originWorld = cell.WorldSpaceId;
        ride.expires = Clock::now() + std::chrono::minutes(3);
        NotifyPartyTravel invite;
        invite.Rider = sender.GetId();
        invite.Trip = packet.Trip;
        invite.Name = sender.GetUsername();
        invite.Kind = InvitePassenger;
        for (auto* guest : m_world.GetPlayerManager())
        {
            if (guest == &sender || guest->GetParty().JoinedPartyId != party || !guest->GetCharacter()
                || guest->GetCellComponent().WorldSpaceId != cell.WorldSpaceId)
                continue;
            auto* movement = m_world.try_get<MovementComponent>(*guest->GetCharacter());
            if (movement && Within(ride.origin, movement->Position, 1200.f))
            {
                ride.passengers.emplace(guest->GetId(), false);
                if (!ride.marker) guest->Send(invite);
            }
        }
        if (!ride.passengers.empty())
            m_rides.emplace(sender.GetId(), std::move(ride));
        return;
    }
    auto i = m_rides.find(packet.Rider);
    if (i == m_rides.end() || i->second.party != *party || i->second.number != packet.Trip || Clock::now() >= i->second.expires)
        return;
    auto& ride = i->second;
    if (sender.GetId() == packet.Rider)
    {
        if (packet.Action == CancelCarriage)
            ride.expires = Clock::now();
        else if (packet.Action == (ride.marker ? ArriveByMarker : ArriveByCarriage) && !ride.destination.Cell && packet.Cell && packet.World == ride.originWorld
                 && std::isfinite(packet.Position.x) && std::isfinite(packet.Position.y) && std::isfinite(packet.Position.z)
                 && !Within(packet.Position, ride.origin, ride.marker ? 1.f : 4096.f))
        {
            ride.destination.Kind = TransportPassenger;
            ride.destination.Rider = packet.Rider;
            ride.destination.Trip = packet.Trip;
            ride.destination.Cell = packet.Cell;
            ride.destination.World = packet.World;
            ride.destination.Position = packet.Position;
            if (ride.marker)
            {
                NotifyPartyTravel invite;
                invite.Kind = InviteMarkerPassenger;
                invite.Rider = packet.Rider;
                invite.Trip = packet.Trip;
                invite.Name = sender.GetUsername();
                for (auto it = ride.passengers.begin(); it != ride.passengers.end(); )
                {
                    auto* guest = m_world.GetPlayerManager().GetById(it->first);
                    const auto* pos = guest && guest->GetCharacter() ? m_world.try_get<MovementComponent>(*guest->GetCharacter()) : nullptr;
                    if (!guest || guest->GetParty().JoinedPartyId != party || guest->GetCellComponent().WorldSpaceId != ride.originWorld ||
                        !pos || !Within(ride.origin, pos->Position, 1200.f))
                        it = ride.passengers.erase(it);
                    else { guest->Send(invite); ++it; }
                }
            }
        }
    }
    else if (auto member = ride.passengers.find(sender.GetId()); member != ride.passengers.end())
    {
        if (packet.Action == AcceptCarriage && (!ride.marker || ride.destination.Cell))
            member->second = true;
        else if (packet.Action == DeclineCarriage)
            ride.passengers.erase(member);
    }
}

void OverlayService::UpdateRides(const UpdateEvent&) noexcept
{
    for (auto i = m_rides.begin(); i != m_rides.end(); )
    {
        auto& ride = i->second;
        auto* rider = m_world.GetPlayerManager().GetById(i->first);
        bool ended = Clock::now() >= ride.expires || !rider || rider->GetParty().JoinedPartyId != ride.party;
        for (auto member = ride.passengers.begin(); member != ride.passengers.end(); )
        {
            auto* guest = m_world.GetPlayerManager().GetById(member->first);
            const auto* movement = guest && guest->GetCharacter()
                ? m_world.try_get<MovementComponent>(*guest->GetCharacter()) : nullptr;
            if (!guest || guest->GetParty().JoinedPartyId != ride.party || ended ||
                guest->GetCellComponent().WorldSpaceId != ride.originWorld ||
                !movement || !Within(ride.origin, movement->Position, 1200.f))
            {
                if (guest && (!ride.marker || ride.destination.Cell))
                {
                    NotifyPartyTravel cancel;
                    cancel.Kind = CancelPassenger;
                    cancel.Rider = i->first;
                    cancel.Trip = ride.number;
                    guest->Send(cancel);
                }
            }
            else if (member->second && ride.destination.Cell)
                guest->Send(ride.destination);
            else
            {
                ++member;
                continue;
            }
            member = ride.passengers.erase(member);
        }
        if (ride.passengers.empty())
            i = m_rides.erase(i);
        else
            ++i;
    }
}
