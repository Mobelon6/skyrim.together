#include <TiltedOnlinePCH.h>
#include <World.h>
#include <PlayerCharacter.h>
#include <Services/PlayerService.h>
#include <Misc/BSString.h>
#include <Games/Memory.h>
#include <Games/Misc/MenuTopicManager.h>
#include <Forms/TESNPC.h>
#include <Windows.h>
#include <Messages/RequestCoopInteraction.h>
#include <atomic>
#include <cstring>

namespace
{
// CrosshairPickData::target, AE address library and TESNPC::GetActivateText (CommonLibSSE-NG).
struct PickData { uint32_t padding, target; };
TESObjectREFR* Picked()
{
    POINTER_SKYRIMSE(PickData*, singleton, 401585);
    const auto* pick = *singleton.Get();
    return pick && pick->target ? TESObjectREFR::GetByHandle(pick->target) : nullptr;
}
using ActivateText = bool (*)(TESNPC*, TESObjectREFR*, BSString&);
ActivateText nextText{};
std::atomic_uint32_t invitedNpc{}, invitedSpeaker{};
bool JoinText(TESNPC* base, TESObjectREFR* ref, BSString& destination)
{
    const bool result = nextText(base, ref, destination);
    const auto* pick = Picked();
    if (!pick || pick->baseForm != base) return result;
    const bool join = invitedNpc.load() && (pick->formID == invitedNpc.load() || pick->formID == invitedSpeaker.load());
    if (!join && World::Get().GetOverlayService().TradePeer(pick->formID)==UINT32_MAX)
        return result;
    // Vanilla HUD accepts "verb\\nname". Preserve the name supplied by Skyrim or other UI mods.
    const char* text = destination.AsAscii();
    const char* name = text ? std::strchr(text, '\n') : nullptr;
    auto& world = World::Get();
    const bool fallen = world.ctx().at<PlayerService>().IsFallenPeer(world.GetOverlayService().TradePeer(pick->formID));
    std::string replacement = fallen ? "Вылечить" : join ? "Присоединиться" : "Обменяться";
    if (name)
        replacement += name;
    return destination.Assign(replacement.c_str()) || result;
}
void Install(TESNPC* base)
{
    if (nextText || !base)
        return;
    auto** slot = *reinterpret_cast<void***>(base) + 0x4C;
    DWORD old{};
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old))
        return;
    nextText = reinterpret_cast<ActivateText>(*slot);
    InterlockedExchangePointer(slot, reinterpret_cast<void*>(&JoinText));
    DWORD ignored{};
    VirtualProtect(slot, sizeof(*slot), old, &ignored);
}
}

bool BSString::Assign(const char* text) noexcept
{
    const auto bytes = std::strlen(text) + 1;
    if (bytes > UINT16_MAX)
        return false;
    if (!data || bytes > capacity)
    {
        auto* buffer = static_cast<char*>(Memory::Allocate(bytes));
        if (!buffer)
            return false;
        if (data)
            Memory::Free(const_cast<char*>(data));
        data = buffer;
        capacity = static_cast<uint16_t>(bytes);
    }
    std::memcpy(const_cast<char*>(data), text, bytes);
    length = static_cast<uint16_t>(bytes - 1);
    return true;
}

void OverlayService::PublishInvitation(uint32_t speaker, uint32_t session, uint32_t npcForm, uint32_t speakerForm) noexcept
{
    m_invitedTalk = session ? (uint64_t(speaker) << 32) | session : 0;
    m_invitedSpeaker = speakerForm;
    m_invitedNpc = npcForm;
    if (!nextText && PlayerCharacter::Get()) Install(Cast<TESNPC>(PlayerCharacter::Get()->baseForm));
    if (npcForm && !nextText)
        if (auto* actor = Cast<Actor>(TESForm::GetById(npcForm)))
            Install(Cast<TESNPC>(actor->baseForm));
    invitedSpeaker.store(speakerForm);
    invitedNpc.store(npcForm);
}

bool OverlayService::TryJoinConversation(uint32_t form) noexcept
{
    if (!form || !m_invitedTalk || (form != m_invitedNpc && form != m_invitedSpeaker))
        return false;
    auto* pick = Picked();
    auto* player = PlayerCharacter::Get();
    const auto* topicMenu = MenuTopicManager::Get();
    if (!pick || pick->formID != form || !player || player->IsDead() || player->IsInCombat() ||
        m_active || !m_world.GetPartyService().IsInParty() || (topicMenu && topicMenu->IsDialogueOpen()))
        return false;
    RequestCoopInteraction request;
    request.Event.Kind = Interaction::Join;
    request.Event.Speaker = static_cast<uint32_t>(m_invitedTalk >> 32);
    request.Event.Session = static_cast<uint32_t>(m_invitedTalk);
    m_pendingJoin = m_invitedTalk;
    m_transport.Send(request);
    return true;
}
