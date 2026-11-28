#include <Misc/BSScript.h>
#include <Misc/NativeFunction.h>

#include <World.h>
#include <Events/PapyrusFunctionRegisterEvent.h>
#include <Forms/TESForm.h>
#include <Misc/GameVM.h>
#include <Actor.h>
#include <PlayerCharacter.h>
#include <Games/ActorExtension.h>
#include <Games/PapyrusFunctions.h>
#include <mutex>
#include <cstring>
#include <atomic>
#include <map>
#include <cmath>
#include <Forms/ActorValueInfo.h>
#include <Forms/TESQuest.h>
#include <UniqueQuestReward.h>
#include <CampaignRewardPolicy.h>
#include <array>
#include <Services/ScriptedTravelClient.h>
#include <Services/QuestPhysicalStageClient.h>
#include <AI/Movement/PlayerControls.h>
#include <Games/Memory.h>
#include <HomeCityPurchase.h>
#include <Services/CityHomeClient.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESFaction.h>

TP_THIS_FUNCTION(TRegisterPapyrusFunction, void, BSScript::IVirtualMachine, NativeFunction*);
TP_THIS_FUNCTION(TBindEverythingToScript, void, BSScript::IVirtualMachine*);
TP_THIS_FUNCTION(TSignaturesMatch, bool, BSScript::NativeFunction, BSScript::NativeFunction*);
TP_THIS_FUNCTION(TCompareVariables, int64_t, void, BSScript::Variable*, BSScript::Variable*);

TRegisterPapyrusFunction* RealRegisterPapyrusFunction = nullptr;
TBindEverythingToScript* RealBindEverythingToScript = nullptr;
TSignaturesMatch* RealSignaturesMatch = nullptr;
TCompareVariables* RealCompareVariables = nullptr;

namespace
{
std::mutex wordNativeMutex;
void* teachWordAddress{};
bool teachWordLong{};
}
bool TeachCampaignWord(TESForm* word) noexcept
{
    if (!CampaignRewardPolicy::SupplementWords) return false;
    if (!word||static_cast<uint8_t>(word->formType)!=0x76) return false;
    if (word->flags&0x40u) return true;
    void* address{};
    bool longSignature{};
    {
        std::lock_guard lock(wordNativeMutex);
        address=teachWordAddress;longSignature=teachWordLong;
    }
    auto* vm=GameVM::Get();
    if (!address||!vm||!vm->virtualMachine||vm->inactive) return false;
    // Static native methods take a null StaticFunctionTag. The long form
    // additionally takes VM and stack id.
    if (longSignature)
        reinterpret_cast<void(__fastcall*)(BSScript::IVirtualMachine*,uint32_t,void*,TESForm*)>(address)
            (vm->virtualMachine,0,nullptr,word);
    else
        reinterpret_cast<void(__fastcall*)(void*,TESForm*)>(address)(nullptr,word);
    return (word->flags&0x40u)!=0;
}

// Skyrim 1.6.1170: CommonLibSSE-NG Stack::top/StackFrame fields and
// STR's existing NativeFunctionBase::Call (AE ID 104651).
// Inspect only the exact Actor.ModActorValue registration and its immediate
// MQKillDragonScript.DeathSequence caller; never alter the VM frame.
#ifdef TP_SKYRIM_1_6_1170
TP_THIS_FUNCTION(TDragonNativeCall, BSScript::CallResult, BSScript::NativeFunctionBase,
                 void*, void*, BSScript::IVirtualMachine*, bool);
TDragonNativeCall* realDragonNativeCall{};
std::atomic<const void*> modActorValueNative{},addItemNative{},removeItemNative{},moveToNative{};
std::atomic<const void*> cityFactionOwner{};
std::atomic_bool cityFactionOwnerLong{};
thread_local uint32_t cityChargeContext{};
uint32_t CityHomeClient::ChargeContext() noexcept {return cityChargeContext;}
bool CityHomeClient::NativeReady() noexcept
{return realDragonNativeCall&&removeItemNative.load(std::memory_order_acquire)&&cityFactionOwner.load(std::memory_order_acquire);}
thread_local uint32_t nativeQuestRemovalQuest{};
uint32_t NativeQuestRemovalQuest() noexcept {return nativeQuestRemovalQuest;}
thread_local uint32_t nativeUniqueRewardQuest{};
thread_local uint16_t nativeUniqueRewardStage{};
thread_local uint32_t nativeUniqueRewardInstance{};
uint32_t NativeUniqueRewardQuest() noexcept {return nativeUniqueRewardQuest;}
uint16_t NativeUniqueRewardStage() noexcept {return nativeUniqueRewardStage;}
uint32_t NativeUniqueRewardInstance() noexcept {return nativeUniqueRewardInstance;}
namespace
{
enum class DragonAwardState { NativePending, NativePaid, SupplementPending, SupplementPaid, Retired };
std::mutex dragonAwardMutex;
std::map<uint32_t,DragonAwardState> dragonAwards;
}
bool DragonAwardHookReady() noexcept {return CampaignRewardPolicy::SupplementDragonSouls&&realDragonNativeCall&&modActorValueNative.load(std::memory_order_acquire);}
void ResetCampaignDragonAwards() noexcept
{
    std::lock_guard lock(dragonAwardMutex);dragonAwards.clear();
}
void RefreshCampaignDragonAwards() noexcept
{
    std::lock_guard lock(dragonAwardMutex);
    for (auto& [form,state]:dragonAwards)
        if (auto* actor=Cast<Actor>(TESForm::GetById(form));actor&&!actor->IsDead())
            state=DragonAwardState::Retired;
}
void RememberCampaignDragonAward(uint32_t form) noexcept
{
    try {std::lock_guard lock(dragonAwardMutex);dragonAwards.try_emplace(form,DragonAwardState::SupplementPaid);}
    catch (...) {}
}
int ApplyCampaignDragonAward(uint32_t form) noexcept
{
    try
    {
        auto* player=PlayerCharacter::Get();
        if (!player||!DragonAwardHookReady()) return 0;
        const float before=player->GetActorValue(ActorValueInfo::kDragonSouls);
        if (!std::isfinite(before)||before<0.f||before+1.f==before) return 0;
        {
            std::lock_guard lock(dragonAwardMutex);
            const auto it=dragonAwards.find(form);
            if (it!=dragonAwards.end())
            {
                if (it->second==DragonAwardState::NativePaid) return 1;
                if (it->second==DragonAwardState::SupplementPaid) return 2;
                return 0;
            }
            dragonAwards[form]=DragonAwardState::SupplementPending;
        }
        player->actorValueOwner.ModValue(ActorValueInfo::kDragonSouls,1.f);
        const bool applied=player->GetActorValue(ActorValueInfo::kDragonSouls)==before+1.f;
        {
            std::lock_guard lock(dragonAwardMutex);
            if (applied) dragonAwards[form]=DragonAwardState::SupplementPaid;
            else dragonAwards.erase(form);
        }
        return applied?2:0;
    }
    catch (...) {return 0;}
}
static bool BeginNativeDragonAward(uint32_t form,bool& suppress) noexcept
{
    try
    {
        std::lock_guard lock(dragonAwardMutex);
        const auto it=dragonAwards.find(form);
        if (it!=dragonAwards.end())
        {
            suppress=it->second==DragonAwardState::SupplementPaid||it->second==DragonAwardState::SupplementPending;
            return false;
        }
        dragonAwards.emplace(form,DragonAwardState::NativePending);
        return true;
    }
    catch (...) {return false;}
}
static void FinishNativeDragonAward(uint32_t form,bool paid) noexcept
{
    std::lock_guard lock(dragonAwardMutex);
    const auto it=dragonAwards.find(form);
    if (it!=dragonAwards.end()&&it->second==DragonAwardState::NativePending)
    {
        if (paid) it->second=DragonAwardState::NativePaid;
        else dragonAwards.erase(it);
    }
}
struct DragonFrameView
{
    BSScript::Stack* Parent;
    DragonFrameView* Previous;
    BSScript::IFunction* Function;
    void* ObjectType;
    uint32_t Ip,Pad24;
    BSScript::Variable Self;
    uint32_t Size;
    uint8_t Pad3C[4];
};
static_assert(offsetof(DragonFrameView,Previous)==0x08);
static_assert(offsetof(DragonFrameView,Self)==0x28);
static_assert(offsetof(DragonFrameView,Size)==0x38);
static_assert(sizeof(DragonFrameView)==0x40);
struct DragonStackView
{
    uint8_t Pad[0x60];
    DragonFrameView* Top;
};
static_assert(offsetof(DragonStackView,Top)==0x60);

static Actor* DragonFrameActor(const BSScript::Variable* variable)
{
    auto* vm=GameVM::Get();
    auto* policy=vm&&vm->virtualMachine?vm->virtualMachine->GetObjectHandlePolicy():nullptr;
    auto* object=variable?variable->GetObject():nullptr;
    if (!policy||!object) return nullptr;
    const auto handle=object->GetHandle();
    if (!handle||!policy->HandleIsType(static_cast<uint32_t>(Actor::Type),handle)||
        !policy->IsHandleObjectAvailable(handle)) return nullptr;
    return policy->GetObjectForHandle<Actor>(handle);
}
namespace
{
constexpr auto questFormType=static_cast<uint32_t>(FormType::QuestItem);
constexpr const char* houseScript="HousePurchaseScript";
constexpr const char* houseFragments="QF_HousePurchase_000A7B33";
TESQuest* HouseQuest()
{return Cast<TESQuest>(TESForm::GetById(HomeCityPurchase::Quest));}
uint64_t HouseHandle(BSScript::IVirtualMachine* vm,TESQuest* quest)
{
    auto* p=vm?vm->GetObjectHandlePolicy():nullptr;
    const auto h=p&&quest?p->GetHandleForObject(questFormType,quest):0;
    return h&&p->HandleIsType(questFormType,h)&&p->IsHandleObjectAvailable(h)&&
        p->GetObjectForHandle(questFormType,h)==quest?h:0;
}
// CommonLibSSE-NG Object::DecRef AE 104253, Object::Dtor AE 104246.
// FindBoundObject returns an owning smart pointer, not a borrowed raw pointer.
struct BoundScript
{
    BSScript::Object* Value{};
    ~BoundScript()
    {
        if (!Value) return;
        TP_THIS_FUNCTION(TDecRef,uint32_t,BSScript::Object);
        POINTER_SKYRIMSE(TDecRef,dec,104253);
        if (!TiltedPhoques::ThisCall(dec,Value))
        {
            TP_THIS_FUNCTION(TObjectDtor,void,BSScript::Object);
            POINTER_SKYRIMSE(TObjectDtor,dtor,104246);
            TiltedPhoques::ThisCall(dtor,Value);Memory::Free(Value);
        }
    }
};
struct EmptyHouseArguments final:BSScript::IFunctionArguments
{
    static void operator delete(void* p) noexcept {Memory::Free(p);}
    bool Prepare(Statement& out) const noexcept override {out.SetSize(0);return out.size==0;}
};
// The existing verified simple furniture fragments take one ObjectReference
// argument but do not use akSpeaker after its initial cast. A null reference
// is packed as Papyrus None (CommonLib PackHandle with null source).
// Do not use this argument adapter for arbitrary TIFs or speaker-dependent mods.
struct CityTifArguments final:BSScript::IFunctionArguments
{
    static void operator delete(void* p) noexcept {Memory::Free(p);}
    bool Prepare(Statement& out) const noexcept override
    {
        static_assert(offsetof(Statement,vars)==8&&offsetof(Statement,size)==0x18);
        out.SetSize(1);
        if(!out.vars||out.size!=1||out.capacity<1)return false;
        out.vars[0].Clear();
        return true;
    }
};
bool FrameIsHouseQuest(const DragonFrameView* frame,BSScript::IObjectHandlePolicy* policy)
{
    if (!frame||!frame->Function||!policy) return false;
    auto* object=frame->Self.GetObject();
    const auto h=object?object->GetHandle():0;
    return h&&policy->HandleIsType(questFormType,h)&&policy->IsHandleObjectAvailable(h)&&
        policy->GetObjectForHandle(questFormType,h)==HouseQuest();
}
// Exact native REFR operations used by the non-replaceable TIF fragments.
struct DecorRefs{TESObjectREFR* Marker{};TESObjectREFR* Old{};};
DecorRefs CityDecorRefs(const HomeCityPurchase::House& house)
{
    auto get=[](uint32_t id)->TESObjectREFR*{
        auto* ref=Cast<TESObjectREFR>(TESForm::GetById(id));
        return ref&&ref->baseForm&&ref->baseForm->formID==0x3B&&
            !ref->IsDeleted()&&!ref->IsTemporary()?ref:nullptr;
    };
    return {get(house.Marker),house.OldMarker?get(house.OldMarker):nullptr};
}
CityHomeClient::Local ReadCityDecor(const HomeCityPurchase::House& house)
{
    const auto refs=CityDecorRefs(house);
    if(!refs.Marker||(house.OldMarker&&!refs.Old))return CityHomeClient::Local::Unknown;
    if(refs.Marker->IsDisabled()!=house.Hide)return CityHomeClient::Local::Unowned;
    if(refs.Old&&!refs.Old->IsDisabled())return CityHomeClient::Local::Partial;
    return CityHomeClient::Local::Owned;
}
bool ApplyCityDecor(const HomeCityPurchase::House& house)
{
    if(ReadCityDecor(house)!=CityHomeClient::Local::Unowned)return false;
    const auto refs=CityDecorRefs(house);
    if(!refs.Marker||(house.OldMarker&&!refs.Old))return false;
    if(house.Hide)refs.Marker->Disable();else refs.Marker->Enable();
    if(refs.Old&&!refs.Old->IsDisabled())refs.Old->Disable();
    return true; // read back on the normal main-thread update
}
// Only a player's exact native RemoveItem within PurchaseHouse, below the
// verified QF branch and with the matching CELL argument, may skip a debit.
uint32_t CityCharge(void* stackArgument)
{
    if(!stackArgument||!World::Exists()||World::Get().IsLoadingSave())return 0;
    auto* stack=*static_cast<DragonStackView**>(stackArgument);
    auto* frame=stack?stack->Top:nullptr;
    auto* caller=frame?frame->Previous:nullptr;
    auto* vm=GameVM::Get();
    auto* policy=vm&&vm->virtualMachine&&!vm->inactive?vm->virtualMachine->GetObjectHandlePolicy():nullptr;
    if(!frame||!frame->Parent||!caller||caller->Parent!=frame->Parent||!policy||
        !PlayerCharacter::Get()||DragonFrameActor(&frame->Self)!=PlayerCharacter::Get()||
        !caller->Function)return 0;
    const auto* script=caller->Function->GetObjectTypeName().AsAscii();
    const auto* name=caller->Function->GetName().AsAscii();
    if(!script||!name)return 0;
    // INFO (0x4C), actual bound script + original plugin identity. This
    // observes the buyer's genuine TIF payment, not any unrelated debit.
    constexpr uint32_t infoType=0x4C;
    if(auto* obj=caller->Self.GetObject();obj)
    {
        const auto h=obj->GetHandle();
        if(h&&policy->HandleIsType(infoType,h)&&policy->IsHandleObjectAvailable(h))
        {
            const auto* form=static_cast<const TESForm*>(policy->GetObjectForHandle(infoType,h));
            if(form&&static_cast<uint32_t>(form->formType)==infoType)
                if(const auto* op=HomeCityPurchase::Find(CityHomeClient::OperationForInfo(form->formID));
                    op&&op->Parent&&!_stricmp(script,op->Variable)&&!_stricmp(name,op->Fragment))
                    return op->Cell;
        }
    }
    if(!FrameIsHouseQuest(caller,policy)||caller->Size<4||
        _stricmp(script,houseScript)||_stricmp(name,"PurchaseHouse")||
        caller->Function->GetParamCount()!=4)return 0;
    auto* args=reinterpret_cast<BSScript::StackFrame*>(caller);
    auto* arg=args->GetStackFrameVariable(0,args->GetPageForFrame());
    auto* object=arg?arg->GetObject():nullptr;
    const auto handle=object?object->GetHandle():0;
    constexpr uint32_t cellType=60;
    if(!handle||!policy->HandleIsType(cellType,handle)||!policy->IsHandleObjectAvailable(handle))return 0;
    const auto* cell=static_cast<const TESForm*>(policy->GetObjectForHandle(cellType,handle));
    const auto* house=cell?HomeCityPurchase::Find(cell->formID):nullptr;
    if(!house||static_cast<uint32_t>(cell->formType)!=cellType)return 0;
    auto* fragment=caller->Previous;
    if(!fragment||fragment->Parent!=frame->Parent||!FrameIsHouseQuest(fragment,policy))return 0;
    const auto* f=fragment->Function->GetObjectTypeName().AsAscii();
    const auto* method=fragment->Function->GetName().AsAscii();
    return f&&method&&!_stricmp(f,houseFragments)&&!_stricmp(method,house->Fragment)?house->Cell:0;
}
}
CityHomeClient::Local CityHomeClient::ReadNative(uint32_t id) noexcept
{
    try
    {
        const auto* house=HomeCityPurchase::Find(id);
        if(house&&house->Parent)
            return ReadNative(house->Parent)==Local::Owned?ReadCityDecor(*house):Local::Unknown;
        auto* gameVM=GameVM::Get();auto* quest=HouseQuest();
        auto* cell=TESForm::GetById(id);
        if(!house||!quest||quest->IsStopped()||!gameVM||!gameVM->virtualMachine||gameVM->inactive||
            !cell||static_cast<uint32_t>(cell->formType)!=60||!NativeReady())return Local::Unknown;
        auto* vm=gameVM->virtualMachine;const auto handle=HouseHandle(vm,quest);
        BoundScript object;
        if(!handle||!vm->FindBoundObject(handle,houseScript,object.Value)||!object.Value)return Local::Unknown;
        BSScript::Variable value;
        if(!vm->GetPropertyValue(object.Value,house->Variable,value)||
            value.type!=BSScript::Variable::kInteger||value.data.i<0)return Local::Unknown;
        auto* address=cityFactionOwner.load(std::memory_order_acquire);
        if(!address)return Local::Unknown;
        const auto* owner=cityFactionOwnerLong.load(std::memory_order_acquire)?
            reinterpret_cast<TESFaction*(__fastcall*)(BSScript::IVirtualMachine*,uint32_t,const TESForm*)>(const_cast<void*>(address))(vm,0,cell):
            reinterpret_cast<TESFaction*(__fastcall*)(const TESForm*)>(const_cast<void*>(address))(cell);
        const bool faction=owner&&owner->formID==HomeCityPurchase::PlayerFaction;
        if(value.data.i>0&&faction)return Local::Owned;
        if(value.data.i==0&&!faction)return Local::Unowned;
        return Local::Partial;
    }
    catch (...) {return Local::Unknown;}
}
CityHomeClient::TifDispatch CityHomeClient::TryTif(uint32_t id) noexcept
{
    // Do not create/bind an empty TIF object: its VMAD properties and quest
    // context must have been supplied by Skyrim itself.
    try
    {
        const auto* op=HomeCityPurchase::Find(id);
        if(!op||!op->Parent||ReadNative(id)!=Local::Unowned||!NativeReady())
            return TifDispatch::Rejected;
        auto* gameVM=GameVM::Get();
        auto* info=TESForm::GetById(RuntimeInfo(id));
        constexpr uint32_t infoType=0x4C;
        if(!gameVM||!gameVM->virtualMachine||gameVM->inactive||!info||
            static_cast<uint32_t>(info->formType)!=infoType||OperationForInfo(info->formID)!=id)
            return TifDispatch::Unavailable;
        auto* vm=gameVM->virtualMachine;
        auto* policy=vm->GetObjectHandlePolicy();
        const auto handle=policy?policy->GetHandleForObject(infoType,info):0;
        if(!handle||!policy->HandleIsType(infoType,handle)||!policy->IsHandleObjectAvailable(handle)||
            policy->GetObjectForHandle(infoType,handle)!=info)
        {
            spdlog::info("STM TIF {:X}: native INFO handle unavailable before dispatch",info->formID);
            return TifDispatch::Unavailable;
        }
        BoundScript object;
        if(!vm->FindBoundObject(handle,op->Variable,object.Value)||!object.Value||object.Value->GetHandle()!=handle)
        {
            spdlog::info("STM TIF {:X}: script {} not bound before dispatch",info->formID,op->Variable);
            return TifDispatch::Unavailable;
        }
        BSFixedString scope(op->Variable),method(op->Fragment);void* callback=nullptr;
        auto* storage=Memory::Allocate(sizeof(CityTifArguments));
        if(!storage)return TifDispatch::Rejected;
        auto* args=::new(storage) CityTifArguments;
        const bool accepted=vm->DispatchMethodCall2(handle,scope,method,args,callback);
        spdlog::info("STM TIF {:X}: {}.{} dispatch accepted={}",info->formID,op->Variable,op->Fragment,accepted);
        return accepted?TifDispatch::Dispatched:TifDispatch::Rejected;
    }
    catch (...) {return TifDispatch::Rejected;}
}

bool CityHomeClient::DispatchNative(uint32_t id) noexcept
{
    try
    {
        const auto* house=HomeCityPurchase::Find(id);
        if(house&&house->Parent)
            return ReadNative(house->Parent)==Local::Owned&&ApplyCityDecor(*house);
        auto* gameVM=GameVM::Get();auto* quest=HouseQuest();
        if(!house||!quest||!gameVM||!gameVM->virtualMachine||gameVM->inactive||
            ReadNative(id)!=Local::Unowned)return false;
        auto* vm=gameVM->virtualMachine;const auto handle=HouseHandle(vm,quest);
        BoundScript object;
        if(!handle||!vm->FindBoundObject(handle,houseFragments,object.Value)||!object.Value)return false;
        BSFixedString scope(houseFragments),method(house->Fragment);void* callback=nullptr;
        auto* storage=Memory::Allocate(sizeof(EmptyHouseArguments));
        if(!storage)return false;
        auto* args=::new(storage) EmptyHouseArguments;
        return vm->DispatchMethodCall2(handle,scope,method,args,callback);
    }
    catch (...) {return false;}
}
static uint32_t NativeDragonSource(void* stackArgument,BSScript::Variable*& reward)
{
    if (!stackArgument) return 0;
    auto* stack=*static_cast<DragonStackView**>(stackArgument);
    auto* frame=stack?stack->Top:nullptr;
    auto* caller=frame?frame->Previous:nullptr;
    if (!frame||!frame->Parent||frame->Size<2||!caller||!caller->Parent||
        !caller->Function||caller->Size<1) return 0;
    const auto* scope=caller->Function->GetObjectTypeName().AsAscii();
    const auto* name=caller->Function->GetName().AsAscii();
    if (!scope||!name||_stricmp(scope,"MQKillDragonScript")||
        _stricmp(name,"DeathSequence")||caller->Function->GetParamCount()<1) return 0;
    auto* nativeFrame=reinterpret_cast<BSScript::StackFrame*>(frame);
    auto* callerFrame=reinterpret_cast<BSScript::StackFrame*>(caller);
    const auto page=nativeFrame->GetPageForFrame();
    auto* av=nativeFrame->GetStackFrameVariable(0,page);
    auto* amount=nativeFrame->GetStackFrameVariable(1,page);
    if (!av||av->type!=BSScript::Variable::kString||!av->data.s||
        _stricmp(av->data.s,"DragonSouls")||!amount||
        amount->type!=BSScript::Variable::kFloat||amount->data.f!=1.f) return 0;
    auto* player=PlayerCharacter::Get();
    if (!player||DragonFrameActor(&frame->Self)!=player) return 0;
    auto* dragon=DragonFrameActor(callerFrame->GetStackFrameVariable(0,callerFrame->GetPageForFrame()));
    if (!dragon||!dragon->IsDragon()||!dragon->IsDead()||dragon->IsTemporary()) return 0;
    reward=amount;return dragon->formID;
}
namespace
{
// Exact native registrations, not script or quest names. ObjectReference.psc
// declares these methods in SE. TranslateToRef calls native TranslateTo.
constexpr const char* physicalMethods[]={"Enable","Disable","EnableNoWait","DisableNoWait",
    "Lock","SetOpen","SetDestroyed","MoveTo","MoveToMyEditorLocation","MoveToNode",
    "MoveToInteractionLocation","SetPosition","SetAngle","SetScale","SetMotionType",
    "TranslateTo","PlayAnimation","PlayAnimationAndWait"};
std::array<std::atomic<const void*>,std::size(physicalMethods)> physicalNatives{};
struct PhysicalCall
{
    uint32_t Form{},Instance{};uint16_t Stage{};uint64_t Epoch{};
    QuestPhysicalStage::Effect Effect{};
};
std::optional<PhysicalCall> ObservePhysicalCall(const void* function,void* stackArgument)
{
    size_t index=std::size(physicalMethods);
    for(size_t i=0;i<std::size(physicalMethods);++i)
        if(function==physicalNatives[i].load(std::memory_order_acquire)){index=i;break;}
    if(index==std::size(physicalMethods)||!stackArgument||!World::Exists()||World::Get().IsLoadingSave()) return {};
    auto* stack=*static_cast<DragonStackView**>(stackArgument);
    auto* frame=stack?stack->Top:nullptr;
    auto* gameVM=GameVM::Get();
    auto* policy=gameVM&&gameVM->virtualMachine&&!gameVM->inactive?gameVM->virtualMachine->GetObjectHandlePolicy():nullptr;
    if(!frame||!frame->Parent||!policy) return {};
    auto* self=frame->Self.GetObject();
    const auto handle=self?self->GetHandle():0;
    if(!handle||!policy->IsHandleObjectAvailable(handle)) return {};
    using E=QuestPhysicalStage::Effect;
    E effect=E::None;
    const bool moves=index>=7&&index<=15;
    if(moves&&DragonFrameActor(&frame->Self)==PlayerCharacter::Get()&&PlayerCharacter::Get())
        effect=E::PlayerTravel;
    else
    {
        constexpr uint32_t refrType=61;
        if(!policy->HandleIsType(refrType,handle)) return {};
        auto* ref=static_cast<TESObjectREFR*>(policy->GetObjectForHandle(refrType,handle));
        if(!ref||static_cast<uint32_t>(ref->formType)!=refrType||ref->IsDeleted()||!ref->baseForm) return {};
        const auto type=static_cast<uint8_t>(ref->baseForm->formType);
        if(type!=0x18&&type!=0x19&&type!=0x1C&&type!=0x1D&&type!=0x22&&type!=0x23&&type!=0x24) return {};
        // Ignore no-op enable/disable, and do not classify actor, loose
        // inventory, lights or sound/effect records as geometry.
        if((index==0||index==2)&&!ref->IsDisabled()) return {};
        if((index==1||index==3)&&ref->IsDisabled()) return {};
        effect=E::Geometry;
    }
    // Walk through synchronous helpers/latent continuation frames to the
    // real QF fragment. Do not assign arbitrary quest methods to a stage.
    for(auto* caller=frame->Previous;caller&&caller->Parent==frame->Parent;caller=caller->Previous)
    {
        if(!caller->Function) continue;
        const auto* script=caller->Function->GetObjectTypeName().AsAscii();
        const auto* method=caller->Function->GetName().AsAscii();
        if(!script||!method||_strnicmp(script,"qf_",3)||_strnicmp(method,"fragment_",9)) continue;
        auto* object=caller->Self.GetObject();const auto qh=object?object->GetHandle():0;
        constexpr uint32_t qt=static_cast<uint32_t>(FormType::QuestItem);
        if(!qh||!policy->HandleIsType(qt,qh)||!policy->IsHandleObjectAvailable(qh)) return {};
        auto* quest=static_cast<TESQuest*>(policy->GetObjectForHandle(qt,qh));
        if(!quest||quest->IsTemporary()||(quest->flags&TESQuest::AllowRepeatStages)) return {};
        return PhysicalCall{quest->formID,quest->currentInstanceID,quest->currentStage,ScriptedTravelEpoch(),effect};
    }
    return {};
}
}

static std::optional<ScriptedMoveObservation> NativeScriptedMove(void* stackArgument)
{
    if (!stackArgument||!World::Exists()||World::Get().IsLoadingSave()||
        BSInputEnableManager::IsMovementControlsEnabled()) return {};
    auto* stack=*static_cast<DragonStackView**>(stackArgument);
    auto* frame=stack?stack->Top:nullptr;
    auto* caller=frame?frame->Previous:nullptr;
    if (!frame||!frame->Parent||frame->Size<5||!caller||!caller->Parent||!caller->Function||
        DragonFrameActor(&frame->Self)!=PlayerCharacter::Get()||!PlayerCharacter::Get()) return {};
    auto* gameVM=GameVM::Get();
    auto* policy=gameVM&&gameVM->virtualMachine&&!gameVM->inactive?gameVM->virtualMachine->GetObjectHandlePolicy():nullptr;
    auto* self=caller->Self.GetObject();
    const auto handle=self?self->GetHandle():0;
    constexpr auto qt=static_cast<uint32_t>(FormType::QuestItem);
    if (!policy||!handle||!policy->HandleIsType(qt,handle)||!policy->IsHandleObjectAvailable(handle)) return {};
    auto* quest=static_cast<TESQuest*>(policy->GetObjectForHandle(qt,handle));
    if (!quest||quest->IsTemporary()||quest->IsStopped()||!(quest->flags&TESQuest::RunOnce)||
        (quest->flags&(TESQuest::Failed|TESQuest::AllowRepeatStages))||!quest->currentStage) return {};
    auto* f=reinterpret_cast<BSScript::StackFrame*>(frame);
    const auto page=f->GetPageForFrame();
    auto* arg=f->GetStackFrameVariable(0,page);
    auto* object=arg?arg->GetObject():nullptr;
    const auto targetHandle=object?object->GetHandle():0;
    // Papyrus ObjectReference type REFR (61), not a dynamic actor target.
    constexpr uint32_t refType=61;
    if (!targetHandle||!policy->HandleIsType(refType,targetHandle)||!policy->IsHandleObjectAvailable(targetHandle)) return {};
    auto* target=static_cast<TESObjectREFR*>(policy->GetObjectForHandle(refType,targetHandle));
    if (!target||target->IsTemporary()||static_cast<uint32_t>(target->formType)!=refType||
        target->IsDeleted()||!target->GetParentCellEx()) return {};
    // Only the standard marker MoveTo without offsets is observed here.
    for (uint32_t i=1;i<=3;++i)
    {
        auto* offset=f->GetStackFrameVariable(i,page);
        if (!offset||offset->type!=BSScript::Variable::kFloat||offset->data.f!=0.f) return {};
    }
    const auto* name=caller->Function->GetName().AsAscii();
    const auto* script=caller->Function->GetObjectTypeName().AsAscii();
    if (!name||!script||!*script) return {};
    ScriptedMoveObservation result;
    result.Quest=quest->formID;result.Target=target->formID;result.Instance=quest->currentInstanceID;
    result.Stage=quest->currentStage;result.Epoch=ScriptedTravelEpoch();
    result.Script=script;result.WasAtTarget=AtScriptedTravelTarget(target);
    // Stage fragments and their quest methods already execute on both
    // clients through native stage replay. Only a missing external sleep
    // trigger is reproduced, never arbitrary methods from a network string.
    if (!_stricmp(name,"OnSleepStart")&&caller->Function->GetParamCount()==2&&caller->Size>=2)
    {
        auto* cf=reinterpret_cast<BSScript::StackFrame*>(caller);
        const auto cp=cf->GetPageForFrame();
        auto* start=cf->GetStackFrameVariable(0,cp);
        auto* end=cf->GetStackFrameVariable(1,cp);
        if (start&&end&&start->type==BSScript::Variable::kFloat&&end->type==BSScript::Variable::kFloat&&
            std::isfinite(start->data.f)&&std::isfinite(end->data.f)&&start->data.f>=0.f&&
            end->data.f>start->data.f&&end->data.f-start->data.f<=24.5f)
        {result.Sleep=true;result.Start=start->data.f;result.End=end->data.f;}
    }
    return result;
}

namespace
{
// Engine owns the IFunctionArguments after DispatchMethodCall2. Allocate
// on the Skyrim heap and use the exact bool-returning callback ABI.
struct SleepTravelArguments final:BSScript::IFunctionArguments
{
    float Start,End;
    SleepTravelArguments(float start,float end):Start(start),End(end){}
    static void operator delete(void* p) noexcept {Memory::Free(p);}
    bool Prepare(Statement& out) const noexcept override
    {
        static_assert(offsetof(Statement,vars)==8&&offsetof(Statement,size)==0x18);
        out.SetSize(2);
        if (!out.vars||out.size!=2||out.capacity<2) return false;
        out.vars[0].Set(Start);out.vars[1].Set(End);
        return true;
    }
};
}
bool DispatchScriptedSleep(uint32_t questId,const std::string& script,float start,float end) noexcept
{
    try
    {
        auto* vm=GameVM::Get();
        auto* quest=Cast<TESQuest>(TESForm::GetById(questId));
        if (!vm||!vm->virtualMachine||vm->inactive||!quest||quest->IsStopped()||
            !std::isfinite(start)||!std::isfinite(end)||end<=start||end-start>24.5f) return false;
        auto* policy=vm->virtualMachine->GetObjectHandlePolicy();
        constexpr auto qt=static_cast<uint32_t>(FormType::QuestItem);
        const auto handle=policy?policy->GetHandleForObject(qt,quest):0;
        if (!handle||!policy->HandleIsType(qt,handle)||!policy->IsHandleObjectAvailable(handle)||
            policy->GetObjectForHandle(qt,handle)!=quest) return false;
        BSFixedString scope(script.c_str()),method("OnSleepStart");
        void* callback=nullptr;
        auto* storage=Memory::Allocate(sizeof(SleepTravelArguments));
        if (!storage) return false;
        auto* args=::new(storage) SleepTravelArguments(start,end);
        return vm->virtualMachine->DispatchMethodCall2(handle,scope,method,args,callback);
    }
    catch (...) {return false;}
}

namespace
{
struct SleepBonusArguments final:BSScript::IFunctionArguments
{
    static void operator delete(void* p) noexcept {Memory::Free(p);}
    bool Prepare(Statement& out) const noexcept override
    {
        out.SetSize(1);
        if(!out.vars||out.size!=1||out.capacity<1)return false;
        out.vars[0].Set(false);
        return true;
    }
};
}
bool DispatchPersonalSleepBonus() noexcept
{
    // Do not broadcast sleep events: DBEntranceQuest and other story
    // listeners must only see the initiating native sleep.
    try
    {
        auto* game=GameVM::Get();
        auto* quest=Cast<TESQuest>(TESForm::GetById(0xFC1A2));
        if(!game||!game->virtualMachine||game->inactive||!quest||quest->IsStopped())return false;
        auto* vm=game->virtualMachine;
        const auto handle=HouseHandle(vm,quest);
        BoundScript object;
        constexpr const char* script="PlayerSleepQuestScript";
        if(!handle||!vm->FindBoundObject(handle,script,object.Value)||!object.Value||object.Value->GetHandle()!=handle)return false;
        BSFixedString scope(script),method("OnSleepStop");
        void* callback=nullptr;
        auto* storage=Memory::Allocate(sizeof(SleepBonusArguments));
        if(!storage)return false;
        return vm->DispatchMethodCall2(handle,scope,method,::new(storage) SleepBonusArguments(),callback);
    }
    catch (...) {return false;}
}

void RecordNativeDragonAbsorption(uint32_t form) noexcept;
BSScript::CallResult TP_MAKE_THISCALL(HookDragonNativeCall, BSScript::NativeFunctionBase,
    void* stackArgument, void* logger, BSScript::IVirtualMachine* vm, bool flag)
{
    uint32_t dragon{};
    float before{};
    BSScript::Variable* reward{};
    bool tracked=false,suppress=false;
    if (CampaignRewardPolicy::SupplementDragonSouls&&
        apThis==modActorValueNative.load(std::memory_order_acquire)&&World::Exists()&&
        !World::Get().IsLoadingSave())
    {
        dragon=NativeDragonSource(stackArgument,reward);
        if (dragon)
        {
            tracked=BeginNativeDragonAward(dragon,suppress);
            before=PlayerCharacter::Get()->GetActorValue(ActorValueInfo::kDragonSouls);
            // Only the copied float argument of the exact native award.
            if (suppress&&reward) reward->data.f=0.f;
        }
    }
    // Bind AddItem to the actual quest handle and completion fragment,
    // not merely a QF script name or the quest's maximum stage.
    struct RestoreScope
    {
        uint32_t Quest;uint16_t Stage;uint32_t Instance,RemovalQuest;
        ~RestoreScope(){nativeUniqueRewardQuest=Quest;nativeUniqueRewardStage=Stage;nativeUniqueRewardInstance=Instance;nativeQuestRemovalQuest=RemovalQuest;}
    } restore{nativeUniqueRewardQuest,nativeUniqueRewardStage,nativeUniqueRewardInstance,nativeQuestRemovalQuest};
    nativeUniqueRewardQuest=0;nativeUniqueRewardStage=0;nativeUniqueRewardInstance=0;nativeQuestRemovalQuest=0;
    const bool removing=apThis==removeItemNative.load(std::memory_order_acquire);
    struct CityScope
    {
        uint32_t Previous;
        ~CityScope(){cityChargeContext=Previous;}
    } cityScope{cityChargeContext};
    cityChargeContext=0;
    if(removing)
        try {cityChargeContext=CityCharge(stackArgument);} catch (...) {}
    if ((removing||apThis==addItemNative.load(std::memory_order_acquire))&&stackArgument)
    {
        auto* stack=*static_cast<DragonStackView**>(stackArgument);
        auto* frame=stack?stack->Top:nullptr;
        auto* caller=frame?frame->Previous:nullptr;
        if (frame&&frame->Parent&&caller&&caller->Parent&&caller->Function)
        {
            auto* gameVM=GameVM::Get();
            auto* policy=gameVM&&gameVM->virtualMachine?gameVM->virtualMachine->GetObjectHandlePolicy():nullptr;
            auto* object=caller->Self.GetObject();
            const auto handle=object?object->GetHandle():0;
            constexpr auto questType=static_cast<uint32_t>(FormType::QuestItem); // QUST (77)
            if (policy&&handle&&policy->HandleIsType(questType,handle)&&policy->IsHandleObjectAvailable(handle))
            {
                const auto* quest=static_cast<const TESQuest*>(policy->GetObjectForHandle(questType,handle));
                const auto* script=caller->Function->GetObjectTypeName().AsAscii();
                const auto* fragment=caller->Function->GetName().AsAscii();
                // The actual QUST handle and native QF fragment establish the
                // source, not a list of item or quest IDs. The grant remains native.
                if (quest&&quest->formType==FormType::QuestItem&&script&&fragment&&
                    !_strnicmp(script,"qf_",3)&&!_strnicmp(fragment,"fragment_",9)&&
                    (quest->flags&TESQuest::RunOnce)&&!(quest->flags&TESQuest::AllowRepeatStages)&&quest->currentStage)
                {
                    if (removing) nativeQuestRemovalQuest=quest->formID;
                    else
                    {
                        nativeUniqueRewardQuest=quest->formID;
                        nativeUniqueRewardStage=quest->currentStage;
                        nativeUniqueRewardInstance=quest->currentInstanceID;
                    }
                }
            }
        }
    }
    std::optional<ScriptedMoveObservation> move;
    if (apThis==moveToNative.load(std::memory_order_acquire))
        try {move=NativeScriptedMove(stackArgument);} catch (...) {}
    std::optional<PhysicalCall> physical;
    try {physical=ObservePhysicalCall(apThis,stackArgument);} catch (...) {}
    const auto result=TiltedPhoques::ThisCall(realDragonNativeCall,apThis,stackArgument,logger,vm,flag);
    if(physical&&(result==BSScript::CallResult::kCompleted||result==BSScript::CallResult::kInProgress))
        ObserveQuestPhysicalEffect(physical->Form,physical->Instance,physical->Stage,physical->Effect,physical->Epoch);
    if (move&&(result==BSScript::CallResult::kCompleted||result==BSScript::CallResult::kInProgress))
        ObserveScriptedTravel(std::move(*move));
    bool paid=false;
    if (dragon&&!suppress&&result==BSScript::CallResult::kCompleted&&World::Exists()&&!World::Get().IsLoadingSave())
    {
        auto* player=PlayerCharacter::Get();
        paid=player&&std::isfinite(before)&&player->GetActorValue(ActorValueInfo::kDragonSouls)==before+1.f;
        if (paid) World::Get().GetRunner().Queue([dragon]{RecordNativeDragonAbsorption(dragon);});
    }
    if (tracked) FinishNativeDragonAward(dragon,paid);
    return result;
}
#else
uint32_t CityHomeClient::ChargeContext() noexcept {return 0;}
bool CityHomeClient::NativeReady() noexcept {return false;}
CityHomeClient::Local CityHomeClient::ReadNative(uint32_t) noexcept {return Local::Unknown;}
bool CityHomeClient::DispatchNative(uint32_t) noexcept {return false;}
CityHomeClient::TifDispatch CityHomeClient::TryTif(uint32_t) noexcept {return TifDispatch::Unavailable;}
uint32_t NativeUniqueRewardQuest() noexcept {return 0;}
uint16_t NativeUniqueRewardStage() noexcept {return 0;}
uint32_t NativeUniqueRewardInstance() noexcept {return 0;}
uint32_t NativeQuestRemovalQuest() noexcept {return 0;}
bool DispatchScriptedSleep(uint32_t,const std::string&,float,float) noexcept {return false;}
bool DispatchPersonalSleepBonus() noexcept {return false;}
bool DragonAwardHookReady() noexcept {return false;}
void ResetCampaignDragonAwards() noexcept {}
void RefreshCampaignDragonAwards() noexcept {}
void RememberCampaignDragonAward(uint32_t) noexcept {}
int ApplyCampaignDragonAward(uint32_t) noexcept {return 0;}
#endif

void TP_MAKE_THISCALL(HookRegisterPapyrusFunction, BSScript::IVirtualMachine, NativeFunction* apFunction)
{
#ifdef TP_SKYRIM_1_6_1170
    if(apFunction&&apFunction->typeName.AsAscii()&&apFunction->functionName.AsAscii()&&
        !_stricmp(apFunction->typeName.AsAscii(),"Cell")&&!_stricmp(apFunction->functionName.AsAscii(),"GetFactionOwner")&&
        apFunction->usesLongSignature<=1)
    {
        cityFactionOwnerLong.store(apFunction->usesLongSignature==1,std::memory_order_release);
        cityFactionOwner.store(apFunction->functionAddress,std::memory_order_release);
    }
    if (CampaignRewardPolicy::SupplementDragonSouls&&apFunction&&apFunction->typeName.AsAscii()&&apFunction->functionName.AsAscii()&&
        !_stricmp(apFunction->typeName.AsAscii(),"Actor")&&
        !_stricmp(apFunction->functionName.AsAscii(),"ModActorValue"))
        modActorValueNative.store(apFunction,std::memory_order_release);
    if (apFunction&&apFunction->typeName.AsAscii()&&apFunction->functionName.AsAscii()&&
        !_stricmp(apFunction->typeName.AsAscii(),"ObjectReference")&&
        !_stricmp(apFunction->functionName.AsAscii(),"AddItem"))
        addItemNative.store(apFunction,std::memory_order_release);
    if (apFunction&&apFunction->typeName.AsAscii()&&apFunction->functionName.AsAscii()&&
        !_stricmp(apFunction->typeName.AsAscii(),"ObjectReference")&&
        !_stricmp(apFunction->functionName.AsAscii(),"RemoveItem"))
        removeItemNative.store(apFunction,std::memory_order_release);
    if (apFunction&&apFunction->typeName.AsAscii()&&apFunction->functionName.AsAscii()&&
        !_stricmp(apFunction->typeName.AsAscii(),"ObjectReference")&&
        !_stricmp(apFunction->functionName.AsAscii(),"MoveTo"))
        moveToNative.store(apFunction,std::memory_order_release);
    if(apFunction&&apFunction->typeName.AsAscii()&&apFunction->functionName.AsAscii()&&
        !_stricmp(apFunction->typeName.AsAscii(),"ObjectReference"))
        for(size_t i=0;i<std::size(physicalMethods);++i)
            if(!_stricmp(apFunction->functionName.AsAscii(),physicalMethods[i]))
                physicalNatives[i].store(apFunction,std::memory_order_release);
#endif
    if (CampaignRewardPolicy::SupplementWords&&apFunction&&apFunction->typeName.AsAscii()&&apFunction->functionName.AsAscii()&&
        !std::strcmp(apFunction->typeName.AsAscii(),"Game")&&
        !std::strcmp(apFunction->functionName.AsAscii(),"TeachWord"))
    {
        std::lock_guard lock(wordNativeMutex);
        teachWordAddress=apFunction->usesLongSignature<=1?apFunction->functionAddress:nullptr;
        teachWordLong=apFunction->usesLongSignature==1;
    }
    auto& runner = World::Get().GetRunner();

    PapyrusFunctionRegisterEvent event(apFunction->functionName.AsAscii(), apFunction->typeName.AsAscii(), apFunction->functionAddress);

    runner.Trigger(std::move(event));

    TiltedPhoques::ThisCall(RealRegisterPapyrusFunction, apThis, apFunction);
}

void TP_MAKE_THISCALL(HookBindEverythingToScript, BSScript::IVirtualMachine*)
{
    (*apThis)->BindNativeMethod(new BSScript::IsRemotePlayerFunc("IsRemotePlayer", "SkyrimTogetherUtils", PapyrusFunctions::IsRemotePlayer, BSScript::Variable::kBoolean));
    (*apThis)->BindNativeMethod(new BSScript::IsPlayerFunc("IsPlayer", "SkyrimTogetherUtils", PapyrusFunctions::IsPlayer, BSScript::Variable::kBoolean));
    (*apThis)->BindNativeMethod(new BSScript::DidLaunchSkyrimTogetherFunc("DidLaunchSkyrimTogether", "SkyrimTogetherVerifyLaunchScript", PapyrusFunctions::DidLaunchSkyrimTogether, BSScript::Variable::kBoolean));

    TiltedPhoques::ThisCall(RealBindEverythingToScript, apThis);
}

bool TP_MAKE_THISCALL(HookSignaturesMatch, BSScript::NativeFunction, BSScript::NativeFunction* apOther)
{
    /*
    if (!strcmp(apThis->GetName().AsAscii(), "IsRemotePlayer"))
        DebugBreak();
    */

    return TiltedPhoques::ThisCall(RealSignaturesMatch, apThis, apOther);
}

// This is a neat hack, but it has been disabled since it messes up other things like beastform.
// These kinds of issues should be solved with custom scripts now that we have SkyrimTogether.esp anyway.
int64_t TP_MAKE_THISCALL(HookCompareVariables, void, BSScript::Variable* apVar1, BSScript::Variable* apVar2)
{
    BSScript::Object* pObject1 = apVar1->GetObject();
    BSScript::Object* pObject2 = apVar2->GetObject();

    if (!pObject1 || !pObject2)
        return TiltedPhoques::ThisCall(RealCompareVariables, apThis, apVar1, apVar2);

    uint64_t handle1 = pObject1->GetHandle();
    uint64_t handle2 = pObject2->GetHandle();

    auto* pPolicy = GameVM::Get()->virtualMachine->GetObjectHandlePolicy();

    if (!pPolicy || !handle1 || !handle2 || !pPolicy->HandleIsType((uint32_t)Actor::Type, handle1) || !pPolicy->HandleIsType((uint32_t)Actor::Type, handle2) || !pPolicy->IsHandleObjectAvailable(handle1) || !pPolicy->IsHandleObjectAvailable(handle2))
    {
        return TiltedPhoques::ThisCall(RealCompareVariables, apThis, apVar1, apVar2);
    }

    Actor* pActor1 = pPolicy->GetObjectForHandle<Actor>(handle1);
    Actor* pActor2 = pPolicy->GetObjectForHandle<Actor>(handle2);

    if (!pActor1 || !pActor2)
        return TiltedPhoques::ThisCall(RealCompareVariables, apThis, apVar1, apVar2);

    if (pActor1 == PlayerCharacter::Get())
    {
        auto* pExtension = pActor2->GetExtension();
        if (pExtension && pExtension->IsPlayer())
            return 0;
    }
    else if (pActor2 == PlayerCharacter::Get())
    {
        auto* pExtension = pActor1->GetExtension();
        if (pExtension && pExtension->IsPlayer())
            return 0;
    }

    return TiltedPhoques::ThisCall(RealCompareVariables, apThis, apVar1, apVar2);
}

static TiltedPhoques::Initializer s_vmHooks(
    []()
    {
        POINTER_SKYRIMSE(TRegisterPapyrusFunction, s_registerPapyrusFunction, 104788);
        POINTER_SKYRIMSE(TBindEverythingToScript, s_bindEverythingToScript, 55739);
        POINTER_SKYRIMSE(TSignaturesMatch, s_signaturesMatch, 104359);

        // POINTER_SKYRIMSE(TCompareVariables, s_compareVariables, 105220);

        RealRegisterPapyrusFunction = s_registerPapyrusFunction.Get();
        RealBindEverythingToScript = s_bindEverythingToScript.Get();
        RealSignaturesMatch = s_signaturesMatch.Get();
        // RealCompareVariables = s_compareVariables.Get();

        TP_HOOK(&RealRegisterPapyrusFunction, HookRegisterPapyrusFunction);
        TP_HOOK(&RealBindEverythingToScript, HookBindEverythingToScript);
        TP_HOOK(&RealSignaturesMatch, HookSignaturesMatch);
#ifdef TP_SKYRIM_1_6_1170
        POINTER_SKYRIMSE(TDragonNativeCall, s_dragonNativeCall, 104651);
        realDragonNativeCall=s_dragonNativeCall.Get();
        TP_HOOK(&realDragonNativeCall, HookDragonNativeCall);
#endif
        // TP_HOOK(&RealCompareVariables, HookCompareVariables);
    });
