#include <TiltedOnlinePCH.h>
#include <World.h>
#include <Services/OverlayService.h>
#include <Services/OverlayClient.h>
#include <Services/PapyrusService.h>
#include <Services/CampaignAccess.h>
#include <Services/NativeTrade.h>
#include <Games/Overrides.h>
#include <VersionDb.h>
#include <atomic>
#include <cstdlib>
#include <cmath>
#include <TradeLedger.h>
#include <Services/PartyService.h>
#include <OverlayApp.hpp>
#include <include/cef_parser.h>
#include <Messages/RequestPartyTrade.h>
#include <Messages/NotifyPartyTrade.h>
#include <PlayerCharacter.h>
#include <Forms/TESGlobal.h>
#include <Games/TES.h>
#include <ExtraData/ExtraContainerChanges.h>
#include <Services/HomeInventory.h>
#include <Services/HomeProjection.h>
#include <Services/HomeInstance.h>
#include <Services/HomeActorProtection.h>
#include <Services/HomePlacementClient.h>
#include <Services/HomeGuestSave.h>
#include <mutex>
#include <Forms/TESObjectCELL.h>
#include <ExtraData/ExtraCount.h>
#include <ExtraData/ExtraHealth.h>
#include <ExtraData/ExtraCharge.h>
#include <ExtraData/ExtraSoul.h>
#include <ExtraData/ExtraEnchantment.h>
#include <ExtraData/ExtraPoison.h>
#include <ExtraData/ExtraTextDisplayData.h>
#include <Components/TESContainer.h>
#include <Forms/EnchantmentItem.h>
#include <Forms/AlchemyItem.h>
#include <InventoryEvidence.h>
#include <Games/Memory.h>
#include <Games/BGSCreatedObjectManager.h>
#include <Magic/EffectItem.h>
#include <Games/ActorExtension.h>
#include <Utils.h>
#include <Interface/UI.h>
#include <Interface/IMenu.h>
#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <set>

namespace
{
using Clock=std::chrono::steady_clock;
using namespace std::chrono_literals;
struct Wallet
{
    TESGlobal* fields[4]{};
    const void* setter{};
    bool Load(World& world)
    {
        auto* plugin=ModManager::Get()->GetByName("SkyrimCoopTrade.esp");
        if (!plugin || !plugin->IsLoaded()) return false;
        for (unsigned i=0;i<4;++i)
        {
            auto* f=TESForm::GetById(plugin->GetFormId(0x800+i));
            if (!f || static_cast<unsigned>(f->formType)!=9) return false;
            fields[i]=static_cast<TESGlobal*>(f);
            const auto value=fields[i]->f;
            const auto limit=i==2?1048575.f:i==3?16777214.f:4194303.f;
            if (!std::isfinite(value) || value<0 || value>limit || std::floor(value)!=value) return false;
        }
        if (!Stream() && Cursor()) return false;
        setter=world.ctx().at<PapyrusService>().Get("GlobalVariable","SetValue");
        return setter!=nullptr;
    }
    uint64_t Stream() const {return uint64_t(fields[0]->f) | (uint64_t(fields[1]->f)<<22) | (uint64_t(fields[2]->f)<<44);}
    uint32_t Cursor() const {return static_cast<uint32_t>(fields[3]->f);}
    bool Write(uint64_t stream, uint32_t cursor)
    {
        PapyrusFunction<void,TESGlobal,float> set(setter);
        if (!Stream()) for (unsigned i=0;i<3;++i) set(fields[i],float((stream>>(22*i))&0x3FFFFF));
        set(fields[3],float(cursor));
        return Stream()==stream && Cursor()==cursor;
    }
};
uint32_t session{},peer{};
bool tradeOpen{}, ownsUI{}, dirty{}, receiptFailure{}, walletReady{};
bool receiptFailureCampaign{};
uint64_t readyStream{};
uint32_t readyCursor{};
uint32_t lastConflict=UINT32_MAX;
std::string partner;
Clock::time_point pollAt{};
using LocalItem=std::pair<uint32_t,std::string>;
using LocalStock=std::map<LocalItem,int>;
LocalStock buffer,incomingStock;
uint32_t stockRemaining{};
bool stockComplete{}, nativeRequested{}, nativeOpened{};
bool shadowCleanupPending=true;
Clock::time_point cleanupRetry{};
Clock::time_point nativeDeadline{};
struct NativeIntent { LocalItem Item; uint32_t Count; bool Take; };
std::deque<NativeIntent> nativeIntents;
bool nativeAwaiting{}, takeAllRegistered{};
Clock::time_point nativeReceiptDeadline{};
void ResetNativeIntents()
{
    nativeIntents.clear();
    nativeAwaiting=false;
    nativeReceiptDeadline={};
}
// Only a local scratch reference is passed to ContainerMenu; it is never a
// real escrow or a peer's actor. IDs are looked up anew, pointers are not retained.
std::atomic_uint32_t proxyId{}, nativeToken{};
thread_local bool shadowMutation{};
struct ShadowWrite
{
    ScopedInventoryOverride suppressNetwork;
    ShadowWrite(){shadowMutation=true;}
    ~ShadowWrite(){shadowMutation=false;}
};
bool NativeEnabled()
{
    // Experimental during Interaction07-dev until actual Skyrim/SkyUI acceptance.
    const auto* env=std::getenv("STM_NATIVE_TRADE");
    return env && std::string_view(env)=="1";
}
template<class T> T* Address(uint64_t id)
{
    return reinterpret_cast<T*>(VersionDb::Get().FindAddressById(id));
}
TESObjectREFR* Proxy()
{
    auto* manager=ModManager::Get();
    auto* mod=manager?manager->GetByName("SkyrimCoopTrade.esp"):nullptr;
    if (!mod || !mod->IsLoaded()) return nullptr;
    const auto id=mod->GetFormId(0x812);
    proxyId.store(id);
    auto* object=Cast<TESObjectREFR>(TESForm::GetById(id));
    if (!object || object->IsDeleted() || !object->baseForm ||
        object->baseForm->formID!=mod->GetFormId(0x810)) return nullptr;
    return object;
}
bool NativeMenuOpen()
{
    if (VersionDb::Get().GetLoadedVersionString()!="1.6.1170.0") return false;
    auto* ui=UI::Get();
    if (!ui || !ui->GetMenuOpen("ContainerMenu")) return false;
    auto* target=Address<uint32_t>(405962); // RE::ContainerMenu::GetTargetRefHandle, AE
    auto* object=Proxy();
    return target && object &&
        *target==object->GetHandle().handle.iBits && *target;
}
void NativeHide()
{
    if (!NativeMenuOpen()) return;
    auto** singleton=Address<void*>(400445);
    using AddMessage=void(void*,const BSFixedString&,uint32_t,void*);
    auto* add=Address<AddMessage>(13631);
    if (singleton && *singleton && add) add(*singleton,BSFixedString("ContainerMenu"),3,nullptr);
}
// The normal Scaleform callback uses FxDelegateArgs, including for SkyUI.
// Interpose registration rather than guessing which low-level inventory
// calls the engine's bulk operation will use.
using Processor=FxDelegateHandler::CallbackProcessor;
using Callback=FxDelegateHandler::CallbackFn;
using AcceptFn=void(IMenu*,Processor*);
AcceptFn* previousContainerAccept{};
Callback* previousTakeAll{};
void** guardedContainerTable{};

void TakeAll(const FxDelegateArgs* args)
{
    const auto token=nativeToken.load();
    if (!token && !NativeMenuOpen())
    {
        if (previousTakeAll) previousTakeAll(args);
        return;
    }
    if (!token || !World::Exists() || !args || args->GetArgCount()!=0) return;
    World::Get().GetRunner().Queue([token]()
    {
        auto& world=World::Get();
        if (nativeToken.load()!=token || !world.GetTransport().IsOnline() ||
            !CampaignAccess::GameReady() || !NativeMenuOpen()) return;
        if (!stockComplete || dirty || nativeAwaiting || !nativeIntents.empty())
        {
            Utils::ShowHudMessage("Обмен обновляется. Повторите после завершения операции.");
            return;
        }
        // Capture only the displayed stock, not later deposits.
        for (const auto& [item,count]:buffer)
            if (count>0) nativeIntents.push_back({item,static_cast<uint32_t>(count),true});
    });
}
class TradeCallbackProcessor final : public Processor
{
public:
    explicit TradeCallbackProcessor(Processor& original):original(original){}
    void Process(const void* method,Callback* fn) override
    {
        // CommonLibSSE-NG GString: tagged DataDesc pointer at offset 0;
        // clear heap flags (2 bits), DataDesc::data at 0xC.
        uintptr_t desc{};
        if (method) std::memcpy(&desc,method,sizeof(desc));
        const auto* name=(desc&~uintptr_t(3)) ?
            reinterpret_cast<const char*>((desc&~uintptr_t(3))+0xC) : nullptr;
        if (name && std::strcmp(name,"TakeAllItems")==0 && fn && fn!=&TakeAll)
        {
            previousTakeAll=fn;
            original.Process(method,&TakeAll);
            if (nativeToken.load()) takeAllRegistered=true;
            return;
        }
        original.Process(method,fn);
    }
private:
    Processor& original;
};
void ContainerAccept(IMenu* menu,Processor* processor)
{
    if (!processor) {previousContainerAccept(menu,processor);return;}
    TradeCallbackProcessor wrapper(*processor);
    previousContainerAccept(menu,&wrapper);
}
bool GuardTakeAll()
{
    // VTABLE_ContainerMenu[0], AE 215061; FxDelegateHandler::Accept slot 1.
    auto** table=Address<void*>(215061);
    if (!table) return false;
    if (guardedContainerTable)
        return table==guardedContainerTable && table[1]==reinterpret_cast<void*>(&ContainerAccept);
    if (!table[1]) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(table[1],&info,sizeof(info)) || info.State!=MEM_COMMIT ||
        (info.Protect&(PAGE_GUARD|PAGE_NOACCESS))) return false;
    const auto prot=info.Protect&0xff;
    if (prot!=PAGE_EXECUTE && prot!=PAGE_EXECUTE_READ && prot!=PAGE_EXECUTE_READWRITE &&
        prot!=PAGE_EXECUTE_WRITECOPY) return false;
    DWORD prior{};
    if (!VirtualProtect(table+1,sizeof(void*),PAGE_READWRITE,&prior)) return false;
    previousContainerAccept=reinterpret_cast<AcceptFn*>(table[1]);
    InterlockedExchangePointer(table+1,reinterpret_cast<void*>(&ContainerAccept));
    guardedContainerTable=table;
    DWORD unused{};
    VirtualProtect(table+1,sizeof(void*),prior,&unused);
    return true;
}
using RemoveFn=BSPointerHandle<TESObjectREFR>*(TESObjectREFR*,BSPointerHandle<TESObjectREFR>*,
    TESBoundObject*,int32_t,ITEM_REMOVE_REASON,ExtraDataList*,TESObjectREFR*,const NiPoint3*,const NiPoint3*);
using AddFn=void(TESObjectREFR*,TESBoundObject*,ExtraDataList*,int32_t,TESObjectREFR*);
RemoveFn* previousPlayerRemove{};
AddFn* previousPlayerAdd{};
void** guardedPlayerTable{};
BSPointerHandle<TESObjectREFR>* GuardPlayerRemove(TESObjectREFR* from,BSPointerHandle<TESObjectREFR>* result,
    TESBoundObject* item,int32_t count,ITEM_REMOVE_REASON reason,ExtraDataList* extra,TESObjectREFR* to,
    const NiPoint3* drop,const NiPoint3* rotate)
{
    if ((!to && reason==ITEM_REMOVE_REASON::kDropping &&
         HomePlacementClient::InterceptDrop(from,item,extra,count,drop,rotate)) ||
        NativeTrade::BlockRemove(from,to,item,count,extra))
    {
        if (result) result->handle.iBits=0;
        return result;
    }
    return previousPlayerRemove(from,result,item,count,reason,extra,to,drop,rotate);
}
void GuardPlayerAdd(TESObjectREFR* to,TESBoundObject* item,ExtraDataList* extra,int32_t count,TESObjectREFR* from)
{
    if (NativeTrade::BlockAdd(to,from)) return;
    previousPlayerAdd(to,item,extra,count,from);
}
bool GuardPlayer(PlayerCharacter* player)
{
    // Actor overrides both TESObjectREFR virtuals. The existing reference
    // detours alone do not guarantee that the deposit path is intercepted.
    if (!player) return false;
    auto** table=*reinterpret_cast<void***>(player);
    if (!table) return false;
    if (guardedPlayerTable)
        return table==guardedPlayerTable && table[0x56]==reinterpret_cast<void*>(&GuardPlayerRemove) &&
            table[0x5A]==reinterpret_cast<void*>(&GuardPlayerAdd);
    if (!table[0x56] || !table[0x5A]) return false;
    DWORD prior{};
    if (!VirtualProtect(table+0x56,5*sizeof(void*),PAGE_READWRITE,&prior)) return false;
    previousPlayerRemove=reinterpret_cast<RemoveFn*>(table[0x56]);
    previousPlayerAdd=reinterpret_cast<AddFn*>(table[0x5A]);
    InterlockedExchangePointer(table+0x56,reinterpret_cast<void*>(&GuardPlayerRemove));
    InterlockedExchangePointer(table+0x5A,reinterpret_cast<void*>(&GuardPlayerAdd));
    guardedPlayerTable=table;
    DWORD unused{};
    VirtualProtect(table+0x56,5*sizeof(void*),prior,&unused);
    return true;
}
// Use the same static-form resolution as the existing diagnostic code,
// but no diagnostic sampling or network mod ID is required.
std::optional<TradeProperties::FormKey> PropertyKey(uint32_t form)
{
    std::vector<InventoryEvidence::PluginSlot> slots;
    if (auto* manager=ModManager::Get())
        for (const auto* mod:manager->mods)
        {
            if (!mod || !mod->IsLoaded()) continue;
            const auto* end=std::find(mod->filename,mod->filename+sizeof(mod->filename),'\0');
            if (end==mod->filename+sizeof(mod->filename)) continue;
            slots.push_back({mod->GetId(),mod->IsLite(),std::string(mod->filename,end)});
        }
    auto key=InventoryEvidence::Resolve(form,slots);
    if (!key) return {};
    TradeProperties::FormKey result{key->Plugin,key->Local};
    if (!result.Valid()) return {};
    return result;
}
TESForm* ResolvePropertyForm(const TradeProperties::FormKey& key)
{
    auto* manager=ModManager::Get();
    auto* mod=manager?manager->GetByName(key.Plugin.c_str()):nullptr;
    if (!key.Valid() || !mod || !mod->IsLoaded() || (mod->IsLite()&&key.Local>0xFFF)) return nullptr;
    auto id=mod->GetFormId(key.Local);
    auto resolved=PropertyKey(id);
    if (!resolved || resolved->Plugin!=key.Plugin || resolved->Local!=key.Local) return nullptr;
    return TESForm::GetById(id);
}
template<class T> T* PropertyForm(const TradeProperties::FormKey& key)
{
    return Cast<T>(ResolvePropertyForm(key));
}
// CommonLibSSE-NG ExtraOwnership: type 0x21, owner at 0x10, size 0x18.
// A static NPC base or faction, never a reference or a connection ID.
struct TradeOwnership : BSExtraData
{
    TESForm* Owner{};
};
static_assert(sizeof(TradeOwnership)==0x18);
constexpr auto OwnershipType=static_cast<ExtraDataType>(0x21);
bool SupportedOwner(const TESForm* owner)
{
    return owner && owner->formID!=7 && !owner->IsTemporary() && !owner->IsDeleted() &&
        (owner->formType==FormType::Npc || owner->formType==static_cast<FormType>(0x0B));
}
TESForm* PropertyOwner(const TradeProperties::FormKey& key)
{
    auto* owner=ResolvePropertyForm(key);
    return SupportedOwner(owner)?owner:nullptr;
}
// SKSE v2.2.8 GameExtraData.cpp: ExtraOwnership vtable, Steam 1.6.1170.
void** ResolveOwnershipTable()
{
    if (VersionDb::Get().GetLoadedVersionString()!="1.6.1170.0") return nullptr;
    auto module=GetModuleHandleW(nullptr);
    if (!module) return nullptr;
    auto** table=reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(module)+0x01770830);
    const auto check=[module](const void* addr,bool executable,size_t bytes)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(addr,&info,sizeof(info)) || info.AllocationBase!=module ||
            info.State!=MEM_COMMIT || (info.Protect&(PAGE_GUARD|PAGE_NOACCESS))) return false;
        const auto start=reinterpret_cast<uintptr_t>(addr);
        const auto end=reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize;
        if (start>end || bytes>end-start) return false;
        const auto prot=info.Protect&0xff;
        if (executable) return prot==PAGE_EXECUTE || prot==PAGE_EXECUTE_READ ||
            prot==PAGE_EXECUTE_READWRITE || prot==PAGE_EXECUTE_WRITECOPY;
        return prot==PAGE_READONLY || prot==PAGE_READWRITE || prot==PAGE_WRITECOPY ||
            prot==PAGE_EXECUTE_READ || prot==PAGE_EXECUTE_READWRITE || prot==PAGE_EXECUTE_WRITECOPY;
    };
    return check(table,false,3*sizeof(void*)) && check(table[0],true,1) &&
        check(table[1],true,1) && check(table[2],true,1)?table:nullptr;
}
bool SetTradeOwner(ExtraDataList& list,const TradeProperties::FormKey& key)
{
    auto* owner=PropertyOwner(key);
    auto** table=ResolveOwnershipTable();
    if (!owner || !table || list.Contains(OwnershipType)) return false;
    auto* data=Memory::Allocate<TradeOwnership>();
    if (!data) return false;
    std::memset(static_cast<void*>(data),0,sizeof(*data));
    *reinterpret_cast<void***>(data)=table;
    data->Owner=owner;
    if (!list.Add(OwnershipType,data))
    {
        Memory::Free(data);return false;
    }
    return true;
}
// Base COED may contain ownership, rank/global and health. Do not infer
// a clean implicit stack from a base entry with unrepresented metadata.
bool UnambiguousBase(const TESObjectREFR& object,uint32_t id)
{
    if (const auto* base=object.GetContainer())
    {
        if (base->count && !base->entries) return false;
        for (uint32_t i=0;i<base->count;++i)
        {
            const auto* entry=base->entries[i];
            if (entry && entry->form && entry->form->formID==id && entry->data) return false;
        }
    }
    return true;
}
bool UnownedProxy(const TESObjectREFR& object)
{
    // CommonLibSSE-NG TESObjectREFR::GetOwner, AE 20194.
    if (VersionDb::Get().GetLoadedVersionString()!="1.6.1170.0") return false;
    using GetOwner=TESForm*(const TESObjectREFR*);
    auto* owner=Address<GetOwner>(20194);
    return owner && !owner(&object);
}
// SKSE v2.2.8 GameExtraData.cpp/GameExtraData.h and GameBSExtraData.cpp:
// Steam 1.6.1170, ExtraTextDisplayData vtable RVA and native SetName.
// Do not use the obsolete absolute 0x1416244D0 from upstream STR.
struct NameApi
{
    void** Table{};
    using Setter=void(ExtraTextDisplayData*,const char*);
    Setter* Set{};
};
std::optional<NameApi> ResolveNameApi()
{
    if (VersionDb::Get().GetLoadedVersionString()!="1.6.1170.0") return {};
    const auto module=GetModuleHandleW(nullptr);
    if (!module) return {};
    auto base=reinterpret_cast<uintptr_t>(module);
    auto** table=reinterpret_cast<void**>(base+0x01771390);
    auto* setter=reinterpret_cast<NameApi::Setter*>(base+0x00184230);
    const auto committed=[module](const void* addr,bool executable,size_t bytes)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(addr,&info,sizeof(info)) || info.AllocationBase!=module ||
            info.State!=MEM_COMMIT || (info.Protect&(PAGE_GUARD|PAGE_NOACCESS))) return false;
        const auto start=reinterpret_cast<uintptr_t>(addr);
        const auto end=reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize;
        if (start>end || bytes>end-start) return false;
        const auto prot=info.Protect&0xff;
        if (executable) return prot==PAGE_EXECUTE || prot==PAGE_EXECUTE_READ ||
            prot==PAGE_EXECUTE_READWRITE || prot==PAGE_EXECUTE_WRITECOPY;
        return prot==PAGE_READONLY || prot==PAGE_READWRITE || prot==PAGE_WRITECOPY ||
            prot==PAGE_EXECUTE_READ || prot==PAGE_EXECUTE_READWRITE || prot==PAGE_EXECUTE_WRITECOPY;
    };
    if (!committed(table,false,2*sizeof(void*)) ||
        !committed(table[0],true,1) || !committed(table[1],true,1) ||
        !committed(reinterpret_cast<void*>(setter),true,1)) return {};
    return NameApi{table,setter};
}
std::optional<std::string> ReadCustomName(const ExtraTextDisplayData& name)
{
    const auto api=ResolveNameApi();
    if (!api || *reinterpret_cast<void* const* const*>(&name)!=api->Table ||
        name.iOwnerInstance!=-2 || name.pDisplayNameText || name.pOwnerQuest ||
        !name.usCustomNameLength || name.usCustomNameLength>TradeProperties::MaxNameBytes ||
        !name.DisplayName.data) return {};
    // CommonLib: customNameLength excludes the generated temper suffix.
    std::string value;
    for (size_t i=0;i<name.usCustomNameLength;++i)
    {
        const char ch=name.DisplayName.data[i];
        if (!ch) return {};
        value+=ch;
    }
    if (!TradeProperties::ValidName(value)) return {};
    return value;
}
bool SetCustomName(ExtraDataList& list,const std::string& name)
{
    auto api=ResolveNameApi();
    if (!api || !TradeProperties::ValidName(name) ||
        list.Contains(ExtraDataType::TextDisplayData)) return false;
    auto* data=Memory::Allocate<ExtraTextDisplayData>();
    if (!data) return false;
    // Equivalent to SKSE BSExtraData::Create: game heap, zeroed storage,
    // engine vtable, default owner and temper factor, then native setter.
    std::memset(static_cast<void*>(data),0,sizeof(*data));
    *reinterpret_cast<void***>(data)=api->Table;
    data->iOwnerInstance=-1;
    data->fTemperFactor=1.f;
    api->Set(data,name.c_str());
    const auto actual=ReadCustomName(*data);
    if (!actual || *actual!=name || !list.Add(ExtraDataType::TextDisplayData,data))
    {
        if (data->DisplayName.data) data->DisplayName.Release();
        Memory::Free(data);
        return false;
    }
    return true;
}
std::optional<TradeProperties::CreatedEnchant> DescribeCreated(const EnchantmentItem& item)
{
    if ((item.formID>>24)!=0xFF) return {};
    TradeProperties::CreatedEnchant out;
    // The two existing STR constructors distinguish offensive and defensive.
    if (item.eCastingType==MagicSystem::CONSTANT_EFFECT && item.eDelivery==MagicSystem::SELF)
        out.Weapon=false;
    else if (item.eCastingType==MagicSystem::FIRE_AND_FORGET && item.eDelivery==MagicSystem::TOUCH)
        out.Weapon=true;
    else return {};
    out.Metadata={static_cast<uint32_t>(item.iCostOverride),static_cast<uint32_t>(item.iFlags),
        static_cast<uint32_t>(item.eCastingType),static_cast<uint32_t>(item.iChargeOverride),
        static_cast<uint32_t>(item.eDelivery),static_cast<uint32_t>(item.eSpellType),
        std::bit_cast<uint32_t>(item.fChargeTime)};
    if (item.pBaseEnchantment)
    {
        auto key=PropertyKey(item.pBaseEnchantment->formID);
        if (!key) return {};
        out.Base=*key;
    }
    if (item.pWornRestrictions)
    {
        auto key=PropertyKey(item.pWornRestrictions->formID);
        if (!key) return {};
        out.Restrictions=*key;
    }
    if (item.fullName.value.data)
    {
        const auto* text=item.fullName.value.data;
        for (size_t i=0;text[i];++i)
        {
            if (i>=TradeProperties::MaxNameBytes) return {};
            out.Name+=text[i];
        }
    }
    for (const auto* effect:item.listOfEffects)
    {
        // STR's constructor cannot preserve instance-level TESCondition.
        // Conditions on the static EffectSetting itself remain in its plugin.
        if (!effect || !effect->pEffectSetting || effect->Condition.pHead ||
            out.Effects.size()>=8 || effect->data.iArea<0 || effect->data.iDuration<0) return {};
        auto key=PropertyKey(effect->pEffectSetting->formID);
        if (!key) return {};
        out.Effects.push_back({*key,std::bit_cast<uint32_t>(effect->data.fMagnitude),
            static_cast<uint32_t>(effect->data.iArea),static_cast<uint32_t>(effect->data.iDuration),
            std::bit_cast<uint32_t>(effect->fRawCost)});
    }
    if (!out.Valid() || out.Encode().size()>3000) return {};
    return out;
}
bool CreatedAvailable(const TradeProperties::CreatedEnchant& value)
{
    if ((!value.Base.Empty()&&!PropertyForm<EnchantmentItem>(value.Base)) ||
        (!value.Restrictions.Empty()&&!PropertyForm<BGSListForm>(value.Restrictions))) return false;
    for (const auto& e:value.Effects)
        if (!PropertyForm<EffectSetting>(e.Setting)) return false;
    return true;
}
EnchantmentItem* RecreateEnchant(const TradeProperties::CreatedEnchant& value)
{
    if (!value.Valid() || !CreatedAvailable(value) || !BGSCreatedObjectManager::Get()) return nullptr;
    Inventory::EnchantmentData data;
    data.IsWeapon=value.Weapon;
    auto& mods=World::Get().GetModSystem();
    for (const auto& e:value.Effects)
    {
        auto* setting=PropertyForm<EffectSetting>(e.Setting);
        Inventory::EffectItem effect;
        if (!setting || !mods.GetServerModId(setting->formID,effect.EffectId) ||
            mods.GetGameId(effect.EffectId)!=setting->formID) return nullptr;
        effect.Magnitude=std::bit_cast<float>(e.Magnitude);
        effect.Area=static_cast<int32_t>(e.Area);effect.Duration=static_cast<int32_t>(e.Duration);
        effect.RawCost=std::bit_cast<float>(e.Cost);
        data.Effects.push_back(effect);
    }
    auto* enchant=EnchantmentItem::Create(data);
    auto restored=enchant?DescribeCreated(*enchant):std::nullopt;
    // Never mutate shared/deduplicated engine enchantment forms to force a match.
    return restored && restored->Encode()==value.Encode()?enchant:nullptr;
}
bool PropertiesAvailable(const TradeProperties::Values& value)
{
    return (!(value.Mask&value.HasEnchant) || (value.Created?CreatedAvailable(*value.Created):
        PropertyForm<EnchantmentItem>(value.Enchant)!=nullptr)) &&
        (!(value.Mask&value.HasPoison) || PropertyForm<AlchemyItem>(value.Poison)) &&
        (!(value.Mask&value.HasName) || ResolveNameApi().has_value()) &&
        (!(value.Mask&value.HasOwnership) || (PropertyOwner(value.Owner) && ResolveOwnershipTable()));
}
struct ObservedExtra { std::string Variant; int Count=1; };
std::optional<ObservedExtra> ReadExtra(ExtraDataList* list)
{
    if (!list) return ObservedExtra{};
    TradeProperties::Values value;
    ObservedExtra out;
    bool valid=true,hasCount=false;
    list->lock.LockForRead();
    for (auto* data=list->data;data;data=data->next)
    {
        uint32_t bit=0;
        switch (data->GetType())
        {
        case ExtraDataType::Count:
            if (hasCount) valid=false;
            hasCount=true;out.Count=static_cast<ExtraCount*>(data)->count;
            break;
        case OwnershipType:
        {
            const auto* owner=static_cast<TradeOwnership*>(data)->Owner;
            if (!SupportedOwner(owner)) {valid=false;break;}
            auto key=PropertyKey(owner->formID);
            if (!key) {valid=false;break;}
            bit=value.HasOwnership;value.Owner=*key;
            break;
        }
        case ExtraDataType::Health:
            bit=value.HasHealth;value.Health=std::bit_cast<uint32_t>(static_cast<ExtraHealth*>(data)->fHealth);break;
        case ExtraDataType::Charge:
            bit=value.HasCharge;value.Charge=std::bit_cast<uint32_t>(static_cast<ExtraCharge*>(data)->fCharge);break;
        case ExtraDataType::Soul:
            bit=value.HasSoul;value.Soul=static_cast<uint32_t>(static_cast<ExtraSoul*>(data)->cSoul);break;
        case ExtraDataType::Enchantment:
        {
            auto* enchant=static_cast<ExtraEnchantment*>(data);
            if (!enchant->pEnchantment) {valid=false;break;}
            bit=value.HasEnchant;
            if ((enchant->pEnchantment->formID>>24)==0xFF)
            {
                auto created=DescribeCreated(*enchant->pEnchantment);
                if (!created || value.Created) {valid=false;break;}
                bit|=value.HasCreated;value.Created=std::move(*created);
            }
            else
            {
                auto key=PropertyKey(enchant->pEnchantment->formID);
                if (!key) {valid=false;break;}
                value.Enchant=*key;
            }
            value.EnchantCharge=enchant->usCharge;value.RemoveOnUnequip=enchant->bRemoveOnUnequip;
            break;
        }
        case ExtraDataType::Poison:
        {
            auto* poison=static_cast<ExtraPoison*>(data);
            auto key=poison->pPoison?PropertyKey(poison->pPoison->formID):std::nullopt;
            if (!key) {valid=false;break;}
            bit=value.HasPoison;value.Poison=*key;value.PoisonCount=poison->uiCount;
            break;
        }
        case ExtraDataType::TextDisplayData:
        {
            // Derived text is regenerated; quest/message-owned text is
            // never copied. Explicit names retain their exact original bytes.
            auto* name=static_cast<ExtraTextDisplayData*>(data);
            if (name->iOwnerInstance==-2)
            {
                auto custom=ReadCustomName(*name);
                if (!custom) {valid=false;break;}
                bit=value.HasName;value.Name=std::move(*custom);
            }
            else if (name->iOwnerInstance!=-1 || name->usCustomNameLength ||
                name->pDisplayNameText || name->pOwnerQuest) valid=false;
            break;
        }
        default:valid=false;break;
        }
        if (bit && (value.Mask&bit)) valid=false;
        value.Mask|=bit;
        if (!valid) break;
    }
    list->lock.UnlockRead();
    if (!valid || out.Count<=0 || !value.Valid()) return {};
    out.Variant=value.Encode();
    if (!TradeProperties::Decode(out.Variant)) return {};
    return out;
}
// Exact property groups are counted from live extra-data lists, not from
// Inventory::Entry::CanBeMerged (which can ignore individual properties).
struct VariantCount { int64_t Count{}; ExtraDataList* First{}; int FirstCount{}; };
std::optional<VariantCount> CountVariant(TESObjectREFR& object,const LocalItem& key)
{
    auto properties=TradeProperties::Decode(key.second);
    if (key.second.empty() || !properties || !PropertiesAvailable(*properties)) return {};
    auto* form=TESForm::GetById(key.first);
    if (!form || !UnambiguousBase(object,key.first)) return {};
    const auto total=object.GetItemCountInInventory(form);
    if (total<0 || total>INT32_MAX) return {};
    VariantCount result;
    const auto* changes=object.GetContainerChanges();
    if (!changes) return result;
    if (!changes->entries) return {};
    for (auto* entry:*changes->entries)
    {
        if (!entry || !entry->form || entry->form->formID!=key.first) continue;
        if (entry->IsQuestObject()) return {};
        if (!entry->dataList) continue;
        for (auto* list:*entry->dataList)
        {
            auto extra=ReadExtra(list);
            if (!extra || extra->Variant!=key.second) continue;
            result.Count+=extra->Count;
            if (result.Count>total) return {};
            if (!result.First) {result.First=list;result.FirstCount=extra->Count;}
        }
    }
    return result;
}
bool ChangeVariant(TESObjectREFR& object,const LocalItem& key,int delta)
{
    auto value=TradeProperties::Decode(key.second);
    auto before=CountVariant(object,key);
    auto* form=Cast<TESBoundObject>(TESForm::GetById(key.first));
    if (!value || !value->Mask || !before || !form || !delta ||
        before->Count+delta<0 || before->Count+delta>INT32_MAX) return false;
    const auto totalBefore=object.GetItemCountInInventory(form);
    if (totalBefore<0 || totalBefore+delta<0 || totalBefore+delta>INT32_MAX) return false;
    if (delta>0)
    {
        auto* enchant=(value->Mask&value->HasEnchant)?(value->Created?RecreateEnchant(*value->Created):
            PropertyForm<EnchantmentItem>(value->Enchant)):nullptr;
        auto* poison=(value->Mask&value->HasPoison)?PropertyForm<AlchemyItem>(value->Poison):nullptr;
        if (((value->Mask&value->HasEnchant)&&!enchant) || ((value->Mask&value->HasPoison)&&!poison)) return false;
        auto* list=ExtraDataList::New();
        if (!list) return false;
        if (value->Mask&value->HasHealth) list->SetHealth(std::bit_cast<float>(value->Health));
        if (value->Mask&value->HasCharge) list->SetChargeData(std::bit_cast<float>(value->Charge));
        if (value->Mask&value->HasSoul) list->SetSoulData(static_cast<SOUL_LEVEL>(value->Soul));
        if (enchant) list->SetEnchantmentData(enchant,static_cast<uint16_t>(value->EnchantCharge),value->RemoveOnUnequip!=0);
        if (poison) list->SetPoison(poison,value->PoisonCount);
        if ((value->Mask&value->HasName) && !SetCustomName(*list,value->Name)) return false;
        if ((value->Mask&value->HasOwnership) && !SetTradeOwner(*list,value->Owner)) return false;
        // Validate the generated representation before admitting it.
        auto generated=ReadExtra(list);
        if (!generated || generated->Variant!=key.second)
        {
            // Ownership of a newly allocated engine list is deliberately not
            // passed to the inventory when a setter cannot represent the value.
            spdlog::error("Trade: engine could not recreate item properties");
            return false;
        }
        object.AddObjectToContainer(form,list,delta,nullptr);
    }
    else
    {
        int remaining=-delta;
        while (remaining)
        {
            // Resolve again after each mutation. Never retain a list pointer
            // across an engine call or network round trip.
            auto current=CountVariant(object,key);
            if (!current || !current->First || current->Count<remaining) return false;
            const int chunk=std::min(remaining,current->FirstCount);
            object.RemoveItem(form,chunk,ITEM_REMOVE_REASON::kRemove,current->First,nullptr);
            auto after=CountVariant(object,key);
            if (!after || after->Count!=current->Count-chunk) return false;
            remaining-=chunk;
        }
    }
    auto after=CountVariant(object,key);
    return after && after->Count==before->Count+delta &&
        object.GetItemCountInInventory(form)==totalBefore+delta;
}

std::optional<std::map<uint32_t,int>> ShadowCounts(TESObjectREFR& object)
{
    std::map<uint32_t,int> result;
    auto* changes=object.GetContainerChanges();
    if (!changes) return result;
    if (!changes->entries) return {};
    for (const auto* entry:*changes->entries)
    {
        if (!entry || !entry->form || entry->count<0) return {};
        auto& count=result[entry->form->formID];
        if (entry->count>INT32_MAX-count) return {};
        count+=entry->count;
    }
    std::erase_if(result,[](const auto& row){return !row.second;});
    return result;
}
std::optional<LocalStock> ShadowVariants(TESObjectREFR& object)
{
    auto counts=ShadowCounts(object);
    if (!counts) return {};
    LocalStock result;
    for (const auto& [id,n]:*counts) result[{id,{}}]=n;
    const auto* changes=object.GetContainerChanges();
    if (changes && changes->entries)
        for (auto* entry:*changes->entries)
        {
            if (!entry || !entry->form || !entry->dataList) continue;
            for (auto* list:*entry->dataList)
            {
                auto extra=ReadExtra(list);
                if (!extra) return {};
                if (extra->Variant.empty()) continue;
                auto& plain=result[{entry->form->formID,{}}];
                if (plain<extra->Count) return {};
                plain-=extra->Count;
                result[{entry->form->formID,extra->Variant}]+=extra->Count;
            }
        }
    std::erase_if(result,[](const auto& row){return !row.second;});
    return result;
}
bool SetShadow(World&,const LocalStock& desired)
{
    auto* object=Proxy();
    if (!object || !UnownedProxy(*object)) return false;
    auto current=ShadowVariants(*object);
    if (!current) return false;
    if (*current==desired) return true;
    auto keys=*current;
    for (const auto& [key,count]:desired) keys.try_emplace(key,0);
    using Refresh=void(TESObjectREFR*,const TESBoundObject*);
    auto* refresh=Address<Refresh>(52849);
    if (!refresh) return false;
    // Remove variants before plain stacks; adding happens only after removals.
    // If a plain stack shares a base with a variant, rebuild the scratch chest
    // instead of using an ambiguous plain RemoveItem.
    {
        ShadowWrite guard;
        const auto totals=ShadowCounts(*object);
        if (!totals) return false;
        for (const auto& [id,count]:*totals)
        {
            auto* form=Cast<TESBoundObject>(TESForm::GetById(id));
            if (!form || count<=0) return false;
            object->RemoveItem(form,count,ITEM_REMOVE_REASON::kRemove,nullptr,nullptr);
        }
        auto empty=ShadowCounts(*object);
        if (!empty || !empty->empty()) return false;
        for (const auto& [key,count]:desired)
        {
            if (count<=0) return false;
            if (!key.second.empty())
            {
                if (!ChangeVariant(*object,key,count)) return false;
            }
            else
            {
                auto* form=Cast<TESBoundObject>(TESForm::GetById(key.first));
                if (!form) return false;
                object->AddObjectToContainer(form,nullptr,count,nullptr);
            }
        }
    }
    auto updated=ShadowVariants(*object);
    if (!updated || *updated!=desired) return false;
    if (NativeMenuOpen())
        for (const auto& [key,_]:keys)
            if (auto* form=Cast<TESBoundObject>(TESForm::GetById(key.first))) refresh(object,form);
    return true;
}
// The scratch copy is not authoritative. Clear it even without server mod IDs.
bool ClearShadow()
{
    auto* object=Proxy();
    if (!object) return false;
    auto current=ShadowCounts(*object);
    if (!current) return false;
    if (current->empty()) return true;
    std::vector<std::pair<TESBoundObject*,int>> items;
    for (const auto& [id,count]:*current)
    {
        auto* form=Cast<TESBoundObject>(TESForm::GetById(id));
        if (!form || count<=0) return false;
        items.emplace_back(form,count);
    }
    {
        ShadowWrite guard;
        for (const auto& [form,count]:items)
            object->RemoveItem(form,count,ITEM_REMOVE_REASON::kRemove,nullptr,nullptr);
    }
    auto remaining=ShadowCounts(*object);
    return remaining && remaining->empty();
}
void MaintainShadow()
{
    if (!shadowCleanupPending || nativeToken.load() || !CampaignAccess::GameReady()) return;
    const auto now=Clock::now();
    if (now<cleanupRetry) return;
    cleanupRetry=now+1s;
    if (NativeMenuOpen()) {NativeHide();return;}
    shadowCleanupPending=!ClearShadow();
}
bool OpenNative(World& world)
{
    if (VersionDb::Get().GetLoadedVersionString()!="1.6.1170.0") return false;
    auto* object=Proxy();
    auto* ui=UI::Get();
    using Open=void(const TESObjectREFR*,int32_t);
    auto* open=Address<Open>(51140); // RE::TESObjectREFR::OpenContainer, AE
    auto** queue=Address<void*>(400445);
    if (!object || !ui || !open || !queue || !*queue || !Address<void>(13631) ||
        !Address<uint32_t>(405962) || !Address<void>(52849) ||
        ui->GetMenuOpen("ContainerMenu") || ui->IsUnpausedMenuOpen() ||
        !GuardPlayer(PlayerCharacter::Get()) || !GuardTakeAll() || !ClearShadow() || !SetShadow(world,buffer)) return false;
    shadowCleanupPending=true;
    takeAllRegistered=false;
    nativeToken.store(session);
    open(object,0); // ContainerMode::kLoot, no actor inventory or pickpocket
    return true;
}

bool Send(PartyTrade e)
{
    auto& world=World::Get();
    if ((receiptFailure && e.Op!=TradeOp::Close) || !world.GetTransport().IsOnline() || !CampaignAccess::GameReady() || !CampaignAccess::TradeEnabled()) return false;
    using Mode=CampaignReplication::Link::Mode;
    if (CampaignAccess::Mode()!=Mode::Disabled)
    {
        const auto identity=CampaignAccess::Current(world);
        if (!identity || (e.Stream && e.Stream!=identity->Save.Campaign)) return false;
        e.Campaign=identity->Save.Campaign;e.Character=identity->Save.Character;
        e.SaveTag=identity->Save.Applied.Tag;e.SaveRevision=identity->Save.Applied.Revision;
        e.Profile=identity->Profile.c_str();e.Credential=identity->Credential.Secret.c_str();
    }
    RequestPartyTrade m; m.Event=std::move(e); return world.GetTransport().Send(m);
}
std::set<uint32_t> Unsuitable(PlayerCharacter& p)
{
    std::set<uint32_t> bad;
    auto* changes=p.GetContainerChanges();
    if (!changes || !changes->entries) return bad;
    for (auto* e:*changes->entries)
    {
        if (!e || !e->form) continue;
        if (e->IsQuestObject()) {bad.insert(e->form->formID);continue;}
        if (!e->dataList) continue;
        for (auto* list:*e->dataList)
        {
            if (!list) continue;
            list->lock.LockForRead();
            bool special=false;
            for (auto* x=list->data;x;x=x->next)
                if (x->GetType()!=ExtraDataType::Count) {special=true;break;}
            list->lock.UnlockRead();
            if (special) {bad.insert(e->form->formID);break;}
        }
    }
    return bad;
}
std::map<uint32_t,int> Pocket(World& world, PlayerCharacter& p)
{
    std::map<uint32_t,int> items;
    const auto bad=Unsuitable(p);
    for (const auto& e:p.GetInventory().Entries)
    {
        const auto id=world.GetModSystem().GetGameId(e.BaseId);
        if (id && (id>>24)!=0xFF && e.Count>0 && !e.ContainsExtraData() && !bad.count(id))
            items[id]+=e.Count;
    }
    return items;
}
bool Ready(const Wallet& w)
{
    return !receiptFailure && walletReady && !nativeAwaiting && readyCursor==w.Cursor() && (!w.Stream() || readyStream==w.Stream());
}
std::optional<int64_t> ReceiptCount(World& world,PlayerCharacter& player,const LocalItem& key,bool credit=false)
{
    if (!UnambiguousBase(player,key.first)) return {};
    if (!key.second.empty())
    {
        auto observed=CountVariant(player,key);
        if (!observed) return {};
        return observed->Count;
    }
    auto* form=TESForm::GetById(key.first);
    if (!form) return {};
    const auto total=player.GetItemCountInInventory(form);
    if (total<0 || total>INT32_MAX) return {};
    if (!credit)
    {
        // Without a selected list, an implicit clean debit of a mixed stack is ambiguous.
        if (Unsuitable(player).count(key.first) || total!=Pocket(world,player)[key.first]) return {};
        return total;
    }
    // Clean credit/refund may coexist with known special stacks. Count only
    // the clean remainder, never an owned stack as part of a clean receipt.
    int64_t special=0,listed=0;
    const auto* changes=player.GetContainerChanges();
    if (changes && !changes->entries) return {};
    if (changes)
        for (auto* entry:*changes->entries)
        {
            if (!entry || !entry->form || entry->form->formID!=key.first) continue;
            if (entry->IsQuestObject()) return {};
            if (!entry->dataList) continue;
            for (auto* list:*entry->dataList)
            {
                if (!list) continue;
                auto extra=ReadExtra(list);
                if (!extra) return {};
                listed+=extra->Count;
                if (listed>total) return {};
                if (!extra->Variant.empty()) special+=extra->Count;
            }
        }
    return total-special;
}
bool SubmitTransfer(World& world,const Wallet& wallet,const LocalItem& key,uint32_t count,bool take)
{
    if (!tradeOpen || !stockComplete || !count || count>9999 ||
        (!nativeRequested && !key.second.empty())) return false;
    auto* player=PlayerCharacter::Get();
    const auto properties=TradeProperties::Decode(key.second);
    if (!player || !properties) return false;
    if (properties->Created)
    {
        auto* base=TESForm::GetById(key.first);
        if (!base || (properties->Created->Weapon?base->formType!=FormType::Weapon:base->formType!=FormType::Armor) ||
            !RecreateEnchant(*properties->Created)) return false;
    }
    auto before=ReceiptCount(world,*player,key,take);
    if (!before || (take ? (*before+count>INT32_MAX || !buffer.count(key) || buffer[key]<int(count))
                         : (*before<count))) return false;
    PartyTrade e{take?TradeOp::Take:TradeOp::Deposit,peer,session,wallet.Cursor(),count,wallet.Stream()};
    if (!world.GetModSystem().GetServerModId(key.first,e.Item)) return false;
    e.Variant=key.second.c_str();e.BeforePlusOne=static_cast<uint32_t>(*before+1);
    if (!Send(e)) return false;
    walletReady=false;
    if (nativeRequested)
    {
        nativeAwaiting=true;nativeReceiptDeadline=Clock::now()+15s;
    }
    return true;
}
std::map<uint32_t,int> PlainBuffer()
{
    std::map<uint32_t,int> result;
    for (const auto& [item,count]:buffer)
        if (item.second.empty()) result[item.first]=count;
    return result;
}

CefString Rows(const std::map<uint32_t,int>& items)
{
    auto list=CefListValue::Create();
    for (const auto& [id,count]:items)
    {
        auto* f=TESForm::GetById(id);
        const char* label=f?f->GetName():nullptr;
        if (!label || !*label || count<=0) continue;
        auto row=CefDictionaryValue::Create();
        row->SetDouble("id",id); row->SetString("name",label); row->SetInt("count",count);
        list->SetDictionary(list->GetSize(),row);
    }
    auto value=CefValue::Create(); value->SetList(list); return CefWriteJSON(value,JSON_WRITER_DEFAULT);
}
}

std::optional<std::vector<HomeSnapshot::Item>> HomeInventory::CaptureLoose(TESObjectREFR& object)
{
    if (!object.baseForm || !HomeSnapshot::LooseType(static_cast<uint32_t>(object.baseForm->formType)) ||
        object.IsTemporary() || object.baseForm->IsTemporary()) return {};
    if (object.IsDeleted() || object.IsDisabled()) return std::vector<HomeSnapshot::Item>{};
    auto base=PropertyKey(object.baseForm->formID);
    auto* extra=object.GetExtraDataList();
    if (extra && extra->HasQuestObjectAlias()) return {};
    auto observed=ReadExtra(extra);
    if (!base || !observed || observed->Count<=0 || observed->Count>9999) return {};
    auto props=TradeProperties::Decode(observed->Variant);
    if (!props || !PropertiesAvailable(*props)) return {};
    return std::vector<HomeSnapshot::Item>{{*base,static_cast<uint32_t>(observed->Count),observed->Variant}};
}

std::optional<std::vector<HomeSnapshot::Item>> HomeInventory::Capture(TESObjectREFR& object)
{
    // Base counts and runtime deltas can coexist. The trade proxy's
    // ShadowCounts assumes an empty base, so do not use it here.
    auto* base=object.GetContainer();
    if (!base || (base->count && !base->entries)) return {};
    std::map<uint32_t,TESForm*> forms;
    for (uint32_t i=0;i<base->count;++i)
    {
        const auto* row=base->entries[i];
        if (!row || !row->form || row->data) return {};
        forms.emplace(row->form->formID,row->form);
    }
    const auto* changes=object.GetContainerChanges();
    if (changes && !changes->entries) return {};
    if (changes)
        for (auto* row:*changes->entries)
        {
            if (!row || !row->form || row->IsQuestObject()) return {};
            forms.emplace(row->form->formID,row->form);
        }
    std::vector<HomeSnapshot::Item> out;
    for (const auto& [id,form]:forms)
    {
        const auto total=object.GetItemCountInInventory(form);
        if (total<0 || total>INT32_MAX) return {};
        int64_t listed=0,special=0;
        std::map<std::string,int64_t> variants;
        if (changes)
            for (auto* row:*changes->entries)
            {
                if (row->form!=form || !row->dataList) continue;
                for (auto* list:*row->dataList)
                {
                    if (!list) continue;
                    auto extra=ReadExtra(list);
                    if (!extra || (listed+=extra->Count)>total) return {};
                    if (!extra->Variant.empty())
                    {
                        variants[extra->Variant]+=extra->Count;
                        special+=extra->Count;
                    }
                }
            }
        if (!total) continue;
        auto key=PropertyKey(id);
        if (!key || !Cast<TESBoundObject>(form)) return {};
        if (total>special) variants[{}]=total-special;
        for (const auto& [variant,count]:variants)
            out.push_back({*key,static_cast<uint32_t>(count),variant});
    }
    std::sort(out.begin(),out.end(),[](const auto& a,const auto& b)
    {return std::pair{a.Base.Encode(),a.Variant}<std::pair{b.Base.Encode(),b.Variant};});
    return out;
}

namespace
{
constexpr std::string_view ProjectionPlugin="skyrimcoophomeinstances.esp";
bool ProjectionContainerReady(TESObjectREFR& object,const std::vector<HomeSnapshot::Item>& desired)
{
    const auto* base=object.GetContainer();
    if (!base || base->count || !UnownedProxy(object) || !HomeInventory::Capture(object)) return false;
    for (const auto& item:desired)
    {
        auto value=TradeProperties::Decode(item.Variant);
        auto* form=Cast<TESBoundObject>(ResolvePropertyForm(item.Base));
        if (!form || !value || !PropertiesAvailable(*value)) return false;
    }
    return true;
}
bool MatchTransform(TESObjectREFR& ref,const HomeSnapshot::Ref& row)
{
    const std::array<float,6> actual{ref.position.x,ref.position.y,ref.position.z,
        ref.rotation.x,ref.rotation.y,ref.rotation.z};
    for (size_t i=0;i<actual.size();++i)
    {
        const auto target=std::bit_cast<float>(row.Transform[i]);
        if (!std::isfinite(actual[i]) || std::abs(actual[i]-target)>(i<3?0.05f:0.001f)) return false;
    }
    return ref.scale==row.Scale;
}
}
bool HomeProjection::Apply(TESObjectCELL& cell,const HomeSnapshot::State& state,const std::vector<Binding>& bindings)
{
    auto cellKey=PropertyKey(cell.formID);
    if (!cellKey || cellKey->Plugin!=ProjectionPlugin || cell.worldspace || !cell.IsAttached()) return false;
    const auto plan=Plan(state,bindings,ProjectionPlugin);
    if (!plan) return false;
    // Check every target before the first mutation. Only known isolated
    // references may be touched, and no actor or quest-owned container.
    for (const auto& step:*plan)
    {
        auto* ref=PropertyForm<TESObjectREFR>(step.Destination.Target);
        auto* base=ResolvePropertyForm(step.Destination.TargetBase);
        const auto& row=*step.State;
        if (!ref || !base || ref->baseForm!=base || ref->GetParentCellEx()!=&cell ||
            ref->IsDeleted() || Cast<Actor>(ref) || NativeTrade::IsProxy(ref) ||
            row.Scale!=ref->scale || static_cast<uint32_t>(base->formType)!=row.Type ||
            (row.LockState==0 && ref->GetLock()) ||
            (row.Inventory==1 && !ProjectionContainerReady(*ref,row.Items))) return false;
    }
    for (const auto& step:*plan)
    {
        auto* ref=PropertyForm<TESObjectREFR>(step.Destination.Target);
        const auto& row=*step.State;
        if (!ref || ref->GetParentCellEx()!=&cell || ref->IsDeleted()) return false;
        // No direct edits of the source ref, and no writes to shared base forms.
        if (!MatchTransform(*ref,row))
        {
            ref->SetRotation(std::bit_cast<float>(row.Transform[3]),std::bit_cast<float>(row.Transform[4]),
                std::bit_cast<float>(row.Transform[5]));
            ref->MoveTo(&cell,glm::vec3{std::bit_cast<float>(row.Transform[0]),std::bit_cast<float>(row.Transform[1]),
                std::bit_cast<float>(row.Transform[2])});
            if (ref->IsDeleted() || ref->GetParentCellEx()!=&cell || !MatchTransform(*ref,row)) return false;
        }
        if (row.Inventory==1)
        {
            // All real transfers are through the server receipt proxy.
            // Never create a second spendable copy of the owner's stock.
            const auto current=HomeInventory::Capture(*ref);
            if (!current || !current->empty()) return false;
        }
        if (row.LockState)
        {
            auto* lock=ref->GetLock();
            if (!lock) lock=ref->CreateLock();
            if (!lock) return false;
            if (lock->lockLevel!=row.LockLevel || lock->IsLocked()!=(row.LockState==2))
            {
                lock->lockLevel=static_cast<uint8_t>(row.LockLevel);
                lock->SetLock(row.LockState==2);ref->LockChange();
            }
            if (lock->lockLevel!=row.LockLevel || lock->IsLocked()!=(row.LockState==2)) return false;
        }
        if (ref->IsDisabled()!=bool(row.Flags&1))
        {
            if (row.Flags&1) ref->Disable();
            else ref->Enable();
        }
        if (ref->IsDisabled()!=bool(row.Flags&1)) return false;
    }
    return true;
}

namespace
{
// Projection identity is transient; only the ledger account is persistent.
struct PlacementDisplay
{
    uint32_t Ref{},Handle{},Base{},Cell{},Revision{};
    HomePlacement::Pose Committed{};
    std::optional<HomePlacement::Pose> Pending;
    Clock::time_point Retry{},ObserveUntil{};
    bool Grabbed{};
};
std::map<std::string,PlacementDisplay> placedDisplays;
std::mutex placementGuard;
std::map<uint32_t,const TESObjectREFR*> protectedPlacements;
std::set<uint32_t> legacySources;
std::atomic_uint32_t legacyCheckedCell{};
std::set<uint32_t> reportedLegacyConflicts;
uint64_t displayCampaign{},displayCharacter{};
uint32_t displaySlot{},displayCell{};
std::atomic_uint64_t placementEpoch{1};
Clock::time_point placementAt{};
TESObjectREFR* DisplayRef(const PlacementDisplay& d) noexcept
{
    if (!d.Ref) return nullptr;
    const TESObjectREFR* expected{};
    {
        std::scoped_lock lock(placementGuard);
        const auto it=protectedPlacements.find(d.Ref);
        if (it==protectedPlacements.end()) return nullptr;
        expected=it->second;
    }
    // A detached cell may invalidate the old handle. Resolve only the
    // exact previously registered object, not a new object of the same base.
    auto* r=d.Handle?TESObjectREFR::GetByHandle(d.Handle):nullptr;
    if (r!=expected) r=Cast<TESObjectREFR>(TESForm::GetById(d.Ref));
    if (!r || r!=expected || r->formID!=d.Ref || !r->IsTemporary() ||
        r->IsDeleted() || !r->baseForm || r->baseForm->formID!=d.Base) return nullptr;
    return r;
}
auto FindPlacement(const TESObjectREFR* r)
{
    return std::find_if(placedDisplays.begin(),placedDisplays.end(),[&](const auto& entry)
    {return r && entry.second.Ref==r->formID && DisplayRef(entry.second)==r;});
}
void ForgetGuard(uint32_t form)
{
    std::scoped_lock lock(placementGuard);protectedPlacements.erase(form);
}
void ErasePlacement(std::map<std::string,PlacementDisplay>::iterator it)
{
    auto* r=DisplayRef(it->second);
    const auto form=it->second.Ref;
    // Keep the pickup guard until the physical reference is disabled.
    if (r) {r->Disable();r->Delete();}
    ForgetGuard(form);
    placedDisplays.erase(it);
}
HomePlacement::Pose PlacementPose(const NiPoint3& p,const NiPoint3& r)
{
    return {std::bit_cast<uint32_t>(p.x),std::bit_cast<uint32_t>(p.y),std::bit_cast<uint32_t>(p.z),
            std::bit_cast<uint32_t>(r.x),std::bit_cast<uint32_t>(r.y),std::bit_cast<uint32_t>(r.z)};
}
NiPoint3 PlacementPoint(const HomePlacement::Pose& pose,size_t i)
{return glm::vec3{std::bit_cast<float>(pose[i]),std::bit_cast<float>(pose[i+1]),std::bit_cast<float>(pose[i+2])};}
uint32_t ScopedSlot(const TESObjectREFR* r) noexcept
{
    const auto* cell=r?r->GetParentCellEx():nullptr;
    uint32_t mod{},base{};
    return cell && HomeInstance::Encode(cell->formID,mod,base) && HomeRegistry::Scoped(mod)?
        mod&HomeRegistry::MaxSlot:0;
}
bool SameLegacyItem(const HomeSnapshot::LegacyStack& a,const HomeSnapshot::LegacyStack& b)
{
    return a.Source==b.Source && a.Value.Base.Encode()==b.Value.Base.Encode() &&
        a.Value.Count==b.Value.Count && a.Value.Variant==b.Value.Variant;
}
bool SetDisplayCount(ExtraDataList& extra,uint32_t count)
{
    if (!count || count>9999 || VersionDb::Get().GetLoadedVersionString()!="1.6.1170.0") return false;
    if (auto* data=extra.GetByType(ExtraDataType::Count))
        static_cast<ExtraCount*>(data)->count=static_cast<int16_t>(count);
    else if (count!=1)
    {
        // CommonLibSSE-NG ExtraCount constructor, AE vtable ID 186679.
        auto** table=Address<void*>(186679);
        MEMORY_BASIC_INFORMATION info{};
        if (!table || !VirtualQuery(table,&info,sizeof(info)) || info.State!=MEM_COMMIT ||
            (info.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
            reinterpret_cast<uintptr_t>(table)+3*sizeof(void*)>reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize ||
            !table[0] || !table[1] || !table[2]) return false;
        auto* data=Memory::Allocate<ExtraCount>();
        if (!data) return false;
        std::memset(static_cast<void*>(data),0,sizeof(*data));
        *reinterpret_cast<void***>(data)=table;
        data->count=static_cast<int16_t>(count);
        if (!extra.Add(ExtraDataType::Count,data)) {Memory::Free(data);return false;}
    }
    const auto* stored=extra.GetByType(ExtraDataType::Count);
    return stored?static_cast<const ExtraCount*>(stored)->count==static_cast<int>(count):count==1;
}
bool ApplyDisplayProperties(TESObjectREFR& ref,const HomeSnapshot::Item& item)
{
    auto props=TradeProperties::Decode(item.Variant);
    auto* list=ref.GetExtraDataList();
    if (!props || !list || !PropertiesAvailable(*props) || !SetDisplayCount(*list,item.Count)) return false;
    auto* enchant=(props->Mask&props->HasEnchant)?(props->Created?RecreateEnchant(*props->Created):
        PropertyForm<EnchantmentItem>(props->Enchant)):nullptr;
    auto* poison=(props->Mask&props->HasPoison)?PropertyForm<AlchemyItem>(props->Poison):nullptr;
    if (((props->Mask&props->HasEnchant)&&!enchant) || ((props->Mask&props->HasPoison)&&!poison)) return false;
    if (props->Mask&props->HasHealth) list->SetHealth(std::bit_cast<float>(props->Health));
    if (props->Mask&props->HasCharge) list->SetChargeData(std::bit_cast<float>(props->Charge));
    if (props->Mask&props->HasSoul) list->SetSoulData(static_cast<SOUL_LEVEL>(props->Soul));
    if (enchant) list->SetEnchantmentData(enchant,static_cast<uint16_t>(props->EnchantCharge),props->RemoveOnUnequip!=0);
    if (poison) list->SetPoison(poison,props->PoisonCount);
    if ((props->Mask&props->HasName) && !SetCustomName(*list,props->Name)) return false;
    if ((props->Mask&props->HasOwnership) && !SetTradeOwner(*list,props->Owner)) return false;
    return true;
}
bool SpawnPlacement(TESObjectCELL& cell,const HomePlacement::Stack& stack,PlacementDisplay& display)
{
    auto* base=ResolvePropertyForm(stack.Item.Base);
    auto* mods=ModManager::Get();
    if (!mods || !base || base->IsTemporary() || !Cast<TESBoundObject>(base) ||
        !HomeSnapshot::LooseType(static_cast<uint32_t>(base->formType))) return false;
    auto p=PlacementPoint(stack.Position,0),r=PlacementPoint(stack.Position,3);
    const auto handle=mods->SpawnObject(p,r,&cell,base);
    auto* ref=handle?TESObjectREFR::GetByHandle(handle):nullptr;
    if (!ref || !ref->IsTemporary() || ref->baseForm!=base) return false;
    ref->SetSkipSaveFlag(true);
    const auto form=ref->formID;
    {std::scoped_lock lock(placementGuard);protectedPlacements[form]=ref;}
    if (ref->GetParentCellEx()!=&cell || ref->GetHandle().handle.iBits!=handle || !ApplyDisplayProperties(*ref,stack.Item))
    {ref->Disable();ref->Delete();ForgetGuard(form);return false;}
    display.Ref=form;display.Handle=handle;display.Base=base->formID;display.Cell=cell.formID;
    display.Revision=stack.Revision;display.Committed=stack.Position;
    return true;
}
void ApplyPlacementPose(TESObjectREFR& ref,TESObjectCELL& cell,const HomePlacement::Pose& pose)
{
    const auto r=PlacementPoint(pose,3);
    ref.SetRotation(r.x,r.y,r.z);
    ref.MoveTo(&cell,PlacementPoint(pose,0));
}
uint32_t GrabbedHandle(const PlayerCharacter& player) noexcept
{
    // CommonLibSSE-NG GetGrabbedRef: ObjectRefHandle at 0x8D0 for 1.6.629+.
    if (VersionDb::Get().GetLoadedVersionString()!="1.6.1170.0") return 0;
    uint32_t handle{};
    std::memcpy(&handle,reinterpret_cast<const uint8_t*>(&player)+0x8D0,sizeof(handle));
    return handle;
}
struct ManagedContainer {uint32_t Slot{};TradeProperties::FormKey Ref;bool Ready{};};
std::optional<ManagedContainer> ManagedHome(const TESObjectREFR* object)
{
    if (!object || !object->baseForm) return {};
    const bool loose=HomeSnapshot::LooseType(static_cast<uint32_t>(object->baseForm->formType));
    if (!loose && object->baseForm->formType!=FormType::Container) return {};
    auto* cell=const_cast<TESObjectREFR*>(object)->GetParentCellEx();
    if (!cell || cell->worldspace) return {};
    auto& world=World::Get();
    if (HomeInstance::ProjectionForm(object->formID) || HomeInstance::NativeGuestCell(cell->formID))
    {
        uint32_t source{};
        const auto slot=HomeInstance::GuestReference(object->formID,source);
        auto ref=PropertyKey(source);
        if (!slot || !ref) return {};
        return ManagedContainer{slot,*ref,true};
    }
    auto id=CampaignAccess::LocalIdentity(world);
    if (!id) return {};
    auto root=PropertyKey(cell->formID),ref=PropertyKey(object->formID);
    if (!root || !ref) return {};
    const auto slot=CampaignAccess::HomeScope(id->Save.Campaign,id->Save.Character,root->Encode());
    if (!slot) return {};
    const auto status=CampaignAccess::HomeContainerState(id->Save.Campaign,slot,ref->Encode());
    // An explicitly untracked legacy loose reference remains native for its owner.
    // The projection plan rejects that baseline; never invent its balance.
    if (status==3 || (status==2 && !loose)) return {};
    return ManagedContainer{slot,*ref,status==1};
}
bool ProtectHome(const TESObjectREFR* object) noexcept
{
    // Protect originals during pending/active/exit; don't block player receipts.
    if (object && object->formID!=0x14 && !Cast<Actor>(const_cast<TESObjectREFR*>(object)))
        if (const auto* cell=object->GetParentCellEx();cell && HomeInstance::NativeGuestCell(cell->formID))
            return true;
    if (object && (HomeInstance::ProjectionForm(object->formID) || HomePlacementClient::Tracked(object) ||
        HomePlacementClient::LegacyBlocked(object))) return true;
    try {return ManagedHome(object).has_value();}
    catch (...) {return false;}
}
}
bool HomePlacementClient::Tracked(const TESObjectREFR* object) noexcept
{
    if (!object || !object->IsTemporary()) return false;
    std::scoped_lock lock(placementGuard);
    const auto it=protectedPlacements.find(object->formID);
    return it!=protectedPlacements.end() && it->second==object;
}
std::optional<HomeSnapshot::LegacyStack> HomePlacementClient::CaptureLegacy(TESObjectREFR& object)
{
    if (!object.IsTemporary() || object.IsDeleted() || object.scale!=100 ||
        !object.baseForm || object.baseForm->IsTemporary() || Cast<Actor>(&object) ||
        !HomeSnapshot::LooseType(static_cast<uint32_t>(object.baseForm->formType)) ||
        NativeTrade::IsProxy(&object) || Tracked(&object)) return {};
    auto base=PropertyKey(object.baseForm->formID);
    auto* extra=object.GetExtraDataList();
    if (extra && extra->HasQuestObjectAlias()) return {};
    auto observed=ReadExtra(extra);
    if (!base || !observed || observed->Count<=0 || observed->Count>9999) return {};
    auto props=TradeProperties::Decode(observed->Variant);
    if (!props || !PropertiesAvailable(*props)) return {};
    HomeSnapshot::LegacyStack result{object.formID,
        {*base,static_cast<uint32_t>(observed->Count),observed->Variant},
        PlacementPose(object.position,object.rotation)};
    return result.Valid()?std::optional<HomeSnapshot::LegacyStack>(result):std::nullopt;
}
bool HomePlacementClient::LegacyBlocked(const TESObjectREFR* object) noexcept
{
    if (!object || !object->IsTemporary() || !object->baseForm || !Scoped(object) ||
        !HomeSnapshot::LooseType(static_cast<uint32_t>(object->baseForm->formType))) return false;
    {
        std::scoped_lock lock(placementGuard);
        if (legacySources.count(object->formID)) return true;
    }
    const auto* cell=object->GetParentCellEx();
    if (!cell || Tracked(object) || HomeInstance::ProjectionForm(cell->formID) ||
        legacyCheckedCell.load()==cell->formID) return false;
    // Until the initial snapshot is reconciled, do not allow native pickup
    // to race its publication. Capture/ExtraData reads stay on the game thread.
    return true;
}
bool HomePlacementClient::Scoped(const TESObjectREFR* object) noexcept
{
    const auto* cell=object?object->GetParentCellEx():nullptr;
    uint32_t mod{},base{};
    return cell && (HomeInstance::ProjectionForm(cell->formID) || HomeInstance::NativeGuestCell(cell->formID) ||
        (HomeInstance::Encode(cell->formID,mod,base) && (HomeRegistry::Scoped(mod)||HomeRegistry::Pending(mod))));
}
void HomePlacementClient::Forget() noexcept
{
    // After a confirmed world replacement, never dereference prior-world handles.
    placedDisplays.clear();
    {std::scoped_lock lock(placementGuard);protectedPlacements.clear();legacySources.clear();}
    legacyCheckedCell.store(0);reportedLegacyConflicts.clear();
    displayCampaign=displayCharacter=0;displaySlot=displayCell=0;
    placementAt={};++placementEpoch;
}
void HomePlacementClient::Clear() noexcept
{
    // Transient Loading Menu is not a new world; keep the guard until safe.
    if (!CampaignAccess::GameReady()) return;
    while (!placedDisplays.empty()) ErasePlacement(placedDisplays.begin());
    Forget();
}
bool HomePlacementClient::InterceptDrop(const TESObjectREFR* from,const TESBoundObject* item,ExtraDataList* extra,
                                        int32_t count,const NiPoint3* drop,const NiPoint3* rotate) noexcept
{
    if (!from || from->formID!=0x14 || !Scoped(from)) return false;
    // Never let an ordinary native debit happen alongside the receipt.
    if (ScopedInventoryOverride::IsOverriden()) return true;
    try
    {
        const auto slot=ScopedSlot(from);
        const auto* cell=from->GetParentCellEx();
        if (!slot || !cell || !item || item->IsTemporary() || count<=0 || count>9999 ||
            !HomeSnapshot::LooseType(static_cast<uint32_t>(item->formType))) return true;
        const auto selected=ReadExtra(extra);
        if (!selected || (extra && extra->HasQuestObjectAlias()) ||
            (!selected->Variant.empty() && count>selected->Count)) return true;
        const auto form=item->formID,cellId=cell->formID;
        auto position=from->position;
        if (drop) position=*drop;
        else {position.x+=60.f*std::sin(from->rotation.z);position.y+=60.f*std::cos(from->rotation.z);position.z+=40.f;}
        NiPoint3 rotation{};if (rotate) rotation=*rotate;
        const auto pose=PlacementPose(position,rotation);
        if (!HomePlacement::Valid(pose)) return true;
        const auto epoch=placementEpoch.load();
        World::Get().GetRunner().Queue([slot,cellId,form,count,selected,pose,epoch]()
        {
            auto& world=World::Get();auto* player=PlayerCharacter::Get();
            if (placementEpoch.load()!=epoch || !player || !player->GetParentCellEx() ||
                player->GetParentCellEx()->formID!=cellId || ScopedSlot(player)!=slot ||
                !CampaignAccess::GameReady() || !CampaignAccess::TradeEnabled()) return;
            const auto id=CampaignAccess::Current(world);
            Wallet w;
            if (!id || !GuardPlayer(player) ||
                !CampaignAccess::HomeCanModify(id->Save.Campaign,slot,id->Save.Character) ||
                !CampaignAccess::HomePlacements(id->Save.Campaign,slot) ||
                session || !w.Load(world) || !Ready(w))
            {pollAt={};Utils::ShowHudMessage("Размещение предмета: ожидается доступ к имуществу дома.");return;}
            const LocalItem key{form,selected->Variant};
            auto props=TradeProperties::Decode(key.second);
            const auto before=ReceiptCount(world,*player,key,false);
            if (!props || !PropertiesAvailable(*props) || !before || *before<count) return;
            if (props->Created && !RecreateEnchant(*props->Created)) return;
            PartyTrade request{TradeOp::HomePlace,slot,0,w.Cursor(),static_cast<uint32_t>(count),w.Stream()};
            request.Text=HomePlacement::EncodePose(pose).c_str();request.Variant=key.second.c_str();
            request.BeforePlusOne=static_cast<uint32_t>(*before+1);
            if (world.GetModSystem().GetServerModId(form,request.Item) && Send(request))
            {walletReady=false;pollAt={};}
        });
    }
    catch (...) {spdlog::error("STM: failed to queue residential drop");}
    return true;
}
void HomePlacementClient::Update(World& world) noexcept
{
    try
    {
        if (!CampaignAccess::GameReady()) return;
        auto* player=PlayerCharacter::Get();
        auto* cell=player?player->GetParentCellEx():nullptr;
        const auto slot=ScopedSlot(player);
        const auto id=slot?CampaignAccess::Current(world):std::nullopt;
        if (!id || !cell || !slot)
        {
            if (displaySlot) Clear();
            return;
        }
        if (displayCampaign!=id->Save.Campaign || displayCharacter!=id->Save.Character ||
            displaySlot!=slot || displayCell!=cell->formID)
        {
            Clear();displayCampaign=id->Save.Campaign;displayCharacter=id->Save.Character;
            displaySlot=slot;displayCell=cell->formID;
        }
        GuardPlayer(player);
        const auto now=Clock::now();
        if (!cell->IsAttached() || now<placementAt) return;
        placementAt=now+200ms;
        const auto bytes=CampaignAccess::HomeBaseline(displayCampaign,slot);
        const auto baseline=bytes?HomeSnapshot::Decode(*bytes):std::nullopt;
        const auto stacks=CampaignAccess::HomePlacements(displayCampaign,slot);
        if (!baseline || !stacks) return;
        std::set<uint32_t> conflicts;
        if (!HomeInstance::ProjectionForm(cell->formID) && !HomeInstance::NativeGuestCell(cell->formID))
        {
            // Reserve the original even after its ledger balance is zero.
            for (const auto& source:baseline->Legacy)
            {
                {std::scoped_lock lock(placementGuard);legacySources.insert(source.Source);}
                auto* native=Cast<TESObjectREFR>(TESForm::GetById(source.Source));
                if (!native || native->IsDeleted()) continue;
                if (native->GetParentCellEx()!=cell || Tracked(native))
                {conflicts.insert(source.Source);continue;}
                const auto actual=CaptureLegacy(*native);
                if (!actual || !SameLegacyItem(*actual,source))
                {conflicts.insert(source.Source);continue;}
                if (!native->IsDisabled()) native->Disable();
                if (!native->IsDisabled()) conflicts.insert(source.Source);
            }
        }
        for (auto source:conflicts)
            if (reportedLegacyConflicts.insert(source).second)
                Utils::ShowHudMessage("Не удалось сопоставить прежний предмет дома. Исходный объект сохранён.");
        legacyCheckedCell.store(cell->formID);
        const bool modify=CampaignAccess::TradeEnabled() &&
            CampaignAccess::HomeCanModify(displayCampaign,slot,displayCharacter);
        const auto grabbed=GrabbedHandle(*player);
        std::set<std::string> seen;
        for (const auto& stack:*stacks)
        {
            const auto account=stack.Id.Encode();
            if (stack.Id.LegacySource && conflicts.count(stack.Id.LegacySource)) continue;
            seen.insert(account);
            auto it=placedDisplays.try_emplace(account).first;
            auto& d=it->second;
            auto* object=DisplayRef(d);
            if (!object)
            {
                ForgetGuard(d.Ref);d={};
                if (!SpawnPlacement(*cell,stack,d))
                {placedDisplays.erase(it);continue;}
                object=DisplayRef(d);
                if (!object) {ForgetGuard(d.Ref);placedDisplays.erase(it);continue;}
                if (!stack.Id.LegacySource && stack.Id.Creator==displayCharacter && !stack.Revision) d.ObserveUntil=now+4s;
            }
            if (object->GetParentCellEx()!=cell)
            {
                ApplyPlacementPose(*object,*cell,stack.Position);
                if (object->GetParentCellEx()!=cell) {ErasePlacement(it);continue;}
            }
            if (stack.Revision!=d.Revision)
            {
                const bool acknowledged=d.Pending && *d.Pending==stack.Position;
                d.Revision=stack.Revision;d.Committed=stack.Position;d.Pending.reset();
                if (!acknowledged) {ApplyPlacementPose(*object,*cell,stack.Position);d.ObserveUntil={};}
            }
            d.Handle=object->GetHandle().handle.iBits;
            const bool holding=grabbed && grabbed==d.Handle;
            if (d.Grabbed && !holding) d.ObserveUntil=now+4s;
            d.Grabbed=holding;
            const auto pose=PlacementPose(object->position,object->rotation);
            if (!HomePlacement::Valid(pose)) continue;
            const bool observing=modify && (holding || now<d.ObserveUntil);
            if (!observing)
            {
                // Observer physics is not an authoritative move.
                if (now<d.Retry) continue;
                d.Pending.reset();
                if (HomePlacement::Different(pose,d.Committed))
                    {ApplyPlacementPose(*object,*cell,d.Committed);d.Retry=now+1s;}
                continue;
            }
            if (now<d.Retry || d.Revision==UINT32_MAX ||
                !HomePlacement::Different(pose,d.Committed)) continue;
            Wallet w;
            if (session || !w.Load(world) || !Ready(w)) continue;
            HomePlacement::Move move{account,d.Revision+1,pose};
            PartyTrade request{TradeOp::HomeMove,slot,0,w.Cursor(),0,w.Stream()};
            request.Text=move.Encode().c_str();
            d.Retry=now+2s;
            if (Send(request)) d.Pending=pose;
        }
        for (auto it=placedDisplays.begin();it!=placedDisplays.end();)
            if (!seen.count(it->first)) {auto stale=it++;ErasePlacement(stale);}
            else ++it;
    }
    catch (...) {spdlog::error("STM: residential placement projection update failed");}
}
bool NativeTrade::IsProxy(const TESObjectREFR* object) noexcept
{
    const auto id=proxyId.load();
    return id && object && object->formID==id;
}
bool NativeTrade::BlockAdd(const TESObjectREFR* to,const TESObjectREFR* from) noexcept
{
    if (ProtectHome(to) || ProtectHome(from)) return true;
    if (HomeActorProtection::Resident(to) || HomeActorProtection::Resident(from))
        return !(ScopedInventoryOverride::IsOverriden() && !from && HomeActorProtection::Resident(to));
    if (shadowMutation && IsProxy(to) && !from) return false;
    return IsProxy(to)||IsProxy(from);
}
bool NativeTrade::BlockRemove(const TESObjectREFR* from,const TESObjectREFR* to,
                              const TESBoundObject* item,int32_t count,ExtraDataList* extra) noexcept
{
    if (ProtectHome(from) || ProtectHome(to)) return true;
    if (HomeActorProtection::Resident(from) || HomeActorProtection::Resident(to))
        return !(ScopedInventoryOverride::IsOverriden() && !to && HomeActorProtection::Resident(from));
    const bool source=IsProxy(from), destination=IsProxy(to);
    if (!source && !destination) return false;
    if (shadowMutation && source && !to) return false;
    const auto token=nativeToken.load();
    // Always suppress native mutations involving this reference, including
    // stale menu callbacks, direct transfers and TakeAll loops.
    if (!token || source==destination || !item || count<=0 ||
        (source ? (!to || to->formID!=0x14) : (!from || from->formID!=0x14))) return true;
    const auto selected=ReadExtra(extra);
    const auto id=item->formID;
    World::Get().GetRunner().Queue([token,id,count,source,selected]()
    {
        auto& world=World::Get();
        if (nativeToken.load()!=token || !world.GetTransport().IsOnline() ||
            !CampaignAccess::GameReady() || !NativeMenuOpen() || !stockComplete) return;
        if (!selected || (!selected->Variant.empty() && count>selected->Count))
        {
            Utils::ShowHudMessage("Этот экземпляр пока нельзя передать через обмен.");
            return;
        }
        if (nativeIntents.size()>=128)
        {Utils::ShowHudMessage("Дождитесь завершения перемещений.");return;}
        nativeIntents.push_back({{id,selected->Variant},static_cast<uint32_t>(count),source});
    });
    return true;
}

uint32_t OverlayService::TradePeer(uint32_t form) const noexcept
{
    if (!form || !m_transport.IsOnline()) return UINT32_MAX;
    auto view=m_world.view<PlayerComponent,FormIdComponent>();
    for (auto e:view)
        if (view.get<FormIdComponent>(e).Id==form)
        {
            const auto id=view.get<PlayerComponent>(e).Id;
            if (id!=m_transport.GetLocalPlayerId()) return id;
        }
    return UINT32_MAX;
}
bool HomeGuestSave::InventoryReady(World& world) noexcept
{
    try
    {
        if (session||nativeRequested||nativeToken.load()||nativeAwaiting||!nativeIntents.empty()||
            shadowCleanupPending||receiptFailure) return false;
        if (!CampaignAccess::TradeEnabled()) return true;
        Wallet w;return w.Load(world)&&Ready(w);
    }
    catch (...) {return false;}
}
bool OverlayService::TryTrade(uint32_t form) noexcept
{
    if (HomeGuestSave::Busy()) return true;
    auto* object=Cast<TESObjectREFR>(TESForm::GetById(form));
    if (object && !HomePlacementClient::Tracked(object) && HomePlacementClient::LegacyBlocked(object)) return true;
    if (object && HomePlacementClient::Tracked(object))
    {
        const auto found=FindPlacement(object);
        const auto id=CampaignAccess::Current(m_world);
        auto* player=PlayerCharacter::Get();
        const auto slot=ScopedSlot(player);
        Wallet w;
        if (found==placedDisplays.end() || !id || !player || !slot || slot!=displaySlot ||
            object->GetParentCellEx()!=player->GetParentCellEx() ||
            !CampaignAccess::TradeEnabled() || session || m_active || !w.Load(m_world) || !Ready(w) ||
            !CampaignAccess::HomeCanModify(id->Save.Campaign,slot,id->Save.Character))
        {pollAt={};return true;}
        const auto stacks=CampaignAccess::HomePlacements(id->Save.Campaign,slot);
        if (!stacks) return true;
        const auto stack=std::find_if(stacks->begin(),stacks->end(),[&](const auto& x){return x.Id.Encode()==found->first;});
        if (stack==stacks->end()) return true;
        auto* base=ResolvePropertyForm(stack->Item.Base);
        const auto props=TradeProperties::Decode(stack->Item.Variant);
        if (!base || base!=object->baseForm || !props || !PropertiesAvailable(*props)) return true;
        if (props->Created && !RecreateEnchant(*props->Created)) return true;
        const LocalItem key{base->formID,stack->Item.Variant};
        const auto before=ReceiptCount(m_world,*player,key,true);
        if (!before || *before+stack->Item.Count>INT32_MAX) return true;
        PartyTrade request{TradeOp::HomeTake,slot,0,w.Cursor(),stack->Item.Count,w.Stream()};
        request.Text=found->first.c_str();request.Variant=key.second.c_str();
        request.BeforePlusOne=static_cast<uint32_t>(*before+1);
        if (m_world.GetModSystem().GetServerModId(base->formID,request.Item) && Send(request))
        {walletReady=false;pollAt={};}
        return true;
    }
    if (auto home=ManagedHome(object))
    {
        Wallet w;
        if (object && object->baseForm && HomeSnapshot::LooseType(static_cast<uint32_t>(object->baseForm->formType)))
        {
            // Normal activation, no second physical pickup or scratch menu.
            const auto id=CampaignAccess::Current(m_world);
            if (!home->Ready || !id || !CampaignAccess::TradeEnabled() || session || m_active ||
                !w.Load(m_world) || !Ready(w))
            {pollAt={};Utils::ShowHudMessage("Предмет дома: ожидается журнал имущества.");return true;}
            const auto bytes=CampaignAccess::HomeBaseline(id->Save.Campaign,home->Slot);
            const auto state=bytes?HomeSnapshot::Decode(*bytes):std::nullopt;
            if (!state) return true;
            for (const auto& row:state->Refs)
            {
                if (row.Key.Encode()!=home->Ref.Encode() || row.Inventory!=3 || (row.Flags&3u) ||
                    row.Items.size()!=1) continue;
                const auto& item=row.Items.front();
                auto* base=ResolvePropertyForm(item.Base);
                const auto props=TradeProperties::Decode(item.Variant);
                auto* player=PlayerCharacter::Get();
                if (!base || object->baseForm!=base || !props || !PropertiesAvailable(*props) || !player) return true;
                if (props->Created && !RecreateEnchant(*props->Created)) return true;
                const LocalItem key{base->formID,item.Variant};
                auto before=ReceiptCount(m_world,*player,key,true);
                if (!before || *before+item.Count>INT32_MAX) return true;
                PartyTrade request{TradeOp::Take,home->Slot,0,w.Cursor(),item.Count,w.Stream()};
                request.Text=home->Ref.Encode().c_str();request.Variant=item.Variant.c_str();
                request.BeforePlusOne=static_cast<uint32_t>(*before+1);
                if (!m_world.GetModSystem().GetServerModId(base->formID,request.Item)) return true;
                if (Send(request)) {walletReady=false;pollAt={};}
                return true;
            }
            return true;
        }
        if (!home->Ready || !m_transport.IsOnline() || !CampaignAccess::TradeEnabled() ||
            !NativeEnabled() || !w.Load(m_world) || !Ready(w) || session || m_active)
        {
            pollAt={};
            Utils::ShowHudMessage("Домашний контейнер: ожидается доступ к журналу имущества.");
            return true;
        }
        if (!HomeInstance::ProjectionForm(form))
            if (const auto* lock=object->GetLock();lock && lock->IsLocked()) return false;
        PartyTrade request{TradeOp::HomeOpen,home->Slot,0,w.Cursor(),0,w.Stream()};
        request.Text=home->Ref.Encode().c_str();
        if (!Send(request)) Utils::ShowHudMessage("Не удалось открыть домашний контейнер.");
        return true;
    }
    if (HomeInstance::ProjectionForm(form)) return true;
    const auto id=TradePeer(form);
    if (id==UINT32_MAX) return false;
    Wallet w;
    if (receiptFailure) {Utils::ShowHudMessage("Обмен остановлен: загрузите сохранение до ошибки квитанции.");return true;}
    if (!CampaignAccess::TradeEnabled()) {Utils::ShowHudMessage("Обмен кампании отключён.");return true;}
    if (!w.Load(m_world)) Utils::ShowHudMessage("Для обмена включите SkyrimCoopTrade.esp.");
    else if (!Ready(w)) {pollAt={};Utils::ShowHudMessage("Обмен: проверка и восстановление квитанций.");}
    else if (!session && !m_active) Send({TradeOp::Invite,id,0,w.Cursor(),0,w.Stream()});
    return true;
}
void OverlayService::ClearTrade() noexcept
{
    nativeToken.store(0);
    ResetNativeIntents();takeAllRegistered=false;
    shadowCleanupPending=true;cleanupRetry={};
    if (nativeRequested && CampaignAccess::GameReady()) NativeHide();
    nativeRequested=nativeOpened=stockComplete=false;
    nativeDeadline={};stockRemaining=0;incomingStock.clear();
    if (session && m_pOverlay) m_pOverlay->ExecuteAsync("hideTrade");
    if (ownsUI && m_gameOverlayClient && GetActive()) m_gameOverlayClient->SetUIVisible(false);
    session=peer=0; tradeOpen=ownsUI=dirty=false; buffer.clear(); partner.clear(); pollAt={};
    walletReady=false;readyStream=0;readyCursor=0;
}
void OverlayService::RecheckTradeAfterLoad() noexcept
{
    HomePlacementClient::Forget();
    ResetNativeIntents();
    shadowCleanupPending=true;cleanupRetry={};
    // A confirmed world replacement permits a new receipt check, not automatic acceptance.
    // Legacy receipts have no verified before-count; retain their failure latch.
    if (receiptFailure && receiptFailureCampaign)
    {
        receiptFailure=false;
        receiptFailureCampaign=false;
    }
    walletReady=false;readyStream=0;readyCursor=0;lastConflict=UINT32_MAX;pollAt={};
}
void OverlayService::TradeAction(uint32_t token,int action,uint32_t id,uint32_t count) noexcept
{
    if (!session || session!=token) return;
    Wallet w;
    if (action==0) {Send({TradeOp::Close,peer,session}); ClearTrade();return;}
    if (!w.Load(m_world) || !Ready(w)) return;
    PartyTrade e{TradeOp::Accept,peer,session,w.Cursor(),count,w.Stream()};
    if (action==1 && !tradeOpen) {Send(e);return;}
    if (action!=2 && action!=3) return;
    if (!SubmitTransfer(m_world,w,{id,{}},count,action==3) && nativeRequested)
        Utils::ShowHudMessage("Предмет недоступен для обмена.");
}
void OverlayService::OnTrade(const NotifyPartyTrade& msg) noexcept
{
    auto* p=PlayerCharacter::Get();
    if ((receiptFailure && msg.Event.Op!=TradeOp::Close) || !m_transport.IsOnline() || !m_inGame || !p || !CampaignAccess::GameReady() || !CampaignAccess::TradeEnabled() || !m_pOverlay || !m_gameOverlayClient) return;
    const auto& e=msg.Event;
    using Mode=CampaignReplication::Link::Mode;
    if (CampaignAccess::Mode()!=Mode::Disabled)
    {
        const auto identity=CampaignAccess::Current(m_world);
        if (!identity || e.Campaign!=identity->Save.Campaign || e.Character!=identity->Save.Character) return;
    }
    else if (e.Campaign || e.Character) return;
    Wallet w; if (!w.Load(m_world)) return;
    if (e.Stream && w.Stream() && w.Stream()!=e.Stream) {Utils::ShowHudMessage("Обмен привязан к другому журналу сервера.");return;}
    if (e.Op==TradeOp::ReturnOffer)
    {
        walletReady=false;
        if (session || !e.Stream || e.Cursor!=w.Cursor() || !e.Count || e.Count>9999 || !e.Item.BaseId || p->IsDead()) return;
        auto* manager=ModManager::Get();
        auto* mod=manager?manager->GetByName(e.Text.c_str()):nullptr;
        if (!mod || !mod->IsLoaded()) return;
        const auto id=mod->GetFormId(e.Item.BaseId);
        auto* form=TESForm::GetById(id);
        if (!form || !Cast<TESBoundObject>(form))
        {
            if (lastConflict!=e.Cursor) {lastConflict=e.Cursor;Utils::ShowHudMessage("Возврат обмена приостановлен: предмет недоступен или имеет особые свойства.");}
            return;
        }
        const LocalItem key{id,{e.Variant.data(),e.Variant.size()}};
        const auto before=ReceiptCount(m_world,*p,key,true);
        if (!before || *before+e.Count>INT32_MAX)
        {
            if (lastConflict!=e.Cursor) {lastConflict=e.Cursor;Utils::ShowHudMessage("Возврат обмена приостановлен: инвентарь не соответствует записи.");}
            return;
        }
        PartyTrade request{TradeOp::Return,0,0,w.Cursor(),e.Count,e.Stream};
        if (!m_world.GetModSystem().GetServerModId(id,request.Item)) return;
        request.BeforePlusOne=static_cast<uint32_t>(*before+1);request.Variant=e.Variant;
        Send(request);return;
    }
    if (e.Op==TradeOp::Receipt)
    {
        if (!e.Stream || e.Cursor!=w.Cursor()+1 || !e.Count || e.Count>9999 || !e.Item.BaseId || p->IsDead() || !p->GetNiNode()) return;
        auto* mod=ModManager::Get()->GetByName(e.Text.c_str());
        if (!mod || !mod->IsLoaded()) return;
        const auto id=mod->GetFormId(e.Item.BaseId);
        auto* form=TESForm::GetById(id);
        if (!form || !Cast<TESBoundObject>(form)) return;
        if (e.Peer>1) return;
        const int count=(e.Peer?1:-1)*int(e.Count);
        const LocalItem key{id,{e.Variant.data(),e.Variant.size()}};
        if (!TradeProperties::Decode(key.second)) return;
        walletReady=false;
        const int64_t checkpoint=e.BeforePlusOne?static_cast<int64_t>(e.BeforePlusOne)-1:-1;
        if (CampaignAccess::Mode()!=Mode::Disabled)
        {
            if (!CampaignAccess::ConfirmReceipt(m_world,e.Stream,w.Stream(),w.Cursor(),e.Cursor,
                                           {e.Text.data(),e.Text.size()},e.Item.BaseId,count,checkpoint,key.second))
                return; // replica may still be behind; poll again after it catches up
        }
        if (CampaignAccess::Mode()!=Mode::Disabled || checkpoint>=0)
        {
            const auto actual=ReceiptCount(m_world,*p,key,count>0);
            TradeLedger::Row row{"","","",e.Item.BaseId,count,checkpoint,false,key.second};
            if (!actual || !TradeLedger::CanApply(row,*actual))
            {
                if (lastConflict!=e.Cursor)
                {
                    lastConflict=e.Cursor;
                    spdlog::warn("Trade recovery conflict at receipt {}: expected {}, found {}",e.Cursor,checkpoint,actual.value_or(-1));
                    Utils::ShowHudMessage("Обмен приостановлен: имущество не соответствует квитанции.");
                }
                ClearTrade();
                return;
            }
        }
        const auto before=ReceiptCount(m_world,*p,key,count>0);
        if (!before || (count<0 && *before<-count) || *before+count>INT32_MAX) return;
        const auto totalBefore=p->GetItemCountInInventory(form);
        if (totalBefore<0 || totalBefore+count<0 || totalBefore+count>INT32_MAX) return;
        Inventory::Entry item{};
        if (!m_world.GetModSystem().GetServerModId(id,item.BaseId)) return;
        item.Count=count;
        // Preserve ordinary STR player inventory synchronization. Only the
        // local display copy suppresses inventory events.
        bool changed=true;
        if (key.second.empty()) p->AddOrRemoveItem(item,true);
        else changed=ChangeVariant(*p,key,count);
        static const BSFixedString inventoryMenu("InventoryMenu");
        if ((nativeRequested && NativeMenuOpen()) || (UI::Get() && UI::Get()->GetMenuOpen(inventoryMenu)))
        {
            using Refresh=void(TESObjectREFR*,const TESBoundObject*);
            if (auto* refresh=Address<Refresh>(52849)) refresh(p,Cast<TESBoundObject>(form));
        }
        const auto after=ReceiptCount(m_world,*p,key,count>0);
        if (!changed || !after || *after!=*before+count ||
            p->GetItemCountInInventory(form)!=totalBefore+count || !w.Write(e.Stream,e.Cursor))
        {
            receiptFailure=true;
            receiptFailureCampaign=CampaignAccess::Mode()==Mode::Ready;
            spdlog::error("Trade receipt {} has uncertain local result; stop further trade until save recovery",e.Cursor);
            ClearTrade();
            Utils::ShowHudMessage("Обмен остановлен: ошибка квитанции. Не сохраняйте игру.");
            return;
        }
        nativeAwaiting=false;nativeReceiptDeadline={};
        lastConflict=UINT32_MAX;
        pollAt={}; return;
    }
    if (e.Op==TradeOp::Ready)
    {
        if (e.Stream && e.Cursor==w.Cursor() && (!w.Stream() || e.Stream==w.Stream()))
        { walletReady=true;readyCursor=e.Cursor;readyStream=e.Stream;lastConflict=UINT32_MAX; }
        return;
    }
    if (e.Op==TradeOp::RecoveryConflict)
    {
        walletReady=false;
        if (lastConflict!=w.Cursor())
        {
            lastConflict=w.Cursor();
            Utils::ShowHudMessage("Обмен приостановлен: сохранение или журнал несовместимы.");
        }
        ClearTrade();return;
    }
    if (e.Op==TradeOp::Error)
    {
        ResetNativeIntents();walletReady=false;pollAt={};
        Utils::ShowHudMessage("Сервер не подтвердил операцию обмена.");return;
    }
    if (e.Op==TradeOp::Offer || e.Op==TradeOp::Open)
    {
        if (!Ready(w) || (session && session!=e.Session) || m_promptOpen || m_showingProgress || m_observerCamera || (GetActive() && !ownsUI))
        {Send({TradeOp::Close,e.Peer,e.Session});return;}
        session=e.Session;peer=e.Peer;partner=e.Text.c_str();tradeOpen=e.Op==TradeOp::Open;
        if (tradeOpen && NativeEnabled())
        {
            if (m_pOverlay) m_pOverlay->ExecuteAsync("hideTrade");
            if (ownsUI && GetActive()) m_gameOverlayClient->SetUIVisible(false);
            ownsUI=false;nativeRequested=true;nativeOpened=false;nativeDeadline=Clock::now()+5s;
        }
        else {ownsUI=true;m_gameOverlayClient->SetUIVisible(true);}
    }
    else if (!session || e.Session!=session) return;
    else if (e.Op==TradeOp::Close) {ClearTrade();return;}
    else if (e.Op==TradeOp::Reset)
    {
        incomingStock.clear();stockRemaining=e.Count;stockComplete=stockRemaining==0;
        if (stockComplete) buffer.clear();
    }
    else if (e.Op==TradeOp::Entry)
    {
        if (!stockRemaining) return;
        auto* mod=ModManager::Get()->GetByName(e.Text.c_str());
        const std::string variant(e.Variant.data(),e.Variant.size());
        if (!mod || !mod->IsLoaded() || !e.Item.BaseId || !e.Count || e.Count>INT32_MAX ||
            !TradeProperties::Decode(variant) || (!nativeRequested && !variant.empty()) ||
            !incomingStock.emplace(LocalItem{mod->GetFormId(e.Item.BaseId),variant},int(e.Count)).second)
        {Send({TradeOp::Close,peer,session});ClearTrade();return;}
        if (!--stockRemaining) {buffer=std::move(incomingStock);stockComplete=true;}
    }
    dirty=true;
}
void OverlayService::UpdateTrade() noexcept
{
    MaintainShadow();
    HomePlacementClient::Update(m_world);
    if (receiptFailure || !m_transport.IsOnline() || !m_inGame || !CampaignAccess::GameReady())
    {
        walletReady=false;pollAt={};return;
    }
    if (session && nativeRequested)
    {
        if (nativeToken.load())
        {
            if (NativeMenuOpen())
            {
                if (!takeAllRegistered)
                {Utils::ShowHudMessage("Не удалось защитить штатное меню обмена.");Send({TradeOp::Close,peer,session});ClearTrade();return;}
                nativeOpened=true;
            }
            else if (nativeOpened || Clock::now()>=nativeDeadline)
            {Send({TradeOp::Close,peer,session});ClearTrade();return;}
        }
        else if (stockComplete)
        {
            if (!OpenNative(m_world))
            {Utils::ShowHudMessage("Не удалось открыть штатное меню обмена.");Send({TradeOp::Close,peer,session});ClearTrade();return;}
        }
        else if (Clock::now()>=nativeDeadline)
        {Send({TradeOp::Close,peer,session});ClearTrade();return;}
        if (stockComplete && dirty && nativeOpened)
        {
            if (!SetShadow(m_world,buffer))
            {Utils::ShowHudMessage("Обмен закрыт: ошибка отображения буфера.");Send({TradeOp::Close,peer,session});ClearTrade();return;}
            dirty=false;
        }
        if (nativeAwaiting && Clock::now()>=nativeReceiptDeadline)
        {
            Utils::ShowHudMessage("Обмен закрыт: ожидание квитанции. Остаток будет возвращён.");
            Send({TradeOp::Close,peer,session});ClearTrade();return;
        }
        if (nativeOpened && stockComplete && !dirty && !nativeIntents.empty())
        {
            Wallet w;
            if (w.Load(m_world) && Ready(w))
            {
                auto& intent=nativeIntents.front();
                if (intent.Take)
                {
                    const auto found=buffer.find(intent.Item);
                    const auto available=found==buffer.end()?0u:static_cast<uint32_t>(found->second);
                    intent.Count=std::min(intent.Count,available);
                }
                if (!intent.Count) nativeIntents.pop_front();
                else
                {
                    const auto chunk=std::min(intent.Count,9999u);
                    SubmitTransfer(m_world,w,intent.Item,chunk,intent.Take);
                    if (!nativeAwaiting)
                    {
                        // A rejected local operation must not silently take a different item.
                        nativeIntents.pop_front();
                        Utils::ShowHudMessage("Часть запрошенных предметов недоступна для обмена.");
                    }
                    else
                    {
                        intent.Count-=chunk;
                        if (!intent.Count) nativeIntents.pop_front();
                    }
                }
            }
        }
    }
    else if (session && !GetActive())
    {Send({TradeOp::Close,peer,session});ClearTrade();}
    if (session && !nativeRequested && dirty && (!tradeOpen || stockComplete) && m_pOverlay)
    {
        dirty=false;
        auto* p=PlayerCharacter::Get();
        auto args=CefListValue::Create();
        args->SetInt(0,session); args->SetString(1,partner);args->SetBool(2,tradeOpen);
        args->SetString(3,tradeOpen?Rows(Pocket(m_world,*p)):CefString("[]"));args->SetString(4,Rows(PlainBuffer()));
        m_pOverlay->ExecuteAsync("partyTrade",args);
    }
    const auto now=Clock::now();
    if (now<pollAt) return;
    pollAt=now+(session?1s:5s);
    Wallet w;
    if (w.Load(m_world)) Send({TradeOp::Poll,peer,session,w.Cursor(),0,w.Stream()});
}
