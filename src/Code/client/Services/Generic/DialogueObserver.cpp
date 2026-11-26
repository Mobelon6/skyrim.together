#include <TiltedOnlinePCH.h>
#include <Services/OverlayService.h>
#include <AI/Movement/PlayerControls.h>
#include <BSGraphics/BSGraphicsRenderer.h>
#include <Camera/PlayerCamera.h>
#include <NetImmerse/NiCamera.h>
#include <PlayerCharacter.h>
#include <Utils.h>
#include <cmath>
#include <atomic>

namespace
{
// CommonLibSSE-NG RE/F/FreeCameraState.h. Only accessed while this exact camera state is active.
std::atomic_bool observerInputCaptured{}, observerExit{};
struct FreeView : TESCameraState
{
    uint8_t inputHandlerAndPadding[0x30 - sizeof(TESCameraState)];
    NiPoint3 translation;
    NiPoint2 rotation; // pitch, yaw in radians
};
static_assert(offsetof(FreeView, translation) == 0x30);
static_assert(offsetof(FreeView, rotation) == 0x3C);

float AngleDelta(float a, float b)
{
    return std::remainder(a - b, 6.28318530718f);
}
}

bool OverlayService::ObservingDialogue() noexcept { return observerInputCaptured.load(); }

void OverlayService::RequestObserverExit() noexcept { observerExit.store(true); }

void OverlayService::EndObserver() noexcept
{
    if (!m_observerCamera && !m_restoreThird)
        return;
    auto* camera = PlayerCamera::Get();
    auto* owned = m_observerCamera;
    if (observerInputCaptured.exchange(false)) ObserverHud(false);
    observerExit.store(false);
    m_sampledCaption.clear();
    CaptionForObserver(0, "", "");
    m_observerCamera = nullptr;
    m_observedTalk = 0;
    bool restore = camera && camera->currentState && (owned ? camera->currentState == owned :
        camera->currentState->id == TESCameraState::kFirstPerson);
    if (restore && owned && owned->id == TESCameraState::kFree)
        camera->ToggleFreeCameraWithoutPause();
    if (restore && m_restoreThird && camera->currentState && camera->currentState->id == TESCameraState::kFirstPerson)
        camera->ForceThirdPerson();
    m_restoreThird = false;
}

void OverlayService::UpdateObserver(const DialogueMarker& frame, uint32_t speaker, uint32_t session, float distance) noexcept
{
    const uint64_t key = (uint64_t(speaker) << 32) | session;
    auto* player = PlayerCharacter::Get();
    auto* camera = PlayerCamera::Get();
    auto* window = BSGraphics::GetMainWindow();
    const bool retry = m_requestedTalk == key && std::chrono::steady_clock::now() < m_rejoinUntil;
    if (m_requestedTalk != key || !frame.HasNpc || !frame.HasSpeaker || !player || !camera || !window || !window->IsForeground() ||
        distance > (m_observerCamera ? 400.f : 300.f) || player->IsDead() || ObserverInCombat(speaker))
    {
        if (retry && player && !player->IsDead() && distance <= 400.f && !m_observerCamera)
            return;
        m_requestedTalk = 0;
        EndObserver();
        return;
    }
    if (m_observerCamera && (m_observedTalk != key || camera->currentState != m_observerCamera))
    {
        // A quest, another camera mod, or the player may have taken over the camera.
        m_dismissedTalk = m_observedTalk;
        EndObserver();
    }
    if (observerExit.exchange(false) || (GetAsyncKeyState(VK_BACK) & 0x8000))
    {
        LeaveConversation(key);
        m_requestedTalk = 0;
        m_dismissedTalk = key;
        EndObserver();
        return;
    }
    if (m_dismissedTalk == key)
    {
        m_requestedTalk = 0;
        return;
    }

    if (!m_observerCamera)
    {
        const auto* state = camera->currentState;
        if (!state || (state->id != TESCameraState::kFirstPerson && state->id != TESCameraState::kThirdPerson)
            || !camera->cameraRoot || !BSInputEnableManager::IsMovementControlsEnabled())
            return;
        if (state->id == TESCameraState::kThirdPerson)
        {
            if (!m_restoreThird) { m_restoreThird = true; camera->ForceFirstPerson(); }
            return; // sample the first-person eye after the camera has updated
        }
        const auto* eye = camera->GetNiCamera();
        if (!eye || glm::distance(static_cast<const glm::vec3&>(eye->worldPosition), frame.Eye) > 850.f)
            return;
        m_observerEye = eye->worldPosition;
        camera->ToggleFreeCameraWithoutPause();
        if (!camera->currentState || camera->currentState->id != TESCameraState::kFree)
        {
            m_dismissedTalk = key;
            return;
        }
        m_observerCamera = camera->currentState;
        observerInputCaptured.store(true);
        m_observedTalk = key;
        const auto* view = static_cast<const FreeView*>(m_observerCamera);
        m_appliedPitch = view->rotation.x;
        m_appliedYaw = view->rotation.y;
        m_offsetPitch = m_offsetYaw = 0.f;
        ObserverHud(true);
    }

    // Keep the game's last collision-resolved eye position. Moving the free camera through a wall
    // to obtain a wider shot would reveal another room. The game routes WASD to free camera;
    // fixing its position leaves only the observer's adjustments to the viewing angle.
    const glm::vec3 target = (frame.Speaker + frame.Npc) * .5f - glm::vec3{0.f, 0.f, 10.f};
    const auto direction = target - m_observerEye;
    const float horizontal = std::hypot(direction.x, direction.y);
    if (horizontal < 1.f)
    {
        EndObserver();
        m_dismissedTalk = key;
        return;
    }
    auto* view = static_cast<FreeView*>(m_observerCamera);
    constexpr float lookScale = .30f;
    m_offsetPitch = std::clamp(m_offsetPitch + lookScale * AngleDelta(view->rotation.x, m_appliedPitch), -.35f, .35f);
    m_offsetYaw = std::clamp(m_offsetYaw + lookScale * AngleDelta(view->rotation.y, m_appliedYaw), -.55f, .55f);
    m_appliedPitch = std::clamp(-std::atan2(direction.z, horizontal) + m_offsetPitch, -1.52f, 1.52f);
    m_appliedYaw = std::atan2(direction.x, direction.y) + m_offsetYaw;
    view->translation = m_observerEye;
    view->rotation.x = m_appliedPitch;
    view->rotation.y = m_appliedYaw;
}
