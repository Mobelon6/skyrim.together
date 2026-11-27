#include <TiltedOnlinePCH.h>
#include <World.h>
#include <Services/HomeInstance.h>
#include <Services/Workbench.h>
#include <HomeSeatLeases.h>
#include <Events/ActivateEvent.h>
#include <AI/AIProcess.h>
#include <Misc/MiddleProcess.h>
#include <Services/HomeOriginalOverlay.h>
#include <Services/HomeGuestSave.h>
#include <Services/CampaignCheckpointClient.h>
#include <Services/PapyrusService.h>
#include <HomeNativeCapabilities.h>
#include <HomeLinkedAccess.h>
#include <HomeEstateConstruction.h>
#include <Forms/TESWorldSpace.h>
#include <cstdlib>
#include <Services/HomePlacementClient.h>
#include <Services/CampaignAccess.h>
#include <HomeRegistry.h>
#include <HomeSnapshot.h>
#include <HomeCatalog.h>
#include <Services/HomeProjection.h>
#include <Services/HomeInventory.h>
#include <Services/NativeTrade.h>
#include <InventoryEvidence.h>
#include <Services/OverlayClient.h>
#include <Services/OverlayService.h>
#include <Services/PapyrusService.h>
#include <OverlayApp.hpp>
#include <PlayerCharacter.h>
#include <Games/TES.h>
#include <Forms/TESFaction.h>
#include <Forms/TESNPC.h>
#include <Games/ActorExtension.h>
#include <HomeResidentPlan.h>
#include <Services/HomeResidentProjection.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/BGSEncounterZone.h>
#include <ExtraData/ExtraDataList.h>
#include <cstring>
#include <Messages/NotifyPartyHome.h>
#include <Messages/RequestPartyHome.h>
#include <Interface/UI.h>
#include <Utils.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>

namespace
{
using Clock=std::chrono::steady_clock;
using namespace std::chrono_literals;
struct Ref {uint32_t original, copy;};
struct House {uint32_t slot{},original{},copy{}; std::vector<Ref> refs;std::string visible;bool owned{};};
struct Door {uint32_t slot; glm::vec3 pos,rotation;};
struct Advert {std::string visible; Clock::time_point until;};
std::map<uint32_t,House> houses;
std::map<uint32_t,Door> doors;
std::map<std::pair<uint32_t,uint32_t>,Advert> ads;
uint32_t door{}, token{}, guestSlot{}, guestOwner{};
std::vector<uint32_t> options;
Clock::time_point refresh{}, applyAt{};
std::string applied;
bool selection{}, initialized{};
struct Route {uint32_t source{},copy{},slot{};std::atomic_uint32_t own{},visit{};};
Route routes[5];
std::atomic_uint32_t routeCount{},self{UINT32_MAX};
constexpr uint32_t invalid=UINT32_MAX;
// Atomic local cell + wire namespace; never retain game pointers in a route.
std::atomic_uint64_t activeHomeRoute{};
std::atomic_uint64_t projectedRoot{}; // copy CELL low, canonical root high
std::atomic_uint32_t originalGuestCell{},previousGuestCell{};
uint32_t RouteBase(uint32_t local,uint32_t mod)
{
    if (HomeRegistry::Pending(mod)) return local;
    const auto root=projectedRoot.load();
    if (HomeRegistry::Scoped(mod) && uint32_t(root)==local) return uint32_t(root>>32);
    return local&((local>>24)==0xFE?0xFFFu:0xFFFFFFu);
}

std::optional<HomeCatalog::Catalog> projectionCatalog;
bool LoadProjectionCatalog()
{
    if (projectionCatalog) return true;
    auto* mods=ModManager::Get();
    auto* plugin=mods?mods->GetByName("SkyrimCoopHomeInstances.esp"):nullptr;
    if (!plugin || !plugin->IsLoaded() || plugin->IsLite()) return false;
    std::ifstream input(std::filesystem::path("Data")/"SkyrimTogetherReborn"/"config"/"CoopHomeInstances.txt");
    auto catalog=HomeCatalog::Read(input);
    if (!catalog) return false;
    projectionCatalog=std::move(catalog);
    return true;
}
uint32_t CatalogForm(const TradeProperties::FormKey& key)
{
    auto* mods=ModManager::Get();
    auto* plugin=mods?mods->GetByName(key.Plugin.c_str()):nullptr;
    if (!key.Valid() || !plugin || !plugin->IsLoaded() || (plugin->IsLite()&&key.Local>0xFFF)) return 0;
    return plugin->GetFormId(key.Local);
}
const HomeCatalog::Cell* CatalogCell(uint32_t id)
{
    if (!LoadProjectionCatalog()) return nullptr;
    for (const auto& [_,cell]:*projectionCatalog)
        if (CatalogForm(cell.Root)==id) return &cell;
    return nullptr;
}
const HomeCatalog::Cell* CatalogEntrance(uint32_t id)
{
    if (!LoadProjectionCatalog()) return nullptr;
    for (const auto& [_,cell]:*projectionCatalog)
        for (const auto& entry:cell.Entrances)
            if (CatalogForm(entry.Door)==id) return &cell;
    return nullptr;
}
// Native ExtraTeleport layout (CommonLibSSE-NG ExtraTeleport.h):
// BSExtraData at 0x00, DoorTeleportData* at 0x10; linked handle at 0x00.
// Do not retain a handle or raw object pointer in the durable home key.
TESObjectREFR* NativeLinkedDoor(TESObjectREFR* source)
{
    if (!source || !source->baseForm || source->baseForm->formType!=FormType::Door ||
        source->IsTemporary() || source->IsDeleted() || HomeInstance::ProjectionForm(source->formID)) return nullptr;
    auto* extra=source->GetExtraDataList();
    const auto* teleport=extra?extra->GetByType(ExtraDataType::Teleport):nullptr;
    if (!teleport) return nullptr;
    const void* data{};
    std::memcpy(&data,reinterpret_cast<const unsigned char*>(teleport)+0x10,sizeof(data));
    if (!data) return nullptr;
    uint32_t handle{};
    std::memcpy(&handle,data,sizeof(handle));
    if (!handle) return nullptr;
    auto* target=TESObjectREFR::GetByHandle(handle);
    if (!target || target==source || !target->baseForm || target->baseForm->formType!=FormType::Door ||
        target->IsTemporary() || target->IsDeleted() || HomeInstance::ProjectionForm(target->formID)) return nullptr;
    return target;
}
TESObjectCELL* NativeDoorDestination(TESObjectREFR* source)
{
    auto* target=NativeLinkedDoor(source);
    if (!target || NativeLinkedDoor(target)!=source) return nullptr;
    auto* outside=source->GetParentCellEx();
    auto* cell=target->GetParentCellEx();
    if (!outside || !cell || cell==outside || !(cell->cellFlags&1) ||
        cell->worldspace || cell->IsTemporary() || HomeInstance::ProjectionForm(cell->formID)) return nullptr;
    return cell;
}
std::optional<TradeProperties::FormKey> NativeFormKey(uint32_t form)
{
    std::vector<InventoryEvidence::PluginSlot> slots;
    if (auto* mods=ModManager::Get())
        for (const auto* mod:mods->mods)
        {
            if (!mod || !mod->IsLoaded()) continue;
            const auto* end=std::find(mod->filename,mod->filename+sizeof(mod->filename),'\0');
            if (end!=mod->filename+sizeof(mod->filename))
                slots.push_back({mod->GetId(),mod->IsLite(),std::string(mod->filename,end)});
        }
    const auto key=InventoryEvidence::Resolve(form,slots);
    if (!key) return {};
    TradeProperties::FormKey root{key->Plugin,key->Local};
    return root.Valid()?std::optional<TradeProperties::FormKey>(std::move(root)):std::nullopt;
}
bool NativeCapability(char tag,const std::string& root,const std::string& ref,const std::string& base)
{
    return HomeNativeCapabilities::Allows(tag,root,ref,base);
}
HomeCatalog::Cell NativeResidentCatalog(uint32_t cell)
{
    HomeCatalog::Cell catalog;
    const auto key=NativeFormKey(cell);
    if (key)
    {
        catalog.Root=*key;catalog.ResidentBases=HomeNativeCapabilities::Residents(key->Encode());
        catalog.Movable=HomeNativeCapabilities::Movable();
    }
    return catalog;
}
bool NativeStanding(World& world,const Actor& actor)
{
    // Same ActorState sit/sleep field used by Interaction02's interpolation.
    if (((actor.actorState.flags1 >> 14) & 15u)!=0) return false;
    const auto* get=world.ctx().at<PapyrusService>().Get("Actor","GetSitState");
    return !get || PapyrusFunction<int32_t,Actor>(get)(&actor)==0;
}
bool NativeSeat(World& world,TESObjectREFR& object,PlayerCharacter& player,const std::string& root) noexcept
{
    try
    {
    if (object.IsTemporary()||object.IsDeleted()||object.IsDisabled()||!object.baseForm||
        static_cast<uint32_t>(object.baseForm->formType)!=40||object.GetParentCellEx()!=player.GetParentCellEx()) return false;
    const auto ref=NativeFormKey(object.formID),base=NativeFormKey(object.baseForm->formID);
    if (!ref||!base||!NativeCapability('S',root,ref->Encode(),base->Encode())) return false;
    auto* extra=object.GetExtraDataList();
    if (!extra||extra->HasQuestObjectAlias()||extra->Contains(ExtraDataType::Teleport)) return false;
    if (const auto* lock=object.GetLock();lock&&lock->IsLocked()) return false;
    auto& papyrus=world.ctx().at<PapyrusService>();
    const auto* sit=papyrus.Get("Actor","GetSitState");
    const auto* use=papyrus.Get("ObjectReference","IsFurnitureInUse");
    return sit&&use&&NativeStanding(world,player)&&!PapyrusFunction<bool,TESObjectREFR,bool>(use)(&object,false);
    }
    catch (...) {return false;}
}
std::optional<TradeProperties::FormKey> NativeEntranceRoot(uint32_t form)
{
    auto* cell=NativeDoorDestination(Cast<TESObjectREFR>(TESForm::GetById(form)));
    return cell?NativeFormKey(cell->formID):std::nullopt;
}
// Prefer the engine's original door. The legacy catalog remains a fallback
// for its existing projection route when a linked reference is unloaded.
std::optional<TradeProperties::FormKey> EntranceRoot(uint32_t form,const HomeCatalog::Cell* catalog)
{
    if (auto root=NativeEntranceRoot(form)) return root;
    if (catalog) return catalog->Root;
    return {};
}
struct CandidateVisit {uint32_t Slot{};uint64_t Owner{};bool Native{};};
struct VisitState
{
    uint64_t Campaign{},Character{};uint32_t Slot{},Nonce{},Door{},Copy{},Base{},Right{};
    std::string Root;
    Clock::time_point Until{},Next{};
    bool Granted{},Moved{},Ready{};
    bool Native{},Prepared{},Applied{},Leaving{},Closing{},ExitIssued{},Warned{},Refreshing{},SeatExitWarned{};
    uint32_t ReturnCell{},ExitDoor{},EntryDoor{},EntryCell{},EntryExit{},EntrySlot{};
    std::array<uint32_t,12> Pose{};
    Clock::time_point MoveUntil{},ShapeAt{};
    std::string Shape;
    std::optional<std::array<uint32_t,6>> ResumePose;
    uint64_t ResumeShape{};
    bool ResumeIssued{};
};
VisitState visit;
std::optional<VisitState> linkedNext;
struct SeatRequest
{
    uint64_t Campaign{},Character{},Nonce{};
    uint32_t Slot{},Cell{},Form{},Await{};
    std::string Root,Ref;
    Clock::time_point Deadline{},Next{},Grace{},Expires{};
    bool Started{},SawSit{},Warned{};
};
SeatRequest seat;
uint64_t seatNonce{};
bool SeatScope(PlayerCharacter& player,uint32_t& slot,std::string& root)
{
    auto* cell=player.GetParentCellEx();
    uint32_t mod{},base{};
    if (!cell||!cell->IsAttached()||HomeInstance::ProjectionForm(cell->formID)||
        !HomeInstance::Encode(cell->formID,mod,base)||!HomeRegistry::Scoped(mod)) return false;
    const auto key=NativeFormKey(cell->formID);
    if (!key||key->Local!=base) return false;
    slot=mod&HomeRegistry::MaxSlot;root=key->Encode();
    if (originalGuestCell.load() && (!visit.Native||!visit.Ready||visit.Leaving||
        !visit.Granted||Clock::now()>=visit.Until||slot!=visit.Slot||root!=visit.Root)) return false;
    return slot!=0;
}
bool SeatCurrent(World& world,PlayerCharacter& player)
{
    uint32_t slot{};std::string root;
    const auto id=CampaignAccess::Current(world);
    return world.GetTransport().IsOnline()&&CampaignAccess::GameReady()&&id&&
        id->Save.Campaign==seat.Campaign&&id->Save.Character==seat.Character&&
        player.GetParentCellEx()&&player.GetParentCellEx()->formID==seat.Cell&&
        SeatScope(player,slot,root)&&slot==seat.Slot&&root==seat.Root;
}
bool SendSeat(World& world,uint32_t action)
{
    const auto id=CampaignAccess::Current(world);
    if (!id||id->Save.Campaign!=seat.Campaign||id->Save.Character!=seat.Character||
        !world.GetTransport().IsOnline()) return false;
    RequestPartyHome req;auto& e=req.Event;
    e.Campaign=seat.Campaign;e.Character=seat.Character;e.House=seat.Slot;
    e.SaveRevision=id->Save.Applied.Revision;e.SaveTag=id->Save.Applied.Tag;
    e.Profile=id->Profile.c_str();e.Credential=id->Credential.Secret.c_str();
    e.Root=seat.Root.c_str();e.Visible=seat.Ref.c_str();e.AccessOp=8;
    e.AccessGuest=seat.Nonce;e.AccessRight=action;
    if (!world.GetTransport().Send(req)) return false;
    if (action) {seat.Await=action;seat.Deadline=Clock::now()+4s;}
    return true;
}
void ReleaseSeat(World& world)
{
    if (seat.Nonce) SendSeat(world,0);
    seat={};
}
bool RequestSeat(World& world,TESObjectREFR& object,PlayerCharacter& player) noexcept
{
    try
    {
        uint32_t slot{};std::string root;
        if (!SeatScope(player,slot,root)||!object.baseForm) return false;
        const auto ref=NativeFormKey(object.formID),base=NativeFormKey(object.baseForm->formID);
        if (!ref||!base||!NativeCapability('S',root,ref->Encode(),base->Encode())) return false;
        if (seat.Nonce||!CampaignAccess::GameReady()||!world.GetTransport().IsOnline()) return true;
        if (!NativeSeat(world,object,player,root)||
            glm::distance(static_cast<const glm::vec3&>(player.position),static_cast<const glm::vec3&>(object.position))>350.f)
            return true;
        const auto id=CampaignAccess::Current(world);
        if (!id) return true;
        if (!++seatNonce) ++seatNonce;
        seat.Campaign=id->Save.Campaign;seat.Character=id->Save.Character;seat.Nonce=seatNonce;
        seat.Slot=slot;seat.Cell=player.GetParentCellEx()->formID;seat.Form=object.formID;
        seat.Root=std::move(root);seat.Ref=ref->Encode();
        if (!SendSeat(world,1)) seat={};
        return true;
    }
    catch (...) {seat={};return true;}
}
void SeatReply(World& world,const PartyHome& e) noexcept
{
    try
    {
    if (!seat.Nonce||!seat.Await||e.AccessGuest!=seat.Nonce||e.AccessRevision!=seat.Await||
        e.House!=seat.Slot||e.Campaign!=seat.Campaign||e.Character!=seat.Character||
        std::string_view(e.Root.data(),e.Root.size())!=seat.Root||
        std::string_view(e.Visible.data(),e.Visible.size())!=seat.Ref||e.AccessRight>1||
        Clock::now()>=seat.Deadline) return;
    const auto action=seat.Await;seat.Await=0;
    if (!e.AccessRight)
    {
        if (!seat.Started) {seat={};Utils::ShowHudMessage("Мебель уже занята или недоступна.");}
        else {seat.Next=Clock::now()+5s;}
        return;
    }
    auto* player=PlayerCharacter::Get();
    if (!player||!SeatCurrent(world,*player)) {ReleaseSeat(world);return;}
    seat.Expires=Clock::now()+20s;seat.Next=Clock::now()+5s;
    if (action==2) return;
    auto* object=Cast<TESObjectREFR>(TESForm::GetById(seat.Form));
    if (HomeGuestSave::Busy()||(originalGuestCell.load()&&visit.Refreshing)||!object||!NativeSeat(world,*object,*player,seat.Root)||
        glm::distance(static_cast<const glm::vec3&>(player->position),static_cast<const glm::vec3&>(object->position))>350.f)
    {ReleaseSeat(world);return;}
    // The existing STR wrapper uses ScopedActivateOverride and calls the real engine function.
    seat.Started=true;seat.Grace=Clock::now()+5s;
    if (!object->Activate(player,0,nullptr,1,0)) ReleaseSeat(world);
    }
    catch (...) {seat={};}
}
void UpdateSeat(World& world) noexcept
{
    try
    {
    if (!seat.Nonce||!CampaignAccess::GameReady()) return;
    auto* player=PlayerCharacter::Get();
    if (!player||!SeatCurrent(world,*player)) {ReleaseSeat(world);return;}
    const auto now=Clock::now();
    if (!seat.Started)
    {
        if (now>=seat.Deadline||HomeGuestSave::Busy()||!NativeStanding(world,*player)) ReleaseSeat(world);
        return;
    }
    const bool sitting=!NativeStanding(world,*player);
    if (sitting) seat.SawSit=true;
    if (!sitting&&(seat.SawSit||now>=seat.Grace)) {ReleaseSeat(world);return;}
    if (seat.Await&&now>=seat.Deadline) seat.Await=0;
    if (now>=seat.Expires&&!seat.Warned)
    {seat.Warned=true;Utils::ShowHudMessage("Связь с бронью мебели потеряна. Встаньте с мебели.");}
    if (!seat.Await&&now>=seat.Next)
    {seat.Next=now+5s;SendSeat(world,2);}
    }
    catch (...) {seat={};}
}

struct WorkbenchRequest
{
    uint64_t Nonce{};
    uint32_t Form{},Cell{},Await{};
    std::string Root,Ref;
    uint8_t Unk{};
    bool DefaultProcessing{},Started{},Warned{};
    Clock::time_point Deadline{},Next{},Expires{};
    HomeSeatLeases::Usage Use;
};
WorkbenchRequest workbench;
uint64_t workbenchNonce{};

bool WorkbenchContext(World& world,PlayerCharacter& player,TESObjectREFR& object) noexcept
{
    auto* cell=player.GetParentCellEx();
    auto* target=object.GetParentCellEx();
    if (!world.GetTransport().IsOnline()||!world.GetPartyService().IsInParty()||
        world.IsLoadingSave()||!CampaignAccess::GameReady()||!cell||!cell->IsAttached()||
        !target||!target->IsAttached()||!Workbench::OrdinaryRoute(&object)||
        HomeInstance::PrivateCell(cell->formID)||HomeInstance::NativeGuestCell(cell->formID)||
        HomeInstance::ProjectionForm(cell->formID)||
        (cell!=target&&(!cell->worldspace||cell->worldspace!=target->worldspace))) return false;
    return glm::distance(static_cast<const glm::vec3&>(player.position),
        static_cast<const glm::vec3&>(object.position))<=350.f;
}
bool SendWorkbench(World& world,uint32_t action)
{
    if (!workbench.Nonce||!world.GetTransport().IsOnline()) return false;
    RequestPartyHome request;auto& e=request.Event;
    e.AccessOp=HomeSeatLeases::WorkbenchOperation;e.AccessGuest=workbench.Nonce;
    e.AccessRight=action;e.Root=workbench.Root.c_str();e.Visible=workbench.Ref.c_str();
    if (!world.GetTransport().Send(request)) return false;
    if (action) {workbench.Await=action;workbench.Deadline=Clock::now()+4s;}
    return true;
}
void ReleaseWorkbench(World& world)
{
    if (workbench.Nonce) SendWorkbench(world,0);
    workbench={};
}
bool RequestWorkbench(World& world,TESObjectREFR& object,PlayerCharacter& player,uint8_t unk,bool processing)
{
    if (!world.GetTransport().IsOnline()||!world.GetPartyService().IsInParty()||
        !Workbench::OrdinaryRoute(&object)) return false;
    if (workbench.Nonce) return true; // one pending or active native use per player
    if (!WorkbenchContext(world,player,object)||!NativeStanding(world,player)) return true;
    const auto root=NativeFormKey(player.GetParentCellEx()->formID),ref=NativeFormKey(object.formID);
    if (!root||!ref) return false; // no stable shared reference: keep native behavior
    if (!++workbenchNonce) ++workbenchNonce;
    workbench.Nonce=workbenchNonce;workbench.Form=object.formID;
    workbench.Cell=player.GetParentCellEx()->formID;workbench.Root=root->Encode();workbench.Ref=ref->Encode();
    workbench.Unk=unk;workbench.DefaultProcessing=processing;
    if (!SendWorkbench(world,1))
    {
        workbench={};Utils::ShowHudMessage("Не удалось проверить занятость станка.");
    }
    return true;
}
bool WorkbenchCurrent(World& world,PlayerCharacter* player,TESObjectREFR* object) noexcept
{
    return player&&object&&player->GetParentCellEx()&&
        player->GetParentCellEx()->formID==workbench.Cell&&WorkbenchContext(world,*player,*object);
}
bool ObservedWorkbenchUse(PlayerCharacter& player,TESObjectREFR& object)
{
    // Keep the reservation through the native crafting menu and furniture
    // entry/exit. NPC occupancy is intentionally not an admission criterion.
    static const BSFixedString crafting("Crafting Menu");
    if (auto* ui=UI::Get();ui&&ui->GetMenuOpen(crafting)) return true;
    if (!player.currentProcess||!player.currentProcess->middleProcess||
        !player.currentProcess->middleProcess->occupiedFurniture) return false;
    const auto handle=player.currentProcess->middleProcess->occupiedFurniture.handle.iBits;
    return ((player.actorState.flags1>>14)&15u)!=0&&TESObjectREFR::GetByHandle(handle)==&object;
}
void WorkbenchReply(World& world,const PartyHome& e)
{
    if (!workbench.Nonce||!workbench.Await||e.AccessGuest!=workbench.Nonce||
        e.AccessRevision!=workbench.Await||e.Owner!=world.GetTransport().GetLocalPlayerId()||
        std::string_view(e.Root.data(),e.Root.size())!=workbench.Root||
        std::string_view(e.Visible.data(),e.Visible.size())!=workbench.Ref||e.AccessRight>1||
        Clock::now()>=workbench.Deadline) return;
    const auto action=workbench.Await;workbench.Await=0;
    if (!e.AccessRight)
    {
        if (!workbench.Started)
        {
            workbench={};Utils::ShowHudMessage("Станок пока занят");
        }
        else
        {
            workbench.Next=Clock::now()+5s;
            if (!workbench.Warned)
            {workbench.Warned=true;Utils::ShowHudMessage("Связь с бронью станка потеряна. Завершите работу.");}
        }
        return;
    }
    auto* player=PlayerCharacter::Get();
    auto* object=Cast<TESObjectREFR>(TESForm::GetById(workbench.Form));
    if (!WorkbenchCurrent(world,player,object)) {ReleaseWorkbench(world);return;}
    const auto now=Clock::now();
    workbench.Expires=now+25s;workbench.Next=now+5s;
    if (action==2) return;
    if (!NativeStanding(world,*player)) {ReleaseWorkbench(world);return;}
    // Use the original activation arguments. Activate bypasses HookActivate,
    // so publish the same ordinary STR event after successful native activation.
    workbench.Started=true;workbench.Use.Grace=now+10s;
    if (!object->Activate(player,workbench.Unk,nullptr,1,workbench.DefaultProcessing))
    {ReleaseWorkbench(world);return;}
    world.GetRunner().Trigger(ActivateEvent(object,player,nullptr,1,workbench.DefaultProcessing,
        workbench.Unk,TESObjectREFR::kNone));
}
void UpdateWorkbench(World& world)
{
    if (!workbench.Nonce) return;
    auto* player=PlayerCharacter::Get();
    auto* object=Cast<TESObjectREFR>(TESForm::GetById(workbench.Form));
    if (!WorkbenchCurrent(world,player,object)) {ReleaseWorkbench(world);return;}
    const auto now=Clock::now();
    if (!workbench.Started)
    {
        if (now>=workbench.Deadline)
        {ReleaseWorkbench(world);Utils::ShowHudMessage("Не удалось проверить занятость станка.");}
        return;
    }
    if (!workbench.Use.Keep(ObservedWorkbenchUse(*player,*object),now))
    {ReleaseWorkbench(world);return;}
    if (workbench.Await&&now>=workbench.Deadline) workbench.Await=0;
    if (now>=workbench.Expires&&!workbench.Warned)
    {workbench.Warned=true;Utils::ShowHudMessage("Связь с бронью станка потеряна. Завершите работу.");}
    if (!workbench.Await&&now>=workbench.Next)
    {workbench.Next=now+5s;SendWorkbench(world,2);}
}

std::string lastResumeDigest;
Clock::time_point resumePoll{};
struct PendingLock {uint32_t Revision{};Clock::time_point Until;};
struct LockContext
{
    uint64_t Campaign{},Character{};uint32_t Slot{},Cell{};
    std::string Root;
    std::map<uint32_t,std::pair<std::string,std::array<uint32_t,3>>> Known;
    std::map<uint32_t,PendingLock> Pending;
    Clock::time_point Next{};
};
LockContext lockContext;
struct AccessChoice {uint64_t Guest{};uint32_t Right{};};
struct AccessMenu
{
    uint64_t Campaign{},Character{};
    uint32_t Slot{},Nonce{},Revision{};
    std::string Root;
    Clock::time_point Until{};
    std::vector<AccessChoice> Choices;
    std::vector<CandidateVisit> Visits;
};
AccessMenu accessMenu;
bool SendAccess(World& world,const CampaignAccess::Identity& id,uint32_t op,
                uint32_t slot,std::string_view root,uint32_t revision,uint64_t guest,uint32_t right)
{
    RequestPartyHome request;auto& e=request.Event;
    e.Campaign=id.Save.Campaign;e.Character=id.Save.Character;
    e.SaveTag=id.Save.Applied.Tag;e.SaveRevision=id.Save.Applied.Revision;
    e.Profile=id.Profile.c_str();e.Credential=id.Credential.Secret.c_str();
    e.Root=std::string(root).c_str();e.House=slot;e.AccessOp=op;
    e.AccessRevision=revision;e.AccessGuest=guest;e.AccessRight=right;
    return world.GetTransport().Send(request);
}

const HomeCatalog::Cell* CopyCell(uint32_t id)
{
    if (!LoadProjectionCatalog()) return nullptr;
    for (const auto& [_,cell]:*projectionCatalog)
        if (CatalogForm(cell.Copy)==id) return &cell;
    return nullptr;
}
void SyncLocks(TESObjectCELL& cell,const CampaignAccess::Identity& id,uint32_t slot,
               std::string_view root,const HomeCatalog::Cell* copy)
{
    auto& ctx=lockContext;
    if (ctx.Campaign!=id.Save.Campaign || ctx.Character!=id.Save.Character ||
        ctx.Slot!=slot || ctx.Cell!=cell.formID)
    {
        ctx={};ctx.Campaign=id.Save.Campaign;ctx.Character=id.Save.Character;
        ctx.Slot=slot;ctx.Cell=cell.formID;ctx.Root=root;
    }
    const auto now=Clock::now();
    if (!slot || now<ctx.Next || !cell.IsAttached()) return;
    ctx.Next=now+1s;
    const auto view=CampaignAccess::HomeLockView(ctx.Campaign,slot);
    if (!view) {ctx.Known.clear();return;}
    ctx.Known.clear();
    for (const auto& [key,state]:*view)
    {
        auto form=TradeProperties::DecodeForm(key);
        if (!form) continue;
        if (copy)
        {
            const auto it=std::find_if(copy->Refs.begin(),copy->Refs.end(),[&](const auto& r){return r.Source.Encode()==key;});
            if (it==copy->Refs.end()) continue;
            form=it->Target;
        }
        auto* object=Cast<TESObjectREFR>(TESForm::GetById(CatalogForm(*form)));
        if (!object || object->GetParentCellEx()!=&cell || object->IsDeleted()) continue;
        auto pending=ctx.Pending.find(object->formID);
        if (pending!=ctx.Pending.end())
        {
            if (pending->second.Revision==state[2] && now<pending->second.Until) continue;
            ctx.Pending.erase(pending);
        }
        // Preserve original key/owner data. No attempt to fabricate a
        // removed native lock or copy a quest key from the owner.
        auto* lock=object->GetLock();
        if (!lock) continue;
        if (lock->lockLevel!=state[1] || lock->IsLocked()!=(state[0]==2))
        {
            lock->lockLevel=static_cast<uint8_t>(state[1]);lock->SetLock(state[0]==2);
            object->LockChange(); // direct trampoline, no outgoing LockChangeEvent
        }
        if (!copy && lock->lockLevel==state[1] && lock->IsLocked()==(state[0]==2))
            ctx.Known.emplace(object->formID,std::pair{key,state});
    }
}
const HomeCatalog::Entrance* Entry(const HomeCatalog::Cell& cell,uint32_t door)
{
    for (const auto& e:cell.Entrances) if (CatalogForm(e.Door)==door) return &e;
    return nullptr;
}
bool NearDoor(PlayerCharacter& p,uint32_t id)
{
    auto* ref=Cast<TESObjectREFR>(TESForm::GetById(id));
    return ref && p.GetParentCellEx()==ref->GetParentCellEx() &&
        glm::distance(static_cast<const glm::vec3&>(p.position),static_cast<const glm::vec3&>(ref->position))<=350.f;
}
std::optional<HomeResidents::Residents> ResidentPlan(const HomeCatalog::Cell& cell,
    const HomeSnapshot::State& state,uint64_t campaign,uint32_t slot)
{
    const auto records=CampaignAccess::HomeResidentView(campaign,slot);
    if (!records) return {};
    return HomeResidentPlan::Build(cell,state,*records);
}
bool ApplyResidents(World& world,TESObjectCELL& target,const HomeCatalog::Cell& cell,
    const HomeSnapshot::State& state,uint64_t campaign,uint32_t slot)
{
    auto plan=ResidentPlan(cell,state,campaign,slot);
    if (!plan) return false;
    std::vector<HomeResidentProjection::Entry> entries;
    for (const auto& [key,record]:*plan)
    {
        const auto source=CatalogForm(record.Value.Reference),base=CatalogForm(record.Value.Base);
        if (!source || !base) return false;
        entries.push_back({source,base,record});
    }
    return HomeResidentProjection::Sync(world,target,slot,entries);
}
// Use only committed balances, never infer removal from a missing shell.
bool ApplyLoose(TESObjectCELL& target,const HomeSnapshot::State& state,const HomeCatalog::Cell* copy)
{
    for (const auto& row:state.Refs)
    {
        if (row.Inventory!=3) continue;
        auto key=row.Key;
        if (copy)
        {
            auto it=std::find_if(copy->Refs.begin(),copy->Refs.end(),
                [&](const auto& ref){return ref.Source.Encode()==key.Encode();});
            if (it==copy->Refs.end()) return false;
            key=it->Target;
        }
        auto* ref=Cast<TESObjectREFR>(TESForm::GetById(CatalogForm(key)));
        if (!ref || !ref->baseForm || ref->baseForm->formID!=CatalogForm(row.Base) ||
            ref->GetParentCellEx()!=&target || ref->IsDeleted())
        {
            // A spent owner reference may already have been removed by the engine.
            if (!copy && row.Items.empty()) continue;
            return false;
        }
        const bool hide=(row.Flags&3u)!=0 || row.Items.empty();
        if (hide && !ref->IsDisabled()) ref->Disable();
        // The durable balance, not the loaded personal save, determines
        // whether this stable stack still exists.
        if (!hide && ref->IsDisabled()) ref->Enable();
        if (ref->IsDisabled()!=hide) return false;
    }
    return true;
}
bool IsExit(const HomeCatalog::Cell& catalog,const HomeCatalog::Ref& binding)
{
    for (const auto& e:catalog.Entrances) if (e.Exit.Encode()==binding.Target.Encode()) return true;
    return false;
}
HomeObjects::State ObjectPose(TESObjectREFR& ref,uint32_t open,uint32_t guestDoor=0)
{
    return {ref.IsDisabled()?1u:0u,open,
        {std::bit_cast<uint32_t>(ref.position.x),std::bit_cast<uint32_t>(ref.position.y),
         std::bit_cast<uint32_t>(ref.position.z),std::bit_cast<uint32_t>(ref.rotation.x),
         std::bit_cast<uint32_t>(ref.rotation.y),std::bit_cast<uint32_t>(ref.rotation.z)},guestDoor};
}
enum class RestoreResult { Ready,Pending,Unavailable };
RestoreResult RestoreObject(World& world,TESObjectCELL& cell,TESObjectREFR& object,
                            uint32_t type,bool disabled,const HomeObjects::State& wanted)
{
    if (!wanted.Valid() || object.IsDeleted() || object.GetParentCellEx()!=&cell) return RestoreResult::Unavailable;
    if (object.IsDisabled()!=disabled)
    {if (disabled) object.Disable();else object.Enable();}
    if (object.IsDisabled()!=disabled) return RestoreResult::Pending;
    if (disabled) return RestoreResult::Ready;
    auto expected=wanted;expected.Disabled=0;
    const auto observed=ObjectPose(object,wanted.Open,wanted.GuestDoor);
    if (!observed.Valid()) return RestoreResult::Unavailable;
    if (observed.Significant(expected))
    {
        const auto pos=[&](size_t i){return std::bit_cast<float>(wanted.Transform[i]);};
        object.SetRotation(pos(3),pos(4),pos(5));
        object.MoveTo(&cell,glm::vec3{pos(0),pos(1),pos(2)});
        if (object.GetParentCellEx()!=&cell) return RestoreResult::Unavailable;
        if (ObjectPose(object,wanted.Open,wanted.GuestDoor).Significant(expected)) return RestoreResult::Pending;
    }
    if (type!=29 || !wanted.Open) return RestoreResult::Ready;
    auto& vm=world.ctx().at<PapyrusService>();
    const auto* get=vm.Get("ObjectReference","GetOpenState");
    const auto* set=vm.Get("ObjectReference","SetOpen");
    if (!get || !set) return RestoreResult::Unavailable;
    const auto local=static_cast<uint32_t>(PapyrusFunction<TESObjectREFR::OpenState,TESObjectREFR>(get)(&object));
    if (local==2 || local==4) return RestoreResult::Pending;
    if (local!=1 && local!=3) return RestoreResult::Unavailable;
    if (local==wanted.Open) return RestoreResult::Ready;
    const PapyrusFunction<void,TESObjectREFR,bool> setOpen{set};
    setOpen(&object,wanted.Open==1);
    return RestoreResult::Pending;
}
bool ApplyObjectChanges(World& world,TESObjectCELL& cell,const HomeCatalog::Cell& copy,
                        const HomeSnapshot::State& state,uint64_t campaign,uint32_t slot)
{
    const auto view=CampaignAccess::HomeObjectView(campaign,slot);
    if (!view) return false;
    for (const auto& row:state.Refs)
    {
        const auto found=view->find(row.Key.Encode());
        if (found==view->end() || !found->second.Revision) continue;
        const auto binding=std::find_if(copy.Refs.begin(),copy.Refs.end(),
            [&](const auto& r){return r.Source.Encode()==row.Key.Encode();});
        if (binding==copy.Refs.end()) return false;
        if (IsExit(copy,*binding)) continue;
        auto* object=Cast<TESObjectREFR>(TESForm::GetById(CatalogForm(binding->Target)));
        if (!object || !object->baseForm || object->baseForm->formID!=CatalogForm(binding->TargetBase) ||
            object->GetParentCellEx()!=&cell || object->IsDeleted() || Cast<Actor>(object)) return false;
        if (RestoreObject(world,cell,*object,row.Type,(row.Flags&1u)!=0,found->second.Value)==RestoreResult::Unavailable)
            return false;
    }
    return true;
}
bool ApplyNativeResidents(World& world,TESObjectCELL& target,uint64_t campaign,uint32_t slot,const std::string& root)
{
    const auto bytes=CampaignAccess::HomeBaseline(campaign,slot);
    const auto state=bytes?HomeSnapshot::Decode(*bytes):std::nullopt;
    const auto records=CampaignAccess::HomeResidentView(campaign,slot);
    if (!state||!records||!HomeNativeCapabilities::Covers(HomeNativeCapabilities::Residents(root),*state,*records,
        HomeNativeCapabilities::Movable().get()))
        return false;
    if (!target.refData.refArray||target.refData.capacity>131072) return false;
    for (uint32_t i=0;i<target.refData.capacity;++i)
    {
        auto* ref=target.refData.refArray[i].Get();
        auto* actor=ref?Cast<Actor>(ref):nullptr;
        if (!actor||actor->formID==0x14||actor->IsDisabled()) continue;
        if (!actor->IsTemporary()||!actor->GetExtension()||!actor->GetExtension()->IsRemote()) return false;
    }
    std::vector<HomeResidentProjection::Entry> entries;
    for (const auto& [key,record]:*records)
    {
        const auto source=CatalogForm(record.Value.Reference),base=CatalogForm(record.Value.Base);
        if (!source||!base) return false;
        entries.push_back({source,base,record});
    }
    return HomeResidentProjection::Sync(world,target,slot,entries);
}
Clock::time_point residentDisplayNext{};
// Opt-in until save suspension/resumption is connected. No generated CELL is
// needed for a compatible original interior.
bool NativeVisitsEnabled()
{
    const auto* flag=std::getenv("STM_NATIVE_HOME_VISITS");
    return HomeGuestSave::HooksReady() && flag && std::string_view(flag)=="1";
}
std::optional<std::string> NativeVisitShape(uint64_t campaign,uint32_t slot,const std::string& root)
{
    const auto bytes=CampaignAccess::HomeBaseline(campaign,slot);
    auto state=bytes?HomeSnapshot::Decode(*bytes):std::nullopt;
    const auto residents=CampaignAccess::HomeResidentView(campaign,slot);
    const auto objects=CampaignAccess::HomeObjectView(campaign,slot);
    if (!state || state->Omitted || !state->Legacy.empty() || !residents || !objects ||
        !HomeNativeCapabilities::Covers(HomeNativeCapabilities::Residents(root),*state,*residents,
            HomeNativeCapabilities::Movable().get())) return {};
    for (auto& r:state->Refs)
    {
        if (r.Inventory==1 || r.Inventory==3) r.Items.clear();
        // Stable loose stacks can disappear through the property ledger.
        // Their visibility is refreshed separately, not a structural change.
        if (r.Inventory==3) r.Flags&=~1u;
    }
    auto shape=state->Encode();
    for (const auto& [key,record]:*objects)
        shape+=" "+key+":"+std::to_string(record.Value.GuestDoor)+":"+
            std::to_string(record.Value.GuestDoor?0u:record.Value.Open);
    const auto exterior=CampaignAccess::EstateRoomShape(campaign,slot,root);
    if (!exterior) return {};
    shape+=*exterior;
    return shape;
}
bool NativeEntry(PlayerCharacter& p,VisitState& v)
{
    auto* source=Cast<TESObjectREFR>(TESForm::GetById(v.Door));
    auto* target=NativeLinkedDoor(source);
    auto* cell=NativeDoorDestination(source);
    auto* outside=p.GetParentCellEx();
    if (!source || !target || !cell || !outside || cell->formID!=v.Copy ||
        source->GetParentCellEx()!=outside || !NearDoor(p,v.Door)) return false;
    auto* extras=source->GetExtraDataList();
    const auto* teleport=extras?extras->GetByType(ExtraDataType::Teleport):nullptr;
    const void* data{};
    if (!teleport) return false;
    std::memcpy(&data,reinterpret_cast<const unsigned char*>(teleport)+0x10,sizeof(data));
    if (!data) return false;
    // DoorTeleportData: position 0x04 and rotation 0x10.
    std::memcpy(v.Pose.data(),reinterpret_cast<const unsigned char*>(data)+4,6*sizeof(uint32_t));
    v.Pose[6]=std::bit_cast<uint32_t>(p.position.x);v.Pose[7]=std::bit_cast<uint32_t>(p.position.y);
    v.Pose[8]=std::bit_cast<uint32_t>(p.position.z);v.Pose[9]=std::bit_cast<uint32_t>(p.rotation.x);
    v.Pose[10]=std::bit_cast<uint32_t>(p.rotation.y);v.Pose[11]=std::bit_cast<uint32_t>(p.rotation.z);
    for (auto x:v.Pose) if (!std::isfinite(std::bit_cast<float>(x))) return false;
    v.ReturnCell=outside->formID;v.ExitDoor=target->formID;return true;
}
// Re-entry into a secondary room is a checkpoint restore, not a new door traversal.
// The original exterior entrance still supplies the personal return point.
bool PrepareNativeEntry(PlayerCharacter& player,VisitState& v)
{
    if (!v.EntrySlot||v.EntrySlot==v.Slot)
    {
        if (!NativeEntry(player,v)) return false;
        v.EntryDoor=v.Door;v.EntryCell=v.Copy;v.EntryExit=v.ExitDoor;v.EntrySlot=v.Slot;
        return true;
    }
    if (!v.ResumePose||!v.ResumeShape) return false;
    const auto root=NativeEntranceRoot(v.Door);
    const auto target=NativeFormKey(v.Copy);
    if (!root||!target||target->Encode()!=v.Root||root->Encode()==v.Root) return false;
    const auto& rows=HomeNativeCapabilities::Loaded();
    const auto graph=rows?HomeLinkedAccess::BuildGraph(*rows):std::nullopt;
    if (!graph||!HomeLinkedAccess::Connected(*graph,root->Encode()).count(v.Root)) return false;
    uint64_t owner{};
    for (const auto& [slot,candidate]:CampaignAccess::HomeOffers(v.Campaign,v.Character,root->Encode()))
        if (slot==v.EntrySlot) owner=candidate;
    bool allowed=false;
    for (const auto& [slot,candidate]:CampaignAccess::HomeOffers(v.Campaign,v.Character,v.Root))
        if (slot==v.Slot&&candidate==owner&&owner) allowed=true;
    if (!allowed) return false;
    const auto shape=NativeVisitShape(v.Campaign,v.Slot,v.Root);
    if (!shape||HomeSnapshot::Digest(*shape)!=v.ResumeShape) return false;
    VisitState entry;entry.Door=v.Door;entry.Copy=CatalogForm(*root);
    if (!entry.Copy||!NativeEntry(player,entry)) return false;
    v.EntryDoor=v.Door;v.EntryCell=entry.Copy;v.EntryExit=entry.ExitDoor;
    v.ReturnCell=entry.ReturnCell;v.ExitDoor=0;
    std::copy_n(entry.Pose.begin()+6,6,v.Pose.begin()+6);
    // v.Pose[0..5] was captured from the last validated linked transition.
    // Exact player pose is applied only after overlay readback.
    for (auto bits:v.Pose) if (!std::isfinite(std::bit_cast<float>(bits))) return false;
    return true;
}
// A linked room remains a separate ledger/lease, but never changes house owner.
bool PlanLinkedVisit(World& world,PlayerCharacter& player,TESObjectREFR& source)
{
    if (!visit.Native||!visit.Ready||visit.Leaving||visit.Refreshing||!visit.Granted||
        Clock::now()>=visit.Until||linkedNext||seat.Nonce||!NativeStanding(world,player)||
        !HomeGuestSave::InventoryReady(world)||!NearDoor(player,source.formID)) return false;
    const auto currentShape=NativeVisitShape(visit.Campaign,visit.Slot,visit.Root);
    if (!currentShape||*currentShape!=visit.Shape)
    {
        visit.Ready=false;visit.Leaving=true;
        Utils::ShowHudMessage("Состояние жилья изменилось. Войдите повторно.");
        return false;
    }
    const auto identity=CampaignAccess::Current(world);
    if (!identity||identity->Save.Campaign!=visit.Campaign||identity->Save.Character!=visit.Character) return false;
    auto* target=NativeLinkedDoor(&source);
    auto* cell=NativeDoorDestination(&source);
    const auto key=NativeFormKey(source.formID);
    const auto dest=target?NativeFormKey(target->formID):std::nullopt;
    const auto root=cell?NativeFormKey(cell->formID):std::nullopt;
    if (!key||!dest||!root||!cell||!NativeCapability('L',visit.Root,key->Encode(),dest->Encode())||
        !NativeCapability('L',root->Encode(),dest->Encode(),key->Encode())) return false;
    auto* extra=source.GetExtraDataList();
    if (!extra||extra->HasQuestObjectAlias()||extra->Contains(static_cast<ExtraDataType>(0x36))||
        extra->Contains(static_cast<ExtraDataType>(0x37))||source.IsDisabled()||source.IsDeleted()) return false;
    if (const auto* lock=source.GetLock();lock&&lock->IsLocked()) return false;
    uint64_t owner{};
    for (const auto& [slot,candidate]:CampaignAccess::HomeOffers(visit.Campaign,visit.Character,visit.Root))
        if (slot==visit.Slot) owner=candidate;
    if (!owner) return false;
    for (const auto& [slot,candidate]:CampaignAccess::HomeOffers(visit.Campaign,visit.Character,root->Encode()))
    {
        if (candidate!=owner||slot==visit.Slot) continue;
        const auto shape=NativeVisitShape(visit.Campaign,slot,root->Encode());
        const auto bytes=CampaignAccess::HomeBaseline(visit.Campaign,slot);
        const auto snapshot=bytes?HomeSnapshot::Decode(*bytes):std::nullopt;
        if (!shape||!snapshot) continue;
        const auto row=std::find_if(snapshot->Refs.begin(),snapshot->Refs.end(),
            [&](const auto& r){return r.Key.Encode()==dest->Encode();});
        if (row==snapshot->Refs.end()||row->Type!=29||(row->Flags&3u)) continue;
        VisitState next;
        next.Native=true;next.Campaign=visit.Campaign;next.Character=visit.Character;
        next.Slot=slot;next.Copy=cell->formID;next.Base=root->Local;next.Root=root->Encode();next.Door=source.formID;
        if (!NativeEntry(player,next)) return false;
        next.EntryDoor=visit.EntryDoor;next.EntryCell=visit.EntryCell;next.EntryExit=visit.EntryExit;next.EntrySlot=visit.EntrySlot;
        next.ReturnCell=visit.ReturnCell;std::copy_n(visit.Pose.begin()+6,6,next.Pose.begin()+6);
        next.Shape=*shape;next.ExitDoor=next.Copy==next.EntryCell?next.EntryExit:0;
        linkedNext=std::move(next);
        visit.Ready=false;visit.Leaving=true;
        return true;
    }
    return false;
}
void AddVisits(const CampaignAccess::Identity& id,std::string_view root,
               const HomeCatalog::Cell* cell,uint32_t entrance,std::string& labels)
{
    accessMenu.Visits.clear();
    const auto nativeRoot=NativeVisitsEnabled()?NativeEntranceRoot(entrance):std::nullopt;
    for (const auto& [slot,owner]:CampaignAccess::HomeOffers(id.Save.Campaign,id.Save.Character,std::string(root)))
    {
        const bool native=nativeRoot && nativeRoot->Encode()==root && NativeVisitShape(id.Save.Campaign,slot,std::string(root)).has_value();
        if (!native)
        {
            const auto bytes=CampaignAccess::HomeBaseline(id.Save.Campaign,slot);
            const auto state=bytes?HomeSnapshot::Decode(*bytes):std::nullopt;
            if (!cell || cell->Root.Encode()!=root || !state || !ResidentPlan(*cell,*state,id.Save.Campaign,slot)) continue;
        }
        if (!labels.empty()) labels+='\n';
        labels+="В гости к персонажу "+std::to_string(owner);
        if (native) labels+=" [тест оригинальной ячейки]";
        accessMenu.Visits.push_back({slot,owner,native});
    }
}
void MoveAt(PlayerCharacter& p,TESObjectCELL& cell,const std::array<uint32_t,12>& pose,size_t start)
{
    const auto f=[&](size_t i){return std::bit_cast<float>(pose[start+i]);};
    p.SetRotation(f(3),f(4),f(5));p.MoveTo(&cell,glm::vec3{f(0),f(1),f(2)});
}
bool LeaveCopy(PlayerCharacter& p,const HomeCatalog::Cell& cell,uint32_t preferred)
{
    for (const auto& e:cell.Entrances)
    {
        if (preferred && CatalogForm(e.Door)!=preferred) continue;
        auto* out=Cast<TESObjectCELL>(TESForm::GetById(CatalogForm(e.Outside)));
        if (!out || HomeInstance::ProjectionForm(out->formID)) continue;
        HomeResidentProjection::Clear();residentDisplayNext={};
        visit={};
        const auto local=p.GetParentCellEx()->formID;
        const auto id=World::Get().GetTransport().GetLocalPlayerId();
        activeHomeRoute.store(id<=0xFFFFu?(uint64_t(HomeRegistry::PendingPrefix|id)<<32)|local:0);
        projectedRoot.store(0);
        MoveAt(p,*out,e.Pose,6);return true;
    }
    if (preferred) return LeaveCopy(p,cell,0);
    return false;
}

bool Init()
{
    if (initialized) return !houses.empty();
    auto* mods=ModManager::Get();
    if (!mods) return false;
    auto* p=mods->GetByName("SkyrimCoopGuestHomes.esp");
    if (!p || !p->IsLoaded()) {initialized=true;return false;}
    std::ifstream f(std::filesystem::path("Data")/"SkyrimTogetherReborn"/"config"/"CoopHomes.txt");
    if (!f) return false;
    char kind{};uint32_t original{},slot{},copy{};std::string mod;
    while (f>>kind)
    {
        if (kind=='H' && (f>>original>>slot))
        {
            auto& h=houses[slot];h.slot=slot;h.original=original;h.copy=p->GetFormId(slot);
        }
        else if (kind=='R' && (f>>std::quoted(mod)>>original>>copy>>slot))
        {
            auto* m=mods->GetByName(mod.c_str());
            houses[slot].refs.push_back({m && m->IsLoaded()?m->GetFormId(original):0,p->GetFormId(copy)});
        }
        else if (kind=='D' && (f>>std::quoted(mod)>>original>>slot))
        {
            Door d{slot};f>>d.pos.x>>d.pos.y>>d.pos.z>>d.rotation.x>>d.rotation.y>>d.rotation.z;
            if (auto* m=mods->GetByName(mod.c_str());m && m->IsLoaded()) doors[m->GetFormId(original)]=d;
        }
        else {houses.clear();break;}
    }
    initialized=true;
    if (!f.eof() || houses.empty() || houses.size()>std::size(routes)) {houses.clear();return false;}
    uint32_t i=0;
    for (const auto& [id,h]:houses)
    {
        auto& r=routes[i++];r.source=h.original;r.copy=h.copy;r.slot=id;
    }
    routeCount.store(i);return true;
}
bool Owned(World& w,TESObjectCELL* cell,PlayerCharacter* p)
{
    if (!cell || !p) return false;
    auto& vm=w.ctx().at<PapyrusService>();
    auto* a=vm.Get("Cell","GetActorOwner");
    auto* f=vm.Get("Cell","GetFactionOwner");
    if (a)
    {
        // Cell ownership may name Player [NPC_:00000007] even when the
        // runtime player's appearance/base is a different object.
        auto* owner=PapyrusFunction<TESNPC*,TESObjectCELL>(a)(cell);
        if (owner && (owner->formID==7 || owner==p->baseForm)) return true;
    }
    if (!f) return false;
    auto* faction=PapyrusFunction<TESFaction*,TESObjectCELL>(f)(cell);
    return faction && faction->formID==0xDB1; // PlayerFaction, assigned by the purchase fragment
}
struct HomeAck {uint64_t campaign{},character{};uint32_t owner{},slot{};std::string root;};
HomeAck acknowledgedHome;
struct ResidentTrack
{
    uint32_t Revision{};
    std::optional<HomeResidents::State> Pending;
    Clock::time_point Retry{};
};
struct ResidentContext
{
    uint64_t Campaign{},Character{};
    uint32_t Slot{},Cell{},SaveRevision{};
    uint64_t SaveTag{};
    Clock::time_point Next{};
    std::map<std::string,ResidentTrack> Tracks;
    std::set<std::string> RecoveryConflicts;
    std::string LastKey;
};
ResidentContext residentContext;
std::optional<HomeResidents::State> ObserveResident(const std::string& key,TESObjectCELL& cell,
    const std::vector<InventoryEvidence::PluginSlot>& slots)
{
    auto reference=TradeProperties::DecodeForm(key);
    if (!reference) return {};
    const auto local=CatalogForm(*reference);
    auto* actor=local?Cast<Actor>(TESForm::GetById(local)):nullptr;
    if (!actor || actor->formID==0x14 || actor->IsTemporary() ||
        actor->IsStillLoading() || !actor->GetExtension() || actor->GetExtension()->IsRemote() ||
        actor->GetExtension()->IsPlayer()) return {};
    const bool away=actor->GetParentCellEx()!=&cell;
    // GetParentCellEx may fall back to a save parent. Only an actual parent
    // pointer to another original cell proves a crossing, not unload.
    if (away && (!actor->parentCell || actor->parentCell==&cell ||
        HomeInstance::ProjectionForm(actor->parentCell->formID))) return {};
    auto* npc=actor->GetLeveledPick();
    if (!npc || npc->IsTemporary()) npc=Cast<TESNPC>(actor->baseForm);
    if (!npc || npc->IsTemporary()) return {};
    auto base=InventoryEvidence::Resolve(npc->formID,slots);
    if (!base) return {};
    HomeResidents::State s;
    s.Reference=*reference;s.Base={base->Plugin,base->Local};
    s.Flags=(actor->IsDisabled()?1u:0u)|(actor->IsDead()?2u:0u)|(actor->IsDeleted()?4u:0u)|(away?8u:0u);
    s.Scale=actor->scale;
    s.Transform={std::bit_cast<uint32_t>(actor->position.x),std::bit_cast<uint32_t>(actor->position.y),
        std::bit_cast<uint32_t>(actor->position.z),std::bit_cast<uint32_t>(actor->rotation.x),
        std::bit_cast<uint32_t>(actor->rotation.y),std::bit_cast<uint32_t>(actor->rotation.z)};
    if (!s.Valid()) return {};
    return s;
}

HomeResidents::Recovery RestoreResident(TESObjectCELL& cell,const HomeResidents::State& committed,
    const HomeResidents::State& observed,const std::vector<InventoryEvidence::PluginSlot>& slots)
{
    using HomeResidents::Recovery;
    const auto plan=HomeResidents::RecoveryPlan(committed,observed);
    if (plan!=Recovery::Restore) return plan;
    auto* actor=Cast<Actor>(TESForm::GetById(CatalogForm(committed.Reference)));
    if (!actor || actor->formID==0x14 || actor->IsTemporary() || actor->IsDeleted() ||
        actor->GetParentCellEx()!=&cell || actor->IsStillLoading() ||
        !actor->GetExtension() || actor->GetExtension()->IsRemote()) return Recovery::Conflict;
    const bool disabled=(committed.Flags&1u)!=0;
    if (actor->IsDisabled()!=disabled)
    {if (disabled) actor->Disable();else actor->Enable();}
    if (actor->IsDisabled()!=disabled) return Recovery::Restore;
    if (!disabled)
    {
        const auto v=[&](size_t n){return std::bit_cast<float>(committed.Transform[n]);};
        actor->SetRotation(v(3),v(4),v(5));
        actor->MoveTo(&cell,glm::vec3{v(0),v(1),v(2)});
    }
    const auto actual=ObserveResident(committed.Reference.Encode(),cell,slots);
    return actual?HomeResidents::RecoveryPlan(committed,*actual):Recovery::Restore;
}
void SyncResidents(World& world,TESObjectCELL& cell,const CampaignAccess::Identity& identity,
                   uint32_t slot,const std::string& root,const std::vector<InventoryEvidence::PluginSlot>& slots)
{
    auto& ctx=residentContext;
    if (ctx.Campaign!=identity.Save.Campaign || ctx.Character!=identity.Save.Character ||
        ctx.Slot!=slot || ctx.Cell!=cell.formID || ctx.SaveRevision!=identity.Save.Applied.Revision ||
        ctx.SaveTag!=identity.Save.Applied.Tag)
    {
        ctx={};ctx.Campaign=identity.Save.Campaign;ctx.Character=identity.Save.Character;
        ctx.Slot=slot;ctx.Cell=cell.formID;ctx.SaveRevision=identity.Save.Applied.Revision;ctx.SaveTag=identity.Save.Applied.Tag;
    }
    const auto now=Clock::now();
    if (now<ctx.Next || !cell.IsAttached()) return;
    ctx.Next=now+3s;
    const auto nativeCatalog=NativeResidentCatalog(cell.formID);
    const auto* catalog=CatalogCell(cell.formID);
    if (!catalog) catalog=&nativeCatalog;
    const auto view=CampaignAccess::HomeResidentView(ctx.Campaign,slot);
    if (!view) {ctx.Tracks.clear();return;}
    // Keep immutable static coverage, observe previously admitted occupants
    // even after they move away, and discover eligible stable arrivals.
    std::set<std::string> candidates;
    for (const auto& [key,_]:catalog->ResidentBases) candidates.insert(key);
    for (const auto& [key,_]:*view)
        if (HomeCatalog::ResidentBase(*catalog,key)) candidates.insert(key);
    if (catalog->Movable && cell.refData.refArray && cell.refData.capacity<=131072)
        for (uint32_t i=0;i<cell.refData.capacity;++i)
        {
            auto* ref=cell.refData.refArray[i].Get();
            auto* actor=ref?Cast<Actor>(ref):nullptr;
            if (!actor || actor->formID==0x14 || actor->IsTemporary() ||
                actor->GetParentCellEx()!=&cell || !actor->GetExtension() ||
                actor->GetExtension()->IsRemote() || actor->GetExtension()->IsPlayer()) continue;
            const auto key=InventoryEvidence::Resolve(actor->formID,slots);
            if (!key) continue;
            const auto encoded=TradeProperties::FormKey{key->Plugin,key->Local}.Encode();
            if (HomeCatalog::ResidentBase(*catalog,encoded)) candidates.insert(encoded);
        }
    if (candidates.empty() || candidates.size()>HomeResidents::MaxResidents) return;
    // Bound traffic; missing/unloaded references do not establish departure.
    unsigned budget=4;
    auto next=candidates.upper_bound(ctx.LastKey);
    for (size_t checked=0;checked<candidates.size() && budget;++checked)
    {
        if (next==candidates.end()) next=candidates.begin();
        const auto& key=*next++;ctx.LastKey=key;
        if (catalog==&nativeCatalog)
        {
            const auto binding=TradeProperties::DecodeForm(key);
            auto* original=binding?Cast<Actor>(TESForm::GetById(CatalogForm(*binding))):nullptr;
            auto* extra=original?original->GetExtraDataList():nullptr;
            if (!extra||extra->HasQuestObjectAlias()||extra->Contains(static_cast<ExtraDataType>(0x36))||
                extra->Contains(static_cast<ExtraDataType>(0x37))) continue;
        }
        auto state=ObserveResident(key,cell,slots);
        if (!state) continue;
        const auto* expectedBase=HomeCatalog::ResidentBase(*catalog,key);
        if (!expectedBase || state->Base.Encode()!=expectedBase->Encode())
        {ctx.Tracks.erase(key);continue;}
        const auto found=view->find(key);
        const auto* old=found==view->end()?nullptr:&found->second;
        // Do not register an unrelated off-site actor discovered only through
        // the global pool. A static binding or prior observation is required.
        if (!old && state->Away() && !catalog->ResidentBases.count(key)) continue;
        auto tracked=ctx.Tracks.find(key);
        const auto revision=old?old->Revision:0;
        // Recognize our acknowledged observation before deciding whether a
        // revision mismatch needs restoration. An ACK is not a new save.
        if (tracked!=ctx.Tracks.end() && tracked->second.Pending && old &&
            tracked->second.Revision!=UINT32_MAX && revision==tracked->second.Revision+1 &&
            old->Value.Encode()==tracked->second.Pending->Encode())
        {
            tracked->second.Revision=revision;tracked->second.Pending.reset();
            tracked->second.Retry=now+15s;
        }
        if (old && (tracked==ctx.Tracks.end() || tracked->second.Revision!=revision))
        {
            if (HomeResidents::CanObservePresence(*old,ctx.SaveRevision,*state))
            {
                ctx.RecoveryConflicts.erase(key);
                ctx.Tracks.insert_or_assign(key,ResidentTrack{revision,{},{}});
                continue;
            }
            const auto result=RestoreResident(cell,old->Value,*state,slots);
            if (result==HomeResidents::Recovery::Conflict)
            {
                if (ctx.RecoveryConflicts.insert(key).second)
                    spdlog::warn("STM: residential NPC recovery conflict (slot {}, reference {})",slot,key);
                ctx.Tracks.erase(key);continue;
            }
            if (result!=HomeResidents::Recovery::Matched)
            {ctx.Tracks.erase(key);continue;}
            ctx.RecoveryConflicts.erase(key);
            ctx.Tracks.insert_or_assign(key,ResidentTrack{revision,{},{}});
            // Never publish a value in the same tick that restores it.
            continue;
        }
        if (tracked==ctx.Tracks.end())
            tracked=ctx.Tracks.emplace(key,ResidentTrack{revision,{},{}}).first;
        auto& t=tracked->second;
        if (now<t.Retry) continue;
        t.Pending.reset();
        if (old && (ctx.SaveRevision<old->SaveRevision || old->Value.Base.Encode()!=state->Base.Encode()))
            continue;
        if (old && (state->Flags&9u)) state->Transform=old->Value.Transform;
        if (old && !state->Significant(old->Value)) continue;
        RequestPartyHome req;auto& e=req.Event;
        e.House=slot;e.Campaign=ctx.Campaign;e.Character=ctx.Character;
        e.SaveRevision=ctx.SaveRevision;e.SaveTag=ctx.SaveTag;
        e.Profile=identity.Profile.c_str();e.Credential=identity.Credential.Secret.c_str();e.Root=root.c_str();
        e.AccessOp=5;e.AccessRevision=t.Revision;e.Visible=state->Encode().c_str();
        t.Retry=now+15s;
        if (world.GetTransport().Send(req)) {t.Pending=*state;--budget;}
    }
}

struct ObjectTrack
{
    uint32_t Revision{};
    std::optional<HomeObjects::State> Pending;
    Clock::time_point Retry{};
};
struct ObjectContext
{
    uint64_t Campaign{},Character{},SaveTag{};
    uint32_t Slot{},Cell{},SaveRevision{};
    size_t NextRef{};
    Clock::time_point Next{};
    std::map<std::string,ObjectTrack> Tracks;
};
ObjectContext objectContext;
void SyncObjects(World& world,TESObjectCELL& cell,const CampaignAccess::Identity& identity,
                 uint32_t slot,const std::string& root,const HomeSnapshot::State& baseline)
{
    auto& ctx=objectContext;
    if (ctx.Campaign!=identity.Save.Campaign || ctx.Character!=identity.Save.Character ||
        ctx.Slot!=slot || ctx.Cell!=cell.formID || ctx.SaveRevision!=identity.Save.Applied.Revision ||
        ctx.SaveTag!=identity.Save.Applied.Tag)
    {
        ctx={};ctx.Campaign=identity.Save.Campaign;ctx.Character=identity.Save.Character;
        ctx.Slot=slot;ctx.Cell=cell.formID;ctx.SaveRevision=identity.Save.Applied.Revision;ctx.SaveTag=identity.Save.Applied.Tag;
    }
    const auto now=Clock::now();
    if (now<ctx.Next || !cell.IsAttached()) return;
    ctx.Next=now+1s;
    const auto* catalog=CatalogCell(cell.formID);
    const auto view=CampaignAccess::HomeObjectView(ctx.Campaign,slot);
    if (!view) {ctx.Tracks.clear();return;}
    // Original reference keys already exist in the authoritative baseline.
    // Preserve the legacy catalog's filtering when present, otherwise use
    // those keys directly. No generated forms or map are needed for owners.
    const size_t count=catalog?catalog->Refs.size():baseline.Refs.size();
    if (!count) return;
    unsigned sent=0;
    for (size_t i=0;i<std::min<size_t>(count,128) && sent<8;++i)
    {
        const auto index=ctx.NextRef++%count;
        if (catalog && IsExit(*catalog,catalog->Refs[index])) continue;
        const auto& source=catalog?catalog->Refs[index].Source:baseline.Refs[index].Key;
        const auto& base=catalog?catalog->Refs[index].Base:baseline.Refs[index].Base;
        const auto key=source.Encode();
        const auto it=view->find(key);
        if (it==view->end()) continue;
        const auto& old=it->second;
        auto* object=Cast<TESObjectREFR>(TESForm::GetById(CatalogForm(source)));
        if (!object || object->IsTemporary() || object->IsDeleted() || !object->baseForm ||
            object->GetParentCellEx()!=&cell || object->baseForm->formID!=CatalogForm(base) ||
            static_cast<uint32_t>(object->baseForm->formType)!=old.Type || Cast<Actor>(object) ||
            (old.Inventory==3 && object->IsDisabled())) {ctx.Tracks.erase(key);continue;}
        bool parentControlled=false;
        if (!catalog)
        {
            auto* extra=object->GetExtraDataList();
            // Do not treat a transition door or quest alias as ordinary
            // furniture. Unknown form types stay outside the fallback.
            if (!extra || extra->HasQuestObjectAlias() || extra->Contains(ExtraDataType::Teleport) ||
                !(old.Type==28 || old.Type==29 || old.Type==34 || old.Type==35 ||
                  old.Type==36 || old.Type==40 || old.Inventory==3))
            {ctx.Tracks.erase(key);continue;}
            // CommonLibSSE-NG ExtraDataTypes.h: kEnableStateParent = 0x36.
            // The parent's state controls visibility; never force a child
            // against its enable parent during owner recovery.
            parentControlled=extra->Contains(static_cast<ExtraDataType>(0x36));
        }
        auto tracked=ctx.Tracks.find(key);
        // Restore committed state before admitting a new owner observation.
        // A spent loose stack is still governed exclusively by ApplyLoose.
        if (old.Revision && (tracked==ctx.Tracks.end() ||
            (tracked->second.Revision!=old.Revision &&
             !(tracked->second.Pending && tracked->second.Revision!=UINT32_MAX &&
               old.Revision==tracked->second.Revision+1 &&
               tracked->second.Pending->Encode()==old.Value.Encode()))))
        {
            if (parentControlled && object->IsDisabled()!=(old.Value.Disabled!=0))
            {ctx.Tracks.erase(key);continue;}
            const auto result=RestoreObject(world,cell,*object,old.Type,old.Value.Disabled!=0,old.Value);
            if (result!=RestoreResult::Ready) {ctx.Tracks.erase(key);continue;}
            ctx.Tracks.insert_or_assign(key,ObjectTrack{old.Revision,{},{}});
            // Never publish a value in the same tick that restores it.
            continue;
        }
        uint32_t open=old.Value.Open;
        if (old.Type==29 && !object->IsDisabled())
        {
            auto& vm=world.ctx().at<PapyrusService>();
            const auto* get=vm.Get("ObjectReference","GetOpenState");
            if (!get) continue;
            const auto n=static_cast<uint32_t>(PapyrusFunction<TESObjectREFR::OpenState,TESObjectREFR>(get)(object));
            if (n!=1 && n!=3) continue;
            open=n;
        }
        const bool nativeDoor=open&&!parentControlled&&NativeCapability('D',root,key,base.Encode());
        auto state=ObjectPose(*object,open,open && (catalog?catalog->GuestDoors.count(key):nativeDoor)?1u:0u);
        if (state.Disabled) state.Transform=old.Value.Transform;
        if (!HomeObjects::Allowed(old,state)) continue;
        if (tracked==ctx.Tracks.end())
            tracked=ctx.Tracks.emplace(key,ObjectTrack{old.Revision,{},{}}).first;
        auto& t=tracked->second;
        if (t.Pending && t.Revision!=UINT32_MAX && old.Revision==t.Revision+1 &&
            old.Value.Encode()==t.Pending->Encode())
        {t.Revision=old.Revision;t.Pending.reset();}
        if (old.Revision!=t.Revision)
        {ctx.Tracks.erase(tracked);continue;}
        if (ctx.SaveRevision<old.SaveRevision)
        {
            // An older authenticated checkpoint cannot replace newer state.
            if (!parentControlled || object->IsDisabled()==(old.Value.Disabled!=0))
                RestoreObject(world,cell,*object,old.Type,old.Value.Disabled!=0,old.Value);
            continue;
        }
        if (now<t.Retry || old.Revision==UINT32_MAX || !state.Significant(old.Value)) continue;
        HomeObjects::Change c{slot,old.Revision+1,ctx.SaveRevision,key,state};
        RequestPartyHome req;auto& e=req.Event;
        e.House=slot;e.Campaign=ctx.Campaign;e.Character=ctx.Character;e.SaveRevision=ctx.SaveRevision;e.SaveTag=ctx.SaveTag;
        e.Profile=identity.Profile.c_str();e.Credential=identity.Credential.Secret.c_str();e.Root=root.c_str();
        e.AccessOp=6;e.AccessRevision=old.Revision;e.Visible=c.Encode().c_str();
        t.Retry=now+5s;
        if (world.GetTransport().Send(req)) {t.Pending=state;++sent;}
    }
}

struct BaselineUpload
{
    uint32_t Slot{},Next{};
    uint64_t Campaign{},Character{};
    std::string Bytes;
    Clock::time_point Retry{};
    bool Confirmed{};
};
BaselineUpload baselineUpload;
std::optional<std::string> CaptureStatic(TESObjectCELL& cell,const std::vector<InventoryEvidence::PluginSlot>& slots,
    const HomeResidents::Residents& records)
{
    if (!cell.IsAttached() || !cell.refData.refArray || !cell.refData.capacity ||
        cell.refData.capacity>131072) return {};
    HomeSnapshot::State state;
    const auto nativeCatalog=NativeResidentCatalog(cell.formID);
    const auto* catalog=CatalogCell(cell.formID);
    if (!catalog) catalog=&nativeCatalog;
    for (uint32_t i=0;i<cell.refData.capacity;++i)
    {
        auto* ref=cell.refData.refArray[i].Get();
        if (!ref) continue;
        if (ref->formID==0x14 || NativeTrade::IsProxy(ref) || HomePlacementClient::Tracked(ref)) continue;
        if (auto* actor=Cast<Actor>(ref);actor&&actor->IsTemporary()&&actor->GetExtension()&&
            actor->GetExtension()->IsRemote()&&actor->GetExtension()->IsPlayer()) continue;
        if (ref->IsTemporary() && !Cast<Actor>(ref) && ref->GetParentCellEx()==&cell)
        {
            if (ref->IsDeleted() || ref->IsDisabled()) continue;
            if (auto item=HomePlacementClient::CaptureLegacy(*ref))
            {
                state.Legacy.push_back(std::move(*item));
                if (state.Legacy.size()>2048) return {};
                continue;
            }
        }
        if (catalog && Cast<Actor>(ref) && !ref->IsTemporary())
        {
            const auto key=InventoryEvidence::Resolve(ref->formID,slots);
            if (key)
            {
                const auto encoded=TradeProperties::FormKey{key->Plugin,key->Local}.Encode();
                if (const auto* base=HomeCatalog::ResidentBase(*catalog,encoded))
                {
                    const auto observed=ObserveResident(encoded,cell,slots);
                    if (!observed || observed->Base.Encode()!=base->Encode()) return {};
                    if (catalog->ResidentBases.count(encoded)) state.ResidentKeys.insert(encoded);
                    else
                    {
                        // The static snapshot is not an occupant ledger. Wait
                        // for an actual journal observation of the moved-in NPC.
                        const auto recorded=records.find(encoded);
                        if (recorded==records.end() || !recorded->second.Value.SameStatus(*observed)) return {};
                    }
                    continue;
                }
            }
        }
        if (!ref->baseForm || ref->IsTemporary() || ref->baseForm->IsTemporary() ||
            Cast<Actor>(ref) ||
            ref->GetParentCellEx()!=&cell)
        {++state.Omitted;continue;}
        auto key=InventoryEvidence::Resolve(ref->formID,slots);
        auto base=InventoryEvidence::Resolve(ref->baseForm->formID,slots);
        if (!key || !base) {++state.Omitted;continue;}
        HomeSnapshot::Ref row;
        row.Key={key->Plugin,key->Local};row.Base={base->Plugin,base->Local};
        row.Type=static_cast<uint32_t>(ref->baseForm->formType);
        row.Flags=(ref->IsDisabled()?1u:0u)|(ref->IsDeleted()?2u:0u);
        row.Scale=ref->scale;
        if (auto* lock=ref->GetLock())
        {row.LockState=lock->IsLocked()?2u:1u;row.LockLevel=static_cast<uint8_t>(lock->lockLevel);}
        row.Transform={std::bit_cast<uint32_t>(ref->position.x),std::bit_cast<uint32_t>(ref->position.y),
            std::bit_cast<uint32_t>(ref->position.z),std::bit_cast<uint32_t>(ref->rotation.x),
            std::bit_cast<uint32_t>(ref->rotation.y),std::bit_cast<uint32_t>(ref->rotation.z)};
        if (ref->baseForm->formType==FormType::Container)
        {
            auto items=HomeInventory::Capture(*ref);
            row.Inventory=items?1:2;
            if (items) row.Items=std::move(*items);
        }
        else if (HomeSnapshot::LooseType(row.Type))
        {
            auto items=HomeInventory::CaptureLoose(*ref);
            if (items) {row.Inventory=3;row.Items=std::move(*items);}
            else ++state.Omitted;
        }
        state.Refs.push_back(std::move(row));
        if (state.Refs.size()>16384) return {};
    }
    if (catalog)
        for (const auto& [key,base]:catalog->ResidentBases)
        {
            if (state.ResidentKeys.count(key)) continue;
            const auto observed=ObserveResident(key,cell,slots);
            const auto recorded=records.find(key);
            // An explicit off-site observation covers an original resident
            // without placing it in the visitor's interior.
            if (!observed || !observed->Away() || observed->Base.Encode()!=base.Encode() ||
                recorded==records.end() || !recorded->second.Value.SameStatus(*observed)) return {};
            state.ResidentKeys.insert(key);
        }
    std::sort(state.Refs.begin(),state.Refs.end(),[](const auto& a,const auto& b){return a.Key.Encode()<b.Key.Encode();});
    std::sort(state.Legacy.begin(),state.Legacy.end(),[](const auto& a,const auto& b){return a.Source<b.Source;});
    if (!state.Valid()) return {};
    auto bytes=state.Encode();
    if (bytes.size()>HomeSnapshot::MaxBytes) return {};
    return bytes;
}
void UploadBaseline(World& world,TESObjectCELL& cell,const CampaignAccess::Identity& identity,
                    uint32_t slot,const std::string& root,const std::vector<InventoryEvidence::PluginSlot>& slots)
{
    auto& u=baselineUpload;
    if (u.Slot!=slot || u.Campaign!=identity.Save.Campaign || u.Character!=identity.Save.Character)
    {u={};u.Slot=slot;u.Campaign=identity.Save.Campaign;u.Character=identity.Save.Character;}
    if (u.Confirmed) return;
    const auto state=CampaignAccess::HomeBaselineState(u.Campaign,slot);
    if (state!=0) {u.Bytes.clear();u.Confirmed=true;return;}
    const auto now=Clock::now();
    if (now<u.Retry) return;
    if (!u.Next)
    {
        const auto records=CampaignAccess::HomeResidentView(u.Campaign,slot);
        auto observed=records?CaptureStatic(cell,slots,*records):std::nullopt;
        if (!observed) {u.Bytes.clear();return;}
        // Require two matching game-thread observations before publishing.
        if (u.Bytes!=*observed) {u.Bytes=std::move(*observed);return;}
    }
    const auto count=static_cast<uint32_t>((u.Bytes.size()+HomeSnapshot::PartBytes-1)/HomeSnapshot::PartBytes);
    const auto digest=HomeSnapshot::Digest(u.Bytes);
    for (unsigned i=0;i<8 && u.Next<count;++i)
    {
        RequestPartyHome req;auto& e=req.Event;
        e.House=slot;e.Campaign=identity.Save.Campaign;e.Character=identity.Save.Character;
        e.SaveRevision=identity.Save.Applied.Revision;e.SaveTag=identity.Save.Applied.Tag;
        e.Profile=identity.Profile.c_str();e.Credential=identity.Credential.Secret.c_str();e.Root=root.c_str();
        e.SnapshotDigest=digest;e.SnapshotPart=u.Next;e.SnapshotParts=count;
        e.Visible=u.Bytes.substr(u.Next*HomeSnapshot::PartBytes,HomeSnapshot::PartBytes).c_str();
        if (!world.GetTransport().Send(req)) {u.Next=0;u.Bytes.clear();u.Retry=now+5s;return;}
        ++u.Next;
    }
    if (u.Next==count) {u.Next=0;u.Bytes.clear();u.Retry=now+30s;}
}

void SetVisit(uint32_t slot,uint32_t owner)
{
    for (unsigned i=0;i<routeCount.load();++i) routes[i].visit.store(routes[i].slot==slot ? owner+1 : 0);
    guestSlot=slot;guestOwner=owner;applied.clear();applyAt={};
}
}
namespace HomeInstance
{
bool Tagged(uint32_t mod) noexcept {return HomeRegistry::Scoped(mod) || HomeRegistry::Pending(mod) || (mod&0xFFFF0000u)==0xFFFE0000u;}
bool Foreign(uint32_t mod) noexcept
{
    if (HomeRegistry::Scoped(mod) || HomeRegistry::Pending(mod))
        return uint32_t(activeHomeRoute.load()>>32)!=mod;
    return Tagged(mod) && (mod&0xffff)!=self.load();
}
bool Encode(uint32_t local,uint32_t& mod,uint32_t& base) noexcept
{
    const auto route=activeHomeRoute.load();
    if (route && local==uint32_t(route))
    {mod=uint32_t(route>>32);base=RouteBase(local,mod);return true;}
    for (uint32_t i=0;i<routeCount.load();++i)
    {
        const auto& r=routes[i];
        uint32_t who=local==r.source?r.own.load():local==r.copy?r.visit.load():0;
        if (who && who<=65535) {mod=0xFFFE0000u | (who-1);base=r.slot;return true;}
    }
    return false;
}
uint32_t Decode(uint32_t mod,uint32_t base) noexcept
{
    if (HomeRegistry::Scoped(mod) || HomeRegistry::Pending(mod))
    {
        const auto route=activeHomeRoute.load();const auto local=uint32_t(route);
        return route && uint32_t(route>>32)==mod && RouteBase(local,mod)==base?local:0;
    }
    uint32_t who=(mod&0xffff)+1;
    for (uint32_t i=0;i<routeCount.load();++i)
        if (routes[i].slot==base)
        {
            if (routes[i].own.load()==who) return routes[i].source;
            if (routes[i].visit.load()==who) return routes[i].copy;
        }
    return 0;
}
bool PrivateCell(uint32_t local) noexcept
{
    if (local && local==uint32_t(activeHomeRoute.load())) return true;
    for (uint32_t i=0;i<routeCount.load();++i)
        if (local==routes[i].copy || (local==routes[i].source && routes[i].own.load())) return true;
    return false;
}
}
namespace
{
struct ConstructionTrack
{
    uint64_t Campaign{},Character{},SaveTag{},Binding{};
    uint32_t Revision{};
    std::string Plot,Previous,Pending,Sample;
    Clock::time_point Next{};
    bool Armed{},ConflictWarned{};
};
ConstructionTrack constructionTrack;
void ObserveEstateConstruction(World& world)
{
    auto& track=constructionTrack;
    const bool shared=HomeEstateConstruction::SharedMode();
    auto* player=PlayerCharacter::Get();
    auto* cell=player?player->GetParentCellEx():nullptr;
    if (!player||!cell||!cell->IsAttached()||!cell->worldspace||
        originalGuestCell.load()||visit.Nonce||HomeGuestSave::Busy()||
        (!shared&&!NativeVisitsEnabled())||!CampaignAccess::GameReady()||!world.GetTransport().IsOnline())
    {track={};return;}
    const auto now=Clock::now();
    if (now<track.Next) return;
    track.Next=now+1s;
    const auto id=CampaignAccess::Current(world);
    const auto worldKey=NativeFormKey(cell->worldspace->formID);
    const auto& catalog=HomeEstateCatalog::Host();
    if (!id||!worldKey||!catalog) {track={};return;}
    const HomeEstateCatalog::Estate* estate=nullptr;
    for (const auto& [_,entry]:catalog->Estates)
        if (!entry.Construction.empty()&&entry.ContainsCandidate(worldKey->Encode(),player->position.x,player->position.y))
        {
            if (estate) {track={};return;}
            estate=&entry;
        }
    if (!estate) {track={};return;}
    const auto binding=HomeEstateConstruction::Binding(*estate);
    if (track.Campaign!=id->Save.Campaign||track.Character!=id->Save.Character||
        track.SaveTag!=id->Save.Applied.Tag||track.Plot!=estate->Quest||track.Binding!=binding)
    {
        track={};track.Campaign=id->Save.Campaign;track.Character=id->Save.Character;
        track.SaveTag=id->Save.Applied.Tag;track.Plot=estate->Quest;track.Binding=binding;track.Next=now+1s;
    }
    std::string bits;bits.reserve(estate->Construction.size());
    for (const auto& [source,base]:estate->Construction)
    {
        const auto key=TradeProperties::DecodeForm(source),baseKey=TradeProperties::DecodeForm(base);
        if (!key||!baseKey) {track.Armed=false;return;}
        auto* object=Cast<TESObjectREFR>(TESForm::GetById(CatalogForm(*key)));
        if (!object||object->IsTemporary()||object->IsDeleted()||Cast<Actor>(object)||
            !object->baseForm||object->baseForm->formID!=CatalogForm(*baseKey))
        {track.Armed=false;return;}
        bits+=object->IsDisabled()?'0':'1';
    }
    const auto prior=shared?CampaignAccess::SharedEstateConstruction(track.Campaign,track.Plot):
        CampaignAccess::EstateConstruction(track.Campaign,track.Character,track.Plot);
    if (!prior||(prior->Revision&&(prior->BindingId!=binding||prior->Bits.size()!=bits.size()||
        (!shared&&id->Save.Applied.Revision<prior->SaveRevision)))) {track.Armed=false;return;}
    const auto conflict=[&]()
    {
        track.Armed=false;track.Pending.clear();track.Sample.clear();
        if (shared&&!track.ConflictWarned)
        {
            track.ConflictWarned=true;
            Utils::ShowHudMessage("Строительство отличается от состояния кампании. Требуется согласование.");
        }
    };
    if (!track.Armed)
    {
        // A divergent older/local save must not silently replace server state.
        if (prior->Revision&&prior->Bits!=bits) {conflict();return;}
        track.ConflictWarned=false;
        track.Armed=true;track.Revision=prior->Revision;track.Previous=prior->Bits;
        track.Pending.clear();track.Sample.clear();
    }
    if (prior->Revision!=track.Revision)
    {
        if (shared&&prior->Bits==bits)
        {
            track.Revision=prior->Revision;track.Previous=prior->Bits;
            track.Pending.clear();track.Sample.clear();track.ConflictWarned=false;return;
        }
        if (track.Pending.empty()||prior->Revision!=track.Revision+1||prior->Bits!=track.Pending)
        {conflict();return;}
        track.Revision=prior->Revision;track.Previous=prior->Bits;track.Pending.clear();
    }
    if (bits==track.Previous) {track.Sample.clear();return;}
    if (track.Sample!=bits) {track.Sample=bits;return;}
    if (!track.Pending.empty()&&track.Pending!=bits) return;
    RequestPartyHome request;auto& e=request.Event;
    e.Campaign=track.Campaign;e.Character=track.Character;e.SaveTag=id->Save.Applied.Tag;
    e.SaveRevision=id->Save.Applied.Revision;e.Profile=id->Profile.c_str();e.Credential=id->Credential.Secret.c_str();
    e.Root=track.Plot.c_str();e.AccessOp=shared?10:9;e.AccessRevision=track.Revision;e.SnapshotDigest=binding;
    const auto payload=shared?std::to_string(HomeEstateConstruction::PriorDigest(*prior))+" "+bits:bits;
    e.Visible=payload.c_str();
    if (world.GetTransport().Send(request)) {track.Pending=bits;track.Next=now+3s;}
}
}

bool Workbench::IsStandard(const TESObjectREFR* object) noexcept
{
    if (!object||!object->baseForm||static_cast<uint32_t>(object->baseForm->formType)!=40) return false;
    // CommonLibSSE-NG TESFurniture.h: WorkBenchData at E0, benchType at 0.
    // https://ng.commonlib.dev/TESFurniture_8h_source.html
    uint8_t type{};
    std::memcpy(&type,reinterpret_cast<const unsigned char*>(object->baseForm)+0xE0,sizeof(type));
    return HomeSeatLeases::IsWorkbenchType(type);
}
bool Workbench::OrdinaryRoute(const TESObjectREFR* object) noexcept
{
    if (!IsStandard(object)||object->IsTemporary()||object->IsDeleted()||object->IsDisabled()) return false;
    auto* cell=const_cast<TESObjectREFR*>(object)->GetParentCellEx();
    return cell&&!HomeInstance::ProjectionForm(cell->formID)&&!HomeInstance::PrivateCell(cell->formID)&&
        !HomeInstance::NativeGuestCell(cell->formID)&&
        (HomeEstateConstruction::SharedMode()||!HomeInstance::ManagedCell(cell->formID));
}
bool Workbench::Owns(const TESObjectREFR* object) noexcept
{return object&&workbench.Started&&workbench.Form==object->formID;}
bool Workbench::Defer(TESObjectREFR* object,uint8_t unk,bool processing) noexcept
{
    try
    {
        if (!World::Exists()||!object) return false;
        auto* player=PlayerCharacter::Get();
        return player&&RequestWorkbench(World::Get(),*object,*player,unk,processing);
    }
    catch (...) {return true;}
}

void OverlayService::ClearHomes() noexcept
{
    ReleaseWorkbench(m_world);
    constructionTrack={};
    seat={};linkedNext.reset();
    HomeResidentProjection::Clear();residentDisplayNext={};
    HomePlacementClient::Clear();
    if (selection && m_pOverlay && m_gameOverlayClient)
    {m_pOverlay->ExecuteAsync("hideHome");m_gameOverlayClient->SetUIVisible(false);}
    selection=false;door=0;options.clear();accessMenu={};
    if (visit.Native && visit.Moved && originalGuestCell.load()) {visit.Leaving=true;visit.Ready=false;}
    else visit={};
    lockContext={};projectedRoot.store(0);ads.clear();SetVisit(0,0);refresh={};self.store(invalid);
    activeHomeRoute.store(0);acknowledgedHome={};baselineUpload={};residentContext={};objectContext={};
    for (unsigned i=0;i<routeCount.load();++i) routes[i].own.store(0);
    for (auto& [_,h]:houses){h.owned=false;h.visible.clear();}
}
void OverlayService::OnHome(const NotifyPartyHome& msg) noexcept
{
    const auto& e=msg.Event;
    if (e.AccessOp==HomeSeatLeases::WorkbenchOperation)
    {
        try {WorkbenchReply(m_world,e);} catch (...) {ReleaseWorkbench(m_world);}
        return;
    }
    if (CampaignAccess::Mode()!=CampaignReplication::Link::Mode::Disabled)
    {
        if (e.AccessOp)
        {
            auto identity=CampaignAccess::Current(m_world);
            if (!identity || e.Campaign!=identity->Save.Campaign || e.Character!=identity->Save.Character ||
                e.Owner!=m_transport.GetLocalPlayerId() || !e.Profile.empty() || !e.Credential.empty()) return;
            if (e.AccessOp==8) {SeatReply(m_world,e);return;}
            if (e.AccessOp==3)
            {
                if (linkedNext&&linkedNext->Nonce&&e.AccessGuest==linkedNext->Nonce&&e.House==linkedNext->Slot&&
                    e.Campaign==linkedNext->Campaign&&e.Character==linkedNext->Character&&e.AccessRight<=2&&
                    std::string_view(e.Root.data(),e.Root.size())==linkedNext->Root)
                {
                    linkedNext->Right=e.AccessRight;linkedNext->Granted=e.AccessRight!=0;
                    linkedNext->Until=linkedNext->Granted?Clock::now()+20s:Clock::time_point{};
                    return;
                }
                if (!visit.Nonce || e.AccessGuest!=visit.Nonce || e.House!=visit.Slot ||
                    e.Campaign!=visit.Campaign || e.Character!=visit.Character ||
                    std::string_view(e.Root.data(),e.Root.size())!=visit.Root || e.AccessRight>2) return;
                visit.Right=e.AccessRight;visit.Granted=e.AccessRight!=0;
                visit.Until=visit.Granted?Clock::now()+20s:Clock::time_point{};
                return;
            }
            if (e.AccessOp==1)
            {
                Utils::ShowHudMessage("Права доступа к дому обновлены.");
                return;
            }
            if (e.AccessOp!=2 || !accessMenu.Nonce || e.AccessGuest!=accessMenu.Nonce ||
                e.House!=accessMenu.Slot || e.Campaign!=accessMenu.Campaign ||
                e.Character!=accessMenu.Character || std::string_view(e.Root.data(),e.Root.size())!=accessMenu.Root ||
                Clock::now()>accessMenu.Until || selection || m_active || !m_inGame ||
                !m_pOverlay || !m_gameOverlayClient) return;
            auto* p=PlayerCharacter::Get();
            auto* entrance=Cast<TESObjectREFR>(TESForm::GetById(door));
            if (!p || !entrance || p->GetParentCellEx()!=entrance->GetParentCellEx() ||
                glm::distance(static_cast<const glm::vec3&>(p->position),static_cast<const glm::vec3&>(entrance->position))>350.f)
            {accessMenu={};return;}
            accessMenu.Nonce=0;accessMenu.Revision=e.AccessRevision;
            std::istringstream input(std::string(e.Visible.data(),e.Visible.size()));
            uint64_t guest{};uint32_t right{};std::string name,labels;
            std::set<uint64_t> seen;
            while (input>>guest>>right>>std::quoted(name))
            {
                if (!guest || guest==identity->Save.Character || right>3 || !seen.insert(guest).second ||
                    name.empty() || name.find_first_of("\r\n")!=std::string::npos) {accessMenu={};return;}
                for (uint32_t desired=0;desired<=2;++desired)
                {
                    if (desired==right) continue;
                    if (!labels.empty()) labels+='\n';
                    labels+=(desired==0?"Отозвать доступ: ":desired==1?"Разрешить посещение: ":"Разрешить изменение имущества: ")+name;
                    accessMenu.Choices.push_back({guest,desired});
                }
            }
            if (!input.eof()) {accessMenu={};return;}
            const auto* catalog=CatalogEntrance(door);
            const auto actual=EntranceRoot(door,catalog);
            if (!actual || actual->Encode()!=accessMenu.Root) {accessMenu={};return;}
            AddVisits(*identity,accessMenu.Root,catalog,door,labels);
            selection=true;
            auto args=CefListValue::Create();args->SetInt(0,token);args->SetString(1,labels);
            m_pOverlay->ExecuteAsync("offerHome",args);m_gameOverlayClient->SetUIVisible(true);
            return;
        }
        if (e.AccessRevision || e.AccessGuest || e.AccessRight) return;
        if (!e.Campaign || !e.Character || !e.House || e.House>HomeRegistry::MaxSlot ||
            e.Owner!=m_transport.GetLocalPlayerId() || !e.Visible.empty() ||
            !e.Profile.empty() || !e.Credential.empty() || e.Root.size()>220) return;
        auto identity=CampaignAccess::Current(m_world);
        auto root=TradeProperties::DecodeForm({e.Root.data(),e.Root.size()});
        if (!identity || !root || !root->Valid() || e.Campaign!=identity->Save.Campaign ||
            e.Character!=identity->Save.Character) return;
        acknowledgedHome={e.Campaign,e.Character,e.Owner,e.House,root->Encode()};
        return;
    }
    if (!Init()) return;
    const auto& members=m_world.GetPartyService().GetPartyMembers();
    if (!houses.count(e.House) || std::find(members.begin(),members.end(),e.Owner)==members.end() ||
        (!e.Visible.empty() && e.Visible.size()!=houses.at(e.House).refs.size())) return;
    auto& a=ads[{e.House,e.Owner}];a.visible.assign(e.Visible.begin(),e.Visible.end());a.until=Clock::now()+12s;
}
bool OverlayService::TryHouseDoor(uint32_t form) noexcept
{
    if (HomeGuestSave::Busy()) return true;
    if (HomeEstateConstruction::SharedMode()&&!originalGuestCell.load())
    {
        const auto* p=PlayerCharacter::Get();
        const auto* cell=p?p->GetParentCellEx():nullptr;
        if (!cell||!HomeInstance::ProjectionForm(cell->formID)) return false;
    }
    // Original owners must use the same reservation path as guests.
    if (!originalGuestCell.load()&&m_transport.IsOnline()&&CampaignAccess::GameReady())
    {
        auto* p=PlayerCharacter::Get();
        auto* ref=Cast<TESObjectREFR>(TESForm::GetById(form));
        if (p&&ref&&ref->baseForm&&static_cast<uint32_t>(ref->baseForm->formType)==40&&
            ref->GetParentCellEx()==p->GetParentCellEx()&&RequestSeat(m_world,*ref,*p)) return true;
    }
    if (originalGuestCell.load())
    {
        auto* player=PlayerCharacter::Get();
        auto* cell=player?player->GetParentCellEx():nullptr;
        if (!cell || !HomeInstance::NativeGuestCell(cell->formID) || !visit.Ready || visit.Leaving || visit.Refreshing) return true;
        if (form==visit.ExitDoor && NearDoor(*player,form))
        {visit.Leaving=true;visit.Ready=false;return true;}
        auto* object=Cast<TESObjectREFR>(TESForm::GetById(form));
        if (auto* actor=Cast<Actor>(object);actor && actor->GetExtension() && actor->GetExtension()->IsPlayer()) return false;
        if (object && object->baseForm && object->GetParentCellEx()==cell && !object->IsDisabled() && !object->IsDeleted())
        {
            const auto type=object->baseForm->formType;
            if (static_cast<uint32_t>(type)==40 && visit.Granted && Clock::now()<visit.Until &&
                CampaignAccess::GameReady() && m_transport.IsOnline() && NearDoor(*player,form))
            {
                const auto identity=CampaignAccess::Current(m_world);
                if (identity && identity->Save.Campaign==visit.Campaign && identity->Save.Character==visit.Character &&
                    NativeSeat(m_world,*object,*player,visit.Root)) RequestSeat(m_world,*object,*player);
            }
            if (type==FormType::Door)
            {
                if (auto* extra=object->GetExtraDataList();extra&&extra->Contains(ExtraDataType::Teleport))
                {
                    if (!PlanLinkedVisit(m_world,*player,*object))
                        Utils::ShowHudMessage("Связанное помещение пока недоступно для посещения.");
                    return true;
                }
                if (!visit.Granted || Clock::now()>=visit.Until || visit.Right!=2)
                {Utils::ShowHudMessage("Нет разрешения изменять имущество этого дома.");return true;}
                if (!CampaignAccess::GameReady() || !m_transport.IsOnline() || !NearDoor(*player,form)) return true;
                const auto identity=CampaignAccess::Current(m_world);
                const auto key=NativeFormKey(form);
                if (!identity||identity->Save.Campaign!=visit.Campaign||
                    identity->Save.Character!=visit.Character||!key) return true;
                const auto view=CampaignAccess::HomeObjectView(visit.Campaign,visit.Slot);
                if (!view) return true;
                const auto it=view->find(key->Encode());
                const auto open=HomeOriginalOverlay::DoorState(form);
                if (!open||it==view->end()||!it->second.Revision||it->second.Type!=29||
                    !it->second.Value.GuestDoor||it->second.Value.Disabled||it->second.Value.Open!=open) return true;
                if (const auto* lock=object->GetLock();lock&&lock->IsLocked())
                {Utils::ShowHudMessage("Дверь заперта.");return true;}
                const auto* get=m_world.ctx().at<PapyrusService>().Get("ObjectReference","GetOpenState");
                if (!get||static_cast<uint32_t>(PapyrusFunction<TESObjectREFR::OpenState,TESObjectREFR>(get)(object))!=open) return true;
                RequestPartyHome request;auto& e=request.Event;
                e.Campaign=visit.Campaign;e.Character=visit.Character;e.House=visit.Slot;
                e.SaveRevision=identity->Save.Applied.Revision;e.SaveTag=identity->Save.Applied.Tag;
                e.Profile=identity->Profile.c_str();e.Credential=identity->Credential.Secret.c_str();e.Root=visit.Root.c_str();
                e.AccessOp=7;e.AccessRevision=it->second.Revision;e.AccessRight=open==1?3u:1u;
                e.Visible=key->Encode().c_str();
                m_world.GetTransport().Send(request);
                return true;
            }
            if (type==FormType::Container ||
                (visit.Right==2 && HomeSnapshot::LooseType(static_cast<uint32_t>(type)) && HomeOriginalOverlay::CanTake(form)))
                TryTrade(form);
        }
        return true;
    }
    if (CampaignAccess::Mode()!=CampaignReplication::Link::Mode::Disabled)
    {
        auto* player=PlayerCharacter::Get();
        auto* current=player?player->GetParentCellEx():nullptr;
        if (current && HomeInstance::ProjectionForm(current->formID))
        {
            const auto* copy=CopyCell(current->formID);
            if (copy)
            {
                for (const auto& e:copy->Entrances)
                    if (CatalogForm(e.Exit)==form)
                    {
                        ClearTrade();LeaveCopy(*player,*copy,CatalogForm(e.Door));return true;
                    }
            }
            // Exits are handled above; property uses the durable trade path.
            auto* object=Cast<TESObjectREFR>(TESForm::GetById(form));
            if (visit.Ready && copy && object && object->baseForm &&
                object->baseForm->formType==FormType::Door && object->GetParentCellEx()==current &&
                HomeInstance::ProjectionForm(form))
            {
                if (!visit.Granted || Clock::now()>=visit.Until || visit.Right!=2)
                {Utils::ShowHudMessage("Нет разрешения изменять имущество этого дома.");return true;}
                if (!CampaignAccess::GameReady() || !m_world.GetTransport().IsOnline() ||
                    object->IsDisabled() || object->IsDeleted() || !NearDoor(*player,form)) return true;
                const auto identity=CampaignAccess::Current(m_world);
                if (!identity || identity->Save.Campaign!=visit.Campaign ||
                    identity->Save.Character!=visit.Character || copy->Root.Encode()!=visit.Root) return true;
                const auto binding=std::find_if(copy->Refs.begin(),copy->Refs.end(),
                    [&](const auto& r){return CatalogForm(r.Target)==form;});
                if (binding==copy->Refs.end() || IsExit(*copy,*binding) ||
                    object->baseForm->formID!=CatalogForm(binding->TargetBase)) return true;
                const auto key=binding->Source.Encode();
                if (!copy->GuestDoors.count(key)) return true;
                const auto view=CampaignAccess::HomeObjectView(visit.Campaign,visit.Slot);
                if (!view) return true;
                const auto it=view->find(key);
                if (it==view->end() || !it->second.Revision || it->second.Type!=29 ||
                    !it->second.Value.GuestDoor || it->second.Value.Disabled || !it->second.Value.Open) return true;
                if (const auto* lock=object->GetLock();lock && lock->IsLocked())
                {Utils::ShowHudMessage("Дверь заперта.");return true;}
                auto& vm=m_world.ctx().at<PapyrusService>();
                const auto* get=vm.Get("ObjectReference","GetOpenState");
                if (!get) return true;
                const auto open=static_cast<uint32_t>(PapyrusFunction<TESObjectREFR::OpenState,TESObjectREFR>(get)(object));
                if (open!=it->second.Value.Open) return true; // wait for confirmed replay/animation
                RequestPartyHome request;auto& e=request.Event;
                e.Campaign=visit.Campaign;e.Character=visit.Character;e.House=visit.Slot;
                e.SaveRevision=identity->Save.Applied.Revision;e.SaveTag=identity->Save.Applied.Tag;
                e.Profile=identity->Profile.c_str();e.Credential=identity->Credential.Secret.c_str();e.Root=visit.Root.c_str();
                e.AccessOp=7;e.AccessRevision=it->second.Revision;e.AccessRight=open==1?3u:1u;
                e.Visible=key.c_str();
                m_world.GetTransport().Send(request);
                return true; // never activate the copy or modify state before journal ACK
            }
            if (visit.Ready && object && object->baseForm && object->GetParentCellEx()==current &&
                HomeInstance::ProjectionForm(form) &&
                (object->baseForm->formType==FormType::Container ||
                 HomeSnapshot::LooseType(static_cast<uint32_t>(object->baseForm->formType))))
                TryTrade(form); // Always consume activation, even when the trade gate refuses.
            return true;
        }
        if (!m_inGame || m_active || !m_pOverlay || !m_gameOverlayClient || selection ||
            !CampaignAccess::GameReady()) return false;
        const auto* cell=CatalogEntrance(form);
        const auto identity=CampaignAccess::Current(m_world);
        const auto destination=EntranceRoot(form,cell);
        if (!destination || !identity) return false;
        const auto root=destination->Encode();
        const auto slot=CampaignAccess::HomeScope(identity->Save.Campaign,identity->Save.Character,root);
        auto* p=PlayerCharacter::Get();
        auto* original=Cast<TESObjectCELL>(TESForm::GetById(CatalogForm(*destination)));
        if (visit.Nonce) return true;
        if (!p || !NearDoor(*p,form)) return false;
        if (accessMenu.Nonce && Clock::now()<accessMenu.Until) return true;
        accessMenu={};
        if (!++token) ++token;
        door=form;accessMenu.Campaign=identity->Save.Campaign;accessMenu.Character=identity->Save.Character;
        accessMenu.Slot=slot;accessMenu.Root=root;accessMenu.Nonce=token;accessMenu.Until=Clock::now()+5s;
        if (slot && Owned(m_world,original,p))
        {
            if (!SendAccess(m_world,*identity,2,slot,root,0,token,0)) {accessMenu={};return false;}
            return true;
        }
        std::string labels;
        AddVisits(*identity,root,cell,form,labels);
        accessMenu.Nonce=0;
        if (accessMenu.Visits.empty()) {accessMenu={};return false;}
        selection=true;auto args=CefListValue::Create();args->SetInt(0,token);args->SetString(1,labels);
        m_pOverlay->ExecuteAsync("offerHome",args);m_gameOverlayClient->SetUIVisible(true);return true;
    }
    if (!m_world.GetPartyService().IsInParty() || !m_inGame || m_active || !m_pOverlay || !m_gameOverlayClient ||
        !doors.count(form) || selection) return false;
    const auto slot=doors.at(form).slot;
    if (!houses[slot].owned) return false;
    options.clear();std::string labels;
    const auto& players=m_world.GetPartyService().GetPlayers();
    for (const auto& [key,a]:ads)
        if (key.first==slot && a.until>Clock::now() && players.count(key.second))
        {
            auto name=players.at(key.second);
            for(char& c:name) if (c=='\n' || c=='\r') c=' ';
            if (!options.empty()) labels+='\n';
            labels+="В гости к ";labels+=name.c_str();options.push_back(key.second);
        }
    if (options.empty()) return false;
    selection=true;door=form;if (!++token) ++token;
    auto args=CefListValue::Create();args->SetInt(0,token);args->SetString(1,labels);
    m_pOverlay->ExecuteAsync("offerHome",args);m_gameOverlayClient->SetUIVisible(true);return true;
}
void OverlayService::ChooseHome(uint32_t nonce,int choice) noexcept
{
    if (!selection || nonce!=token) return;
    selection=false;m_pOverlay->ExecuteAsync("hideHome");m_gameOverlayClient->SetUIVisible(false);
    auto* player=PlayerCharacter::Get();
    auto* entrance=Cast<TESObjectREFR>(TESForm::GetById(door));
    if (!player || !entrance || choice<0 ||
        (CampaignAccess::Mode()==CampaignReplication::Link::Mode::Disabled && !doors.count(door)) ||
        player->GetParentCellEx()!=entrance->GetParentCellEx() ||
        glm::distance(static_cast<const glm::vec3&>(player->position),static_cast<const glm::vec3&>(entrance->position))>350.f) return;
    if (!choice) {accessMenu={};entrance->Activate(player,0,nullptr,1,0);return;}
    if (CampaignAccess::Mode()!=CampaignReplication::Link::Mode::Disabled)
    {
        const auto menu=std::move(accessMenu);accessMenu={};
        auto identity=CampaignAccess::Current(m_world);
        const auto destination=EntranceRoot(door,CatalogEntrance(door));
        if (!identity || !destination || destination->Encode()!=menu.Root ||
            identity->Save.Campaign!=menu.Campaign || identity->Save.Character!=menu.Character ||
            static_cast<size_t>(choice)>menu.Choices.size()+menu.Visits.size()) return;
        if (static_cast<size_t>(choice)>menu.Choices.size())
        {
            const auto& chosen=menu.Visits[choice-1-menu.Choices.size()];
            const auto* cell=CatalogEntrance(door);
            if (!chosen.Native && (!cell || cell->Root.Encode()!=menu.Root || !Entry(*cell,door))) return;
            if (chosen.Native && (!NativeVisitsEnabled() || !NativeEntranceRoot(door) ||
                !NativeVisitShape(menu.Campaign,chosen.Slot,menu.Root))) return;
            visit={};visit.Campaign=menu.Campaign;visit.Character=menu.Character;
            visit.Slot=chosen.Slot;visit.Nonce=token;visit.Door=door;visit.Root=menu.Root;
            visit.Native=chosen.Native;
            visit.Copy=chosen.Native?CatalogForm(*destination):CatalogForm(cell->Copy);
            visit.Base=destination->Local;visit.Until=Clock::now()+10s;
            visit.Next=Clock::now()+3s;
            if (!visit.Copy || !SendAccess(m_world,*identity,3,visit.Slot,visit.Root,0,visit.Nonce,0)) visit={};
            return;
        }
        const auto& selected=menu.Choices[choice-1];
        if (!SendAccess(m_world,*identity,1,menu.Slot,menu.Root,menu.Revision,selected.Guest,selected.Right))
            Utils::ShowHudMessage("Не удалось отправить изменение доступа.");
        return;
    }
    if (static_cast<size_t>(choice)>options.size()) return;
    const auto& d=doors.at(door);auto owner=options[choice-1];auto a=ads.find({d.slot,owner});
    if (a==ads.end() || Clock::now()>=a->second.until) return;
    auto* copy=Cast<TESObjectCELL>(TESForm::GetById(houses.at(d.slot).copy));
    if (!copy) return;
    SetVisit(d.slot,owner);player->SetRotation(d.rotation.x,d.rotation.y,d.rotation.z);
    player->MoveTo(copy,d.pos);
}
void OverlayService::UpdateHomes() noexcept
{
    if (auto* p=PlayerCharacter::Get();!p || !p->GetParentCellEx() || p->GetParentCellEx()->formID!=lockContext.Cell ||
        !CampaignAccess::GameReady() || !m_transport.IsOnline()) lockContext={};
    UpdateSeat(m_world);
    try {UpdateWorkbench(m_world);} catch (...) {ReleaseWorkbench(m_world);}
    try {ObserveEstateConstruction(m_world);} catch (...) {constructionTrack={};}
    if (HomeEstateConstruction::SharedMode()&&!originalGuestCell.load())
    {
        auto* p=PlayerCharacter::Get();
        auto* cell=p?p->GetParentCellEx():nullptr;
        if (!cell||!HomeInstance::ProjectionForm(cell->formID)) return;
    }
    if (!visit.Nonce && !HomeGuestSave::Busy() && NativeVisitsEnabled() &&
        CampaignAccess::GameReady() && m_transport.IsOnline() && Clock::now()>=resumePoll)
    {
        resumePoll=Clock::now()+1s;
        if (auto record=CampaignCheckpointClient::Loaded(m_world);record &&
            !record->Entrance.Empty() && record->Digest!=lastResumeDigest)
        {
            lastResumeDigest=record->Digest;
            HomeGuestSave::Resume(m_world,*record);
        }
    }
    if (visit.Native)
    {
        if (!CampaignAccess::GameReady()) return;
        auto* p=PlayerCharacter::Get();
        auto* current=p?p->GetParentCellEx():nullptr;
        if (!current) return;
        const auto now=Clock::now();
        const auto identity=CampaignAccess::Current(m_world);
        const bool valid=visit.Nonce && identity && identity->Save.Campaign==visit.Campaign &&
            identity->Save.Character==visit.Character && now<visit.Until;
        if (!visit.Moved)
        {
            if (!valid) {visit={};return;}
            if (!visit.Granted) return;
            const auto shape=NativeVisitShape(visit.Campaign,visit.Slot,visit.Root);
            if (!shape || !PrepareNativeEntry(*p,visit))
            {visit={};Utils::ShowHudMessage("Не удалось подготовить посещение оригинального жилья.");return;}
            auto* target=Cast<TESObjectCELL>(TESForm::GetById(visit.Copy));
            if (!target) {visit={};return;}
            ClearTrade();HomePlacementClient::Clear();
            visit.Shape=*shape;visit.Moved=true;visit.MoveUntil=now+15s;
            previousGuestCell.store(0);originalGuestCell.store(visit.Copy);
            MoveAt(*p,*target,visit.Pose,0);return;
        }
        const auto warn=[&]()
        {
            if (visit.Warned) return;
            visit.Warned=true;
            Utils::ShowHudMessage("Не удалось восстановить личное жильё. Загрузите последнее личное сохранение.");
        };
        if (visit.ExitIssued)
        {
            if (current->formID==visit.ReturnCell && current->IsAttached())
            {originalGuestCell.store(0);previousGuestCell.store(0);linkedNext.reset();visit={};projectedRoot.store(0);activeHomeRoute.store(0);}
            else if (now>=visit.MoveUntil) warn();
            return;
        }
        if (!valid || !visit.Granted) {visit.Leaving=true;visit.Ready=false;}
        if (current->formID==visit.Copy&&current->IsAttached()) previousGuestCell.store(0);
        if (current->formID!=visit.Copy)
        {
            if (!visit.Prepared && now<visit.MoveUntil) return;
            visit.Leaving=true;visit.Ready=false;
        }
        if (visit.Refreshing)
        {
            const auto result=HomeOriginalOverlay::Switch(m_world,true);
            if (result==HomeOriginalOverlay::Result::Pending) return;
            visit.Refreshing=false;visit.ShapeAt={};
            if (result!=HomeOriginalOverlay::Result::Ready) {visit.Leaving=true;visit.Ready=false;}
        }
        if (!visit.Leaving && visit.Ready && now>=visit.ShapeAt)
        {
            visit.ShapeAt=now+1s;
            const auto shape=NativeVisitShape(visit.Campaign,visit.Slot,visit.Root);
            auto result=shape&&*shape==visit.Shape?
                HomeOriginalOverlay::RefreshLoose(m_world,visit.Campaign,visit.Slot):HomeOriginalOverlay::Result::Unavailable;
            if (result==HomeOriginalOverlay::Result::Ready)
                result=HomeOriginalOverlay::RefreshDoors(m_world,visit.Campaign,visit.Slot);
            if (result==HomeOriginalOverlay::Result::Pending) {visit.Refreshing=true;return;}
            if (result!=HomeOriginalOverlay::Result::Ready)
            {
                visit.Leaving=true;visit.Ready=false;
                Utils::ShowHudMessage("Состояние жилья изменилось. Войдите повторно.");
            }
        }
        if (!visit.Leaving && !visit.Prepared)
        {
            if (!current->IsAttached()) return;
            visit.Prepared=HomeOriginalOverlay::Prepare(m_world,*current,visit.Campaign,visit.Slot,visit.Root);
            if (!visit.Prepared)
            {visit.Leaving=true;Utils::ShowHudMessage("Это жильё пока не поддерживает безопасную подмену.");}
        }
        if (visit.Prepared && !visit.Applied)
        {
            const auto result=HomeOriginalOverlay::Switch(m_world,true);
            if (result==HomeOriginalOverlay::Result::Pending) return;
            if (result==HomeOriginalOverlay::Result::Unavailable)
            {
                visit.Leaving=true;visit.Ready=false;
                if (HomeOriginalOverlay::Dirty()) {warn();return;}
            }
            else visit.Applied=true;
        }
        if (visit.Leaving)
        {
            visit.Ready=false;
            const auto id=m_transport.GetLocalPlayerId();
            activeHomeRoute.store(m_transport.IsOnline() && id<=0xFFFFu && current->formID==visit.Copy?
                (uint64_t(HomeRegistry::PendingPrefix|id)<<32)|visit.Copy:0);
            if (!visit.Closing)
            {visit.Closing=true;ClearTrade();HomePlacementClient::Clear();HomeResidentProjection::Clear();}
            if (current->formID==visit.Copy&&!NativeStanding(m_world,*p))
            {
                if (!visit.SeatExitWarned) Utils::ShowHudMessage("Встаньте с мебели для выхода из чужого дома.");
                visit.SeatExitWarned=true;return;
            }
            if (visit.Applied)
            {
                const auto result=HomeOriginalOverlay::Switch(m_world,false);
                if (result==HomeOriginalOverlay::Result::Pending) return;
                if (result!=HomeOriginalOverlay::Result::Ready) {warn();return;}
                visit.Applied=false;
            }
            if (!HomeOriginalOverlay::Release()) {warn();return;}
            visit.Prepared=false;
            // Source is restored before requesting a different room lease.
            // Keep the original route quarantined until the new room attaches.
            if (linkedNext)
            {
                auto& next=*linkedNext;
                const bool same=identity&&m_transport.IsOnline()&&identity->Save.Campaign==next.Campaign&&
                    identity->Save.Character==next.Character&&current->formID==visit.Copy&&NearDoor(*p,next.Door);
                if (!same||(!next.Nonce&&!valid)) linkedNext.reset();
                else if (!next.Nonce)
                {
                    if (!++token) ++token;
                    next.Nonce=token;next.Until=now+5s;
                    if (SendAccess(m_world,*identity,3,next.Slot,next.Root,0,next.Nonce,0)) return;
                    linkedNext.reset();
                }
                else if (now>=next.Until) linkedNext.reset();
                else if (!next.Granted) return;
                else
                {
                    auto* source=Cast<TESObjectREFR>(TESForm::GetById(next.Door));
                    auto* target=NativeDoorDestination(source);
                    const auto shape=NativeVisitShape(next.Campaign,next.Slot,next.Root);
                    if (target&&target->formID==next.Copy&&shape&&*shape==next.Shape)
                    {
                        auto moved=std::move(next);linkedNext.reset();
                        moved.Moved=true;moved.MoveUntil=now+15s;moved.Next=now+3s;
                        if (moved.Copy==moved.EntryCell) moved.Door=moved.EntryDoor;
                        previousGuestCell.store(visit.Copy);originalGuestCell.store(moved.Copy);
                        visit=std::move(moved);
                        MoveAt(*p,*target,visit.Pose,0);return;
                    }
                    linkedNext.reset();
                }
            }
            auto* out=Cast<TESObjectCELL>(TESForm::GetById(visit.ReturnCell));
            if (!out) {warn();return;}
            visit.ExitIssued=true;visit.MoveUntil=now+15s;
            MoveAt(*p,*out,visit.Pose,6);return;
        }
        if (visit.Applied && visit.ResumePose)
        {
            const auto& pose=*visit.ResumePose;
            if (HomeSnapshot::Digest(visit.Shape)!=visit.ResumeShape)
            {
                Utils::ShowHudMessage("Обстановка дома изменилась: возвращение у входа.");
                visit.ResumePose.reset();
            }
            else
            {
                const auto coord=[&](size_t i){return std::bit_cast<float>(pose[i]);};
                if (!visit.ResumeIssued)
                {
                    const auto* fn=m_world.ctx().at<PapyrusService>().Get("ObjectReference","SetPosition");
                    if (!fn) {visit.Leaving=true;return;}
                    p->SetRotation(coord(3),coord(4),coord(5));
                    auto setter=PapyrusFunction<void,TESObjectREFR,float,float,float>(fn);
                    setter(p,coord(0),coord(1),coord(2));
                    visit.ResumeIssued=true;visit.MoveUntil=now+5s;
                }
                const auto dx=p->position.x-coord(0),dy=p->position.y-coord(1),dz=p->position.z-coord(2);
                if (!std::isfinite(dx+dy+dz) || dx*dx+dy*dy+dz*dz>256.f)
                {
                    if (now>=visit.MoveUntil) {visit.Leaving=true;Utils::ShowHudMessage("Не удалось восстановить позицию визита.");}
                    return;
                }
                visit.ResumePose.reset();
            }
        }
        if (visit.Applied)
        {
            if (!visit.Ready||now>=residentDisplayNext)
            {
                residentDisplayNext=now+1s;
                if (!ApplyNativeResidents(m_world,*current,visit.Campaign,visit.Slot,visit.Root))
                {visit.Leaving=true;visit.Ready=false;return;}
            }
            visit.Ready=true;
        }
        if (valid && visit.Nonce && now>=visit.Next)
        {
            visit.Next=now+3s;
            if (!SendAccess(m_world,*identity,3,visit.Slot,visit.Root,0,visit.Nonce,0)) visit.Until={};
        }
        return;
    }
    // A saved or disconnected projection must be exited even when the
    // campaign link is unavailable. Never fall through to original activation.
    if (auto* p=PlayerCharacter::Get();p && CampaignAccess::GameReady())
    {
        auto* current=p->GetParentCellEx();
        const auto* copy=current && HomeInstance::ProjectionForm(current->formID)?CopyCell(current->formID):nullptr;
        const auto identity=(copy || visit.Nonce)?CampaignAccess::Current(m_world):std::nullopt;
        const auto now=Clock::now();
        const bool valid=visit.Nonce && identity && identity->Save.Campaign==visit.Campaign &&
            identity->Save.Character==visit.Character && now<visit.Until;
        if (current && HomeInstance::ProjectionForm(current->formID) && !copy) return;
        if (copy)
        {
            if (!valid || !visit.Granted || visit.Copy!=current->formID || !visit.Moved)
            {
                ClearTrade();LeaveCopy(*p,*copy,visit.Door);return;
            }
            if (!visit.Ready && current->IsAttached())
            {
                const auto bytes=CampaignAccess::HomeBaseline(visit.Campaign,visit.Slot);
                const auto state=bytes?HomeSnapshot::Decode(*bytes):std::nullopt;
                const auto bindings=state?HomeCatalog::Bind(*copy,*state):std::nullopt;
                if (!bindings || !ResidentPlan(*copy,*state,visit.Campaign,visit.Slot) ||
                    !HomeProjection::Apply(*current,*state,*bindings) ||
                    !ApplyObjectChanges(m_world,*current,*copy,*state,visit.Campaign,visit.Slot) ||
                    !ApplyResidents(m_world,*current,*copy,*state,visit.Campaign,visit.Slot))
                {ClearTrade();LeaveCopy(*p,*copy,visit.Door);return;}
                projectedRoot.store((uint64_t(visit.Base)<<32)|visit.Copy);
                visit.Ready=true;
            }
            if (visit.Ready && identity)
            {
                SyncLocks(*current,*identity,visit.Slot,visit.Root,copy);
                if (now>=residentDisplayNext && current->IsAttached())
                {
                    residentDisplayNext=now+1s;
                    const auto bytes=CampaignAccess::HomeBaseline(visit.Campaign,visit.Slot);
                    const auto state=bytes?HomeSnapshot::Decode(*bytes):std::nullopt;
                    if (!state || !ApplyLoose(*current,*state,copy) ||
                        !ApplyObjectChanges(m_world,*current,*copy,*state,visit.Campaign,visit.Slot) ||
                        !ApplyResidents(m_world,*current,*copy,*state,visit.Campaign,visit.Slot))
                    {ClearTrade();LeaveCopy(*p,*copy,visit.Door);return;}
                }
            }
        }
        else if (visit.Moved && visit.Ready)
        {HomeResidentProjection::Clear();residentDisplayNext={};ClearTrade();visit={};projectedRoot.store(0);}
        else if (visit.Nonce && !valid) visit={};
        else if (valid && visit.Granted && !visit.Moved)
        {
            const auto* cell=CatalogEntrance(visit.Door);
            const auto* entry=cell?Entry(*cell,visit.Door):nullptr;
            auto* target=Cast<TESObjectCELL>(TESForm::GetById(visit.Copy));
            if (!entry || !target || !NearDoor(*p,visit.Door)) {visit={};return;}
            ClearTrade();visit.Moved=true;MoveAt(*p,*target,entry->Pose,0);
        }
        if (valid && visit.Nonce && now>=visit.Next)
        {
            visit.Next=now+3s;
            if (!SendAccess(m_world,*identity,3,visit.Slot,visit.Root,0,visit.Nonce,0)) visit.Until={};
        }
    }
    if (CampaignAccess::Mode()!=CampaignReplication::Link::Mode::Disabled)
    {
        if (selection && !GetActive()) {selection=false;accessMenu={};if (m_pOverlay) m_pOverlay->ExecuteAsync("hideHome");}
        if (accessMenu.Nonce && Clock::now()>accessMenu.Until) {accessMenu={};Utils::ShowHudMessage("Список доступа не получен; активируйте дверь повторно.");}
        return;
    }
    if (!m_transport.IsConnected() || !m_world.GetPartyService().IsInParty() || !m_inGame) return;
    auto* p=PlayerCharacter::Get();
    if (!p || !p->parentCell || !Init()) return;
    if (selection && !GetActive()) {selection=false;m_pOverlay->ExecuteAsync("hideHome");}
    auto* current=p->GetParentCellEx();
    const auto now=Clock::now();
    if (now>=refresh)
    {
        refresh=now+3s;
        const auto id=m_transport.GetLocalPlayerId();self.store(id);
        unsigned i=0;
        for (auto& [slot,h]:houses)
        {
            auto* cell=Cast<TESObjectCELL>(TESForm::GetById(h.original));
            h.owned=Owned(m_world,cell,p);routes[i++].own.store(h.owned?id+1:0);
            if (!h.owned) continue;
            if (cell==current && cell->IsAttached())
            {
                h.visible.clear();h.visible.reserve(h.refs.size());
                for (const auto& entry:h.refs)
                {
                    auto* ref=entry.original?Cast<TESObjectREFR>(TESForm::GetById(entry.original)):nullptr;
                    h.visible+=ref?(ref->IsDisabled()?'0':'1'):'?';
                }
            }
            RequestPartyHome m;m.Event.House=slot;m.Event.Visible=h.visible.c_str();m_transport.Send(m);
        }
    }
    if (!guestSlot || !current || current->formID!=houses.at(guestSlot).copy || !current->IsAttached() || now<applyAt) return;
    applyAt=now+1s;
    auto a=ads.find({guestSlot,guestOwner});if (a==ads.end()) return;
    auto& h=houses.at(guestSlot);
    const auto& visible=a->second.visible;
    if (visible.size()!=h.refs.size()) return;
    if (applied.size()!=visible.size()) applied.assign(visible.size(),'?');
    for (size_t i=0;i<visible.size();++i)
    {
        if (visible[i]=='?' || visible[i]==applied[i]) continue;
        auto* ref=Cast<TESObjectREFR>(TESForm::GetById(h.refs[i].copy));
        if (!ref) continue;
        if (visible[i]=='1' && ref->IsDisabled()) ref->Enable();
        if (visible[i]=='0' && !ref->IsDisabled()) ref->Disable();
        applied[i]=visible[i];
    }
}

// Registration uses an actual owned interior, not the five generated copies.
// Run before cell discovery so the first cell announcement has the right
// namespace. Until the replicated registration exists, use a private
// connection-local quarantine. No guest projection is enabled in this mode.
std::optional<HomeGuestSave::Context> HomeGuestSave::CaptureAndLeave(World& world) noexcept
{
    try
    {
        auto* p=PlayerCharacter::Get();
        auto* cell=p?p->GetParentCellEx():nullptr;
        if (!p||!cell||!cell->IsAttached()||!visit.Native||!visit.Ready||!visit.Applied||
            visit.Leaving||visit.Refreshing||!visit.Granted||Clock::now()>=visit.Until||cell->formID!=visit.Copy||
            !InventoryReady(world)||!NativeStanding(world,*p)||seat.Nonce||linkedNext) return {};
        if (!visit.EntrySlot||!visit.EntryCell||!visit.EntryDoor) return {};
        const auto shape=NativeVisitShape(visit.Campaign,visit.Slot,visit.Root);
        const auto entrance=NativeFormKey(visit.EntryDoor);
        auto record=CampaignCheckpointClient::Capture(world);
        if (!shape||*shape!=visit.Shape||!entrance||!record||record->HomeSlot!=visit.Slot||
            record->Save.Campaign!=visit.Campaign||record->Save.Character!=visit.Character||
            record->Cell.Encode()!=visit.Root) return {};
        record->Entrance=*entrance;record->Shape=HomeSnapshot::Digest(*shape);
        if (visit.Copy!=visit.EntryCell)
        {
            if (visit.EntrySlot==visit.Slot) return {};
            record->EntrySlot=visit.EntrySlot;
            std::copy_n(visit.Pose.begin(),6,record->ArrivalPose.begin());
        }
        if (!record->Shape||!record->Valid()) return {};
        Context result;result.Checkpoint=std::move(*record);result.ReturnCell=visit.ReturnCell;
        std::copy_n(visit.Pose.begin()+6,6,result.ReturnPose.begin());
        if (!result.ReturnCell) return {};
        visit.Ready=false;visit.Leaving=true;
        return result;
    }
    catch (...) {return {};}
}
bool HomeGuestSave::AtReturn(const Context& context) noexcept
{
    auto* p=PlayerCharacter::Get();
    auto* cell=p?p->GetParentCellEx():nullptr;
    if (!cell||!cell->IsAttached()||cell->formID!=context.ReturnCell||
        HomeInstance::NativeGuestActive()||HomeOriginalOverlay::Dirty()) return false;
    const auto& pos=context.ReturnPose;
    const auto dx=p->position.x-std::bit_cast<float>(pos[0]);
    const auto dy=p->position.y-std::bit_cast<float>(pos[1]);
    const auto dz=p->position.z-std::bit_cast<float>(pos[2]);
    return std::isfinite(dx+dy+dz)&&dx*dx+dy*dy+dz*dz<=4096.f;
}
bool HomeGuestSave::Resume(World& world,const CampaignCheckpoint::Record& record) noexcept
{
    try
    {
        if (!NativeVisitsEnabled()||visit.Nonce||HomeInstance::NativeGuestActive()||
            HomeOriginalOverlay::Dirty()||!record.Entrance.Valid()||!record.Shape||!record.HomeSlot||
            !CampaignAccess::GameReady()) return false;
        const auto identity=CampaignAccess::Current(world);
        auto* p=PlayerCharacter::Get();
        const auto entrance=CatalogForm(record.Entrance);
        const auto root=NativeEntranceRoot(entrance);
        if (!record.Valid()||!identity||identity->Save!=record.Save||!p||!root||
            (!record.EntrySlot&&root->Encode()!=record.Cell.Encode())||!NearDoor(*p,entrance)) return false;
        bool allowed=false;
        for (const auto& [slot,owner]:CampaignAccess::HomeOffers(record.Save.Campaign,record.Save.Character,record.Cell.Encode()))
        {
            (void)owner;
            if (slot==record.HomeSlot) allowed=true;
        }
        if (!allowed||!NativeVisitShape(record.Save.Campaign,record.HomeSlot,record.Cell.Encode())) return false;
        VisitState next;
        if (!++token) ++token;
        next.Campaign=record.Save.Campaign;next.Character=record.Save.Character;
        next.Slot=record.HomeSlot;next.Nonce=token;next.Door=entrance;next.Root=record.Cell.Encode();
        next.Copy=CatalogForm(record.Cell);next.Base=record.Cell.Local;next.Native=true;
        next.Until=Clock::now()+10s;next.Next=Clock::now()+3s;
        next.ResumePose=record.Pose;next.ResumeShape=record.Shape;next.EntrySlot=record.EntrySlot;
        if (record.EntrySlot) std::copy_n(record.ArrivalPose.begin(),6,next.Pose.begin());
        if (!next.Copy||!PrepareNativeEntry(*p,next)||!SendAccess(world,*identity,3,next.Slot,next.Root,0,next.Nonce,0)) return false;
        visit=std::move(next);return true;
    }
    catch (...) {return false;}
}
bool HomeInstance::NativeGuestCell(uint32_t form) noexcept
{return form && (form==originalGuestCell.load()||form==previousGuestCell.load());}
bool HomeInstance::NativeGuestActive() noexcept
{return originalGuestCell.load()!=0;}
bool HomeInstance::NativeResidentBinding(uint32_t source,uint32_t base) noexcept
{
    try
    {
        const auto ref=NativeFormKey(source),npc=NativeFormKey(base);
        return ref&&npc&&HomeNativeCapabilities::ResidentMatches(*HomeNativeCapabilities::Movable(),
            ref->Encode(),npc->Encode());
    }
    catch (...) {return false;}
}
bool HomeInstance::NativeGuestResident(uint32_t source,uint32_t base) noexcept
{
    try
    {
        if (!visit.Native||!visit.Ready||visit.Leaving||!visit.Granted||Clock::now()>=visit.Until||
            !NativeResidentBinding(source,base)) return false;
        const auto ref=NativeFormKey(source),npc=NativeFormKey(base);
        if (!ref||!npc) return false;
        const auto records=CampaignAccess::HomeResidentView(visit.Campaign,visit.Slot);
        if (!records) return false;
        const auto found=records->find(ref->Encode());
        // The live spawn is already scope/owner checked by STR and server.
        // A first arrival can precede its journal observation; only the live
        // protected clone is allowed, not an uncommitted offline display.
        return found==records->end()||found->second.Value.Base.Encode()==npc->Encode();
    }
    catch (...) {return false;}
}
void HomeInstance::ForgetOriginalVisit() noexcept
{
    if (World::Exists()) ReleaseWorkbench(World::Get()); else workbench={};
    constructionTrack={};
    if (visit.Native) {visit={};activeHomeRoute.store(0);projectedRoot.store(0);}
    originalGuestCell.store(0);previousGuestCell.store(0);linkedNext.reset();lastResumeDigest.clear();resumePoll={};seat={};
}
bool HomeInstance::ProjectionForm(uint32_t form) noexcept
{
    auto* mods=ModManager::Get();
    auto* plugin=mods?mods->GetByName("SkyrimCoopHomeInstances.esp"):nullptr;
    return form && plugin && plugin->IsLoaded() && !plugin->IsLite() &&
        plugin->GetFormId(form&0xFFFFFFu)==form;
}
uint32_t HomeInstance::GuestReference(uint32_t local,uint32_t& source) noexcept
{
    source=0;
    if (!visit.Ready || !visit.Granted || visit.Leaving || Clock::now()>=visit.Until) return 0;
    if (visit.Native)
    {
        auto* object=Cast<TESObjectREFR>(TESForm::GetById(local));
        auto* parent=object?object->GetParentCellEx():nullptr;
        if (!parent || !NativeGuestCell(parent->formID) || object->IsTemporary() || object->IsDeleted() ||
            object->IsDisabled() || !object->baseForm || visit.Refreshing) return 0;
        const auto type=object->baseForm->formType;
        if (type!=FormType::Container &&
            !(visit.Right==2 && HomeSnapshot::LooseType(static_cast<uint32_t>(type)) && HomeOriginalOverlay::CanTake(local))) return 0;
        source=local;return visit.Slot;
    }
    const auto* cell=CopyCell(visit.Copy);
    if (!cell) return 0;
    for (const auto& row:cell->Refs)
        if (CatalogForm(row.Target)==local)
        {source=CatalogForm(row.Source);return source?visit.Slot:0;}
    return 0;
}
bool HomeInstance::ManagedCell(uint32_t local) noexcept
{
    try
    {
        if (ProjectionForm(local) || PrivateCell(local) || NativeGuestCell(local)) return true;
        if (CampaignAccess::Mode()==CampaignReplication::Link::Mode::Disabled) return false;
        return CatalogCell(local)!=nullptr;
    }
    catch (...) {return false;}
}
void HomeInstance::ObserveLock(uint32_t reference) noexcept
{
    try
    {
        auto& world=World::Get();auto& ctx=lockContext;
        if (NativeGuestActive() || !ctx.Slot || !CampaignAccess::GameReady() || ProjectionForm(reference)) return;
        auto* player=PlayerCharacter::Get();
        auto* ref=Cast<TESObjectREFR>(TESForm::GetById(reference));
        auto identity=CampaignAccess::Current(world);
        if (!identity || identity->Save.Campaign!=ctx.Campaign || identity->Save.Character!=ctx.Character ||
            !player || !ref || !ref->GetParentCellEx() || player->GetParentCellEx()!=ref->GetParentCellEx() ||
            ref->GetParentCellEx()->formID!=ctx.Cell || ctx.Pending.count(reference)) return;
        auto it=ctx.Known.find(reference);
        const auto* lock=ref->GetLock();
        if (it==ctx.Known.end() || !lock) return;
        const auto& [key,state]=it->second;
        const uint32_t desired=lock->IsLocked()?2:1;
        if (desired==state[0] && lock->lockLevel==state[1]) return;
        RequestPartyHome req;auto& e=req.Event;
        e.Campaign=ctx.Campaign;e.Character=ctx.Character;e.SaveTag=identity->Save.Applied.Tag;
        e.SaveRevision=identity->Save.Applied.Revision;e.Profile=identity->Profile.c_str();
        e.Credential=identity->Credential.Secret.c_str();e.Root=ctx.Root.c_str();e.House=ctx.Slot;
        e.AccessOp=4;e.AccessRevision=state[2];e.AccessRight=desired;e.AccessGuest=lock->lockLevel;
        e.Visible=key.c_str();
        if (world.GetTransport().Send(req)) ctx.Pending[reference]={state[2],Clock::now()+10s};
    }
    catch (...) {}
}
void HomeInstance::Prepare(World& world,TESObjectCELL* cell) noexcept
{
    static uint32_t observed{}, observedPlayer=invalid;
    static Clock::time_point checkAt{};
    try
    {
        if (!world.GetTransport().IsOnline() || !cell)
        {
            activeHomeRoute.store(0);acknowledgedHome={};baselineUpload={};residentContext={};objectContext={};observed=0;checkAt={};return;
        }
        auto* player=PlayerCharacter::Get();
        if (!player) return;
        const auto id=world.GetTransport().GetLocalPlayerId();
        const auto mode=CampaignAccess::Mode();
        if (HomeEstateConstruction::SharedMode()&&!NativeGuestCell(cell->formID)&&
            !HomeInstance::ProjectionForm(cell->formID))
        {
            activeHomeRoute.store(0);projectedRoot.store(0);
            acknowledgedHome={};baselineUpload={};residentContext={};objectContext={};lockContext={};
            return; // ordinary physical CELL; no private registration or owner recovery
        }
        if (NativeGuestCell(cell->formID))
        {
            residentContext={};objectContext={};lockContext={};baselineUpload={};
            if (id>0xFFFFu) {activeHomeRoute.store(0);return;}
            const bool ready=visit.Native && visit.Copy==cell->formID && visit.Ready && !visit.Leaving && visit.Granted &&
                Clock::now()<visit.Until && mode==CampaignReplication::Link::Mode::Ready;
            activeHomeRoute.store((uint64_t(ready?HomeRegistry::ScopePrefix|visit.Slot:HomeRegistry::PendingPrefix|id)<<32)|cell->formID);
            return; // no owner observation, registration or inventory recovery
        }
        if (HomeInstance::ProjectionForm(cell->formID))
        {
            residentContext={};objectContext={};
            if (id>0xFFFFu) {activeHomeRoute.store(0);return;}
            const bool ready=visit.Ready && visit.Granted && visit.Copy==cell->formID &&
                Clock::now()<visit.Until && mode==CampaignReplication::Link::Mode::Ready;
            activeHomeRoute.store((uint64_t(ready?HomeRegistry::ScopePrefix|visit.Slot:HomeRegistry::PendingPrefix|id)<<32)|cell->formID);
            return;
        }
        if (mode==CampaignReplication::Link::Mode::Disabled)
        {
            residentContext={};objectContext={};
            activeHomeRoute.store(0);
            if (!world.GetPartyService().IsInParty() || !Init()) return;
            const auto previousSelf=self.exchange(id);
            if (previousSelf==id && observed==cell->formID && observedPlayer==id && Clock::now()<checkAt) return;
            observed=cell->formID;observedPlayer=id;checkAt=Clock::now()+1s;
            for (unsigned i=0;i<routeCount.load();++i)
                if (routes[i].source==cell->formID)
                    routes[i].own.store(Owned(world,cell,player) && id<65535?id+1:0);
            return;
        }
        const auto now=Clock::now();
        const auto old=activeHomeRoute.load();
        const auto nativeKey=NativeFormKey(cell->formID);
        const bool candidate=CatalogCell(cell->formID) || (nativeKey&&HomeNativeCapabilities::LinkedCell(nativeKey->Encode())) ||
            (cell->loadedCellData && cell->loadedCellData->encounterZone &&
            cell->loadedCellData->encounterZone->formID==0xF90B1u && cell->formID!=0xEEC55u);
        if (observed!=cell->formID || observedPlayer!=id || (candidate && !old))
        {
            observed=cell->formID;observedPlayer=id;checkAt={};baselineUpload={};residentContext={};objectContext={};
            activeHomeRoute.store(0);
        }
        if (now<checkAt) return;
        checkAt=now+1s;
        // NoResetZone is STR's existing conservative home classification;
        // explicit ownership is required in addition. More home types will
        // need a catalog/root-boundary mapping, not a guessed cell owner.
        if (!candidate || id>0xFFFFu)
        {
            activeHomeRoute.store(0);return;
        }
        // Quarantine even while ownership/identity is not yet readable.
        // Never publish the canonical shared CELL for an unready home.
        const auto quarantine=(uint64_t(HomeRegistry::PendingPrefix|id)<<32)|cell->formID;
        if (!activeHomeRoute.load()) activeHomeRoute.store(quarantine);
        if (mode!=CampaignReplication::Link::Mode::Ready || !CampaignAccess::GameReady() || !Owned(world,cell,player))
        {residentContext={};objectContext={};activeHomeRoute.store(quarantine);return;}
        const auto identity=CampaignAccess::Current(world);
        if (!identity) {residentContext={};objectContext={};activeHomeRoute.store(quarantine);return;}
        std::vector<InventoryEvidence::PluginSlot> slots;
        if (auto* mods=ModManager::Get())
            for (const auto* mod:mods->mods)
            {
                if (!mod || !mod->IsLoaded()) continue;
                const auto* end=std::find(mod->filename,mod->filename+sizeof(mod->filename),'\0');
                if (end!=mod->filename+sizeof(mod->filename))
                    slots.push_back({mod->GetId(),mod->IsLite(),std::string(mod->filename,end)});
            }
        auto key=InventoryEvidence::Resolve(cell->formID,slots);
        if (!key) {activeHomeRoute.store(quarantine);return;}
        TradeProperties::FormKey root{key->Plugin,key->Local};
        if (!root.Valid()) {activeHomeRoute.store(quarantine);return;}
        const auto encoded=root.Encode();
        const auto slot=CampaignAccess::HomeScope(identity->Save.Campaign,identity->Save.Character,encoded);
        if (slot && acknowledgedHome.campaign==identity->Save.Campaign &&
            acknowledgedHome.character==identity->Save.Character && acknowledgedHome.owner==id &&
            acknowledgedHome.slot==slot && acknowledgedHome.root==encoded)
        {
            activeHomeRoute.store((uint64_t(HomeRegistry::ScopePrefix|slot)<<32)|cell->formID);
            UploadBaseline(world,*cell,*identity,slot,encoded,slots);
            SyncLocks(*cell,*identity,slot,encoded,nullptr);
            SyncResidents(world,*cell,*identity,slot,encoded,slots);
            static Clock::time_point looseNext{};
            if (Clock::now()>=looseNext && cell->IsAttached())
            {
                looseNext=Clock::now()+1s;
                const auto bytes=CampaignAccess::HomeBaseline(identity->Save.Campaign,slot);
                const auto state=bytes?HomeSnapshot::Decode(*bytes):std::nullopt;
                if (state)
                {
                    ApplyLoose(*cell,*state,nullptr);
                    SyncObjects(world,*cell,*identity,slot,encoded,*state);
                }
            }
            return;
        }
        activeHomeRoute.store(quarantine);
        RequestPartyHome request;
        auto& e=request.Event;
        e.Campaign=identity->Save.Campaign;e.Character=identity->Save.Character;
        e.SaveTag=identity->Save.Applied.Tag;e.SaveRevision=identity->Save.Applied.Revision;
        e.Profile=identity->Profile.c_str();e.Credential=identity->Credential.Secret.c_str();e.Root=encoded.c_str();
        world.GetTransport().Send(request);
    }
    catch (const std::exception& e) {spdlog::error("Preparing home instance: {}",e.what());}
}
