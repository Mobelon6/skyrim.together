"""Compile production method bodies with an in-memory game, without a Windows Skyrim install."""
from pathlib import Path
from tempfile import TemporaryDirectory
import subprocess as sp
import sys

code = Path(sys.argv[1]).resolve() / 'Code'
def extract(file, start, end):
    source = (code / file).read_text()
    offset = source.index(start)
    return source[offset:source.index(end, offset)]
def run(name, source, extra=()):
    with TemporaryDirectory() as directory:
        path = Path(directory)
        (path / 'test.cpp').write_text(source)
        sp.run(['g++', '-std=c++20', '-Wall', '-Werror','-Wno-reorder','-Wno-unused-variable', '-Wno-unused-parameter',
                '-fsanitize=undefined','-fno-sanitize-recover=undefined',*extra,str(path/'test.cpp'),'-o',str(path/'test')],check=True)
        sp.run([str(path/'test')],check=True)
    print('PASS', name, flush=True)

headers = '''
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <vector>
'''
inventory = extract('encoding/Structs/Inventory.cpp', 'void Inventory::AddOrRemoveEntry', 'void Inventory::UpdateEquipment')
game_id = extract('encoding/Structs/GameId.cpp','GameId::GameId','void GameId::Serialize')
run('actual inventory snapshot methods',headers+'''
namespace TiltedPhoques {
struct Buffer { struct Reader; struct Writer; };
using String=std::string;
template<class T> using Vector=std::vector<T>;
}
#include <Structs/Inventory.h>
'''+game_id+inventory+'''
int main(){
    Inventory inv;
    Inventory::Entry sword;
    sword.BaseId={0,10};
    auto change=[&](Inventory::Entry item,int count){item.Count=count;inv.AddOrRemoveEntry(item);};
    change(sword,-1);change(sword,0);assert(inv.Entries.empty());
    Inventory::Entry frost=sword,fire=sword;
    Inventory::EffectItem magic;magic.EffectId={0,31};magic.Magnitude=12;
    frost.EnchantData.Effects.push_back(magic);magic.EffectId={0,32};fire.EnchantData.Effects.push_back(magic);
    change(frost,2);change(fire,3);change(sword,8);assert(inv.Entries.size()==3);
    change(frost,-8);assert(inv.Entries.size()==2&&inv.Entries[0].CanBeMerged(fire)&&inv.Entries[0].Count==3);
    const auto max=std::numeric_limits<int32_t>::max();
    change(sword,max);assert(inv.Entries.size()==3&&inv.Entries[1].Count==max&&inv.Entries[2].Count==8);
    change(sword,std::numeric_limits<int32_t>::min());assert(inv.Entries.size()==2&&inv.Entries[1].Count==7);
    change(sword,-7);change(fire,-3);change(fire,-3);assert(inv.Entries.empty());
    change(sword,1);change(sword,-1);assert(inv.Entries.empty());
    auto other=frost;other.EnchantData.Effects.front().Duration=25;assert(!other.CanBeMerged(frost));
    other=frost;other.EnchantData.IsWeapon=true;assert(!other.CanBeMerged(frost));
}
''', ['-I'+str(code/'encoding')])

qs='client/Services/Generic/QuestService.cpp'
bodies = (extract(qs,'void QuestService::OnUpdate(', 'BSTEventResult QuestService::OnEvent(')
        +extract(qs,'void QuestService::HoldStage','void QuestService::NotifyOverlayOfQuestUpdate')
        +extract(qs,'bool QuestService::StopQuest','static constexpr std::array kNonSyncableQuestIds'))
# The simulator supplies the standard map in place of TiltedCore's map adapter.
bodies=bodies.replace('it.value()', 'it->second').replace('deferred.value()', 'deferred->second').replace('retry.value()', 'retry->second')
run('actual client quest handlers with simulated Skyrim scenes',headers+'''
#define __FUNCTION__ "test"
struct GameId{uint32_t BaseId;uint32_t ModId=0;GameId(uint32_t x):BaseId(x){}operator uint32_t()const{return BaseId;} uint32_t LogFormat()const{return BaseId;}};
template<class T> using Vector=std::vector<T>;
template<class K,class V> using Map=std::map<K,V>;
struct UpdateEvent{};struct DisconnectedEvent{};struct PartyLeftEvent{};
struct ScopedQuestOverride{ScopedQuestOverride(){}};
struct NotifyQuestUpdate{enum {StageUpdate,Started,Stopped};GameId Id;uint16_t Stage;uint8_t Status;uint8_t ClientQuestType{};};
struct TESForm{static TESForm* GetById(uint32_t);};
template<class T> T* Cast(TESForm* p){return static_cast<T*>(p);}
struct Conditions{uint16_t wants;};
struct BGSScenePhase{Conditions completionConditions;};
struct PhaseList{int length=1;BGSScenePhase* phase;auto operator[](int)const{return phase;}};
struct BGSScene{bool isPlaying=true;int currentPhaseIndex=0;PhaseList phases;int formID=44;bool ready=false;};
struct TESQuest:TESForm{
struct{struct{const char* AsAscii(){return "Quest";}}value;}fullName;enum class Type{Other,None,Miscellaneous};Type type=Type::Other;
uint32_t formID=2;uint16_t currentStage=0;bool stopped=false;
enum Flags{RunOnce=256,Completed=2};uint16_t flags{};
enum class State{WaitingPromotion,Running,Stopped};bool pendingPromotion=false;int nativeStarts=0;
State getState(){return pendingPromotion?State::WaitingPromotion:stopped?State::Stopped:State::Running;}
bool EnsureQuestStarted(bool& success,bool force){assert(!force);++nativeStarts;success=true;stopped=false;return true;}
std::set<uint16_t> reached,reject;std::vector<int> actions;Vector<BGSScene*> scenes;
bool IsStageReached(uint16_t s){return reached.contains(s);}
bool ScriptSetStage(uint16_t s){if(reject.contains(s)||!reached.insert(s).second)return false;stopped=false;actions.push_back(s);currentStage=s;return true;}
unsigned GetStageItemCount(uint16_t stage){return stage==100?1:0;}
bool IsStopped(){return stopped;}void SetActive(bool){}
void SetStopped(){actions.push_back(-1);stopped=true;reached.clear();}
} quest;
TESForm* TESForm::GetById(uint32_t id){return id==2?&quest:nullptr;}
struct ModSystem{uint32_t GetGameId(uint32_t id){return id;}};
struct Party{bool IsLeader(){return true;}};
struct World{static World& Get(){static World w;return w;}ModSystem mods;Party party;ModSystem& GetModSystem(){return mods;}Party& GetPartyService(){return party;}};
namespace spdlog{
template<class...T>void debug(T&&...){}
template<class...T>void info(T&&...){}
template<class...T>void error(T&&...){}
}
struct QuestService{
World& m_world=World::Get();struct HeldStages{Vector<uint16_t> Stages,PendingItems;std::chrono::steady_clock::time_point RetryAt{};};
Map<uint32_t,HeldStages> m_heldStages; Map<uint32_t,Vector<NotifyQuestUpdate>> m_afterHeldStages;
Map<uint32_t,std::chrono::steady_clock::time_point> m_questRetryAt;
void OnUpdate(const UpdateEvent&) noexcept;void OnDisconnected(const DisconnectedEvent&) noexcept;void OnPartyLeft(const PartyLeftEvent&) noexcept;
void HoldStage(TESQuest*,uint16_t) noexcept;HeldStages* GetHeldStages(uint32_t,TESQuest*) noexcept;
void OnQuestUpdate(const NotifyQuestUpdate&) noexcept;static bool StopQuest(uint32_t);
};
using HeldStages=QuestService::HeldStages;
static void ReleaseStagesForWaitingPhases(TESQuest*,HeldStages&) noexcept;
static void ReleaseUncheckedStages(TESQuest*,HeldStages&) noexcept;
static bool IsCheckedByPlayingScene(TESQuest* q,uint16_t){return !q->scenes.empty()&&q->scenes[0]->isPlaying;}
static bool IsCheckedByStage(const Conditions& c,TESQuest*,uint16_t s,bool){return s==c.wants;}
static bool IsPhaseWaitingOnConditions(BGSScene* s,int){return s->ready;}
'''+bodies+'''
int main(){
    QuestService service;
    BGSScenePhase phase{{20}}; BGSScene scene;scene.phases.phase=&phase;quest.scenes={&scene};
    auto send=[&](uint16_t stage,uint8_t status){service.OnQuestUpdate({2,stage,status,0});};
    send(100,0);send(20,0);send(100,0);send(20,2);send(10,1);send(15,0);
    service.OnUpdate({});assert(quest.actions.empty());
    assert((service.m_heldStages.at(2).Stages==Vector<uint16_t>{100,20}));
    scene.ready=true;service.OnUpdate({});assert((quest.actions==Vector<int>{100}));
    service.OnUpdate({});assert((quest.actions==Vector<int>{100}));
    service.m_heldStages.at(2).PendingItems.clear(); // simulated TESQuestStageItemDoneEvent
    service.OnUpdate({});assert((quest.actions==Vector<int>{100,20}));
    scene.isPlaying=false;
    service.OnUpdate({});assert((quest.actions==Vector<int>{100,20,-1,10,15}));
    assert(service.m_afterHeldStages.empty());
    scene.isPlaying=true;send(30,0);send(30,2);service.OnPartyLeft({});assert(service.m_heldStages.empty()&&service.m_afterHeldStages.empty());
    send(40,0);send(40,2);service.OnDisconnected({});assert(service.m_heldStages.empty()&&service.m_afterHeldStages.empty());
    scene.isPlaying=false;quest.reject.insert(50);send(50,0);send(60,0);
    assert(!quest.IsStageReached(50)&&!quest.IsStageReached(60));
    service.OnUpdate({});assert(!quest.IsStageReached(60));
    quest.reject.erase(50);service.m_questRetryAt.at(2)={};
    service.OnUpdate({});assert(quest.IsStageReached(50)&&quest.IsStageReached(60));
    assert(service.m_afterHeldStages.empty()&&service.m_questRetryAt.empty());
    scene.isPlaying=true;scene.ready=false;quest.reject.insert(70);send(70,0);send(80,0);send(80,2);
    scene.isPlaying=false;service.OnUpdate({});
    assert((service.m_heldStages.at(2).Stages==Vector<uint16_t>{70,80}));
    assert(!quest.IsStageReached(80)&&!quest.IsStopped());
    quest.reject.erase(70);service.m_heldStages.at(2).RetryAt={};
    service.OnUpdate({});service.OnUpdate({});
    assert(quest.IsStopped()&&service.m_afterHeldStages.empty());
    assert((Vector<int>(quest.actions.end()-3,quest.actions.end())==Vector<int>{70,80,-1}));
    quest.reject.insert(90);send(90,1);assert(service.m_afterHeldStages.count(2));
    service.OnDisconnected({});assert(service.m_afterHeldStages.empty()&&service.m_questRetryAt.empty());
    // Match the native IsStageReached(0) shortcut in a stopped quest.
    quest.stopped=true;quest.currentStage=0;quest.reached={0};quest.pendingPromotion=true;
    send(0,1);assert(quest.nativeStarts==0&&service.m_afterHeldStages.count(2));
    quest.pendingPromotion=false;service.m_questRetryAt.at(2)={};service.OnUpdate({});
    assert(!quest.IsStopped()&&quest.nativeStarts==1&&service.m_afterHeldStages.empty());
    send(0,1);assert(quest.nativeStarts==1);
    quest.stopped=true;quest.flags=TESQuest::RunOnce|TESQuest::Completed;send(0,1);
    assert(quest.IsStopped()&&quest.nativeStarts==1&&service.m_afterHeldStages.empty());
}
''')

server=extract('server/Services/QuestService.cpp','bool AdvanceQuestRun(', 'std::string QuestKey(')
run('party quest lifecycle and ordered catch-up history',headers+'''
struct NotifyQuestUpdate{enum {StageUpdate,Started,Stopped};uint16_t Stage;uint8_t Status;};
struct QuestRun{unsigned Generation{1};std::set<uint16_t> Stages;std::vector<uint16_t> OrderedStages; bool Stopped=false;bool Started=false;bool CompletedRunOnce=false;};
'''+server+'''
int main(){
    QuestRun run;
    auto receive=[&](uint16_t stage,uint8_t status){NotifyQuestUpdate event{stage,status};return AdvanceQuestRun(run,event);};
    assert(receive(10,1));assert(!receive(10,1));assert(receive(100,0));
    assert(!receive(10,1));assert(!receive(100,0));assert(receive(20,0));
    assert((run.OrderedStages==std::vector<uint16_t>{100,20}));
    assert(receive(20,2));assert(!receive(20,2));assert(!receive(90,0));
    assert(run.OrderedStages.empty());assert(receive(10,1));assert(receive(100,0));
    assert(run.Generation==2&&(run.OrderedStages==std::vector<uint16_t>{100}));
    QuestRun partial;NotifyQuestUpdate stage{80,0};assert(AdvanceQuestRun(partial,stage));
    NotifyQuestUpdate start{10,1};assert(AdvanceQuestRun(partial,start));
    assert((partial.OrderedStages==std::vector<uint16_t>{80}));
    QuestRun finished;assert(AdvanceQuestRun(finished,start));assert(AdvanceQuestRun(finished,stage));
    NotifyQuestUpdate stop{80,2};assert(AdvanceQuestRun(finished,stop,true));
    assert(finished.CompletedRunOnce&&finished.Stopped&&finished.OrderedStages==std::vector<uint16_t>{80});
    assert(!AdvanceQuestRun(finished,start)&&!AdvanceQuestRun(finished,stage)&&!AdvanceQuestRun(finished,stop,true));
    QuestRun unknown;assert(!AdvanceQuestRun(unknown,stop,true));
}
''')

history=extract('client/Services/Generic/QuestJournal.cpp','struct QuestCatchup','struct QuestPickup')
run('active quest catch-up from existing campaign history',headers+'''
#include <sstream>
namespace CampaignJournal {struct Operation{std::string Id,Kind,Payload;};}
struct NotifyQuestUpdate{enum {StageUpdate,Started,Stopped};};
'''+history+'''
int main(){
    using CampaignJournal::Operation;
    std::vector<Operation> ops={{"1","quest-progress-v1","q 1 1 10 6 101"},
        {"2","quest-progress-v1","q 1 0 100 6 101"},
        {"3","quest-progress-v1","q 1 0 20 6 202"},
        {"4","unrelated","data"}};
    auto history=ActiveQuestHistory(ops);
    assert(history&&history->at("q").Generation==1&&!history->at("q").Stopped);
    assert((history->at("q").Stages==std::vector<std::pair<uint16_t,uint8_t>>{{100,6},{20,6}}));
    ops.push_back({"5","quest-progress-v1","q 1 2 20 6 202"});
    history=ActiveQuestHistory(ops);
    assert(history&&history->at("q").Stopped&&history->at("q").Stages.empty());
    ops.push_back({"6","quest-progress-v1","q 2 1 10 6 101"});
    ops.push_back({"7","quest-progress-v1","q 2 0 100 6 101"});
    history=ActiveQuestHistory(ops);
    assert(history&&history->at("q").Generation==2&&!history->at("q").Stopped);
    ops.push_back({"8","quest-progress-v1","q 4 0 90 6 101"});
    assert(!ActiveQuestHistory(ops));
    ops={{"1","quest-progress-v1","q 1 1 10 6 101"},{"2","quest-progress-v1","q 1 0 80 6 101"},{"3","quest-completed-v1","q 1 2 80 6 101"}};
    history=ActiveQuestHistory(ops);assert(history&&history->at("q").Stopped&&history->at("q").CompletedRunOnce);
    assert(history->at("q").Stages.size()==1&&history->at("q").Stages[0].first==80);
    ops.push_back({"4","quest-progress-v1","q 2 1 10 6 202"});assert(!ActiveQuestHistory(ops));
}
''')

pickup=extract('client/Games/Skyrim/Actor.cpp','void* TP_MAKE_THISCALL(HookPickUpObject','void Actor::PickUpObject(')
run('native quest pickup requires actual inventory increase',headers+'''
#define TP_MAKE_THISCALL(name,cls,...) name(cls* apThis, __VA_ARGS__)
struct Base {uint32_t formID=10;};
struct Extra {bool quest=true;};
struct TESObjectREFR {Base* baseForm;Extra extra;bool temporary=false;
 Extra* GetExtraDataList(){return &extra;}bool IsTemporary(){return temporary;}};
struct Inventory {struct Entry{uint32_t BaseId{};int32_t Count{};bool IsQuestItem{};};};
struct Actor{uint32_t formID=20;int64_t count=0;int32_t actual=0;int calls=0;
 int64_t GetItemCountInInventory(Base*){return count;}
 void GetItemFromExtraData(Inventory::Entry& item,Extra* e){item.IsQuestItem=e->quest;}};
struct PlayerCharacter:Actor{static PlayerCharacter* Get(){static PlayerCharacter player;return &player;}};
namespace HomeActorProtection{bool Resident(Actor*){return false;}bool PickupBlocked(TESObjectREFR*){return false;}}
struct ScopedInventoryOverride{static bool IsOverriden(){return false;}};
struct ScopedActivateOverride{static bool IsOverriden(){return false;}};
struct ModSystem{void GetServerModId(uint32_t id,uint32_t& out){out=id;}};
struct World{static World& Get(){static World world;return world;}ModSystem mods;ModSystem& GetModSystem(){return mods;}};
struct InventoryChangeEvent{uint32_t id;Inventory::Entry Item;bool Drop,UpdateClients;uint32_t QuestId{},QuestRef{},QuestAlias{};};
void CaptureQuestPickup(TESObjectREFR* object,InventoryChangeEvent& change){if(!object->temporary){change.QuestId=30;change.QuestRef=40;change.QuestAlias=3;}}
std::vector<InventoryChangeEvent> sent;
void QueueActorInventoryChange(InventoryChangeEvent e){sent.push_back(e);}
void* RealPickUpObject(Actor* actor,TESObjectREFR*,int32_t,bool,float){++actor->calls;actor->count+=actor->actual;return actor;}
namespace TiltedPhoques{template<class F,class... A>auto ThisCall(F f,A... a){return f(a...);}}
namespace spdlog{template<class... A>void debug(A...){}}
'''+pickup+'''
int main(){
 Base base;TESObjectREFR object{&base};auto* player=PlayerCharacter::Get();
 player->actual=0;assert(HookPickUpObject(player,&object,1,false,0)==player);
 assert(sent.empty()&&player->calls==1);
 player->actual=1;HookPickUpObject(player,&object,2,false,0);
 assert(sent.size()==1&&sent[0].Item.Count==1&&sent[0].Item.IsQuestItem&&!sent[0].UpdateClients&&player->calls==2);
 assert(sent[0].QuestId==30&&sent[0].QuestRef==40&&sent[0].QuestAlias==3);
 object.temporary=true;player->actual=2;HookPickUpObject(player,&object,2,false,0);
 assert(sent.size()==2&&sent.back().Item.Count==2&&sent.back().UpdateClients&&!sent.back().QuestId);
 Actor npc;npc.actual=0;HookPickUpObject(&npc,&object,1,false,0);
 assert(sent.size()==3&&npc.calls==1);
 object.extra.quest=false;player->actual=0;HookPickUpObject(player,&object,1,false,0);
 assert(sent.size()==4&&!sent.back().Item.IsQuestItem);
}
''')

item_change=extract('server/Services/QuestService.cpp','bool CanRecordQuestPickup','// A player who has independently accepted')
run('unique quest item consumption matches original holder and reference',headers+'''
struct QuestRun{unsigned Generation{1};bool Stopped{},Started{},CompletedRunOnce{};std::vector<uint16_t> OrderedStages;};
namespace CampaignJournal {struct Operation{std::string Id,Kind,Payload;};}
'''+item_change+'''
int main(){
    using D=QuestItemDecision;
    QuestRun initial;assert(!CanRecordQuestPickup(initial,false,1));
    assert(CanRecordQuestPickup(initial,true,1)&&!CanRecordQuestPickup(initial,true,2));
    initial.Started=true;assert(CanRecordQuestPickup(initial,false,1));
    initial.Stopped=true;assert(!CanRecordQuestPickup(initial,true,1));
    initial.Stopped=false;initial.Generation=2;assert(!CanRecordQuestPickup(initial,true,1));
    initial.Generation=1;initial.CompletedRunOnce=true;assert(!CanRecordQuestPickup(initial,true,1));
    std::vector<CampaignJournal::Operation> ops;
    const std::string prefix="q 1 3 ",pickup=prefix+"ref item 1 101";
    assert(QuestItemChange(ops,prefix,pickup,false)==D::Record);
    assert(QuestItemChange(ops,prefix,pickup,true)==D::Reject);
    ops.push_back({"1","quest-pickup-v1",pickup});
    assert(QuestItemChange(ops,prefix,pickup,false)==D::Duplicate);
    assert(QuestItemChange(ops,prefix,pickup,true)==D::Record);
    assert(QuestItemChange(ops,prefix,prefix+"ref item 1 202",true)==D::Reject);
    assert(QuestItemChange(ops,prefix,prefix+"other item 1 101",true)==D::Reject);
    assert(QuestItemChange(ops,prefix,prefix+"ref item 2 101",true)==D::Reject);
    assert(QuestItemChange(ops,"q 2 3 ","q 2 3 ref item 1 101",true)==D::Reject);
    ops.push_back({"2","quest-consumed-v1",pickup});
    assert(QuestItemChange(ops,prefix,pickup,true)==D::Duplicate);
    assert(QuestItemChange(ops,prefix,pickup,false)==D::Duplicate);
}
''')

pickup_history=extract('client/Services/Generic/QuestJournal.cpp','struct QuestPickup','// Return only a single unambiguous')
run('recovery selects only unconsumed original pickups of the loaded character',headers+'''
#include <sstream>
namespace CampaignJournal {struct Operation{std::string Id,Kind,Payload;};}
'''+pickup_history+'''
int main(){
    using CampaignJournal::Operation;
    std::vector<Operation> ops={{"1","quest-pickup-v1","q 1 3 ref base 1 101"},
      {"2","quest-pickup-v1","q 1 4 ref2 base2 1 202"},
      {"3","quest-pickup-v1","r 1 3 ref3 base3 2 101"}};
    auto history=QuestPickupHistory(ops);
    assert(history&&history->size()==3);
    assert(ShouldRestorePickup(history->at(0),101,1,false));
    assert(!ShouldRestorePickup(history->at(0),202,1,false));
    assert(!ShouldRestorePickup(history->at(0),101,2,false));
    assert(!ShouldRestorePickup(history->at(0),101,1,true));
    assert(!ShouldRestorePickup(history->at(2),101,1,false));
    ops.push_back({"4","quest-consumed-v1","q 1 3 ref base 1 101"});
    history=QuestPickupHistory(ops);assert(history);
    assert(!ShouldRestorePickup(history->at(0),101,1,false));
    assert(ShouldReconcileSpentPickup(history->at(0),101,1,true,true));
    assert(!ShouldReconcileSpentPickup(history->at(0),202,1,true,true));
    assert(!ShouldReconcileSpentPickup(history->at(0),101,2,true,true));
    assert(!ShouldReconcileSpentPickup(history->at(0),101,1,false,true));
    assert(!ShouldReconcileSpentPickup(history->at(0),101,1,true,false));
    assert(!ShouldReconcileSpentPickup(history->at(1),202,1,true,true));
    assert(ShouldRestorePickup(history->at(1),202,1,false));
    ops.push_back({"5","quest-consumed-v1","q 1 4 wrong base2 1 202"});
    assert(!QuestPickupHistory(ops));
}
''')

spent=extract('client/Services/Generic/QuestJournal.cpp','ExtraDataList* FindSpentQuestStack','// Keep attempts local')
run('exact native stack reconciliation after confirmed consumption',headers+'''
using GameId=unsigned;
struct TESBoundObject{};
enum class ExtraDataType{Count};
struct ExtraCount{int count=1;};
struct ExtraDataList{unsigned quest=10,ref=20,alias=3;ExtraCount count;void* GetByType(ExtraDataType){return &count;}};
template<class T>T* Cast(void* p){return static_cast<T*>(p);}
struct Inventory{struct Entry{};};
struct InventoryChangeEvent{unsigned FormId;Inventory::Entry Item;GameId QuestId{},QuestRef{};unsigned QuestAlias{};};
void CaptureQuestItem(ExtraDataList* extra,void*,TESBoundObject*,InventoryChangeEvent& e,bool allowStopped){assert(allowStopped);e.QuestId=extra->quest;e.QuestRef=extra->ref;e.QuestAlias=extra->alias;}
enum class ITEM_REMOVE_REASON{kRemove};
struct ScopedInventoryOverride{};struct ScopedQuestOverride{};struct ScopedEquipOverride{};
struct Entry{TESBoundObject* form;int count;std::vector<ExtraDataList*>* dataList;};
struct Changes{std::vector<Entry*>* entries;};
struct PlayerCharacter{
 unsigned formID=14;std::vector<Entry*> entries;Changes changes{&entries};bool reject=false;int removals=0;
 void* GetContainer(){return this;}Changes* GetContainerChanges(){return &changes;}
 int GetItemCountInInventory(TESBoundObject* base){int n=0;for(auto* e:entries)if(e&&e->form==base)n+=e->count;return n;}
 void RemoveItem(TESBoundObject* base,int n,ITEM_REMOVE_REASON,ExtraDataList* stack,void*){
  assert(n==1&&stack);++removals;if(reject)return;
  for(auto* e:entries)if(e&&e->form==base&&e->dataList){
   auto it=std::find(e->dataList->begin(),e->dataList->end(),stack);
   if(it!=e->dataList->end()){e->dataList->erase(it);--e->count;return;}
  }
 }
};
'''+spent+'''
int main(){
 TESBoundObject base,other;ExtraDataList exact,ordinary,duplicate,wrong;ordinary.quest=0;wrong.ref=21;
 std::vector<ExtraDataList*> stacks{&ordinary,&exact,&wrong},otherStacks{&duplicate};
 Entry a{&base,3,&stacks},b{&other,1,&otherStacks};PlayerCharacter player;player.entries={&a,&b};
 assert(FindSpentQuestStack(player,&base,10,20,3)==&exact);
 exact.count.count=2;assert(!FindSpentQuestStack(player,&base,10,20,3));exact.count.count=1;
 stacks.push_back(&duplicate);++a.count;assert(!ReconcileSpentQuestStack(player,&base,10,20,3)&&!player.removals);stacks.pop_back();--a.count;
 player.reject=true;assert(!ReconcileSpentQuestStack(player,&base,10,20,3)&&player.removals==1);
 player.reject=false;assert(ReconcileSpentQuestStack(player,&base,10,20,3)&&player.removals==2);
 assert(a.count==2&&stacks.size()==2&&stacks[0]==&ordinary&&stacks[1]==&wrong&&b.count==1);
 assert(!ReconcileSpentQuestStack(player,&base,10,20,3)&&player.removals==2);
}
''')

hp=extract('server/Services/ActorValueService.cpp','void ActorValueService::OnHealthChangeBroadcast(','void ActorValueService::OnDeathStateChange')
run('actual server health relay over an actor ownership transfer',headers+'''
#include <cmath>
namespace entt {enum class entity:uint32_t{};}
struct RequestHealthChangeBroadcast{uint32_t Id, OwnershipEpoch;float DeltaHealth;bool KeepAlive=false;};
using NotifyHealthChangeBroadcast=RequestHealthChangeBroadcast;
template<class T>struct PacketEvent{T Packet;int* pPlayer;};
struct ActorValuesComponent{struct{std::map<int,float> ActorValuesList{{24,100.f}};}CurrentActorValues;};
struct OwnerComponent{int* player;uint32_t OwnershipEpoch;int* GetOwner(){return player;}int* GetOwner()const{return player;}};
struct View{ActorValuesComponent& v;OwnerComponent& o;std::optional<int> find(entt::entity id){return (int)id==9?std::optional<int>(1):std::nullopt;}std::optional<int> end(){return std::nullopt;}
template<class T>T& get(int){if constexpr(std::is_same_v<T,OwnerComponent>)return o;else return v;}};
struct World{int owner=2;OwnerComponent o{&owner,1};ActorValuesComponent v;
template<class...T>View view(){return {v,o};}};
struct GameServer{static GameServer* Get(){static GameServer s;return &s;}std::vector<NotifyHealthChangeBroadcast> sent;
bool SendToPlayersInRange(const NotifyHealthChangeBroadcast& m,entt::entity,int*){sent.push_back(m);return true;}};
namespace spdlog{template<class...T>void error(T&&...){}}
struct ActorValueService{mutable World m_world;void OnHealthChangeBroadcast(const PacketEvent<RequestHealthChangeBroadcast>&) const noexcept;};
'''+hp+'''
int main(){ActorValueService svc;int sender=1;
auto send=[&](int id,int epoch,float delta){svc.OnHealthChangeBroadcast({{(uint32_t)id,(uint32_t)epoch,delta},&sender});};
send(9,1,-10);svc.m_world.o.OwnershipEpoch=2;send(9,1,-90);send(7,2,-1);send(9,0,-10);
send(9,2,std::numeric_limits<float>::quiet_NaN());send(9,2,3);
svc.m_world.o.player=nullptr;send(9,2,-3);
auto& got=GameServer::Get()->sent;assert(got.size()==2&&got.back().OwnershipEpoch==2&&got.back().DeltaHealth==3);
assert(svc.m_world.v.CurrentActorValues.ActorValuesList.at(24)==93);
svc.m_world.o.player=&svc.m_world.owner;
svc.OnHealthChangeBroadcast({{9,2,-200.f,true},&sender});
assert(svc.m_world.v.CurrentActorValues.ActorValuesList.at(24)==1.f && got.back().KeepAlive);
send(9,2,-10.f);assert(svc.m_world.v.CurrentActorValues.ActorValuesList.at(24)<=0.f);
}
''')
