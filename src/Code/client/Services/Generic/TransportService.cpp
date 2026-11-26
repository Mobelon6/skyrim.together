
#include <Services/TransportService.h>

#include <Events/ConnectedEvent.h>
#include <Events/ConnectionErrorEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>

#include <Games/References.h>
#include <Games/TES.h>
#include <Forms/TESWorldSpace.h>
#include <Forms/TESObjectCELL.h>

#include <TimeManager.h>

#include <Forms/TESNPC.h>
#include <TiltedOnlinePCH.h>
#include <World.h>
#include <Services/OverlayService.h>
#include <OverlayApp.hpp>
#include <Games/Skyrim/Interface/UI.h>
#include <PlayerCharacter.h>

#include <Messages/AuthenticationRequest.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/NotifySettingsChange.h>
#include <Packet.hpp>
#include <steam/isteamnetworkingutils.h>

#include <ScriptExtender.h>
#include <Services/DiscordService.h>

// #include <imgui_internal.h>

static constexpr wchar_t kMO2DllName[] = L"usvfs_x64.dll";

using TiltedPhoques::Packet;

TransportService::TransportService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
{
    m_updateConnection = m_dispatcher.sink<UpdateEvent>().connect<&TransportService::HandleUpdate>(this);
    m_settingsChangeConnection = m_dispatcher.sink<NotifySettingsChange>().connect<&TransportService::HandleNotifySettingsChange>(this);
    m_connectedConnection = m_dispatcher.sink<ConnectedEvent>().connect<&TransportService::HandleConnected>(this);
    m_disconnectedConnection = m_dispatcher.sink<DisconnectedEvent>().connect<&TransportService::HandleDisconnected>(this);

    m_connected = false;

    auto handlerGenerator = [this](auto& x)
    {
        using T = typename std::remove_reference_t<decltype(x)>::Type;

        m_messageHandlers[T::Opcode] = [this](UniquePtr<ServerMessage>& apMessage)
        {
            const auto pRealMessage = TiltedPhoques::CastUnique<T>(std::move(apMessage));
            m_dispatcher.trigger(*pRealMessage);
        };

        return false;
    };

    ServerMessageFactory::Visit(handlerGenerator);

    // Override authentication response
    m_messageHandlers[AuthenticationResponse::Opcode] = [this](UniquePtr<ServerMessage>& apMessage)
    {
        const auto pRealMessage = TiltedPhoques::CastUnique<AuthenticationResponse>(std::move(apMessage));
        HandleAuthenticationResponse(*pRealMessage);
    };
}

namespace
{
bool TitleOpen()
{
    auto* ui=UI::Get();
    static const BSFixedString title("Main Menu");
    return ui && ui->GetMenuOpen(title);
}
bool ReadyToConnect()
{
    auto* ui=UI::Get();
    static const BSFixedString loading("Loading Menu");
    if (!ui || TitleOpen() || ui->GetMenuOpen(loading)) return false;
    auto* player=PlayerCharacter::Get();
    return player && player->parentCell && player->GetNiNode();
}
}

bool TransportService::ConnectWorld(const std::string& endpoint,const std::string& password) noexcept
{
    if (AtTitle() || !ReadyToConnect() || m_connectionRequested || IsConnected()) return false;
    SetServerPassword(password);
    m_connectionRequested=true;
    if (Client::Connect(endpoint)) return true;
    OnDisconnected(kLocalProblem);
    return false;
}
void TransportService::CloseWorld() noexcept
{
    m_connectionRequested=false;
    m_serverPassword.clear();
    Client::Close(); // also cancels outstanding DNS, even before IsConnected is true
}
void TransportService::SuspendForSaveLoad() noexcept
{
    m_title.store(true,std::memory_order_release);
    m_world.GetRunner().SuspendWorld();
}

bool TransportService::PrepareWorldFrame() noexcept
{
    const bool title=TitleOpen();
    if (title)
    {
        if (!m_title.exchange(true,std::memory_order_acq_rel))
        {
            m_world.GetRunner().SuspendWorld();
            spdlog::info("STM world boundary: entering main menu, invalidating queued work");
            CloseWorld();
        }
        return false;
    }
    if (AtTitle())
    {
        // Do not resume queued work during the new game's initial loading.
        if (!ReadyToConnect()) return false;
        m_world.GetRunner().ResumeWorld();
        m_title.store(false,std::memory_order_release);
        spdlog::info("STM world boundary: gameplay world ready");
    }
    return true;
}

bool TransportService::Send(const ClientMessage& acMessage, const TiltedPhoques::EPacketFlags aPacketFlags) const noexcept
{
    static thread_local ScratchAllocator s_allocator(1 << 18);

    struct ScopedReset
    {
        ~ScopedReset() { s_allocator.Reset(); }
    } allocatorGuard;

    if (!AtTitle() && IsConnected())
    {
        ScopedAllocator _{s_allocator};

        Buffer buffer(1 << 16);
        Buffer::Writer writer(&buffer);
        writer.WriteBits(0, 8); // Write first byte as packet needs it

        const auto opcode=acMessage.GetOpcode();
        if (SpatialWire::Location(opcode)&&!m_spatialEpoch.Next()) return false;
        if (SpatialWire::Stamped(opcode)) acMessage.SpatialEpoch=m_spatialEpoch.Value();
        acMessage.Serialize(writer);
        TiltedPhoques::PacketView packet(reinterpret_cast<char*>(buffer.GetWriteData()), writer.Size());

        Client::Send(&packet, aPacketFlags);

        return true;
    }

    return false;
}

void TransportService::OnConsume(const void* apData, uint32_t aSize)
{
    if (AtTitle() || !m_connectionRequested) return;
    ServerMessageFactory factory;
    TiltedPhoques::ViewBuffer buf((uint8_t*)apData, aSize);
    Buffer::Reader reader(&buf);

    auto pMessage = factory.Extract(reader);
    if (!pMessage)
    {
        spdlog::error("Couldn't parse packet from server");
        return;
    }

    if (!m_connected && pMessage->GetOpcode()!=AuthenticationResponse::Opcode) return;
    if (SpatialWire::Stamped(pMessage->GetOpcode())&&!m_spatialEpoch.Matches(pMessage->SpatialEpoch)) return;
    m_messageHandlers[pMessage->GetOpcode()](pMessage);
}

void TransportService::OnConnected()
{
    if (!m_connectionRequested || AtTitle() || !ReadyToConnect())
    {
        CloseWorld();
        return;
    }
    AuthenticationRequest request{};
    request.Version = BUILD_COMMIT;
    request.SKSEActive = IsScriptExtenderLoaded();
    request.MO2Active = GetModuleHandleW(kMO2DllName);

    request.Token = m_serverPassword;
    m_serverPassword = "";

    PlayerCharacter* pPlayer = PlayerCharacter::Get();

    // null if discord is not active
    // TODO: think about user opt out
    request.DiscordId = m_world.ctx().at<DiscordService>().GetUser().id;
    auto* pNpc = Cast<TESNPC>(pPlayer->baseForm);
    if (pNpc)
    {
        request.Username = pNpc->fullName.value.AsAscii();
    }
    else
    {
        request.Username = "Some dragon boi";
    }

    auto* const cpModManager = ModManager::Get();

    for (auto* pMod : cpModManager->mods)
    {
        if (!pMod->IsLoaded())
            continue;

        auto& entry = request.UserMods.ModList.emplace_back();
        entry.Id = pMod->GetId();
        entry.IsLite = pMod->IsLite();
        entry.Filename = pMod->filename;
    }

    auto& modSystem = m_world.GetModSystem();
    if (pPlayer->GetWorldSpace())
        modSystem.GetServerModId(pPlayer->GetWorldSpace()->formID, request.WorldSpaceId);

    modSystem.GetServerModId(pPlayer->parentCell->formID, request.CellId);

    request.Level = pPlayer->GetLevel();

    auto* pGameTime = TimeData::Get();
    request.PlayerTime.TimeScale = pGameTime->TimeScale->f;
    request.PlayerTime.Time = pGameTime->GameHour->f;
    request.PlayerTime.Year = pGameTime->GameYear->f;
    request.PlayerTime.Month = pGameTime->GameMonth->f;
    request.PlayerTime.Day = pGameTime->GameDay->f;

    Send(request);
}

void TransportService::OnDisconnected(EDisconnectReason aReason)
{
    const bool authenticated=m_connected;
    const bool requested=m_connectionRequested;
    m_connected = false;
    m_spatialEpoch.Reset();
    m_connectionRequested=false;
    m_serverPassword.clear();
    m_world.GetRunner().InvalidateWorld();

    spdlog::warn("Disconnected from server {} (authenticated={})", aReason, authenticated);

    if (authenticated)
    {
        m_dispatcher.trigger(DisconnectedEvent());
        return;
    }

    // No game session was established. Do not restore player settings, delete
    // actors, or apply party/quest cleanup for a failed connection attempt.
    m_localPlayerId=0;
    if (auto* overlay=m_world.GetOverlayService().GetOverlayApp())
    {
        overlay->ExecuteAsync("disconnect");
        if (requested && aReason!=kAborted && !AtTitle())
        {
            ConnectionErrorEvent error{};
            error.ErrorDetail="{\"error\": \"no_reason\"}";
            m_dispatcher.trigger(error);
        }
    }
}

void TransportService::OnUpdate()
{
}

double TransportService::GetMessageReceiveTime() const noexcept
{
    return static_cast<double>(GetCurrentMessageReceiveTime()) / 1000.0;
}

void TransportService::HandleUpdate(const UpdateEvent& acEvent) noexcept
{
    // Taken before polling, so that every message that arrived before this time gets handled by this update
    m_lastPollTime = static_cast<double>(SteamNetworkingUtils()->GetLocalTimestamp()) / 1000.0;

    Update();
}

void TransportService::HandleConnected(const ConnectedEvent& acEvent) noexcept
{
    m_localPlayerId = acEvent.PlayerId;
}

void TransportService::HandleDisconnected(const DisconnectedEvent& acEvent) noexcept
{
    m_localPlayerId = NULL;
}

void TransportService::HandleAuthenticationResponse(const AuthenticationResponse& acMessage) noexcept
{
    using AR = AuthenticationResponse::ResponseType;
    if (acMessage.Type == AR::kAccepted)
    {
        if (m_connected) return; // ignore duplicate authentication replies
        if (!m_connectionRequested || AtTitle() || !ReadyToConnect())
        { CloseWorld(); return; }
        m_connected = true;

        m_world.SetServerSettings(acMessage.Settings);

        m_dispatcher.trigger(acMessage.UserMods);
        m_dispatcher.trigger(acMessage.Settings);
        m_dispatcher.trigger(ConnectedEvent(acMessage.PlayerId));
        return; // quit the function here.
    }

    // error finding

    TiltedPhoques::String ErrorInfo;

    ErrorInfo = "{";

    switch (acMessage.Type)
    {
    case AR::kWrongVersion:
        ErrorInfo += "\"error\": \"wrong_version\", \"data\": {";
        ErrorInfo += fmt::format("\"expectedVersion\": \"{}\", \"version\": \"{}\"", acMessage.Version, BUILD_COMMIT);
        ErrorInfo += "}";
        break;
    case AR::kModsMismatch:
    {
        ErrorInfo += "\"error\": \"mods_mismatch\", \"data\": {\"mods\": [";
        bool first = true;
        for (const auto& m : acMessage.UserMods.ModList)
        {
            if (!first)
                ErrorInfo += ",";
            ErrorInfo += fmt::format("[\"{}\",\"{}\"]", m.Filename.c_str(), m.Id);
            first = false;
        }
        ErrorInfo += "]}";
        break;
    }
    case AR::kClientModsDisallowed:
    {
        ErrorInfo += "\"error\": \"client_mods_disallowed\", \"data\": { \"mods\": [";
        if (acMessage.SKSEActive)
            ErrorInfo += "\"SKSE\"";
        if (acMessage.MO2Active)
            if (acMessage.SKSEActive)
                ErrorInfo += ",";
        ErrorInfo += "\"MO2\"";
        ErrorInfo += "]}";
        break;
    }
    case AR::kWrongPassword:
    {
        ErrorInfo += "\"error\": \"wrong_password\"";
        break;
    }
    case AR::kServerFull:
    {
        ErrorInfo += "\"error\": \"server_full\"";
        break;
    }
    default: ErrorInfo += "\"error\": \"no_reason\""; break;
    }

    ErrorInfo += "}";

    ConnectionErrorEvent errorEvent;
    if (!ErrorInfo.empty())
    {
        spdlog::error(ErrorInfo.c_str());
        errorEvent.ErrorDetail = std::move(ErrorInfo);
    }

    m_dispatcher.trigger(errorEvent);
    CloseWorld();
}

void TransportService::HandleNotifySettingsChange(const NotifySettingsChange& acMessage) noexcept
{
    m_world.SetServerSettings(acMessage.Settings);
    m_dispatcher.trigger(acMessage.Settings);
}
