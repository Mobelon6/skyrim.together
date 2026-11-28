"""Exercise the game's selected INFO, response conditions and post-grant dispatch using its production handlers."""
import subprocess, sys, tempfile
from pathlib import Path

root = Path(sys.argv[1], "Code/client/Games")
src = (root / "Misc/MenuTopicManager.cpp").read_text()
cond = (root / "Skyrim/Components/TESCondition.h").read_text().split("#pragma once")[1]
selection = src[src.index("namespace\n{"):src.index("static TiltedPhoques::Initializer")]
common = (root.parent.parent / "common").resolve()
mocks = r"""
#include <cassert>
#include <QuestPickupPermit.h>
#include <HomeCityPurchase.h>
#include <cmath>
#include <optional>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
struct TESForm{uint32_t formID{};};
struct Handle{struct {unsigned iBits{};}handle;explicit operator bool()const{return handle.iBits!=0;}};
struct TESObjectREFR: TESForm {unsigned handle{};Handle GetHandle(){return {{handle}};}
 static TESObjectREFR* pick;static TESObjectREFR* GetByHandle(unsigned value){return pick && pick->handle==value?pick:nullptr;}};
TESObjectREFR* TESObjectREFR::pick=nullptr;
struct PlayerCharacter:TESObjectREFR{static PlayerCharacter* Get(){static PlayerCharacter p;return &p;}};
struct TESQuest:TESForm{};
static bool defer=false;
namespace CityHomeClient { bool defer=false; bool DeferPurchase(uint32_t quest,uint32_t info,uint32_t npc) noexcept {return defer&&quest==HomeCityPurchase::Quest&&HomeCityPurchase::FromInfo(info)&&npc==10;} }
bool DeferSharedQuestTurnIn(uint32_t quest,uint32_t item,uint32_t npc,uint32_t info) noexcept{return defer&&quest==100&&item==200&&npc==10&&info==49;}
struct TESTopicInfo:TESForm{TESForm* parentTopic{};TESCondition conditions;};
struct MenuTopicManager{struct Opt{const char* text;TESTopicInfo* info;TESForm* quest{};};std::vector<Opt*>* pOptions;Handle speaker;bool menuOpen{};
 static MenuTopicManager* Get(){static MenuTopicManager m{};return &m;}bool IsDialogueOpen() const noexcept{return menuOpen;}bool IsCurrentSpeaker(const Handle& h)const noexcept{return menuOpen&&h.handle.iBits==speaker.handle.iBits;}
 bool OffersHire()const noexcept;bool ResumeHire(int32_t,uint32_t,uint32_t) noexcept;
 bool ResumeQuestTurnIn(uint32_t,uint32_t,uint32_t,uint32_t) noexcept;
 bool ResumeCityHomePurchase(uint32_t,uint32_t) noexcept;};
struct PlayerDialogueEvent{std::string text;unsigned handle;unsigned info;unsigned quest;};
struct OverlayService{static inline unsigned holder{};static bool FollowerBusy(unsigned id){return holder==id;}
 static inline int hires{};bool BeginHire(unsigned,unsigned,int){++hires;return true;}};
struct World {OverlayService overlay;std::vector<PlayerDialogueEvent> sent;auto& GetRunner(){return *this;}
 void Trigger(PlayerDialogueEvent e){sent.push_back(e);}OverlayService& GetOverlayService(){return overlay;}
 static World& Get(){static World w;return w;}};
#define TP_THIS_FUNCTION(T,R,C,...) using T=R(C*,__VA_ARGS__)
#define TP_MAKE_THISCALL(F,C,...) F(C* apThis,__VA_ARGS__)
namespace TiltedPhoques{template<typename F,typename... A>auto ThisCall(F f,A&&... args){return f(static_cast<A&&>(args)...);}}
int played{};bool RealPlayDialogueOption(MenuTopicManager*,int){++played;return true;}
"""
checks = r"""
int main(){
 auto& w=World::Get();auto* m=MenuTopicManager::Get();TESObjectREFR npc{};npc.formID=10;npc.handle=45;TESObjectREFR::pick=&npc;
 m->speaker={{45}};m->menuOpen=true;TESForm count{0xBCC98},faction{0x5C84E},other{0xAB};
 TESConditionItem item{};item.function=74;item.params[0]=&count;TESTopicInfo info{};TESForm topic{0xB0EE6};info.parentTopic=&topic;info.formID=49;info.conditions.pHead=&item;
 MenuTopicManager::Opt hire{"follow",&info};std::vector<MenuTopicManager::Opt*> list{&hire};m->pOptions=&list;
 assert(!HookPlayDialogueOption(m,0));assert(!played&&w.sent.empty()&&w.overlay.hires==1);
 assert(!m->ResumeHire(0,999,49)&&!m->ResumeHire(0,10,78)&&!played);
 list.insert(list.begin(),nullptr);assert(m->ResumeHire(0,10,49)&&played==1&&w.sent.size()==1);
 list.erase(list.begin());assert(w.sent.front().info==49&&w.sent.front().handle==45);
 ConditionContext context{&npc,nullptr};nextCondition=[](TESConditionItem*,ConditionContext&){return false;};
 w.overlay.holder=10;assert(!CheckFollowerCondition(&item,context));item.comparisonValue=1;
 assert(CheckFollowerCondition(&item,context));item.params[0]=&faction;item.function=71;
 assert(!CheckFollowerCondition(&item,context));item.comparisonValue=0;assert(CheckFollowerCondition(&item,context));
 m->speaker={{99}};assert(!CheckFollowerCondition(&item,context));m->speaker={{45}};
 item.params[0]=&other;assert(!CheckFollowerCondition(&item,context));
 item.flags=4;item.params[0]=&count;item.function=74;assert(!CheckFollowerCondition(&item,context));
 info.parentTopic=&other;item.flags=0;item.comparisonValue=0;assert(!IsHireOption(&info));
 for(unsigned flag:{2u,8u,16u}){item.flags=flag;item.params[0]=reinterpret_cast<void*>(1);assert(!CheckFollowerCondition(&item,context)&&!IsHireOption(&info));}
 item.params[0]=&count;
 // Ordinary responses and the full-group refusal use the original once.
 item.flags=0;item.comparisonValue=1;assert(HookPlayDialogueOption(m,0));
 assert(played==2&&w.sent.size()==2&&w.overlay.hires==1);
 TESQuest quest;quest.formID=100;TESForm base{200};
 hire.quest=&quest;item.function=47;item.params[0]=&base;item.object=1;item.comparisonValue=1.f;
 context.quest=&quest;context.target=PlayerCharacter::Get();
 // Every item condition uses the original engine result, including
 // insufficient local stock and a native negative/absence condition.
 nextCondition=[](TESConditionItem*,ConditionContext&){return false;};
 for(unsigned flag:{0u,32u,64u,96u,128u,160u}){
     item.flags=flag;item.comparisonValue=3.f;assert(!CheckFollowerCondition(&item,context));
 }
 nextCondition=[](TESConditionItem*,ConditionContext&){return true;};
 for(unsigned flag:{0u,32u,64u,96u,128u,160u}){
     item.flags=flag;item.comparisonValue=3.f;assert(CheckFollowerCondition(&item,context));
 }
 item.flags=0;item.comparisonValue=1.f;
 defer=true;assert(!HookPlayDialogueOption(m,0)&&played==2);
 assert(!m->ResumeQuestTurnIn(10,49,100,201)&&played==2);
 assert(m->ResumeQuestTurnIn(10,49,100,200)&&played==3);
 defer=false;quest.formID=HomeCityPurchase::Quest;info.formID=HomeCityPurchase::Houses[0].Info;
 CityHomeClient::defer=true;assert(!HookPlayDialogueOption(m,0)&&played==3);
 assert(!m->ResumeCityHomePurchase(999,info.formID)&&!m->ResumeCityHomePurchase(10,1234)&&played==3);
 assert(m->ResumeCityHomePurchase(10,info.formID)&&played==4);
 assert(w.sent.back().quest==HomeCityPurchase::Quest&&w.sent.back().info==info.formID);
 CityHomeClient::defer=false;assert(HookPlayDialogueOption(m,0)&&played==5);
}
"""
with tempfile.TemporaryDirectory() as folder:
    binary = Path(folder, "follower")
    cpp = mocks.replace("struct TESTopicInfo:", cond+"\nstruct TESTopicInfo:")
    subprocess.run(["g++","-std=c++20","-I",str(common),"-x","c++","-","-o",binary],input=cpp+selection+checks,text=True,check=True)
    subprocess.run([binary],check=True)
print("PASS actual response predicates and native dispatch: empty/full slot, foreign follower, unrelated conditions, stale grants, single playback")
