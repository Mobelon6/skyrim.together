
#include <BSGraphics/BSGraphicsRenderer.h>

#include <AI/Movement/PlayerControls.h>
#include <Services/OverlayService.h>
#include <atomic>

struct InputDevice
{
    virtual ~InputDevice() = default;
    virtual void Initialize() = 0;
    virtual void Process(float) = 0;
    virtual void Release() = 0;
    virtual bool GetKeyMapping(uint32_t, BSFixedString&) = 0;
    virtual uint32_t GetMappingKey(BSFixedString) = 0;
    virtual bool GetMappedKeycode(uint32_t, uint32_t&) = 0;
    virtual bool IsEnabled() const = 0;
    virtual void Reset() = 0;
};
struct BSInputDeviceManager { char prefix[0x60]; InputDevice* devices[4]; };
static std::atomic_bool resetPending{};
void PlayerControls::RequestInputReset() noexcept { resetPending.store(true); }

void (*BSInputDeviceManager_PollInputDevices)(BSInputDeviceManager*, float) = nullptr;

void Hook_BSInputDeviceManager_PollInputDevices(BSInputDeviceManager* inputDeviceMgr, float afDelta)
{
    if (!BSGraphics::GetMainWindow()->IsForeground())
        return;

    static bool swallowExit{};
    const bool exitDown = ((GetAsyncKeyState(VK_TAB) | GetAsyncKeyState(VK_ESCAPE)) & 0x8000) != 0;
    if (exitDown && (swallowExit || OverlayService::ObservingDialogue()))
    {
        if (!swallowExit) OverlayService::RequestObserverExit();
        swallowExit = true;
        return;
    }
    if (resetPending.exchange(false) || swallowExit)
    {
        swallowExit = false;
        for (auto* device : inputDeviceMgr->devices)
            if (device) device->Reset();
        if (auto* controls = PlayerControls::GetInstance())
        {
            controls->Data.MoveInputVec = {};
            controls->Data.PrevMoveVec = {};
            controls->Data.bAutoMove = false;
        }
    }
    BSInputDeviceManager_PollInputDevices(inputDeviceMgr, afDelta);
}

static TiltedPhoques::Initializer s_initInputDeviceManager(
    []()
    {
        const VersionDbPtr<void> pollInputDevices(68617);

        BSInputDeviceManager_PollInputDevices = static_cast<decltype(BSInputDeviceManager_PollInputDevices)>(pollInputDevices.GetPtr());

        TP_HOOK_IMMEDIATE(&BSInputDeviceManager_PollInputDevices, &Hook_BSInputDeviceManager_PollInputDevices);
    });
