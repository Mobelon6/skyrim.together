#include <Services/PlayerService.h>

#include <World.h>
#include <Utils.h>

#include <Events/UpdateEvent.h>
#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/GridCellChangeEvent.h>
#include <Events/CellChangeEvent.h>
#include <Events/PlayerDialogueEvent.h>
#include <Games/Misc/MenuTopicManager.h>
#include <Forms/TESWorldSpace.h>
#include <Events/PlayerLevelEvent.h>
#include <Events/PartyJoinedEvent.h>
#include <Events/PartyLeftEvent.h>
#include <Events/BeastFormChangeEvent.h>

#include <Messages/PlayerRespawnRequest.h>
#include <Messages/RequestCoopInteraction.h>
#include <Messages/NotifyPlayerRespawn.h>
#include <Messages/ShiftGridCellRequest.h>
#include <Messages/EnterExteriorCellRequest.h>
#include <Messages/EnterInteriorCellRequest.h>
#include <Messages/PlayerDialogueRequest.h>
#include <Messages/PlayerLevelRequest.h>

#include <Structs/ServerSettings.h>

#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESGlobal.h>
#include <Games/Overrides.h>
#include <Games/References.h>
#include <AI/AIProcess.h>
#include <EquipManager.h>
#include <Forms/TESRace.h>

PlayerService::PlayerService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
    , m_transport(aTransport)
{
    m_updateConnection = m_dispatcher.sink<UpdateEvent>().connect<&PlayerService::OnUpdate>(this);
    m_connectedConnection = m_dispatcher.sink<ConnectedEvent>().connect<&PlayerService::OnConnected>(this);
    m_disconnectedConnection = m_dispatcher.sink<DisconnectedEvent>().connect<&PlayerService::OnDisconnected>(this);
    m_settingsConnection = m_dispatcher.sink<ServerSettings>().connect<&PlayerService::OnServerSettingsReceived>(this);
    m_notifyRespawnConnection = m_dispatcher.sink<NotifyPlayerRespawn>().connect<&PlayerService::OnNotifyPlayerRespawn>(this);
    m_gridCellChangeConnection = m_dispatcher.sink<GridCellChangeEvent>().connect<&PlayerService::OnGridCellChangeEvent>(this);
    m_cellChangeConnection = m_dispatcher.sink<CellChangeEvent>().connect<&PlayerService::OnCellChangeEvent>(this);
    m_playerDialogueConnection = m_dispatcher.sink<PlayerDialogueEvent>().connect<&PlayerService::OnPlayerDialogueEvent>(this);
    m_playerLevelConnection = m_dispatcher.sink<PlayerLevelEvent>().connect<&PlayerService::OnPlayerLevelEvent>(this);
    m_partyJoinedConnection = aDispatcher.sink<PartyJoinedEvent>().connect<&PlayerService::OnPartyJoinedEvent>(this);
    m_partyLeftConnection = aDispatcher.sink<PartyLeftEvent>().connect<&PlayerService::OnPartyLeftEvent>(this);
}

void PlayerService::OnUpdate(const UpdateEvent&) noexcept
{
    RunRespawnUpdates();
    RunPostDeathUpdates();
    RunDifficultyUpdates();
    RunLevelUpdates();
    RunBeastFormDetection();
    UpdateDialogue();
}

void PlayerService::OnConnected(const ConnectedEvent& acEvent) noexcept
{
    // TODO: SkyrimTogether.esm
    TESGlobal* pKillMove = Cast<TESGlobal>(TESForm::GetById(0x100F19));
    pKillMove->f = 0.f;

    TESGlobal* pWorldEncountersEnabled = Cast<TESGlobal>(TESForm::GetById(0xB8EC1));
    pWorldEncountersEnabled->f = 0.f;
}

void PlayerService::OnDisconnected(const DisconnectedEvent& acEvent) noexcept
{
    if (auto* player=PlayerCharacter::Get())
    {
        player->SetDifficulty(m_previousDifficulty);
        ToggleDeathSystem(false);
        if (m_godmodeStart) PlayerCharacter::SetGodMode(false);
    }
    else m_isDeathSystemEnabled=false;
    m_serverDifficulty = m_previousDifficulty = 6;
    ResetWorldTracking();

    TESGlobal* pKillMove = Cast<TESGlobal>(TESForm::GetById(0x100F19));
    if (pKillMove) pKillMove->f = 1.f;

    // Restore to the default value (150 in skyrim, 175 in fallout 4)
    float* greetDistance = Settings::GetGreetDistance();
    if (greetDistance) *greetDistance = 150.f;

    TESGlobal* pWorldEncountersEnabled = Cast<TESGlobal>(TESForm::GetById(0xB8EC1));
    if (pWorldEncountersEnabled) pWorldEncountersEnabled->f = 1.f;
}

void PlayerService::OnServerSettingsReceived(const ServerSettings& acSettings) noexcept
{
    m_previousDifficulty = *Settings::GetDifficulty();
    PlayerCharacter::Get()->SetDifficulty(acSettings.Difficulty);
    m_serverDifficulty = acSettings.Difficulty;

    if (!acSettings.GreetingsEnabled)
    {
        float* greetDistance = Settings::GetGreetDistance();
        *greetDistance = 0.f;
    }

    ToggleDeathSystem(acSettings.DeathSystemEnabled);
}

void PlayerService::OnNotifyPlayerRespawn(const NotifyPlayerRespawn& acMessage) const noexcept
{
    PlayerCharacter::Get()->PayGold(acMessage.GoldLost);

    std::string message = fmt::format("You died and lost {} gold.", acMessage.GoldLost);
    Utils::ShowHudMessage(String(message));
}

void PlayerService::OnGridCellChangeEvent(const GridCellChangeEvent& acEvent) const noexcept
{
    uint32_t baseId = 0;
    uint32_t modId = 0;

    if (m_world.GetModSystem().GetServerModId(acEvent.WorldSpaceId, modId, baseId))
    {
        ShiftGridCellRequest request;
        request.WorldSpaceId = GameId(modId, baseId);
        request.PlayerCell = acEvent.PlayerCell;
        request.CenterCoords = acEvent.CenterCoords;
        request.Cells = acEvent.Cells;

        m_transport.Send(request);
    }
}

void PlayerService::OnCellChangeEvent(const CellChangeEvent& acEvent) noexcept
{
    m_entryCell = 0;
    auto* player = PlayerCharacter::Get();
    auto* cell = player ? player->GetParentCellEx() : nullptr;
    if (cell && !acEvent.WorldSpaceId && !player->actorState.IsBleedingOut())
    {
        m_entryCell = cell->formID;
        m_entryX = player->position.x;
        m_entryY = player->position.y;
        m_entryZ = player->position.z;
    }
    if (acEvent.WorldSpaceId)
    {
        EnterExteriorCellRequest message;
        message.CellId = acEvent.CellId;
        message.WorldSpaceId = acEvent.WorldSpaceId;
        message.CurrentCoords = acEvent.CurrentCoords;

        m_transport.Send(message);
    }
    else
    {
        EnterInteriorCellRequest message;
        message.CellId = acEvent.CellId;

        m_transport.Send(message);
    }
}

uint32_t PlayerService::ActiveDialogueSession(uint32_t form) noexcept
{
    const auto* menu = MenuTopicManager::Get();
    const auto* actor = menu && menu->IsDialogueOpen() ? TESObjectREFR::GetByHandle(menu->speaker.handle.iBits) : nullptr;
    if (!actor || actor->formID != form)
        return 0;
    UpdateDialogue();
    return menu->speaker.handle.iBits == m_dialogueHandle ? m_dialogueSession : 0;
}

void PlayerService::OnPlayerDialogueEvent(const PlayerDialogueEvent& acEvent) noexcept
{
    UpdateDialogue(&acEvent);
}

void PlayerService::UpdateDialogue(const PlayerDialogueEvent* apChoice) noexcept
{
    if (!m_transport.IsOnline())
    {
        m_dialogueHandle = m_closedSpeaker = 0;
        m_dialogueText.clear();
        return;
    }
    const auto* pMenu = MenuTopicManager::Get();
    const uint32_t handle = pMenu && pMenu->IsDialogueOpen() && pMenu->speaker ? pMenu->speaker.handle.iBits : 0;
    const auto now = std::chrono::steady_clock::now();
    if (apChoice && !handle)
    {
        // A goodbye, quest offer or brawl can close the vanilla menu before the game-thread callback runs.
        // Complete the known session without briefly reopening the observer camera.
        const bool closing = m_dialogueHandle && apChoice->SpeakerHandle == m_dialogueHandle;
        if (!closing && (!m_closedSpeaker || m_closedSpeaker != apChoice->SpeakerHandle || now >= m_closedUntil))
            return;
        PlayerDialogueRequest last;
        last.Anchor.Session = m_dialogueSession;
        last.Anchor.Choice = ++m_dialogueChoice;
        if (apChoice->Info)
            m_world.GetModSystem().GetServerModId(apChoice->Info, last.Anchor.TopicInfo);
        if (apChoice->Quest)
            m_world.GetModSystem().GetServerModId(apChoice->Quest, last.Anchor.TopicQuest);
        last.Text = apChoice->Text;
        m_transport.Send(last);
        m_closedSpeaker = apChoice->SpeakerHandle;
        m_closedUntil = now + 1s;
        m_dialogueHandle = 0;
        return;
    }
    if (apChoice && apChoice->SpeakerHandle != handle)
        return;
    auto* speaking = handle ? Cast<Actor>(TESObjectREFR::GetByHandle(handle)) : nullptr;
    const auto* high = speaking && speaking->currentProcess ? speaking->currentProcess->high : nullptr;
    const char* subtitle = high && high->strVoiceSubtitle ? high->strVoiceSubtitle : "";
    if (handle == m_dialogueHandle && (!handle || (now < m_nextDialogueUpdate && !apChoice && m_npcSubtitle == subtitle)))
        return;

    PlayerDialogueRequest request;
    if (handle)
    {
        auto* pTarget = TESObjectREFR::GetByHandle(handle);
        auto* pCell = pTarget ? pTarget->GetParentCellEx() : nullptr;
        if (!pCell || !m_world.GetModSystem().GetServerModId(pCell->formID, request.Anchor.Cell))
            return;
        if (auto* pSpace = pTarget->GetWorldSpace())
            if (!m_world.GetModSystem().GetServerModId(pSpace->formID, request.Anchor.WorldSpace))
                return;
        request.Anchor.Position = pTarget->position;
        if ((pTarget->formID >> 24) != 0xFF)
            m_world.GetModSystem().GetServerModId(pTarget->formID, request.Anchor.TargetForm);
        auto forms = m_world.view<FormIdComponent>();
        for (auto entity : forms)
        {
            if (forms.get<FormIdComponent>(entity).Id == pTarget->formID)
            {
                request.Anchor.TargetEntity = Utils::GetServerId(entity).value_or(UINT32_MAX);
                break;
            }
        }
        if (handle != m_dialogueHandle)
        {
            m_gameInitiated = !m_world.GetOverlayService().WasActivated(pTarget->formID);
            ++m_dialogueSession;
            m_dialogueChoice = 0;
            m_dialogueInfo = m_dialogueQuest = m_closedSpeaker = 0;
            m_dialogueText.clear();
        }
        if (apChoice)
        {
            ++m_dialogueChoice;
            m_dialogueInfo = apChoice->Info;
            m_dialogueQuest = apChoice->Quest;
            m_dialogueText = apChoice->Text;
            m_dialogueTextUntil = now + 5s;
        }
        if (m_dialogueQuest)
            m_world.GetModSystem().GetServerModId(m_dialogueQuest, request.Anchor.TopicQuest);
        if (m_dialogueInfo)
            m_world.GetModSystem().GetServerModId(m_dialogueInfo, request.Anchor.TopicInfo);
        if (now < m_dialogueTextUntil)
            request.Text = m_dialogueText;
    }
    else
    {
        m_closedSpeaker = m_dialogueHandle;
        m_closedUntil = now + 1s;
        m_dialogueText.clear();
    }

    m_npcSubtitle = subtitle;
    request.NpcLine = m_npcSubtitle;
    request.Anchor.GameInitiated = m_gameInitiated;
    request.Anchor.Session = m_dialogueSession;
    request.Anchor.Choice = m_dialogueChoice;
    m_transport.Send(request);
    m_dialogueHandle = handle;
    m_nextDialogueUpdate = now + 1s;
}

void PlayerService::OnPlayerLevelEvent(const PlayerLevelEvent& acEvent) const noexcept
{
    if (!m_transport.IsConnected())
        return;

    PlayerLevelRequest request{};
    request.NewLevel = PlayerCharacter::Get()->GetLevel();

    m_transport.Send(request);
}

void PlayerService::OnPartyJoinedEvent(const PartyJoinedEvent& acEvent) noexcept
{
    if (auto* pEncounters = Cast<TESGlobal>(TESForm::GetById(0xB8EC1)))
        pEncounters->f = acEvent.IsLeader ? 1.f : 0.f;
}

void PlayerService::OnPartyLeftEvent(const PartyLeftEvent& acEvent) noexcept
{
    m_peerToken = 0;
    // TODO: this can be done a bit prettier
    if (World::Get().GetTransport().IsConnected())
    {
        TESGlobal* pWorldEncountersEnabled = Cast<TESGlobal>(TESForm::GetById(0xB8EC1));
        pWorldEncountersEnabled->f = 0.f;
    }
}

void PlayerService::RunRespawnUpdates() noexcept
{
    if (!m_isDeathSystemEnabled)
        return;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || m_world.IsLoadingSave()) return;
    if (!pPlayer->actorState.IsBleedingOut())
    {
        if (m_waitingRescue)
        {
            ReportRescueState(false);
            pPlayer->SetNoBleedoutRecovery(true);
            m_world.GetOverlayService().ShowRescueReturn(false);
            m_waitingRescue = m_returnRequested = m_returnOffered = m_healPending = false;
        }
        m_cachedMainSpellId = pPlayer->magicItems[0] ? pPlayer->magicItems[0]->formID : 0;
        m_cachedSecondarySpellId = pPlayer->magicItems[1] ? pPlayer->magicItems[1]->formID : 0;
        m_cachedPowerId = pPlayer->equippedShout ? pPlayer->equippedShout->formID : 0;

        m_respawnTimerStarted = false;
        return;
    }

    if (!m_respawnTimerStarted)
    {
        m_respawnTimerStarted = true;
        m_respawnDeadline = std::chrono::steady_clock::now() + 5s;
        m_waitingRescue = m_world.GetPartyService().GetPartyMembers().size() == 2;
        if (m_waitingRescue)
        {
            if (!++m_rescueToken) ++m_rescueToken;
            ReportRescueState(true);
            Utils::ShowHudMessage("Вы упали. Напарник может вас вылечить.");
        }
        else FadeOutGame(true, true, 3.0f, true, 2.0f);

        // If a player dies not by its health reaching 0, getting it up from its bleedout state isn't possible
        // just by setting its health back to max. Therefore, put it to 0.
        if (pPlayer->GetActorValue(ActorValueInfo::kHealth) > 0.f)
            pPlayer->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, 0);

    }

    const auto cNow = std::chrono::steady_clock::now();
    if (m_waitingRescue && !m_returnRequested)
    {
        const bool alone = m_world.GetPartyService().GetPartyMembers().size() != 2;
        if (alone != m_returnOffered)
        {
            m_returnOffered = alone;
            m_world.GetOverlayService().ShowRescueReturn(alone);
        }
        if (m_healPending)
        {
            if (cNow > m_healUntil) m_healPending = false;
            else if (pPlayer->GetActorValue(ActorValueInfo::kHealth) > 0.f)
                pPlayer->SetNoBleedoutRecovery(false);
        }
        if (!alone && cNow >= m_respawnDeadline)
        {
            ReportRescueState(true);
            m_respawnDeadline = cNow + 2s;
        }
        return;
    }
    if (m_returnRequested || cNow >= m_respawnDeadline)
    {
        if (m_waitingRescue)
        {
            ReportRescueState(false);
            m_world.GetOverlayService().ShowRescueReturn(false);
        }
        auto* cell = pPlayer->GetParentCellEx();
        if (m_waitingRescue && !pPlayer->GetWorldSpace() && cell)
        {
            // Stay inside the current scripted interior if its entry was not observed.
            const auto entry = cell->formID == m_entryCell ? NiPoint3(glm::vec3(m_entryX,m_entryY,m_entryZ)) : pPlayer->position;
            pPlayer->SetNoBleedoutRecovery(false);
            pPlayer->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, 1000000);
            pPlayer->MoveTo(cell,entry);
            pPlayer->SetNoBleedoutRecovery(true);
        }
        else pPlayer->RespawnPlayer();
        m_waitingRescue = m_returnRequested = m_returnOffered = m_healPending = false;

        m_transport.Send(PlayerRespawnRequest());

        m_respawnTimerStarted = false;

        auto* pEquipManager = EquipManager::Get();
        TESForm* pSpell = TESForm::GetById(m_cachedMainSpellId);
        if (pSpell)
            pEquipManager->EquipSpell(pPlayer, pSpell, 0);
        pSpell = TESForm::GetById(m_cachedSecondarySpellId);
        if (pSpell)
            pEquipManager->EquipSpell(pPlayer, pSpell, 1);
        pSpell = TESForm::GetById(m_cachedPowerId);
        if (pSpell)
            pEquipManager->EquipShout(pPlayer, pSpell);

        m_knockdownDeadline = std::chrono::steady_clock::now() + 1500ms;
        m_knockdownStart = true;
    }
}

// Doesn't seem to respawn quite yet
void PlayerService::RunPostDeathUpdates() noexcept
{
    if (!m_isDeathSystemEnabled)
        return;

    // If a player dies in ragdoll, it gets stuck.
    // This code ragdolls the player again upon respawning.
    // It also makes the player invincible for 10 seconds.
    const auto cNow = std::chrono::steady_clock::now();
    if (m_knockdownStart && cNow >= m_knockdownDeadline)
    {
        PlayerCharacter* pPlayer = PlayerCharacter::Get();

        PlayerCharacter::SetGodMode(true);
        m_godmodeStart = true;

        if (pPlayer->currentProcess) pPlayer->currentProcess->KnockExplosion(pPlayer, &pPlayer->position, 0.f);

        FadeOutGame(false, true, 0.5f, true, 2.f);

        m_godmodeDeadline = std::chrono::steady_clock::now() + 10s;
        m_knockdownStart = false;
    }

    if (m_godmodeStart && cNow >= m_godmodeDeadline)
    {
        PlayerCharacter::SetGodMode(false);

        m_godmodeStart = false;
    }
}

void PlayerService::RunDifficultyUpdates() const noexcept
{
    if (!m_transport.IsConnected())
        return;

    PlayerCharacter::Get()->SetDifficulty(m_serverDifficulty);
}

void PlayerService::RunLevelUpdates() const noexcept
{
    // The LevelUp hook is kinda weird, so ehh, just check periodically, doesn't really cost anything.

    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 1000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    static uint16_t oldLevel = PlayerCharacter::Get()->GetLevel();

    uint16_t newLevel = PlayerCharacter::Get()->GetLevel();
    if (newLevel != oldLevel)
    {
        PlayerLevelRequest request{};
        request.NewLevel = newLevel;

        m_transport.Send(request);

        oldLevel = newLevel;
    }
}

void PlayerService::RunBeastFormDetection() const noexcept
{
    static uint32_t lastRaceFormID = 0;
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 250ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer->race)
        return;

    if (pPlayer->race->formID == lastRaceFormID)
        return;

    if (pPlayer->race->formID == 0x200283A || pPlayer->race->formID == 0xCDD84)
        m_world.GetDispatcher().trigger(BeastFormChangeEvent());

    lastRaceFormID = pPlayer->race->formID;
}

void PlayerService::ToggleDeathSystem(bool aSet) noexcept
{
    m_isDeathSystemEnabled = aSet;

    PlayerCharacter::Get()->SetPlayerRespawnMode(aSet);
}

void PlayerService::ReportRescueState(bool down) noexcept
{
    RequestCoopInteraction packet;
    packet.Event = {down ? Interaction::PlayerDown : Interaction::PlayerUp, 0, m_rescueToken};
    m_transport.Send(packet);
}

void PlayerService::OnRescueState(const CoopInteraction& e) noexcept
{
    if (e.Kind == Interaction::PlayerDown)
    {
        m_peerId = e.Speaker;
        m_peerToken = e.Session;
    }
    if (e.Kind == Interaction::PlayerUp && m_peerId == e.Speaker && m_peerToken == e.Session)
        m_peerToken = 0;
    if (e.Kind == Interaction::ApplyRescue && m_waitingRescue && e.Session == m_rescueToken && e.Value)
    {
        auto* player = PlayerCharacter::Get();
        if (!player || !player->actorState.IsBleedingOut() || m_world.IsLoadingSave()) return;
        const auto hp = std::min(player->GetActorPermanentValue(ActorValueInfo::kHealth), e.Value / 100.f);
        if (hp <= 0) return;
        player->SetNoBleedoutRecovery(false);
        player->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, hp);
        ObserveRescueSpell();
    }
    if (e.Kind == Interaction::PartyWipe && m_waitingRescue && e.Session == m_rescueToken)
        m_returnRequested = true;
}

void PlayerService::RequestReturn() noexcept
{
    if (m_waitingRescue && m_returnOffered && m_world.GetPartyService().GetPartyMembers().size() != 2)
        m_returnRequested = true;
}

void PlayerService::ObserveRescueSpell() noexcept
{
    if (!m_waitingRescue) return;
    m_healPending = true;
    m_healUntil = std::chrono::steady_clock::now() + std::chrono::seconds(3);
}

bool PlayerService::IsFallenPeer(uint32_t peer) const noexcept
{
    const auto& members = m_world.GetPartyService().GetPartyMembers();
    return peer != UINT32_MAX && m_peerToken && m_peerId == peer && members.size() == 2 &&
        std::find(members.begin(),members.end(),peer) != members.end();
}
