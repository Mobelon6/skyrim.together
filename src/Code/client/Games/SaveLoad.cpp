#include <TiltedOnlinePCH.h>

#include <SaveLoad.h>
#include <Games/Overrides.h>

#include <Forms/TESForm.h>

#include <World.h>
#include <VersionDb.h>
#include <Services/CampaignCheckpointClient.h>
#include <Services/HomeOriginalOverlay.h>
#include <Services/HomeInstance.h>
#include <Services/HomeGuestSave.h>
#include <Services/CityHomeClient.h>
#include <Services/CampaignAccess.h>
#include <Utils.h>
#include <atomic>
#include <chrono>
#include <Games/CampaignCheckpointFile.h>
#include <cstdlib>

#include <TiltedCore/Serialization.hpp>
#include <TiltedCore/Buffer.hpp>

using TiltedPhoques::Serialization;
using TiltedPhoques::ViewBuffer;

BGSSaveFormBuffer::BGSSaveFormBuffer()
{
    TP_THIS_FUNCTION(CtorT, BGSSaveFormBuffer*, BGSSaveFormBuffer);

    POINTER_SKYRIMSE(CtorT, ctor, 36035);

    TiltedPhoques::ThisCall(ctor, this);

    position = 0;
}

void BGSSaveFormBuffer::WriteId(uint32_t aId) noexcept
{
    uint32_t modId = 0;
    uint32_t baseId = 0;

    World::Get().GetModSystem().GetServerModId(aId, modId, baseId);

    auto pWriteLocation = reinterpret_cast<uint8_t*>(buffer + position);

    ViewBuffer view(pWriteLocation, capacity - position);
    Buffer::Writer writer(&view);

    Serialization::WriteVarInt(writer, modId);
    Serialization::WriteVarInt(writer, baseId);

    position += writer.Size() & 0xFFFFFFFF;
}

BGSLoadFormBuffer::BGSLoadFormBuffer(const uint32_t aChangeFlags)
{
    TP_THIS_FUNCTION(CtorT, BGSLoadFormBuffer*, BGSLoadFormBuffer);

    POINTER_SKYRIMSE(CtorT, ctor, 35993);

    TiltedPhoques::ThisCall(ctor, this);

    changeFlags = aChangeFlags;
    loadFlag = 0x40;
    position = 0;
    maybeMoreFlags = 0;

    unk1C = -1;
}

TP_THIS_FUNCTION(TBGSLoadFormBuffer_ReadFormId, bool, BGSLoadFormBuffer, uint32_t&);
TP_THIS_FUNCTION(TBGSSaveFormBuffer_WriteFormId, void, BGSSaveFormBuffer, TESForm*);
TP_THIS_FUNCTION(TBGSSaveFormBuffer_WriteId, void, BGSSaveFormBuffer, uint64_t);

static TBGSSaveFormBuffer_WriteFormId* RealBGSSaveFormBuffer_WriteFormId = nullptr;
static TBGSLoadFormBuffer_ReadFormId* RealBGSLoadFormBuffer_ReadFormId = nullptr;
static TBGSSaveFormBuffer_WriteId* RealBGSSaveFormBuffer_WriteId = nullptr;

void TP_MAKE_THISCALL(BGSSaveFormBuffer_WriteFormId, BGSSaveFormBuffer, TESForm* apForm)
{
    if (!ScopedSaveLoadOverride::IsOverriden())
    {
        TiltedPhoques::ThisCall(RealBGSSaveFormBuffer_WriteFormId, apThis, apForm);
        return;
    }

    apThis->WriteId(apForm ? apForm->formID : 0);
}

void TP_MAKE_THISCALL(BGSSaveFormBuffer_WriteId, BGSSaveFormBuffer, uint64_t aId)
{
    if (!ScopedSaveLoadOverride::IsOverriden())
    {
        TiltedPhoques::ThisCall(RealBGSSaveFormBuffer_WriteId, apThis, aId);
        return;
    }

    apThis->WriteId(aId & 0xFFFFFFFF);
}

bool TP_MAKE_THISCALL(BGSLoadFormBuffer_LoadFormId, BGSLoadFormBuffer, uint32_t& aFormId)
{
    if (!ScopedSaveLoadOverride::IsOverriden())
    {
        return TiltedPhoques::ThisCall(RealBGSLoadFormBuffer_ReadFormId, apThis, aFormId);
    }

    uint8_t* pReadLocation = (uint8_t*)(apThis->buffer + apThis->position);

    ViewBuffer buffer(pReadLocation, apThis->capacity - apThis->position);
    ViewBuffer::Reader reader(&buffer);

    const uint32_t modId = Serialization::ReadVarInt(reader) & 0xFFFFFFFF;
    const uint32_t baseId = Serialization::ReadVarInt(reader) & 0xFFFFFFFF;

    aFormId = 0;

    if (modId != 0 || baseId != 0)
    {
        aFormId = World::Get().GetModSystem().GetGameId(modId, baseId);
        if (aFormId == 0)
            spdlog::warn("Failed to resolve form {:X}:{:X} while loading form {:X}", modId, baseId, apThis->formId);
    }

    apThis->position += reader.Size() & 0xFFFFFFFF;

    return true;
}

static TiltedPhoques::Initializer s_saveLoadHooks(
    []()
    {
        POINTER_SKYRIMSE(TBGSLoadFormBuffer_ReadFormId, s_readFormId, 36000);
        POINTER_SKYRIMSE(TBGSSaveFormBuffer_WriteFormId, s_writeFormId, 36048);
        POINTER_SKYRIMSE(TBGSSaveFormBuffer_WriteId, s_writeId, 36047);

        RealBGSLoadFormBuffer_ReadFormId = s_readFormId.Get();
        RealBGSSaveFormBuffer_WriteFormId = s_writeFormId.Get();
        RealBGSSaveFormBuffer_WriteId = s_writeId.Get();

        TP_HOOK(&RealBGSLoadFormBuffer_ReadFormId, BGSLoadFormBuffer_LoadFormId);
        TP_HOOK(&RealBGSSaveFormBuffer_WriteFormId, BGSSaveFormBuffer_WriteFormId);
        TP_HOOK(&RealBGSSaveFormBuffer_WriteId, BGSSaveFormBuffer_WriteId);
    });

namespace
{
// Steam 1.6.1170: SKSE v2.2.8 GameData.h gives Load_Internal at 0x610480
// and LoadGame_HookTarget at 0x605F90. CommonLibSSE-NG maps the former
// to Address Library AE ID 35728. SKSE hooks the CALL to the latter,
// not its entry; retain SKSE's own pre/post notifications.
using LoadRequest=bool(BGSSaveLoadManager*,const char*,int32_t,uint32_t,uint32_t);
using LoadWorld=bool(BGSSaveLoadManager*,uint64_t*,uint32_t,uint32_t,void*);
LoadRequest* realLoadRequest{};
LoadWorld* realLoadWorld{};

using SaveRequest=bool(BGSSaveLoadManager*,int32_t,uint32_t,const char*);
using SaveWorld=void(BGSSaveLoadManager*,uint64_t*);
SaveRequest* realSaveRequest{};
SaveWorld* realSaveWorld{};
struct CheckpointWrite
{
    std::filesystem::path Path;
    std::string Before;
    bool Entered{},Returned{},Invalid{};
};
thread_local CheckpointWrite* currentWrite{};
struct PendingGuestSave
{
    HomeGuestSave::Context Visit;
    int32_t Device{};
    uint32_t Stats{};
    std::optional<std::string> Name;
    std::chrono::steady_clock::time_point Until;
};
std::optional<PendingGuestSave> pendingGuestSave;
std::atomic_bool guestSaveBusy{},guestHooksReady{};
BGSSaveLoadManager* SaveManager()
{
    // CommonLibSSE-NG BGSSaveLoadManager::Singleton, AE 403340.
    auto* address=VersionDb::Get().FindAddressById(403340);
    return address?*reinterpret_cast<BGSSaveLoadManager**>(address):nullptr;
}
void HookSaveWorld(BGSSaveLoadManager* manager,uint64_t* data)
{
    auto* write=currentWrite;
    if (write)
    {
        if (write->Entered) write->Invalid=true;
        write->Entered=true;write->Path=CampaignCheckpointFile::Path(data);
        std::error_code ec;
        const bool exists=!write->Path.empty()&&std::filesystem::exists(write->Path,ec);
        if (write->Path.empty()||ec) write->Invalid=true;
        if (exists)
        {
            auto before=CampaignCheckpointFile::Digest(write->Path);
            if (!before) write->Invalid=true;
            else write->Before=std::move(*before);
        }
    }
    realSaveWorld(manager,data);
    if (write) write->Returned=true;
}
bool WriteWithCheckpoint(BGSSaveLoadManager* manager,int32_t device,uint32_t stats,const char* name,
                         CampaignCheckpoint::Record& record,bool& queued)
{
    CheckpointWrite write;currentWrite=&write;
    struct Reset {~Reset(){currentWrite=nullptr;}} reset;
    const bool success=realSaveRequest(manager,device,stats,name);
    queued=false;
    if (success&&write.Entered&&write.Returned&&!write.Invalid)
    {
        auto digest=CampaignCheckpointFile::Digest(write.Path);
        if (digest&&*digest!=write.Before)
        {
            record.Digest=std::move(*digest);
            queued=CampaignCheckpointClient::Queue(record);
        }
        else spdlog::warn("STM checkpoint skipped: ESS not changed or not readable after save");
    }
    else if (success) spdlog::warn("STM checkpoint skipped: native write boundary not observed");
    return success;
}
bool HookSaveRequest(BGSSaveLoadManager* manager,int32_t device,uint32_t stats,const char* name)
{
    if (currentWrite) {currentWrite->Invalid=true;return false;}
    if (HomeGuestSave::Busy()) return false;
    // Avoid persisting a half-executed native purchase / free replay stack.
    if(CityHomeClient::Busy())
    {Utils::ShowHudMessage("Сохранение: дождитесь завершения покупки дома.");return false;}
    auto* world=World::Exists()?&World::Get():nullptr;
    if (HomeOriginalOverlay::Dirty()||HomeInstance::NativeGuestActive())
    {
        // A deferred native request has NOT saved anything yet. Capture on
        // the game thread only; never wait on a loading/saving worker.
        if (!world||!HomeGuestSave::HooksReady()||manager!=SaveManager()||!world->BeginCheckpointWrite()) return false;
        struct Unlock {World& Instance;~Unlock(){Instance.EndCheckpointWrite();}} unlock{*world};
        try
        {
            PendingGuestSave request;request.Device=device;request.Stats=stats;
            if (name) request.Name=std::string(name);
            if (request.Name&&request.Name->size()>1024) return false;
            auto visit=HomeGuestSave::CaptureAndLeave(*world);
            if (!visit)
            {Utils::ShowHudMessage("Сохранение: дождитесь завершения операций в гостях.");return false;}
            request.Visit=std::move(*visit);
            request.Until=std::chrono::steady_clock::now()+std::chrono::seconds(60);
            pendingGuestSave=std::move(request);guestSaveBusy.store(true);
            Utils::ShowHudMessage("Подготовка личного сохранения. Не перемещайте персонажа.");
        }
        catch (...) {spdlog::warn("STM could not defer guest save");}
        return false;
    }
    if (!world||!world->BeginCheckpointWrite()) return realSaveRequest(manager,device,stats,name);
    struct Guard
    {
        World* Instance;
        ~Guard(){currentWrite=nullptr;if (Instance) Instance->EndCheckpointWrite();}
    } guard{world};
    auto record=CampaignCheckpointClient::Capture(*world);
    if (!record)
    {
        world->EndCheckpointWrite();guard.Instance=nullptr;
        return realSaveRequest(manager,device,stats,name);
    }
    bool queued{};
    return WriteWithCheckpoint(manager,device,stats,name,*record,queued);
}
bool LoadWorldWithCheckpoint(BGSSaveLoadManager* manager,uint64_t* data,uint32_t flags,uint32_t mode,void* context)
{
    const auto* root=_wgetenv(L"STM_CAMPAIGN_ROOT");
    const auto path=root&&*root?CampaignCheckpointFile::Path(data):std::filesystem::path{};
    const auto before=CampaignCheckpointFile::Digest(path);
    const bool ok=realLoadWorld(manager,data,flags,mode,context);
    if (ok&&before&&CampaignCheckpointFile::Digest(path)==before)
        CampaignCheckpointClient::NoteLoadedFile(*before);
    return ok;
}

template <class Fn,class... Args>
bool WithSaveLoadBoundary(bool actualWorldLoad,Fn* original,Args... args)
{
    World* world=World::Exists()? &World::Get():nullptr;
    if (world && !world->BeginSaveLoad()) return false;
    struct Scope
    {
        World* Instance;
        bool ActualWorldLoad;
        bool Success{};
        ~Scope(){ if (Instance) Instance->EndSaveLoad(Success,ActualWorldLoad); }
    } scope{world,actualWorldLoad};
    scope.Success=original(args...);
    return scope.Success;
}

bool HookLoadRequest(BGSSaveLoadManager* manager,const char* name,int32_t device,uint32_t stats,uint32_t check)
{
    return WithSaveLoadBoundary(false,realLoadRequest,manager,name,device,stats,check);
}

bool HookLoadWorld(BGSSaveLoadManager* manager,uint64_t* data,uint32_t flags,uint32_t mode,void* context)
{
    return WithSaveLoadBoundary(true,LoadWorldWithCheckpoint,manager,data,flags,mode,context);
}

bool ExecutableAddress(const void* address)
{
    MEMORY_BASIC_INFORMATION info{};
    if (!address || !VirtualQuery(address,&info,sizeof(info)) || info.State!=MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD|PAGE_NOACCESS))) return false;
    const auto protection=info.Protect & 0xff;
    return protection==PAGE_EXECUTE || protection==PAGE_EXECUTE_READ ||
        protection==PAGE_EXECUTE_READWRITE || protection==PAGE_EXECUTE_WRITECOPY;
}

TiltedPhoques::Initializer s_worldSaveLoadHooks([]()
{
    auto& db=VersionDb::Get();
    if (db.GetLoadedVersionString()!="1.6.1170.0")
    {
        spdlog::warn("STM early save load hooks unavailable on this runtime");
        return;
    }
    unsigned long long offset{};
    if (!db.FindOffsetById(35728,offset) || offset!=0x610480)
    {
        spdlog::error("STM save load hooks skipped: Load_Internal address mismatch");
        return;
    }
    auto* base=reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) return;
    auto* request=db.FindAddressById(35728);
    auto* load=base+0x605F90;
    if (request!=base+offset || !ExecutableAddress(request) || !ExecutableAddress(load))
    {
        spdlog::error("STM save load hooks skipped: invalid executable address");
        return;
    }
    realLoadRequest=reinterpret_cast<LoadRequest*>(request);
    realLoadWorld=reinterpret_cast<LoadWorld*>(load);
    TP_HOOK(&realLoadRequest,HookLoadRequest);
    TP_HOOK(&realLoadWorld,HookLoadWorld);
    spdlog::info("STM registered early save load hooks for Steam 1.6.1170");
    // SKSE 2.2.8 Save_Internal and SaveGame_HookTarget; CommonLib AE 35727.
    unsigned long long saveOffset{};
    if (!db.FindOffsetById(35727,saveOffset)||saveOffset!=0x60FD40)
    {spdlog::warn("STM native save checkpoint hooks unavailable");return;}
    auto* save=db.FindAddressById(35727);
    auto* write=base+0x605880;
    if (save!=base+saveOffset||!ExecutableAddress(save)||!ExecutableAddress(write))
    {spdlog::warn("STM native save checkpoint addresses invalid");return;}
    realSaveRequest=reinterpret_cast<SaveRequest*>(save);
    realSaveWorld=reinterpret_cast<SaveWorld*>(write);
    TP_HOOK(&realSaveRequest,HookSaveRequest);
    TP_HOOK(&realSaveWorld,HookSaveWorld);
    guestHooksReady.store(true);
    spdlog::info("STM registered native save checkpoint hooks for Steam 1.6.1170");
});
}

bool HomeGuestSave::HooksReady() noexcept {return guestHooksReady.load();}
bool HomeGuestSave::Busy() noexcept {return guestSaveBusy.load();}
void HomeGuestSave::Cancel() noexcept
{
    pendingGuestSave.reset();guestSaveBusy.store(false);
}
void HomeGuestSave::Pump(World& world) noexcept
{
    if (!Busy()||!world.BeginCheckpointWrite()) return;
    struct Unlock {World& Instance;~Unlock(){Instance.EndCheckpointWrite();}} unlock{world};
    try
    {
        if (!pendingGuestSave) {Cancel();return;}
        auto& request=*pendingGuestSave;
        if (std::chrono::steady_clock::now()>=request.Until||!world.GetTransport().IsOnline())
        {Cancel();Utils::ShowHudMessage("Отложенное сохранение отменено: переход не завершён или нет соединения.");return;}
        if (HomeInstance::NativeGuestActive()||HomeOriginalOverlay::Dirty()||!CampaignAccess::GameReady()) return;
        const auto identity=CampaignAccess::Current(world);
        if (!identity||identity->Save!=request.Visit.Checkpoint.Save||!AtReturn(request.Visit))
        {Cancel();Utils::ShowHudMessage("Сохранение отменено: личная позиция или персонаж изменились.");return;}
        if (!InventoryReady(world)) return;
        auto* manager=SaveManager();
        if (!manager) {Cancel();return;}
        auto record=request.Visit.Checkpoint;
        bool queued{};
        // Only the personal state and return location can reach the writer.
        // The saved guest pose is metadata, not a mutation of the ESS.
        const bool success=WriteWithCheckpoint(manager,request.Device,request.Stats,
            request.Name?request.Name->c_str():nullptr,record,queued);
        Cancel();
        if (success&&queued) Utils::ShowHudMessage("Личное сохранение записано; точка визита ожидает подтверждения сервера.");
        else Utils::ShowHudMessage("Точка визита не подтверждена: проверьте сохранение.");
        if (!Resume(world,record)) Utils::ShowHudMessage("Возобновление визита недоступно. Вы остались в личном мире.");
    }
    catch (...) {Cancel();spdlog::warn("STM deferred guest save failed");}
}
