#include <TiltedOnlinePCH.h>
#include <World.h>
#include <OverlayApp.hpp>
#include <Services/OverlayClient.h>
#include <Services/OverlayService.h>
#include <Services/PartyService.h>
#include <Services/TransportService.h>
#include <Events/ActivateEvent.h>
#include <Messages/PartyTravelRequest.h>
#include <Messages/NotifyTeleport.h>
#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Forms/TESQuest.h>
#include <Interface/UI.h>

namespace
{
using Clock = std::chrono::steady_clock;

bool IsPassengerSeat(TESObjectREFR* seat)
{
    if (!seat)
        return false;
    // Skyrim.esm DialogueCarriageSystem, CurrentSeat alias. The five linked
    // passenger references also work if the quest has already cleared its alias.
    auto* quest = Cast<TESQuest>(TESForm::GetById(0x17F01));
    if (quest && quest->GetAliasedRef(14) == seat)
        return true;
    constexpr uint32_t seats[] = {0x1070F0, 0x1070ED, 0x1070E4, 0x1070E3, 0x1070E7};
    return std::find(std::begin(seats), std::end(seats), seat->formID) != std::end(seats);
}

bool NearDestination(PlayerCharacter* player)
{
    // ObjectReference properties on CarriageSystemScript from Skyrim.esm.
    constexpr uint32_t stops[] = {0x108D8D, 0xB0EF6, 0xB0A00, 0xB09FF, 0xB09FE, 0x162A4,
        0x177EF, 0x177E4, 0x177D7, 0x177CC, 0x177C3, 0x177B0, 0x177A1, 0x1779A, 0x17791,
        0x17760, 0x17753, 0x1773A, 0x17732};
    auto* world = player->GetWorldSpace();
    if (!world)
        return false;
    for (uint32_t id : stops)
    {
        auto* marker = Cast<TESObjectREFR>(TESForm::GetById(id));
        if (marker && marker->GetWorldSpace() == world &&
            glm::distance(static_cast<const glm::vec3&>(marker->position), static_cast<const glm::vec3&>(player->position)) <= 2400.f)
            return true;
    }
    return false;
}
}

void OverlayService::OnBoardCarriage(const ActivateEvent& event) noexcept
{
    auto* player = PlayerCharacter::Get();
    if (!m_shareNpc || !player || event.pActivator != player || m_boardedTrip || !m_transport.IsConnected() ||
        !m_world.GetPartyService().IsInParty() || !IsPassengerSeat(event.pObject))
        return;
    if (++m_tripCounter == 0)
        ++m_tripCounter;
    if (m_mapTrip)
    {
        PartyTravelRequest cancel;
        cancel.Action = CancelCarriage;
        cancel.Rider = m_transport.GetLocalPlayerId();
        cancel.Trip = m_mapTrip;
        m_transport.Send(cancel);
        m_mapTrip = 0;
    }
    m_boardedTrip = m_tripCounter;
    m_departure = player->position;
    m_boardDeadline = Clock::now() + std::chrono::minutes(3);
    PartyTravelRequest board;
    board.Action = BoardCarriage;
    board.Trip = m_boardedTrip;
    m_transport.Send(board);
}

void OverlayService::CloseTravelPrompt() noexcept
{
    if (m_promptOpen && m_pOverlay)
        m_pOverlay->ExecuteAsync("hideCarriage");
    if (m_ownsTravelOverlay && m_gameOverlayClient && GetActive())
        m_gameOverlayClient->SetUIVisible(false);
    m_promptOpen = m_ownsTravelOverlay = false;
}

void OverlayService::ClearCarriage() noexcept
{
    CloseTravelPrompt();
    m_boardedTrip = m_mapTrip = 0;
    m_mapConfirmed = m_mapWasOpen = false;
    m_offer.reset();
    m_acceptedRide = false;
}

void OverlayService::AnswerCarriage(uint32_t rider, uint32_t trip, bool yes) noexcept
{
    if (!m_offer || m_offer->Rider != rider || m_offer->Trip != trip || m_acceptedRide)
        return;
    PartyTravelRequest response;
    response.Action = yes ? AcceptCarriage : DeclineCarriage;
    response.Rider = rider;
    response.Trip = trip;
    m_transport.Send(response);
    CloseTravelPrompt();
    m_acceptedRide = yes;
    if (!yes)
        m_offer.reset();
}

void OverlayService::OnTravelNotice(const NotifyPartyTravel& notice) noexcept
{
    if (!m_world.GetPartyService().IsInParty())
        return;
    const bool matches = m_offer && m_offer->Rider == notice.Rider && m_offer->Trip == notice.Trip;
    if (notice.Kind == InvitePassenger || notice.Kind == InviteMarkerPassenger)
    {
        if (matches)
            return;
        if (m_offer)
        {
            PartyTravelRequest busy;
            busy.Action = DeclineCarriage;
            busy.Rider = notice.Rider;
            busy.Trip = notice.Trip;
            m_transport.Send(busy);
            return;
        }
        m_offer = notice;
        m_acceptedRide = false;
        m_offerDeadline = Clock::now() + std::chrono::minutes(3);
    }
    else if (matches && notice.Kind == CancelPassenger)
    {
        CloseTravelPrompt();
        m_offer.reset();
        m_acceptedRide = false;
    }
    else if (matches && m_acceptedRide && notice.Kind == TransportPassenger && notice.Cell)
        m_offer = notice;
}

void OverlayService::UpdateCarriage() noexcept
{
    if (!m_boardedTrip && !m_offer)
        return;
    auto* player = PlayerCharacter::Get();
    const auto now = Clock::now();
    auto* ui = UI::Get();
    if (!m_world.GetPartyService().IsInParty() || !m_transport.IsConnected())
    {
        ClearCarriage();
        return;
    }
    if (m_boardedTrip && player && player->parentCell)
    {
        PartyTravelRequest result;
        result.Action = CancelCarriage;
        result.Rider = m_transport.GetLocalPlayerId();
        result.Trip = m_boardedTrip;
        const float moved = glm::distance(m_departure, static_cast<const glm::vec3&>(player->position));
        if (now < m_boardDeadline && moved > 4096.f && player->GetWorldSpace() && NearDestination(player) &&
            m_world.GetModSystem().GetServerModId(player->GetParentCellEx()->formID, result.Cell) &&
            m_world.GetModSystem().GetServerModId(player->GetWorldSpace()->formID, result.World))
        {
            result.Action = ArriveByCarriage;
            result.Position = static_cast<const glm::vec3&>(player->position);
        }
        if (result.Action == ArriveByCarriage || now >= m_boardDeadline || (moved > 900.f && moved < 4096.f))
        {
            m_transport.Send(result);
            m_boardedTrip = 0;
        }
    }
    if (!m_offer)
        return;
    if (now >= m_offerDeadline)
    {
        CloseTravelPrompt();
        m_offer.reset();
        m_acceptedRide = false;
        return;
    }
    if (m_promptOpen && !GetActive())
    {
        AnswerCarriage(m_offer->Rider, m_offer->Trip, false);
        return;
    }
    if (!GetInGame() || !player || !player->parentCell || !ui || ui->BlocksPartyHud() || player->IsInCombat())
        return;
    if (m_offer->Kind == TransportPassenger && m_acceptedRide)
    {
        NotifyTeleport destination;
        destination.CellId = m_offer->Cell;
        destination.WorldSpaceId = m_offer->World;
        destination.Position = m_offer->Position;
        m_offer.reset();
        m_acceptedRide = false;
        OnNotifyTeleport(destination);
    }
    else if (!m_showingProgress && !m_acceptedRide && !m_promptOpen && m_gameOverlayClient && m_pOverlay)
    {
        auto args = CefListValue::Create();
        args->SetInt(0, m_offer->Rider);
        args->SetInt(1, m_offer->Trip);
        args->SetString(2, m_offer->Name.c_str());
        args->SetInt(3, m_offer->Kind == InviteMarkerPassenger ? 1 : 0);
        m_promptOpen = true;
        m_ownsTravelOverlay = !GetActive();
        m_pOverlay->ExecuteAsync("offerCarriage", args);
        if (m_ownsTravelOverlay)
            m_gameOverlayClient->SetUIVisible(true);
    }
}

void OverlayService::ConfirmMarkerTravel() noexcept
{
    // Fired by Skyrim after genuine fast travel. Closing the map or loading a save is insufficient.
    if (m_mapTrip && m_shareMap && !m_boardedTrip && !m_acceptedRide)
        m_mapConfirmed = true;
}

void OverlayService::UpdateMapTravel() noexcept
{
    auto* player = PlayerCharacter::Get();
    auto* ui = UI::Get();
    if (!ui || !player || !m_transport.IsConnected() || !m_world.GetPartyService().IsInParty())
    {
        m_mapTrip = 0;
        m_mapWasOpen = m_mapConfirmed = false;
        return;
    }
    static const BSFixedString map("MapMenu"), loading("Loading Menu");
    const bool open = ui->GetMenuOpen(map);
    const bool enabled = m_shareMap && !m_boardedTrip && !m_acceptedRide && !m_offer;
    const auto now = Clock::now();
    if (enabled && open && !m_mapConfirmed && player->parentCell && player->GetWorldSpace() &&
        (!m_mapTrip || now >= m_mapRefresh))
    {
        if (!m_mapTrip)
        {
            if (++m_tripCounter == 0) ++m_tripCounter;
            m_mapTrip = m_tripCounter;
            m_mapOrigin = player->position;
        }
        PartyTravelRequest arm;
        arm.Action = OpenTravelMap;
        arm.Trip = m_mapTrip;
        m_transport.Send(arm);
        m_mapRefresh = now + std::chrono::seconds(30);
    }
    if (open || ui->GetMenuOpen(loading) || m_mapWasOpen)
        m_mapExpires = now + std::chrono::seconds(5);
    m_mapWasOpen = open;
    if (!m_mapTrip)
        return;
    PartyTravelRequest result;
    result.Action = CancelCarriage;
    result.Trip = m_mapTrip;
    result.Rider = m_transport.GetLocalPlayerId();
    if (enabled && m_mapConfirmed && !open && !ui->GetMenuOpen(loading) &&
        player->parentCell && player->GetWorldSpace() && glm::distance(m_mapOrigin, static_cast<const glm::vec3&>(player->position)) > 1.f &&
        m_world.GetModSystem().GetServerModId(player->GetParentCellEx()->formID, result.Cell) &&
        m_world.GetModSystem().GetServerModId(player->GetWorldSpace()->formID, result.World))
    {
        result.Action = ArriveByMarker;
        result.Position = static_cast<const glm::vec3&>(player->position);
    }
    if (result.Action == ArriveByMarker || !enabled || now >= m_mapExpires)
    {
        m_transport.Send(result);
        m_mapTrip = 0;
        m_mapConfirmed = false;
    }
}
