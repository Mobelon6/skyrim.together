#include "MenuTopicManager.h"

#include <Events/PlayerDialogueEvent.h>
#include <TESObjectREFR.h>
#include <Forms/TESTopicInfo.h>
#include <World.h>
#include <Interface/UI.h>
#include <Forms/TESQuest.h>
#include <PlayerCharacter.h>
#include <QuestPickupPermit.h>

bool DeferSharedQuestTurnIn(uint32_t,uint32_t,uint32_t,uint32_t) noexcept;

TP_THIS_FUNCTION(TPlayDialogueOption, bool, MenuTopicManager, int32_t aIndex);
static TPlayDialogueOption* RealPlayDialogueOption = nullptr;

MenuTopicManager* MenuTopicManager::Get() noexcept
{
    POINTER_SKYRIMSE(MenuTopicManager*, s_singleton, 401099);
    return *s_singleton.Get();
}

bool MenuTopicManager::IsPlayerDialogueSpeaker(const TESObjectREFR* apSpeaker) noexcept
{
    if (!apSpeaker)
        return false;

    const auto* pManager = Get();
    // lastSpeaker survives menu closure; using it here would also capture
    // subsequent ambient speech from a remotely owned NPC.
    return pManager && pManager->IsCurrentSpeaker(apSpeaker->GetHandle());
}

bool MenuTopicManager::IsDialogueOpen() const noexcept
{
    static const BSFixedString dialogue("Dialogue Menu");
    auto* ui = UI::Get();
    return ui && ui->GetMenuOpen(dialogue);
}
namespace
{
// Reuse the existing condition hook. Only a native, non-OR player
// item-count requirement which exactly one item can satisfy is relevant.
uint32_t PlayerItemCondition(const TESConditionItem& condition)
{
    if (condition.function!=47||!condition.params[0]||
        !QuestPickupPermit::CompareSingleItem(condition.flags,condition.comparisonValue,0)) return 0;
    auto* player=PlayerCharacter::Get();
    if (!player||(condition.object!=1&&
        (condition.object!=2||TESObjectREFR::GetByHandle(condition.runOnRef)!=player))) return 0;
    return static_cast<const TESForm*>(condition.params[0])->formID;
}
uint32_t PositivePlayerItem(const TESConditionItem& condition)
{
    return QuestPickupPermit::PositiveSingleItem(condition.flags,condition.comparisonValue)?
        PlayerItemCondition(condition):0;
}
uint32_t TurnInBase(const TESTopicInfo* info)
{
    if (!info) return 0;
    uint32_t base{};bool previousOr=false;unsigned visited=0;
    for (const auto* c=info->conditions.pHead;c;c=c->pNext)
    {
        if (++visited>1024) return 0;
        if (!previousOr)
            if (const auto item=PositivePlayerItem(*c))
            {
                if (base&&base!=item) return 0;
                base=item;
            }
        previousOr=(c->flags&1)!=0;
    }
    return base;
}

bool IsHireOption(const TESTopicInfo* info)
{
    if (!info)
        return false;
    bool requiresFreeSlot = false;
    bool requiresPotentialFollower = false;
    for (const auto* c = info->conditions.pHead; c; c = c->pNext)
    {
        if ((c->function != 74 && c->function != 71) || !c->params[0] || (c->flags & 0x1E) ||
            c->object != 0 || c->GetOpCode() != TESConditionItem::OpCode::kEqualTo)
            continue;
        const auto form = static_cast<const TESForm*>(c->params[0])->formID;
        if (c->function == 74 && form == 0xBCC98 && c->comparisonValue == 0.f)
            requiresFreeSlot = true;
        if (c->function == 71 && form == 0x5C84D && c->comparisonValue == 1.f)
            requiresPotentialFollower = true;
    }
    // Generic recruitment plus standard paid hire / re-hire dialogues. A quest requiring
    // the player to arrive alone can also test PlayerFollowerCount, without recruiting anyone.
    const auto topic = info->parentTopic ? info->parentTopic->formID : 0;
    return requiresFreeSlot && (requiresPotentialFollower || topic == 0xB0EE6 ||
        topic == 0xBCC84 || topic == 0x104F1B || topic == 0x9BE49);
}
}

bool MenuTopicManager::OffersHire() const noexcept
{
    if (!pOptions || !IsDialogueOpen())
        return false;
    for (auto* opt : *pOptions)
        if (opt && opt->text && IsHireOption(opt->info))
            return true;
    return false;
}

bool MenuTopicManager::ResumeHire(int32_t index, uint32_t target, uint32_t info) noexcept
{
    const auto* npc = TESObjectREFR::GetByHandle(speaker.handle.iBits);
    if (!IsDialogueOpen() || !pOptions || !npc || npc->formID != target)
        return false;
    int current = -1;
    for (auto* opt : *pOptions)
    {
        ++current;
        if (opt && opt->info && opt->info->formID == info && opt->text)
        {
            World::Get().GetRunner().Trigger(PlayerDialogueEvent(opt->text, speaker.handle.iBits, info, opt->quest ? opt->quest->formID : 0));
            return TiltedPhoques::ThisCall(RealPlayDialogueOption, this, current);
        }
    }
    return false;
}

bool MenuTopicManager::ResumeQuestTurnIn(uint32_t target,uint32_t info,uint32_t quest,uint32_t base) noexcept
{
    const auto* npc=TESObjectREFR::GetByHandle(speaker.handle.iBits);
    if (!IsDialogueOpen()||!pOptions||!npc||npc->formID!=target) return false;
    int current=-1;
    for (auto* opt:*pOptions)
    {
        ++current;
        if (opt&&opt->info&&opt->quest&&opt->text&&opt->info->formID==info&&
            opt->quest->formID==quest&&TurnInBase(opt->info)==base)
        {
            World::Get().GetRunner().Trigger(PlayerDialogueEvent(opt->text,speaker.handle.iBits,info,quest));
            return TiltedPhoques::ThisCall(RealPlayDialogueOption,this,current);
        }
    }
    return false;
}

bool TP_MAKE_THISCALL(HookPlayDialogueOption, MenuTopicManager, int32_t aIndex)
{
    if (apThis->pOptions)
    {
        int i = 0;
        const char* pText = nullptr;
        uint32_t info{}, quest{};
        for (auto option : *apThis->pOptions)
        {
            if (i == aIndex)
            {
                if (!option)
                    break;
                if (IsHireOption(option->info))
                    if (const auto* npc = TESObjectREFR::GetByHandle(apThis->speaker.handle.iBits))
                        if (World::Get().GetOverlayService().BeginHire(npc->formID, option->info->formID, aIndex))
                            return false;
                if (option->quest&&option->info&&apThis->IsDialogueOpen())
                    if (const auto base=TurnInBase(option->info))
                        if (const auto* npc=TESObjectREFR::GetByHandle(apThis->speaker.handle.iBits))
                            if (DeferSharedQuestTurnIn(option->quest->formID,base,npc->formID,option->info->formID))
                                return false;
                pText = option->text;
                info = option->info ? option->info->formID : 0;
                quest = option->quest ? option->quest->formID : 0;
                break;
            }
            i++;
        }

        if (pText != nullptr)
            World::Get().GetRunner().Trigger(PlayerDialogueEvent(pText, apThis->speaker.handle.iBits, info, quest));
    }

    return TiltedPhoques::ThisCall(RealPlayDialogueOption, apThis, aIndex);
}

namespace
{
// CommonLibSSE-NG: TESConditionItem::IsTrue(ConditionCheckParams&), AE 29924.
// Read only the first two references from the solution. Never change the global or the NPC faction.
struct ConditionContext { TESObjectREFR* action; TESObjectREFR* target; TESQuest* quest; };
static_assert(offsetof(ConditionContext,quest)==0x10);
TP_THIS_FUNCTION(TCheckCondition, bool, TESConditionItem, ConditionContext&);
TCheckCondition* nextCondition{};
bool TP_MAKE_THISCALL(CheckFollowerCondition, TESConditionItem, ConditionContext& context)
{
    const auto fn = apThis->function;
    // Item availability and every comparison are evaluated by Skyrim on the local player.
    const auto* param = static_cast<const TESForm*>(apThis->params[0]);
    // Alias and package-data operands can be numeric slots instead of TESForm pointers.
    if ((fn == 74 || fn == 71) && param && !(apThis->flags & 0x1E) && apThis->object == 0 &&
        apThis->GetOpCode() == TESConditionItem::OpCode::kEqualTo && context.action &&
        OverlayService::FollowerBusy(context.action->formID) &&
        ((fn == 74 && param->formID == 0xBCC98) || (fn == 71 && param->formID == 0x5C84E)))
    {
        const auto* menu = MenuTopicManager::Get();
        if (menu && menu->speaker && context.action->GetHandle().handle.iBits == menu->speaker.handle.iBits)
            return apThis->comparisonValue == (fn == 74 ? 1.f : 0.f);
    }
    return TiltedPhoques::ThisCall(nextCondition, apThis, context);
}
}
static TiltedPhoques::Initializer s_menuTopicHooks(
    []()
    {
        POINTER_SKYRIMSE(TPlayDialogueOption, s_playDialogueOption, 35269);

        POINTER_SKYRIMSE(TCheckCondition, checkCondition, 29924);
        nextCondition = checkCondition.Get();
        TP_HOOK(&nextCondition, CheckFollowerCondition);
        RealPlayDialogueOption = s_playDialogueOption.Get();

        TP_HOOK(&RealPlayDialogueOption, HookPlayDialogueOption);
    });
