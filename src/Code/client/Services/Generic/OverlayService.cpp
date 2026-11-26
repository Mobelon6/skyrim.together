#include <TiltedOnlinePCH.h>

#include <Services/OverlayService.h>

#include <OverlayApp.hpp>
#include <Messages/NotifyCoopInteraction.h>
#include <Messages/NotifyPartyHome.h>
#include <Messages/NotifyPartyTrade.h>

#include <D3D11Hook.hpp>
#include <OverlayRenderHandlerD3D11.hpp>

#include <Systems/RenderSystemD3D11.h>

#include <World.h>

#include <Services/OverlayClient.h>
#include <Services/TransportService.h>

#include <Messages/NotifyChatMessageBroadcast.h>
#include <Messages/NotifyPlayerList.h>
#include <Messages/NotifyPlayerLeft.h>
#include <Messages/NotifyPlayerJoined.h>
#include <Messages/NotifyPlayerDialogue.h>
#include <Messages/NotifyPlayerLevel.h>
#include <Messages/NotifyPlayerCellChanged.h>
#include <Messages/NotifyTeleport.h>
#include <Messages/RequestPlayerHealthUpdate.h>
#include <Messages/NotifyPlayerHealthUpdate.h>

#include <Structs/GridCellCoords.h>

#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/ConnectionErrorEvent.h>
#include <Events/UpdateEvent.h>
#include <Events/PartyJoinedEvent.h>
#include <Events/PartyLeftEvent.h>

#include <PlayerCharacter.h>
#include <Events/ActivateEvent.h>
#include <Services/PartyService.h>
#include <Games/Misc/MenuTopicManager.h>
#include <Interface/Menus/HUDMenuUtils.h>
#include <Services/ImguiService.h>
#include <Interface/UI.h>
#include <NetImmerse/NiNode.h>
#include <Utils.h>
#include <cmath>
#include <limits>
#include <Forms/TESWorldSpace.h>
#include <Forms/TESObjectCELL.h>
#include <Games/ActorExtension.h>

using TiltedPhoques::OverlayRenderHandler;
using TiltedPhoques::OverlayRenderHandlerD3D11;

struct D3D11RenderProvider final : OverlayApp::RenderProvider, OverlayRenderHandlerD3D11::Renderer
{
    explicit D3D11RenderProvider(RenderSystemD3D11* apRenderSystem)
        : m_pRenderSystem(apRenderSystem)
    {
    }

    OverlayRenderHandler* Create() override
    {
        auto* pHandler = new OverlayRenderHandlerD3D11(this);
        pHandler->SetVisible(true);

        return pHandler;
    }

    [[nodiscard]] HWND GetWindow() override { return m_pRenderSystem->GetWindow(); }

    [[nodiscard]] IDXGISwapChain* GetSwapChain() const noexcept override { return m_pRenderSystem->GetSwapChain(); }
    [[nodiscard]] ID3D11Device* GetDevice() const noexcept override { return m_pRenderSystem->GetDevice(); }
    [[nodiscard]] ID3D11DeviceContext* GetDeviceContext() const noexcept override { return m_pRenderSystem->GetDeviceContext(); }

private:
    RenderSystemD3D11* m_pRenderSystem;
};

/**
 * @brief Makes CEF draw the UI on the CPU instead of the GPU.
 *
 * On the GPU, CEF copies every frame back to memory for OnPaint, and that waits behind the game's frames when
 * the game keeps the GPU busy: CEF drops UI frames and the UI shows up half drawn.
 */
struct CpuRenderedOverlayApp final : OverlayApp
{
    using OverlayApp::OverlayApp;

    void OnBeforeCommandLineProcessing(const CefString& acProcessType, CefRefPtr<CefCommandLine> apCommandLine) override
    {
        OverlayApp::OnBeforeCommandLineProcessing(acProcessType, apCommandLine);

        apCommandLine->AppendSwitch("disable-gpu");
        apCommandLine->AppendSwitch("disable-gpu-compositing");

        spdlog::info("Overlay: CEF draws the UI on the CPU (disable-gpu, disable-gpu-compositing)");
    }
};

String GetCellName(const GameId& aWorldSpaceId, const GameId& aCellId) noexcept
{
    auto& modSystem = World::Get().GetModSystem();

    String cellName = "UNKNOWN";

    if (aWorldSpaceId)
    {
        const uint32_t worldSpaceId = modSystem.GetGameId(aWorldSpaceId);
        TESWorldSpace* pWorldSpace = Cast<TESWorldSpace>(TESForm::GetById(worldSpaceId));
        if (pWorldSpace)
            cellName = pWorldSpace->GetName();
    }
    else
    {
        const uint32_t cellId = modSystem.GetGameId(aCellId);
        TESObjectCELL* pCell = Cast<TESObjectCELL>(TESForm::GetById(cellId));
        if (pCell)
            cellName = pCell->GetName();
    }

    return cellName;
}

float CalculateHealthPercentage(Actor* apActor) noexcept
{
    const float maxHealth = apActor->GetActorPermanentValue(ActorValueInfo::kHealth);
    const float tempModHealth = apActor->healthModifiers.temporaryModifier;

    if (maxHealth == 0.f)
        return 0.f;
    const float health = apActor->GetActorValue(ActorValueInfo::kHealth);

    float percentage = health / (maxHealth + tempModHealth) * 100.f;
    if (percentage < 0.f)
        percentage = 0.f;

    return percentage;
}

OverlayService::OverlayService(World& aWorld, TransportService& transport, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_transport(transport)
{
    m_homeConnection = aDispatcher.sink<NotifyPartyHome>().connect<&OverlayService::OnHome>(this);
    m_tradeConnection = aDispatcher.sink<NotifyPartyTrade>().connect<&OverlayService::OnTrade>(this);
    m_interactionConnection = aDispatcher.sink<NotifyCoopInteraction>().connect<&OverlayService::OnInteraction>(this);
    m_boardConnection = aDispatcher.sink<ActivateEvent>().connect<&OverlayService::OnBoardCarriage>(this);
    m_noticeConnection = aDispatcher.sink<NotifyPartyTravel>().connect<&OverlayService::OnTravelNotice>(this);
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&OverlayService::OnUpdate>(this);
    m_connectedConnection = aDispatcher.sink<ConnectedEvent>().connect<&OverlayService::OnConnectedEvent>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&OverlayService::OnDisconnectedEvent>(this);
    m_connectionErrorConnection = aDispatcher.sink<ConnectionErrorEvent>().connect<&OverlayService::OnConnectionError>(this);
    m_chatMessageConnection = aDispatcher.sink<NotifyChatMessageBroadcast>().connect<&OverlayService::OnChatMessageReceived>(this);
    m_playerJoinedConnection = aDispatcher.sink<NotifyPlayerJoined>().connect<&OverlayService::OnPlayerJoined>(this);
    m_playerLeftConnection = aDispatcher.sink<NotifyPlayerLeft>().connect<&OverlayService::OnPlayerLeft>(this);
    m_drawDialogueConnection = aWorld.ctx().at<ImguiService>().OnDraw.connect<&OverlayService::DrawDialogueMarker>(this);
    m_playerDialogueConnection = aDispatcher.sink<NotifyPlayerDialogue>().connect<&OverlayService::OnPlayerDialogue>(this);
    m_playerAddedConnection = m_world.on_destroy<WaitingFor3D>().connect<&OverlayService::OnWaitingFor3DRemoved>(this);
    m_playerRemovedConnection = m_world.on_destroy<PlayerComponent>().connect<&OverlayService::OnPlayerComponentRemoved>(this);
    m_playerLevelConnection = aDispatcher.sink<NotifyPlayerLevel>().connect<&OverlayService::OnPlayerLevel>(this);
    m_cellChangedConnection = aDispatcher.sink<NotifyPlayerCellChanged>().connect<&OverlayService::OnPlayerCellChanged>(this);
    m_teleportConnection = aDispatcher.sink<NotifyTeleport>().connect<&OverlayService::OnNotifyTeleport>(this);
    m_playerHealthConnection = aDispatcher.sink<NotifyPlayerHealthUpdate>().connect<&OverlayService::OnNotifyPlayerHealthUpdate>(this);
    m_partyJoinedConnection = aDispatcher.sink<PartyJoinedEvent>().connect<&OverlayService::OnPartyJoinedEvent>(this);
    m_partyLeftConnection = aDispatcher.sink<PartyLeftEvent>().connect<&OverlayService::OnPartyLeftEvent>(this);
}

OverlayService::~OverlayService() noexcept
{
}

void OverlayService::Create(RenderSystemD3D11* apRenderSystem) noexcept
{
    m_pProvider = TiltedPhoques::MakeUnique<D3D11RenderProvider>(apRenderSystem);
    m_gameOverlayClient = new ::OverlayClient(m_transport, m_pProvider->Create());
    m_pOverlay = new CpuRenderedOverlayApp(m_pProvider.get(), m_gameOverlayClient);

    if (!m_pOverlay->Initialize())
    {
        spdlog::error("Overlay could not be initialized");
        if (int32_t exitCode = CefGetExitCode())
        {
            spdlog::critical("CEF failed to initialize, exit code {}. See 'cef_types.h' for description", exitCode);
        }
    }

    m_pOverlay->GetClient()->Create();
}

void OverlayService::Render() noexcept
{
    auto pPlayer = PlayerCharacter::Get();
    bool inGame = pPlayer && pPlayer->GetNiNode();
    if (inGame && !m_inGame)
        SetInGame(true);
    else if (!inGame && m_inGame)
        SetInGame(false);

    // Game-update callbacks can stop during the system menu. Its UI keeps rendering.
    if (inGame)
    {
        const auto* ui = UI::Get();
        const bool open = !ui || ui->BlocksPartyHud();
        if (m_gameMenuOpen != open)
        {
            auto args = CefListValue::Create();
            args->SetBool(0, open);
            m_pOverlay->ExecuteAsync("openingMenu", args);
            m_gameMenuOpen = open;
        }
    }
    else
        m_gameMenuOpen.reset();

    m_pOverlay->GetClient()->Render();
}

void OverlayService::Reset() const noexcept
{
    m_pOverlay->GetClient()->Reset();
}

void OverlayService::Reload() noexcept
{
    SetInGame(false);
    SetActive(false);
    GetOverlayApp()->GetClient()->GetBrowser()->Reload();
    Initialize();
    SetInGame(true);
    m_pOverlay->ExecuteAsync("enterGame");
    SetActive(true);
}

void OverlayService::Initialize() noexcept
{
    m_gameMenuOpen.reset();
    m_pOverlay->ExecuteAsync("init");
}

void OverlayService::SetActive(bool aActive) noexcept
{
    if (!m_inGame)
        return;
    if (m_active == aActive)
        return;

    m_active = aActive;

    m_pOverlay->ExecuteAsync(m_active ? "activate" : "deactivate");
}

bool OverlayService::GetActive() const noexcept
{
    return m_active;
}

void OverlayService::SetInGame(bool aInGame) noexcept
{
    if (m_inGame == aInGame)
        return;
    if (!aInGame && m_active && m_gameOverlayClient)
        m_gameOverlayClient->SetUIVisible(false);
    m_inGame = aInGame;

    if (m_inGame)
    {
        SetVersion(BUILD_COMMIT);
        m_pOverlay->ExecuteAsync("enterGame");
    }
    else
    {
        m_pOverlay->ExecuteAsync("exitGame");
    }
}

bool OverlayService::GetInGame() const noexcept
{
    return m_inGame;
}

void OverlayService::SetVersion(const std::string& acVersion)
{
    if (!m_pOverlay)
        return;

    auto pArguments = CefListValue::Create();

    pArguments->SetString(0, acVersion);
    m_pOverlay->ExecuteAsync("setVersion", pArguments);
}

void OverlayService::SendSystemMessage(const std::string& acMessage)
{
    if (!m_pOverlay)
        return;

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, kSystemMessage);
    pArguments->SetString(1, acMessage);

    m_pOverlay->ExecuteAsync("message", pArguments);
}

void OverlayService::SetPlayerHealthPercentage(uint32_t aFormId) const noexcept
{
    Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId));
    if (!pActor)
    {
        spdlog::error("{}: cannot find actor for form id {:X}", __FUNCTION__, aFormId);
        return;
    }

    float percentage = CalculateHealthPercentage(pActor);

    auto view = m_world.view<FormIdComponent, PlayerComponent>();
    auto entityIt = std::find_if(view.begin(), view.end(), [view, aFormId](auto aEntity) { return view.get<FormIdComponent>(aEntity).Id == aFormId; });

    if (entityIt == view.end())
    {
        spdlog::error("{}: cannot find player entity for form id {:X}", __FUNCTION__, aFormId);
        return;
    }

    const auto& playerComponent = view.get<PlayerComponent>(*entityIt);

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, playerComponent.Id);
    pArguments->SetDouble(1, static_cast<double>(percentage));
    m_pOverlay->ExecuteAsync("setHealth", pArguments);
}

void OverlayService::OnUpdate(const UpdateEvent&) noexcept
{
    RunDebugDataUpdates();
    RunPlayerHealthUpdates();
    UpdateHomes();
    UpdateTrade();
    UpdateInteraction();
    UpdateDialogueHud();
    UpdateMapTravel();
    UpdateCarriage();
    UpdateProgress();
}

void OverlayService::OnConnectedEvent(const ConnectedEvent& acEvent) noexcept
{
    m_pOverlay->ExecuteAsync("connect");

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acEvent.PlayerId);
    m_pOverlay->ExecuteAsync("setLocalPlayerId", pArguments);
}

void OverlayService::OnDisconnectedEvent(const DisconnectedEvent&) noexcept
{
    ClearInteraction();
    ClearCarriage();
    m_dismissedTalk = 0;
    m_conversations.clear();
    ClearDialogueHud();
    if (m_pOverlay) m_pOverlay->ExecuteAsync("disconnect");
}

void OverlayService::OnWaitingFor3DRemoved(entt::registry& aRegistry, entt::entity aEntity) const noexcept
{
    if (m_transport.AtTitle() || !m_pOverlay) return;
    const auto* pPlayerComponent = m_world.try_get<PlayerComponent>(aEntity);
    if (!pPlayerComponent)
        return;

    const auto* formIdComponent = m_world.try_get<FormIdComponent>(aEntity);
    if (!formIdComponent) return;

    Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent->Id));
    if (!pActor)
    {
        spdlog::error("{}: cannot find actor for form id {:X}", __FUNCTION__, formIdComponent->Id);
        return;
    }

    float percentage = CalculateHealthPercentage(pActor);

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, pPlayerComponent->Id);
    pArguments->SetInt(1, static_cast<int>(percentage));

    m_pOverlay->ExecuteAsync("setPlayer3dLoaded", pArguments);
}

void OverlayService::OnPlayerComponentRemoved(entt::registry& aRegistry, entt::entity aEntity) const noexcept
{
    if (m_transport.AtTitle() || !m_pOverlay) return;
    const auto& playerComponent = m_world.get<PlayerComponent>(aEntity);

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, playerComponent.Id);

    m_pOverlay->ExecuteAsync("setPlayer3dUnloaded", pArguments);
}

void OverlayService::OnChatMessageReceived(const NotifyChatMessageBroadcast& acMessage) noexcept
{
    if (!m_pOverlay)
        return;

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, (int)acMessage.MessageType);
    pArguments->SetString(1, acMessage.ChatMessage.c_str());
    pArguments->SetString(2, acMessage.PlayerName.c_str());

    m_pOverlay->ExecuteAsync("message", pArguments);
}

void OverlayService::OnPlayerDialogue(const NotifyPlayerDialogue& acMessage) noexcept
{
    const auto& party = m_world.GetPartyService();
    const auto& members = party.GetPartyMembers();
    if (!party.IsInParty() || std::find(members.begin(), members.end(), acMessage.SpeakerId) == members.end())
        return;
    if (!acMessage.Anchor.Cell)
        m_conversations.erase(acMessage.SpeakerId);
    else
    {
        uint32_t localForm = acMessage.Anchor.TargetForm ? m_world.GetModSystem().GetGameId(acMessage.Anchor.TargetForm) : 0;
        if (!localForm && acMessage.Anchor.TargetEntity != UINT32_MAX)
            if (auto entity = Utils::FindEntityByServerId(acMessage.Anchor.TargetEntity))
                if (auto* form = m_world.try_get<FormIdComponent>(*entity))
                    localForm = form->Id;
        m_conversations[acMessage.SpeakerId] = {acMessage, localForm, std::chrono::steady_clock::now() + 4s};
    }
}

void OverlayService::ClearDialogueHud() noexcept
{
    PublishInvitation(0, 0, 0, 0);
    if (!m_requestedTalk || !m_conversations.count(static_cast<uint32_t>(m_requestedTalk >> 32)) ||
        std::chrono::steady_clock::now() >= m_rejoinUntil)
        m_requestedTalk = 0;
    EndObserver();
    { std::lock_guard guard(m_markerMutex); m_marker.Active = false; }
    HideDialogueText();
}

void OverlayService::HideDialogueText() noexcept
{
    if (m_dialogueHudVisible && m_pOverlay)
        m_pOverlay->ExecuteAsync("hidePartyConversation");
    m_dialogueHudVisible = false;
}

void OverlayService::UpdateDialogueHud() noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (m_conversations.empty())
    {
        ClearDialogueHud();
        return;
    }
    auto* pPlayer = PlayerCharacter::Get();
    const auto* pMenu = MenuTopicManager::Get();
    const auto& party = m_world.GetPartyService();
    const auto* ui = UI::Get();
    if (!ui || ui->BlocksPartyHud() || !pPlayer || !pPlayer->parentCell || !party.IsInParty() || !m_pOverlay || m_active ||
        !m_inGame || (pMenu && pMenu->IsDialogueOpen()))
    {
        ClearDialogueHud();
        return;
    }

    const auto& members = party.GetPartyMembers();
    const auto* pCell = pPlayer->GetParentCellEx();
    const auto* pWorld = pPlayer->GetWorldSpace();
    const NotifyPlayerDialogue* pSelected = nullptr;
    bool sameSpace = false;
    float nearest = std::numeric_limits<float>::max();
    Vector<uint32_t> expired;
    for (const auto& [id, conversation] : m_conversations)
        if (conversation.Expires <= now || std::find(members.begin(), members.end(), id) == members.end())
            expired.push_back(id);
    for (auto id : expired)
        m_conversations.erase(id);

    for (const auto& [id, conversation] : m_conversations)
    {
        const auto& message = conversation.Last;
        auto& mods = m_world.GetModSystem();
        const auto& anchor = message.Anchor;
        const bool shared = anchor.WorldSpace ? pWorld && mods.GetGameId(anchor.WorldSpace) == pWorld->formID
                                              : !pWorld && pCell && mods.GetGameId(anchor.Cell) == pCell->formID;
        const float distance = shared ? glm::distance(static_cast<const glm::vec3&>(pPlayer->position), static_cast<const glm::vec3&>(anchor.Position))
                                      : std::numeric_limits<float>::max();
        const auto requestedSpeaker = static_cast<uint32_t>(m_requestedTalk >> 32);
        const bool requested = m_requestedTalk && id == requestedSpeaker && message.Anchor.Session == static_cast<uint32_t>(m_requestedTalk);
        if (requested || !pSelected || distance < nearest || (distance == nearest && id == party.GetLeaderPlayerId()))
        {
            pSelected = &message;
            nearest = distance;
            sameSpace = shared;
            if (requested) break;
        }
    }
    if (!pSelected)
    {
        ClearDialogueHud();
        return;
    }

    DialogueMarker marker{};
    uint32_t speakerForm{};
    marker.Active = sameSpace;
    if (sameSpace)
    {
        marker.Anchor = static_cast<const glm::vec3&>(pSelected->Anchor.Position) + glm::vec3{0, 0, 100};
        marker.Eye = static_cast<const glm::vec3&>(pPlayer->position) + glm::vec3{0, 0, pPlayer->GetHeight()};
        const auto getHead = [pCell, pWorld](uint32_t form, glm::vec3& out) {
            auto* actor = form ? Cast<Actor>(TESForm::GetById(form)) : nullptr;
            if (!actor || !actor->GetNiNode() || actor->IsDisabled() ||
                actor->GetWorldSpace() != pWorld || (!pWorld && actor->GetParentCellEx() != pCell))
                return false;
            static BSFixedString head("NPC Head [Head]");
            if (auto* node = actor->GetNiNode()->GetByName(head))
            {
                out = static_cast<const glm::vec3&>(node->worldPosition);
                return true;
            }
            out = static_cast<const glm::vec3&>(actor->position) + glm::vec3{0, 0, actor->GetHeight()};
            return true;
        };
        marker.HasNpc = getHead(m_conversations.at(pSelected->SpeakerId).LocalNpcForm, marker.Npc);
        if (marker.HasNpc)
            marker.Anchor = marker.Npc;
        const auto players = m_world.view<PlayerComponent, FormIdComponent>();
        for (auto entity : players)
        {
            if (players.get<PlayerComponent>(entity).Id == pSelected->SpeakerId)
            {
                speakerForm = players.get<FormIdComponent>(entity).Id;
                marker.HasSpeaker = getHead(speakerForm, marker.Speaker);
                break;
            }
        }
    }
    if (marker.HasSpeaker && marker.HasNpc && nearest <= 300.f && !pPlayer->IsInCombat() && !pPlayer->IsDead())
        PublishInvitation(pSelected->SpeakerId, pSelected->Anchor.Session, m_conversations.at(pSelected->SpeakerId).LocalNpcForm, speakerForm);
    else
        PublishInvitation(0, 0, 0, 0);
    UpdateObserver(marker, pSelected->SpeakerId, pSelected->Anchor.Session, nearest);
    { std::lock_guard guard(m_markerMutex); m_marker = marker; }

    if (m_observerCamera && pSelected->NpcLine != m_sampledCaption)
    {
        m_sampledCaption = pSelected->NpcLine;
        auto* actor = Cast<Actor>(TESForm::GetById(m_conversations.at(pSelected->SpeakerId).LocalNpcForm));
        if (actor && !m_sampledCaption.empty())
            CaptionForObserver(actor->formID, actor->baseForm ? actor->baseForm->GetName() : "", m_sampledCaption.c_str());
    }
    const int mode = sameSpace ? 3 : 0;
    NiPoint3 screenPos{};
    if (m_observerCamera && marker.HasSpeaker && HUDMenuUtils::ProjectForOverlay(marker.Speaker, screenPos) &&
        screenPos.z >= 0.f && std::isfinite(screenPos.x))
    {
        if (screenPos.x < .48f) m_replyLeft = true;
        if (screenPos.x > .52f) m_replyLeft = false;
    }
    if (sameSpace && (!m_observerCamera || nearest > 400.f || ObserverInCombat(pSelected->SpeakerId) || pSelected->Text.empty()))
    {
        HideDialogueText();
        return;
    }
    // The browser handles text only. Camera motion must not generate CEF messages.
    if (m_dialogueHudVisible && m_lastDialogueMode == mode && m_lastSpeaker == pSelected->SpeakerId &&
        m_lastSession == pSelected->Anchor.Session && m_lastChoice == pSelected->Anchor.Choice &&
        m_lastReplyLeft == m_replyLeft && m_lastDialogueText == pSelected->Text)
        return;
    m_lastDialogueMode = mode;
    m_lastReplyLeft = m_replyLeft;
    m_lastSpeaker = pSelected->SpeakerId;
    m_lastSession = pSelected->Anchor.Session;
    m_lastChoice = pSelected->Anchor.Choice;
    m_lastDialogueText = pSelected->Text;
    auto args = CefListValue::Create();
    args->SetInt(0, mode);
    args->SetDouble(1, mode == 3 && m_replyLeft ? 0 : 50);
    args->SetDouble(2, 8);
    args->SetString(3, pSelected->Name.c_str());
    args->SetString(4, mode == 3 ? pSelected->Text.c_str() : "");
    args->SetDouble(5, m_lastSpeaker);
    args->SetDouble(6, m_lastSession);
    args->SetDouble(7, m_lastChoice);
    m_pOverlay->ExecuteAsync("partyConversation", args);
    m_dialogueHudVisible = true;
}

void OverlayService::OnConnectionError(const ConnectionErrorEvent& acConnectedEvent) const noexcept
{
    auto pArgs = CefListValue::Create();
    pArgs->SetString(0, acConnectedEvent.ErrorDetail.c_str());
    m_pOverlay->ExecuteAsync("triggerError", pArgs);
}

void OverlayService::OnPlayerJoined(const NotifyPlayerJoined& acMessage) noexcept
{
    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acMessage.PlayerId);
    pArguments->SetString(1, acMessage.Username.c_str());
    pArguments->SetInt(2, acMessage.Level);

    String cellName = GetCellName(acMessage.WorldSpaceId, acMessage.CellId);
    pArguments->SetString(3, cellName.c_str());

    m_pOverlay->ExecuteAsync("playerConnected", pArguments);
}

void OverlayService::OnPlayerLeft(const NotifyPlayerLeft& acMessage) noexcept
{
    m_conversations.erase(acMessage.PlayerId);
    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acMessage.PlayerId);
    pArguments->SetString(1, acMessage.Username.c_str());
    m_pOverlay->ExecuteAsync("playerDisconnected", pArguments);
}

void OverlayService::OnPlayerLevel(const NotifyPlayerLevel& acMessage) noexcept
{
    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acMessage.PlayerId);
    pArguments->SetInt(1, acMessage.NewLevel);
    m_pOverlay->ExecuteAsync("setLevel", pArguments);
}

void OverlayService::OnPlayerCellChanged(const NotifyPlayerCellChanged& acMessage) const noexcept
{
    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acMessage.PlayerId);
    String cellName = GetCellName(acMessage.WorldSpaceId, acMessage.CellId);
    pArguments->SetString(1, cellName.c_str());
    m_pOverlay->ExecuteAsync("setCell", pArguments);
}

void OverlayService::OnNotifyTeleport(const NotifyTeleport& acMessage) noexcept
{
    auto& modSystem = m_world.GetModSystem();

    const uint32_t cellId = modSystem.GetGameId(acMessage.CellId);
    TESObjectCELL* pCell = Cast<TESObjectCELL>(TESForm::GetById(cellId));
    if (!pCell)
    {
        const uint32_t worldSpaceId = modSystem.GetGameId(acMessage.WorldSpaceId);
        TESWorldSpace* pWorldSpace = Cast<TESWorldSpace>(TESForm::GetById(worldSpaceId));
        if (pWorldSpace)
        {
            GridCellCoords coordinates = GridCellCoords::CalculateGridCellCoords(acMessage.Position);
            pCell = pWorldSpace->LoadCell(coordinates.X, coordinates.Y);
        }

        if (!pCell)
        {
            spdlog::error("Failed to fetch cell to teleport to.");
            m_world.GetOverlayService().SendSystemMessage("Teleporting to player failed.");
            return;
        }
    }

    PlayerCharacter::Get()->MoveTo(pCell, acMessage.Position);
}

void OverlayService::OnNotifyPlayerHealthUpdate(const NotifyPlayerHealthUpdate& acMessage) noexcept
{
    const float percentage = acMessage.Percentage >= 0.f ? acMessage.Percentage : 0.f;

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acMessage.PlayerId);
    pArguments->SetDouble(1, static_cast<double>(percentage));
    m_pOverlay->ExecuteAsync("setHealth", pArguments);
}

void OverlayService::OnPartyJoinedEvent(const PartyJoinedEvent& acEvent) noexcept
{
    if (acEvent.IsLeader)
        m_world.GetOverlayService().GetOverlayApp()->ExecuteAsync("partyCreated");
}

void OverlayService::OnPartyLeftEvent(const PartyLeftEvent& acEvent) noexcept
{
    ClearInteraction();
    ClearCarriage();
    m_conversations.clear();
    ClearDialogueHud();
    m_world.GetOverlayService().GetOverlayApp()->ExecuteAsync("partyLeft");
}

void OverlayService::RunDebugDataUpdates() noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 1000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    auto internalStats = m_transport.GetStatistics();
    auto steamStats = m_transport.GetConnectionStatus();

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, steamStats.m_flOutPacketsPerSec);
    pArguments->SetInt(1, steamStats.m_flInPacketsPerSec);
    pArguments->SetInt(2, steamStats.m_nPing);
    pArguments->SetInt(3, 0);
    pArguments->SetInt(4, internalStats.SentBytes);
    pArguments->SetInt(5, internalStats.RecvBytes);

    m_pOverlay->ExecuteAsync("debugData", pArguments);
}

// TODO(cosideci): this whole thing is a really hacky solution to
// health sync code being somewhat broken for players.
void OverlayService::RunPlayerHealthUpdates() noexcept
{
    if (!m_transport.IsConnected() || !m_world.GetPartyService().IsInParty())
        return;

    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 500ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    static float s_previousPercentage = -1.f;

    const float newPercentage = CalculateHealthPercentage(PlayerCharacter::Get());
    if (newPercentage == s_previousPercentage)
        return;

    s_previousPercentage = newPercentage;

    RequestPlayerHealthUpdate request{};
    request.Percentage = newPercentage;

    m_transport.Send(request);
}

bool OverlayService::CaptionForObserver(uint32_t npc, const char* name, const char* text) noexcept
{
    if (!m_pOverlay || !m_observerCamera)
        return false;
    if (npc)
    {
        const auto talk = m_conversations.find(static_cast<uint32_t>(m_observedTalk >> 32));
        if (talk == m_conversations.end() || talk->second.LocalNpcForm != npc ||
            talk->second.Last.Anchor.Session != static_cast<uint32_t>(m_observedTalk))
            return false;
    }
    auto args = CefListValue::Create();
    args->SetString(0, name ? name : "");
    args->SetString(1, text ? text : "");
    m_pOverlay->ExecuteAsync("observerSubtitle", args);
    return true;
}

bool OverlayService::ObserverInCombat(uint32_t speaker) const noexcept
{
    auto* player = PlayerCharacter::Get();
    if (!player || !player->IsInCombat()) return false;
    const auto it = m_conversations.find(speaker);
    if (it == m_conversations.end() || !it->second.LocalNpcForm) return true;
    const auto form = it->second.LocalNpcForm;
    const auto peace = m_peaceSession.find(form);
    if (peace == m_peaceSession.end() || static_cast<uint32_t>(peace->second >> 32) != speaker ||
        m_rejoinKey != ((uint64_t(speaker) << 32) | it->second.Last.Anchor.Session)) return true;
    const auto* threat = player->GetCombatTarget();
    auto* npc = Cast<Actor>(TESForm::GetById(form));
    return !npc || npc->IsDead() || npc->IsInCombat() || (threat && threat != npc);
}

void OverlayService::ObserverHud(bool active) noexcept
{
    using HudMode = void(const char*);
    POINTER_SKYRIMSE(HudMode, pushMode, 51620);
    POINTER_SKYRIMSE(HudMode, popMode, 51621);
    (active ? pushMode.Get() : popMode.Get())("DialogueMode");
    if (auto* ui = UI::Get()) ui->SetObserverMenu(active);
    if (m_pOverlay)
    {
        auto args = CefListValue::Create();
        args->SetBool(0, active);
        m_pOverlay->ExecuteAsync("observerMode", args);
    }
}
