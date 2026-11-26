#include <TiltedOnlinePCH.h>
#include <World.h>
#include <Services/HomeOriginalOverlay.h>
#include <Services/CampaignAccess.h>
#include <Services/HomeInstance.h>
#include <HomeNativeCapabilities.h>
#include <Games/ActorExtension.h>
#include <Services/PapyrusService.h>
#include <Games/TES.h>
#include <Games/References.h>
#include <Forms/TESObjectCELL.h>
#include <ExtraData/ExtraDataList.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace HomeOriginalOverlay
{
namespace
{
Transaction overlay;
uint32_t cellId{};
constexpr auto EnableParent=static_cast<ExtraDataType>(0x36);
constexpr auto EnableChildren=static_cast<ExtraDataType>(0x37);
uint32_t Local(const TradeProperties::FormKey& key)
{
    auto* mods=ModManager::Get();
    auto* plugin=mods?mods->GetByName(key.Plugin.c_str()):nullptr;
    if (!key.Valid()||!plugin||!plugin->IsLoaded()||(plugin->IsLite()&&key.Local>0xFFF)) return 0;
    return plugin->GetFormId(key.Local);
}
TESObjectREFR* Resolve(const Delta& row)
{
    auto* object=Cast<TESObjectREFR>(TESForm::GetById(row.Ref));
    auto* cell=object?object->GetParentCellEx():nullptr;
    if (!object||!cell||cell->formID!=cellId||object->IsTemporary()||object->IsDeleted()||
        !object->baseForm||object->baseForm->formID!=row.Base||
        static_cast<uint32_t>(object->baseForm->formType)!=row.Type||object->scale!=row.Scale)
        return nullptr;
    auto* actor=Cast<Actor>(object);
    if (row.Type==43)
    {
        if (!actor||actor->IsDead()||!actor->GetExtension()||actor->GetExtension()->IsRemote()||
            actor->GetExtension()->IsPlayer()) return nullptr;
    }
    else if (actor) return nullptr;
    return object;
}
std::optional<Fields> Observe(World& world,const Delta& row)
{
    auto* object=Resolve(row);
    if (!object) return {};
    Fields f;
    f.Disabled=object->IsDisabled();
    if (row.Type==43) return f; // visibility only; never copy actor pose/life/inventory
    if (auto* lock=object->GetLock())
    {f.LockState=lock->IsLocked()?2u:1u;f.LockLevel=lock->lockLevel;}
    if (row.Guest.Open)
    {
        const auto* get=world.ctx().at<PapyrusService>().Get("ObjectReference","GetOpenState");
        if (!get) return {};
        f.Open=static_cast<uint32_t>(PapyrusFunction<TESObjectREFR::OpenState,TESObjectREFR>(get)(object));
        // kOpening/kClosing are intermediate observations, not targets.
        if (!f.Open||f.Open>4) return {};
    }
    f.Pose={std::bit_cast<uint32_t>(object->position.x),std::bit_cast<uint32_t>(object->position.y),
            std::bit_cast<uint32_t>(object->position.z),std::bit_cast<uint32_t>(object->rotation.x),
            std::bit_cast<uint32_t>(object->rotation.y),std::bit_cast<uint32_t>(object->rotation.z)};
    return f.Valid()?std::optional<Fields>(f):std::nullopt;
}
// CommonLibSSE-NG ExtraEnableStateParent / ExtraEnableStateChildren / BSTList:
// parent handle at 0x14; inline child node at 0x10, next at +8.
std::optional<uint32_t> Parent(TESObjectREFR& object)
{
    auto* extra=object.GetExtraDataList();
    if (!extra) return {};
    const auto* record=extra->GetByType(EnableParent);
    if (!record) return 0u;
    uint32_t handle{};
    std::memcpy(&handle,reinterpret_cast<const unsigned char*>(record)+0x14,sizeof(handle));
    auto* parent=handle?TESObjectREFR::GetByHandle(handle):nullptr;
    if (!parent||parent->formID==object.formID) return {};
    return parent->formID;
}
std::optional<std::set<uint32_t>> Children(TESObjectREFR& object)
{
    auto* extra=object.GetExtraDataList();
    if (!extra) return {};
    const auto* record=extra->GetByType(EnableChildren);
    std::set<uint32_t> result;
    if (!record) return result;
    const auto* node=reinterpret_cast<const unsigned char*>(record)+0x10;
    std::set<const void*> seen;
    while (node)
    {
        if (seen.size()>=16384||!seen.insert(node).second) return {};
        uint32_t handle{};const unsigned char* next{};
        std::memcpy(&handle,node,sizeof(handle));std::memcpy(&next,node+8,sizeof(next));
        if (!handle)
        {if (next||!result.empty()) return {};break;}
        auto* child=TESObjectREFR::GetByHandle(handle);
        if (!child||child->formID==object.formID||!result.insert(child->formID).second) return {};
        node=next;
    }
    return result;
}
bool CanChange(TESObjectREFR& object,const Delta& row)
{
    auto* extra=object.GetExtraDataList();
    if (row.Type==43)
        return extra&&!extra->HasQuestObjectAlias()&&!extra->Contains(EnableParent)&&
            !extra->Contains(EnableChildren)&&!extra->Contains(ExtraDataType::Teleport);
    // Field allowlist, not a guarantee about arbitrary attached scripts.
    return extra&&!extra->HasQuestObjectAlias()&&!extra->Contains(ExtraDataType::Teleport)&&
        (row.Type==28||row.Type==29||row.Type==34||row.Type==35||row.Type==36||row.Type==40||
         HomeSnapshot::LooseType(row.Type));
}
bool Write(World& world,const Delta& row,const Fields& target)
{
    auto* object=Resolve(row);
    if (!object||!CanChange(*object,row)) return false;
    const auto current=Observe(world,row);
    if (!current) return false;
    auto& vm=world.ctx().at<PapyrusService>();
    const auto v=[&](size_t i){return std::bit_cast<float>(target.Pose[i]);};
    if (!std::equal(target.Pose.begin(),target.Pose.begin()+3,current->Pose.begin()))
    {
        const auto* set=vm.Get("ObjectReference","SetPosition");
        if (!set) return false;
        PapyrusFunction<void,TESObjectREFR,float,float,float>{set}(object,v(0),v(1),v(2));
    }
    if (!std::equal(target.Pose.begin()+3,target.Pose.end(),current->Pose.begin()+3))
        object->SetRotation(v(3),v(4),v(5));
    if (current->LockState!=target.LockState||current->LockLevel!=target.LockLevel)
    {
        auto* lock=object->GetLock();
        if (!lock||!target.LockState) return false;
        lock->lockLevel=static_cast<uint8_t>(target.LockLevel);
        lock->SetLock(target.LockState==2);object->LockChange();
    }
    if (current->Open!=target.Open)
    {
        if (!row.LiveDoor || (target.Open!=1&&target.Open!=3)) return false;
        const auto* set=vm.Get("ObjectReference","SetOpen");
        if (!set) return false;
        PapyrusFunction<void,TESObjectREFR,bool>{set}(object,target.Open==1);
    }
    if (!row.ParentDriven&&current->Disabled!=target.Disabled)
    {
        const auto* fn=vm.Get("ObjectReference",target.Disabled?"Disable":"Enable");
        if (!fn) return false;
        PapyrusFunction<void,TESObjectREFR,bool>{fn}(object,false);
    }
    return true;
}
}
bool Prepare(World& world,TESObjectCELL& cell,uint64_t campaign,uint32_t slot,const std::string& root) noexcept
{
    try
    {
        if (overlay.Staged()||!CampaignAccess::GameReady()||!campaign||!slot||!cell.IsAttached()||
            !(cell.cellFlags&1)||cell.worldspace||cell.IsTemporary()||HomeInstance::ProjectionForm(cell.formID)) return false;
        const auto bytes=CampaignAccess::HomeBaseline(campaign,slot);
        const auto state=bytes?HomeSnapshot::Decode(*bytes):std::nullopt;
        const auto objects=CampaignAccess::HomeObjectView(campaign,slot);
        const auto records=CampaignAccess::HomeResidentView(campaign,slot);
        if (!state||!objects||!records||state->Omitted||!state->Legacy.empty()||
            !HomeNativeCapabilities::Covers(HomeNativeCapabilities::Residents(root),*state,*records,
                HomeNativeCapabilities::Movable().get())) return false;
        cellId=cell.formID;
        std::map<uint32_t,Delta> rows;
        for (const auto& r:state->Refs)
        {
            Delta d;d.Ref=Local(r.Key);d.Base=Local(r.Base);d.Type=r.Type;d.Scale=r.Scale;
            if (!d.Ref||!d.Base||(r.Flags&2)||!Resolve(d)) return false;
            d.Loose=r.Inventory==3;
            d.Guest.Disabled=(r.Flags&1)!=0;d.Guest.Pose=r.Transform;
            d.Guest.LockState=r.LockState;d.Guest.LockLevel=r.LockLevel;
            const auto found=objects->find(r.Key.Encode());
            if (found!=objects->end()) d.Guest.Open=found->second.Value.Open;
            const auto personal=Observe(world,d);
            if (!personal) return false;
            d.Personal=*personal;
            // Reversible SetOpen only for owner-confirmed plain doors.
            if (found!=objects->end()&&found->second.Value.GuestDoor)
            {
                auto* object=Resolve(d);
                const auto parent=object?Parent(*object):std::nullopt;
                const auto children=object?Children(*object):std::nullopt;
                d.LiveDoor=d.Type==29&&object&&CanChange(*object,d)&&parent&&!*parent&&children&&children->empty()&&
                    !d.Personal.Disabled&&!d.Guest.Disabled&&
                    (d.Personal.Open==1||d.Personal.Open==3)&&(d.Guest.Open==1||d.Guest.Open==3)&&
                    world.ctx().at<PapyrusService>().Get("ObjectReference","SetOpen");
                if (!d.LiveDoor) return false;
            }
            if (bool(d.Personal.LockState)!=bool(d.Guest.LockState)||
                (!d.LiveDoor&&d.Personal.Open!=d.Guest.Open)||
                d.Personal.Open==2||d.Personal.Open==4) return false;
            if (d.Guest.Disabled) d.Guest.Pose=d.Personal.Pose;
            if (!d.Personal.Same(d.Guest))
            {
                auto* object=Resolve(d);
                if (!object||!CanChange(*object,d)) return false;
            }
            if (!rows.emplace(d.Ref,std::move(d)).second) return false;
        }
        // Temporarily hide eligible personal residents using the same transaction.
        if (!cell.refData.refArray || !cell.refData.capacity || cell.refData.capacity>131072) return false;
        std::map<uint32_t,uint32_t> residentBases;
        for (const auto& [key,record]:*records)
        {
            const auto ref=Local(record.Value.Reference),base=Local(record.Value.Base);
            if (!ref||!base||!residentBases.emplace(ref,base).second) return false;
        }
        for (uint32_t i=0;i<cell.refData.capacity;++i)
        {
            auto* ref=cell.refData.refArray[i].Get();
            if (!ref || ref->formID==0x14) continue;
            if (auto* actor=Cast<Actor>(ref))
            {
                if (actor->IsTemporary()&&actor->GetExtension()&&actor->GetExtension()->IsRemote()&&
                    actor->GetExtension()->IsPlayer()) continue;
                const auto found=residentBases.find(actor->formID);
                const auto base=actor->baseForm?actor->baseForm->formID:0u;
                if (!base||!HomeInstance::NativeResidentBinding(actor->formID,base)||
                    (found!=residentBases.end()&&found->second!=base)) return false;
                // Guest's own eligible arrival may not exist in the owner's ledger.
                // Preserve its personal visibility without registering it there.
                Delta d;d.Ref=actor->formID;d.Base=base;d.Type=43;d.Scale=actor->scale;
                auto* object=Resolve(d);
                const auto personal=Observe(world,d);
                if (!object||!personal||!CanChange(*object,d)) return false;
                d.Personal=*personal;d.Guest=*personal;d.Guest.Disabled=true;
                if (!rows.emplace(d.Ref,std::move(d)).second) return false;
                continue;
            }
            if (!rows.count(ref->formID)) return false;
        }
        const auto toggles=[](const Delta& d){return d.Personal.Disabled!=d.Guest.Disabled;};
        // Only toggle the root of a closed enable-state group in this CELL.
        std::map<uint32_t,uint32_t> parents;
        for (auto& [id,d]:rows)
        {
            if (!toggles(d)) continue;
            auto* object=Resolve(d);
            if (!object) return false;
            const auto parent=Parent(*object);
            const auto children=Children(*object);
            if (!parent||!children) return false;
            if (*parent)
            {
                const auto p=rows.find(*parent);
                if (p==rows.end()||!toggles(p->second)) return false;
                auto* root=Resolve(p->second);
                const auto siblings=root?Children(*root):std::nullopt;
                if (!siblings||!siblings->count(id)) return false;
                d.ParentDriven=true;parents.emplace(id,*parent);
            }
            for (auto child:*children)
            {
                const auto c=rows.find(child);
                if (c==rows.end()||!toggles(c->second)) return false;
                auto* ref=Resolve(c->second);
                if (!ref||Parent(*ref)!=std::optional<uint32_t>(id)) return false;
            }
        }
        for (const auto& [child,parent]:parents)
        {
            std::set<uint32_t> chain{child};
            auto node=parent;
            while (parents.count(node))
            {if (!chain.insert(node).second) return false;node=parents.at(node);}
        }
        auto& vm=world.ctx().at<PapyrusService>();
        std::vector<Delta> changes;
        for (auto& [id,d]:rows)
        {
            (void)id;
            if (d.Loose)
            {
                auto* object=Resolve(d);
                const auto parent=object?Parent(*object):std::nullopt;
                const auto children=object?Children(*object):std::nullopt;
                d.LiveLoose=object&&CanChange(*object,d)&&parent&&!*parent&&children&&children->empty()&&
                    vm.Get("ObjectReference","Enable")&&vm.Get("ObjectReference","Disable");
            }
            if (d.Personal.Same(d.Guest)&&!d.Loose&&!d.LiveDoor) continue;
            if (!std::equal(d.Personal.Pose.begin(),d.Personal.Pose.begin()+3,d.Guest.Pose.begin())&&
                !vm.Get("ObjectReference","SetPosition")) return false;
            if (toggles(d)&&!d.ParentDriven&&(!vm.Get("ObjectReference","Enable")||
                !vm.Get("ObjectReference","Disable"))) return false;
            changes.push_back(std::move(d));
        }
        return overlay.Stage(std::move(changes));
    }
    catch (...) {return false;}
}
Result Switch(World& world,bool guest) noexcept
{
    try
    {
        if (!CampaignAccess::GameReady()) return Result::Pending;
        return overlay.Switch(guest,[&](const Delta& d){return Observe(world,d);},
            [&](const Delta& d,const Fields& f){return Write(world,d,f);});
    }
    catch (...) {return Result::Unavailable;}
}
Result RefreshLoose(World& world,uint64_t campaign,uint32_t slot) noexcept
{
    try
    {
        if (!CampaignAccess::GameReady()) return Result::Pending;
        const auto bytes=CampaignAccess::HomeBaseline(campaign,slot);
        const auto state=bytes?HomeSnapshot::Decode(*bytes):std::nullopt;
        if (!state) return Result::Unavailable;
        std::vector<std::pair<uint32_t,bool>> balances;
        for (const auto& row:state->Refs)
            if (row.Inventory==3)
            {
                const auto ref=Local(row.Key);
                if (!ref) return Result::Unavailable;
                balances.emplace_back(ref,(row.Flags&1u)!=0||row.Items.empty());
            }
        return overlay.RefreshHidden(balances,[&](const Delta& row){return Observe(world,row);},
            [&](const Delta& row,const Fields& f){return Write(world,row,f);});
    }
    catch (...) {return Result::Unavailable;}
}
Result RefreshDoors(World& world,uint64_t campaign,uint32_t slot) noexcept
{
    try
    {
        const auto objects=CampaignAccess::HomeObjectView(campaign,slot);
        if (!objects) return Result::Unavailable;
        std::vector<std::pair<uint32_t,uint32_t>> states;
        for (const auto& [key,record]:*objects)
        {
            if (!record.Value.GuestDoor) continue;
            const auto form=TradeProperties::DecodeForm(key);
            const auto ref=form?Local(*form):0;
            if (!ref) return Result::Unavailable;
            states.emplace_back(ref,record.Value.Open);
        }
        return overlay.RefreshDoors(states,[&](const Delta& row){return Observe(world,row);},
            [&](const Delta& row,const Fields& f){return Write(world,row,f);});
    }
    catch (...) {return Result::Unavailable;}
}
uint32_t DoorState(uint32_t ref) noexcept {return overlay.DoorState(ref);}
bool CanTake(uint32_t ref) noexcept {return overlay.CanTake(ref);}
bool Dirty() noexcept {return overlay.Dirty();}
bool Release() noexcept
{
    if (!overlay.Release()) return false;
    cellId=0;return true;
}
void ForgetReplacedWorld() noexcept
{
    overlay.ForgetReplacedWorld();cellId=0;HomeInstance::ForgetOriginalVisit();
}
}
