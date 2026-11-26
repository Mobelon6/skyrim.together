#include <GameServer.h>

#include <Services/OverlayService.h>

#include <ChatMessageTypes.h>

#include <Messages/NotifyChatMessageBroadcast.h>
#include <Messages/SendChatMessageRequest.h>
#include <Messages/PlayerDialogueRequest.h>
#include <Messages/NotifyPlayerDialogue.h>
#include <Messages/NotifyPlayerDialogue.h>
#include <Messages/TeleportRequest.h>
#include <Messages/NotifyTeleport.h>
#include <Messages/RequestPlayerHealthUpdate.h>
#include <Messages/NotifyPlayerHealthUpdate.h>

#include "Game/Player.h"

#include <regex>
#include <Messages/RequestCoopInteraction.h>
#include <Messages/NotifyCoopInteraction.h>
#include <Messages/RequestPartyHome.h>
#include <Messages/RequestPartyTrade.h>
#include <Messages/PartyTravelRequest.h>
#include <Events/UpdateEvent.h>


OverlayService::OverlayService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_homeConnection = aDispatcher.sink<PacketEvent<RequestPartyHome>>().connect<&OverlayService::OnHome>(this);
    m_tradeConnection = aDispatcher.sink<PacketEvent<RequestPartyTrade>>().connect<&OverlayService::OnTrade>(this);
    m_interactionConnection = aDispatcher.sink<PacketEvent<RequestCoopInteraction>>().connect<&OverlayService::OnInteraction>(this);
    m_rideConnection = aDispatcher.sink<PacketEvent<PartyTravelRequest>>().connect<&OverlayService::OnPartyTravel>(this);
    m_tickRides = aDispatcher.sink<UpdateEvent>().connect<&OverlayService::UpdateRides>(this);
    m_chatMessageConnection = aDispatcher.sink<PacketEvent<SendChatMessageRequest>>().connect<&OverlayService::HandleChatMessage>(this);
    m_playerDialogueConnection = aDispatcher.sink<PacketEvent<PlayerDialogueRequest>>().connect<&OverlayService::OnPlayerDialogue>(this);
    m_teleportConnection = aDispatcher.sink<PacketEvent<TeleportRequest>>().connect<&OverlayService::OnTeleport>(this);
    m_playerHealthConnection = aDispatcher.sink<PacketEvent<RequestPlayerHealthUpdate>>().connect<&OverlayService::OnPlayerHealthUpdate>(this);
}

void sendPlayerMessage(const ChatMessageType acType, const String acContent, Player* aSendingPlayer) noexcept
{
    NotifyChatMessageBroadcast notifyMessage{};

    std::regex escapeHtml{"<[^>]+>\\s+(?=<)|<[^>]+>"};
    notifyMessage.MessageType = acType;
    notifyMessage.PlayerName = std::regex_replace(aSendingPlayer->GetUsername(), escapeHtml, "");
    notifyMessage.ChatMessage = std::regex_replace(acContent, escapeHtml, "");

    auto character = aSendingPlayer->GetCharacter();

    switch (notifyMessage.MessageType)
    {
    case kGlobalChat: GameServer::Get()->SendToPlayers(notifyMessage); break;

    case kSystemMessage: spdlog::error("PlayerId {} attempted to send a System Message.", aSendingPlayer->GetId()); break;

    case kPlayerDialogue: GameServer::Get()->SendToParty(notifyMessage, aSendingPlayer->GetParty()); break;

    case kPartyChat: GameServer::Get()->SendToParty(notifyMessage, aSendingPlayer->GetParty()); break;

    case kLocalChat:
        if (character)
        {
            if (!GameServer::Get()->SendToPlayersInRange(notifyMessage, *character))
                spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
        }
        break;
    default: spdlog::error("{} is not a known MessageType", static_cast<uint64_t>(notifyMessage.MessageType)); break;
    }
}

void OverlayService::HandleChatMessage(const PacketEvent<SendChatMessageRequest>& acMessage) const noexcept
{
    auto [canceled, reason] = m_world.GetScriptService().HandleChatMessage(*acMessage.pPlayer->GetCharacter(), acMessage.Packet.ChatMessage);
    if (canceled)
        return;

    sendPlayerMessage(acMessage.Packet.MessageType, acMessage.Packet.ChatMessage, acMessage.pPlayer);
}

void OverlayService::OnPlayerDialogue(const PacketEvent<PlayerDialogueRequest>& acMessage) const noexcept
{
    auto* pSpeaker = acMessage.pPlayer;
    NotifyPlayerDialogue message;
    message.Anchor = acMessage.Packet.Anchor;
    if (!message.Anchor.Session)
        return;
    // Keep a private session anchor for hire validation even without a party.
    // Only party conversations below are relayed or used for quest outcomes.
    auto& hire = m_world.GetPartyService().HireTalks()[pSpeaker->GetId()];
    if (message.Anchor.Cell)
    {
        hire.Anchor = message.Anchor;
        hire.Closed = false;
        hire.Expires = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    }
    else if (hire.Anchor.Session == message.Anchor.Session)
        hire.Closed = true;
    if (!pSpeaker->GetParty().JoinedPartyId)
        return;
    auto* party = m_world.GetPartyService().GetPlayerParty(pSpeaker);
    if (!party || !message.Anchor.Session)
        return;
    auto& talk = party->Talks[pSpeaker->GetId()];
    const uint32_t previousChoice = talk.Anchor.Session == message.Anchor.Session ? talk.Anchor.Choice : 0;
    const bool opening = message.Anchor.Cell && talk.Anchor.Session != message.Anchor.Session;
    if (!message.Anchor.Cell)
    {
        if (talk.Anchor.Session != message.Anchor.Session)
            return;
        if (message.Anchor.Choice > previousChoice)
        {
            talk.Anchor.Choice = message.Anchor.Choice;
            talk.Anchor.TopicInfo = message.Anchor.TopicInfo;
            talk.Anchor.TopicQuest = message.Anchor.TopicQuest;
        }
        talk.Closed = true;
    }
    else
    {
        if (talk.Anchor.Session != message.Anchor.Session || talk.Anchor.TargetForm != message.Anchor.TargetForm ||
            talk.Anchor.TargetEntity != message.Anchor.TargetEntity)
            talk = {};
        talk.Anchor = message.Anchor;
        if (!talk.Encounter) talk.Encounter = message.Anchor.Session;
        talk.Closed = false;
    }
    if (message.Anchor.Choice > previousChoice)
    {
        talk.ChoiceListeners = talk.Listeners;
        talk.ChoiceExpires = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    }
    // A continuation can reuse the explicit participants once, only on a game-initiated
    // conversation with the same opponent after the original conversation closed.
    auto fight = party->Brawls.find(pSpeaker->GetId());
    const bool followup = message.Anchor.Cell && message.Anchor.GameInitiated && fight != party->Brawls.end() &&
        fight->second.MayResume && fight->second.Expires > std::chrono::steady_clock::now() && fight->second.Anchor.Session != talk.Anchor.Session &&
        fight->second.Anchor.TargetForm == talk.Anchor.TargetForm;
    talk.Expires = std::chrono::steady_clock::now() + std::chrono::seconds(talk.Closed ? 6 : 4);
    message.Text = acMessage.Packet.Text;
    message.NpcLine = acMessage.Packet.NpcLine;
    message.SpeakerId = pSpeaker->GetId();
    message.Name = pSpeaker->GetUsername();
    GameServer::Get()->SendToParty(message, pSpeaker->GetParty(), pSpeaker);
    if (followup)
    {
        for (auto* peer : party->Members)
        {
            if (peer == pSpeaker || !peer->GetCharacter())
                continue;
            const auto& cell = peer->GetCellComponent();
            const auto* pos = m_world.try_get<MovementComponent>(*peer->GetCharacter());
            if (cell.WorldSpaceId != talk.Anchor.WorldSpace || (!cell.WorldSpaceId && cell.Cell != talk.Anchor.Cell) ||
                !pos || !PartyService::WithinSyncRange(m_world, *pSpeaker, *peer))
                continue;
            NotifyCoopInteraction recovered;
            recovered.Event = {Interaction::OpponentState, pSpeaker->GetId(), fight->second.Anchor.Session, 2, talk.Anchor.TargetForm};
            peer->Send(recovered);
            if (!fight->second.Listeners.count(peer->GetId()) ||
                glm::distance(pos->Position, static_cast<const glm::vec3&>(talk.Anchor.Position)) > 300.f)
                continue;
            talk.Listeners.insert(peer->GetId());
            NotifyCoopInteraction joined;
            joined.Event = {Interaction::Rejoin, pSpeaker->GetId(), talk.Anchor.Session, 0, talk.Anchor.TargetForm};
            peer->Send(joined);
        }
        talk.Encounter = fight->second.Encounter;
        talk.Settlement = true;
        party->Brawls.erase(fight);
    }
    if (opening && talk.Anchor.TargetForm)
    {
        NotifyCoopInteraction watch;
        watch.Event = {Interaction::WatchOutcomes, pSpeaker->GetId(), talk.Anchor.Session, talk.Settlement ? 1u : 0u, talk.Anchor.TargetForm};
        pSpeaker->Send(watch);
    }
}

void OverlayService::OnTeleport(const PacketEvent<TeleportRequest>& acMessage) const noexcept
{
    Player* pTargetPlayer = m_world.GetPlayerManager().GetById(acMessage.Packet.PlayerId);
    if (!pTargetPlayer)
        return;

    NotifyTeleport response{};

    auto character = pTargetPlayer->GetCharacter();
    if (character)
    {
        const auto* pMovementComponent = m_world.try_get<MovementComponent>(*character);
        if (pMovementComponent)
        {
            const auto& cellComponent = pTargetPlayer->GetCellComponent();
            response.CellId = cellComponent.Cell;
            response.Position = pMovementComponent->Position;
            response.WorldSpaceId = cellComponent.WorldSpaceId;
        }
    }

    acMessage.pPlayer->Send(response);
}

void OverlayService::OnPlayerHealthUpdate(const PacketEvent<RequestPlayerHealthUpdate>& acMessage) const noexcept
{
    NotifyPlayerHealthUpdate notify{};
    notify.PlayerId = acMessage.pPlayer->GetId();
    notify.Percentage = acMessage.Packet.Percentage;

    GameServer::Get()->SendToParty(notify, acMessage.pPlayer->GetParty(), acMessage.GetSender());
}
