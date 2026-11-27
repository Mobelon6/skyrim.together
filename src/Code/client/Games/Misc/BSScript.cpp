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
std::atomic<const void*> modActorValueNative{},addItemNative{};
const UniqueQuestReward::Entry* ResolveUniqueRewardSource(uint32_t,std::string_view,std::string_view,uint64_t&) noexcept;
thread_local const UniqueQuestReward::Entry* nativeUniqueRewardSource{};
thread_local uint64_t nativeUniqueRewardEpoch{};
const UniqueQuestReward::Entry* NativeUniqueRewardSource() noexcept {return nativeUniqueRewardSource;}
uint64_t NativeUniqueRewardEpoch() noexcept {return nativeUniqueRewardEpoch;}
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
        const UniqueQuestReward::Entry* Previous;
        uint64_t Epoch;
        ~RestoreScope(){nativeUniqueRewardSource=Previous;nativeUniqueRewardEpoch=Epoch;}
    } restore{nativeUniqueRewardSource,nativeUniqueRewardEpoch};
    nativeUniqueRewardSource=nullptr;nativeUniqueRewardEpoch=0;
    if (apThis==addItemNative.load(std::memory_order_acquire)&&stackArgument)
    {
        auto* stack=*static_cast<DragonStackView**>(stackArgument);
        auto* frame=stack?stack->Top:nullptr;
        auto* caller=frame?frame->Previous:nullptr;
        if (frame&&frame->Parent&&caller&&caller->Parent&&caller->Function)
        {
            auto* gameVM=GameVM::Get();
            auto* policy=gameVM&&vm&&gameVM->virtualMachine==vm&&!gameVM->inactive?vm->GetObjectHandlePolicy():nullptr;
            auto* object=caller->Self.GetObject();
            const auto handle=object?object->GetHandle():0;
            constexpr auto questType=static_cast<uint32_t>(FormType::QuestItem); // QUST (77)
            if (policy&&handle&&policy->HandleIsType(questType,handle)&&policy->IsHandleObjectAvailable(handle))
            {
                const auto* quest=static_cast<const TESQuest*>(policy->GetObjectForHandle(questType,handle));
                const auto* script=caller->Function->GetObjectTypeName().AsAscii();
                const auto* fragment=caller->Function->GetName().AsAscii();
                if (quest&&quest->formType==FormType::QuestItem&&script&&fragment)
                    if (const auto* source=ResolveUniqueRewardSource(quest->formID,script,fragment,nativeUniqueRewardEpoch);
                        source&&quest->currentStage==source->Stage&&!(quest->flags&TESQuest::AllowRepeatStages))
                        nativeUniqueRewardSource=source;
            }
        }
    }
    const auto result=TiltedPhoques::ThisCall(realDragonNativeCall,apThis,stackArgument,logger,vm,flag);
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
const UniqueQuestReward::Entry* NativeUniqueRewardSource() noexcept {return nullptr;}
uint64_t NativeUniqueRewardEpoch() noexcept {return 0;}
bool DragonAwardHookReady() noexcept {return false;}
void ResetCampaignDragonAwards() noexcept {}
void RefreshCampaignDragonAwards() noexcept {}
void RememberCampaignDragonAward(uint32_t) noexcept {}
int ApplyCampaignDragonAward(uint32_t) noexcept {return 0;}
#endif

void TP_MAKE_THISCALL(HookRegisterPapyrusFunction, BSScript::IVirtualMachine, NativeFunction* apFunction)
{
#ifdef TP_SKYRIM_1_6_1170
    if (CampaignRewardPolicy::SupplementDragonSouls&&apFunction&&apFunction->typeName.AsAscii()&&apFunction->functionName.AsAscii()&&
        !_stricmp(apFunction->typeName.AsAscii(),"Actor")&&
        !_stricmp(apFunction->functionName.AsAscii(),"ModActorValue"))
        modActorValueNative.store(apFunction,std::memory_order_release);
    if (apFunction&&apFunction->typeName.AsAscii()&&apFunction->functionName.AsAscii()&&
        !_stricmp(apFunction->typeName.AsAscii(),"ObjectReference")&&
        !_stricmp(apFunction->functionName.AsAscii(),"AddItem"))
        addItemNative.store(apFunction,std::memory_order_release);
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
