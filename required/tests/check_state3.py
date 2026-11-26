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
radiant=extract(qs,'bool IsPersonalRadiantQuest(TESQuest* quest)','std::string QuestService::CaptureQuestTargets')
radiantMatch=extract(qs,'static bool MatchesQuestTargets','void QuestService::ReceiveQuestProgress')
run('personal radiant classification is independent of native fill state',headers+'''
#include <string_view>
struct TESQuest;
struct NativeQuestAlias {
 struct TypeName{const char* value;const char* AsAscii()const{return value;}} type{"Ref"};
 TESQuest* Owner{};uint16_t FillType{1};
 const TypeName& QType()const{return type;}
};
struct TESQuest{
 enum Flags{RunOnce=256};uint16_t flags{};
 struct Aliases {void** data{};size_t length{},capacity{};
 void** begin()const{return data;}void** end()const{return data?data+length:nullptr;}} aliases;
 bool stopped{};uint32_t currentInstanceID{5};
 bool IsStopped()const{return stopped;}
};
struct NotifyQuestUpdate{
 std::string Targets;bool CheckInstance{};uint32_t ExpectedInstance{};
};
'''+radiant+radiantMatch+'''
int main(){
 TESQuest q,other;NativeQuestAlias a,b;
 a.Owner=b.Owner=&q;void* slots[]={&a,&b};
 q.aliases={slots,2,2};
 assert(!IsPersonalRadiantQuest(&q));
 for(uint16_t fill:{0,2,3,4,5,6,7,8}){
    a.FillType=fill;assert(IsPersonalRadiantQuest(&q));
    q.stopped=true;assert(IsPersonalRadiantQuest(&q));q.stopped=false;
    q.flags=TESQuest::RunOnce;assert(!IsPersonalRadiantQuest(&q));q.flags=0;
 }
 a.FillType=1;b.type.value="Loc";assert(IsPersonalRadiantQuest(&q));
 b.type.value=nullptr;assert(IsPersonalRadiantQuest(&q));b.type.value="Ref";
 b.Owner=&other;assert(IsPersonalRadiantQuest(&q));b.Owner=&q;
 q.aliases.capacity=1;assert(IsPersonalRadiantQuest(&q));q.aliases.capacity=2;
 slots[1]=nullptr;assert(IsPersonalRadiantQuest(&q));slots[1]=&b;
 NotifyQuestUpdate u;assert(MatchesQuestTargets(&q,u));
 a.FillType=0;assert(!MatchesQuestTargets(&q,u));a.FillType=1;
 u.Targets="2:r:616263.1;";assert(!MatchesQuestTargets(&q,u));
 u.Targets.clear();u.CheckInstance=true;u.ExpectedInstance=4;assert(!MatchesQuestTargets(&q,u));
 u.ExpectedInstance=5;assert(MatchesQuestTargets(&q,u));
 q.stopped=true;assert(!MatchesQuestTargets(&q,u));
}
''')
receive=extract(qs,'void QuestService::ReceiveQuestProgress','QuestService::QuestService')
run('automatic accepted party quest dispatch',headers+'''
struct NotifyQuestUpdate {uint32_t Id{},LocalInstance{},SharedRun{},Encounter{};};
struct TESForm { static TESForm* GetById(uint32_t); };
struct TESQuest:TESForm {uint32_t currentInstanceID{17};bool personal{};} quest;
TESForm* TESForm::GetById(uint32_t id){return id==10?&quest:nullptr;}
template<class T>T* Cast(TESForm* form){return static_cast<T*>(form);}
struct World {
    struct Party {bool joined{true};bool IsInParty(){return joined;}} party;
    struct Mods {uint32_t GetGameId(uint32_t id){return id;}} mods;
    Party& GetPartyService(){return party;}
    Mods& GetModSystem(){return mods;}
};
bool matching=true;
bool MatchesQuestTargets(TESQuest*,const NotifyQuestUpdate&){return matching;}
struct QuestService {
    World m_world;std::vector<NotifyQuestUpdate> applied;
    static bool IsNonSyncableQuest(TESQuest* q){return q->personal;}
    void OnQuestUpdate(const NotifyQuestUpdate& u){applied.push_back(u);}
    void ReceiveQuestProgress(const NotifyQuestUpdate&) noexcept;
};
'''+receive+'''
int main(){
    QuestService service;
    NotifyQuestUpdate notice{10,0,1,99};
    service.ReceiveQuestProgress(notice);
    assert(service.applied.size()==1&&service.applied.back().LocalInstance==17);
    assert(service.applied.back().Encounter==99);
    service.m_world.party.joined=false;service.ReceiveQuestProgress(notice);
    service.m_world.party.joined=true;quest.personal=true;service.ReceiveQuestProgress(notice);
    quest.personal=false;matching=false;service.ReceiveQuestProgress(notice);
    matching=true;notice.Id=11;service.ReceiveQuestProgress(notice);
    assert(service.applied.size()==1);
    notice.Id=10;notice.SharedRun=0;service.ReceiveQuestProgress(notice);
    assert(service.applied.size()==2);
}
''')
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
struct NotifyQuestUpdate{enum {StageUpdate,Started,Stopped};GameId Id;uint16_t Stage;uint8_t Status;uint8_t ClientQuestType{};std::string Targets{};uint32_t LocalInstance{};bool CheckInstance{};uint32_t ExpectedInstance{};};
struct TESForm{static TESForm* GetById(uint32_t);};
template<class T> T* Cast(TESForm* p){return static_cast<T*>(p);}
struct Conditions{uint16_t wants;};
struct TESConditionItem {
 enum Function:uint16_t{kGetStageDone=59};enum class OpCode:uint8_t{kEqualTo,kNotEqualTo,kGreaterThan,kGreaterThanOrEqualTo,kLessThan,kLessThanOrEqualTo};
 TESConditionItem* pNext{};float comparisonValue{};uint16_t function=kGetStageDone;void* params[2]{};uint8_t flags{},object{};
 OpCode GetOpCode() const{return static_cast<OpCode>(flags>>5);}
};
struct TESCondition{TESConditionItem* pHead{};};
struct BGSScenePhase{Conditions completionConditions;};
struct PhaseList{int length=1;BGSScenePhase* phase;auto operator[](int)const{return phase;}};
struct BGSScene{bool isPlaying=true;int currentPhaseIndex=0;PhaseList phases;int formID=44;bool ready=false;};
struct TESQuest:TESForm{
struct{struct{const char* AsAscii(){return "Quest";}}value;}fullName;enum class Type{Other,None,Miscellaneous};Type type=Type::Other;
uint32_t formID=2,currentInstanceID=1;uint16_t currentStage=0;bool stopped=false;std::string targets;
enum Flags{RunOnce=256,Completed=2,AllowRepeatStages=8};uint16_t flags{};
struct Stage{uint16_t stageIndex;std::vector<void*> items;};std::vector<Stage*> stages;
bool IsStageDone(uint16_t s){return reached.contains(s);}
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
struct Party{bool leader=true;bool IsLeader(){return leader;}};
struct World{static World& Get(){static World w;return w;}ModSystem mods;Party party;ModSystem& GetModSystem(){return mods;}Party& GetPartyService(){return party;}};
namespace spdlog{
template<class...T>void debug(T&&...){}
template<class...T>void info(T&&...){}
template<class...T>void error(T&&...){}
}
struct QuestService{
World& m_world=World::Get();struct HeldStages{Vector<uint16_t> Stages,PendingItems;std::chrono::steady_clock::time_point RetryAt{};std::string Targets;uint32_t LocalInstance{};bool CheckInstance{};bool BranchConflict{};};
std::map<uint32_t,std::pair<uint32_t,std::string>> m_lastQuestTargets;
std::map<uint32_t,uint32_t> m_branchConflicts;
static std::string CaptureQuestTargets(TESQuest* q){return q->targets;}
Map<uint32_t,HeldStages> m_heldStages; Map<uint32_t,Vector<NotifyQuestUpdate>> m_afterHeldStages;
Map<uint32_t,std::chrono::steady_clock::time_point> m_questRetryAt;
void OnUpdate(const UpdateEvent&) noexcept;void OnDisconnected(const DisconnectedEvent&) noexcept;void OnPartyLeft(const PartyLeftEvent&) noexcept;
void HoldStage(TESQuest*,uint16_t) noexcept;HeldStages* GetHeldStages(uint32_t,TESQuest*) noexcept;
void OnQuestUpdate(const NotifyQuestUpdate&) noexcept;static bool StopQuest(uint32_t);
};
using HeldStages=QuestService::HeldStages;
static bool MatchesQuestTargets(TESQuest* q,const NotifyQuestUpdate& u){return q&&(!u.CheckInstance||(!q->stopped&&q->currentInstanceID==u.ExpectedInstance))&&(u.Targets.empty()||(!q->stopped&&q->currentInstanceID==u.LocalInstance&&q->targets==u.Targets));}
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
    send(91,0);assert(quest.IsStopped()&&!quest.IsStageReached(91));
    // A guest must obey the same scene and fragment barriers as the host.
    quest.flags=0;quest.stopped=false;quest.reached.clear();quest.actions.clear();
    service.m_world.party.leader=false;
    scene.isPlaying=true;scene.ready=false;send(100,0);send(20,0);
    service.OnUpdate({});assert(quest.actions.empty());
    scene.ready=true;service.OnUpdate({});assert((quest.actions==Vector<int>{100}));
    service.OnUpdate({});assert((quest.actions==Vector<int>{100}));
    service.m_heldStages.at(2).PendingItems.clear();
    service.OnUpdate({});assert((quest.actions==Vector<int>{100,20}));
    service.OnPartyLeft({});
    // No playing scene: an applied stage can still start an async fragment.
    scene.isPlaying=false;quest.reached.clear();quest.actions.clear();
    send(100,0);send(20,0);send(20,2);
    assert((quest.actions==Vector<int>{100})&&!quest.IsStopped());
    service.OnUpdate({});assert((quest.actions==Vector<int>{100}));
    service.m_heldStages.at(2).PendingItems.clear();
    service.OnUpdate({});service.OnUpdate({});
    assert((quest.actions==Vector<int>{100,20,-1}));
    // A queued result must not survive a change of native quest instance.
    quest.stopped=false;quest.reached.clear();quest.actions.clear();quest.targets="2:r:616263.1;";
    quest.currentInstanceID=7;scene.isPlaying=true;scene.ready=false;
    service.OnQuestUpdate({2,100,0,0,quest.targets,7});
    assert(service.m_heldStages.count(2));quest.currentInstanceID=8;
    scene.isPlaying=false;service.OnUpdate({});
    assert(quest.actions.empty()&&service.m_heldStages.empty());
    service.OnQuestUpdate({2,20,0,0,"2:r:616263.2;",8});
    assert(quest.actions.empty());
    service.OnQuestUpdate({2,20,0,0,quest.targets,8});
    assert((quest.actions==Vector<int>{20}));
    service.OnPartyLeft({});
    quest.stopped=false;quest.targets.clear();quest.reached={5};quest.actions.clear();
    TESConditionItem excludes;excludes.params[0]=&quest;excludes.params[1]=reinterpret_cast<void*>(5);
    TESCondition entry{&excludes};TESQuest::Stage branch{30,{&entry}};quest.stages={&branch};
    assert(StageExcludedByLocalBranch(&quest,30));
    TESConditionItem unknown;unknown.function=47;excludes.flags=1;excludes.pNext=&unknown;
    assert(!StageExcludedByLocalBranch(&quest,30));
    excludes.flags=0;excludes.pNext=nullptr;TESCondition unrestricted;branch.items.push_back(&unrestricted);
    assert(!StageExcludedByLocalBranch(&quest,30));branch.items.pop_back();
    quest.flags=TESQuest::AllowRepeatStages;assert(!StageExcludedByLocalBranch(&quest,30));quest.flags=0;
    send(30,0);send(40,0);send(40,2);
    assert(quest.actions.empty()&&!quest.stopped&&service.m_branchConflicts.count(2));
    ++quest.currentInstanceID;quest.reached.clear();send(30,0);
    assert((quest.actions==Vector<int>{30})&&!service.m_branchConflicts.count(2));
    service.OnPartyLeft({});quest.reached.clear();quest.actions.clear();scene.isPlaying=true;scene.ready=false;
    send(30,0);send(40,0);send(40,2);quest.reached.insert(5);scene.isPlaying=false;
    service.OnUpdate({});assert(quest.actions.empty()&&!quest.stopped&&service.m_branchConflicts.count(2));
    assert(service.m_heldStages.empty()&&service.m_afterHeldStages.empty());
    service.OnPartyLeft({});quest.stages.clear();quest.reached.clear();quest.actions.clear();
    quest.stopped=false;scene.isPlaying=true;scene.ready=false;
    send(20,2);
    assert(!quest.stopped&&service.m_afterHeldStages.count(2)&&service.m_heldStages.empty());
    service.OnUpdate({});assert(!quest.stopped);
    scene.ready=true;service.m_questRetryAt.at(2)={};service.OnUpdate({});
    assert(quest.stopped&&quest.actions==Vector<int>{-1});
    quest.stopped=false;quest.actions.clear();scene.ready=false;send(20,2);
    assert(service.m_afterHeldStages.count(2));
    ++quest.currentInstanceID;scene.isPlaying=false;service.m_questRetryAt.at(2)={};
    service.OnUpdate({});assert(!quest.stopped&&quest.actions.empty()&&service.m_afterHeldStages.empty());
}
''')

server=extract('server/Services/QuestService.cpp','bool AdvanceQuestRun(', 'std::string QuestKey(')
run('party quest lifecycle and ordered catch-up history',headers+'''
struct NotifyQuestUpdate{enum {StageUpdate,Started,Stopped};uint16_t Stage;uint8_t Status;std::string Targets{};};
// The target format is checked separately; this harness exercises actual run transitions.
bool ValidQuestTargets(std::string_view value){return value.size()<=2048&&value!="!";}
struct QuestRun{unsigned Generation{1};std::set<uint16_t> Stages;std::vector<uint16_t> OrderedStages; bool Stopped=false;bool Started=false;bool CompletedRunOnce=false;std::string Targets;};
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
    QuestRun radiant;NotifyQuestUpdate a{10,1,"2:r:616263.1;"},wrong{20,0,"2:r:616263.2;"},right{20,0,a.Targets};
    assert(AdvanceQuestRun(radiant,a));assert(!AdvanceQuestRun(radiant,wrong));assert(AdvanceQuestRun(radiant,right));
    NotifyQuestUpdate done{20,2,a.Targets};assert(AdvanceQuestRun(radiant,done));
    wrong.Status=1;assert(AdvanceQuestRun(radiant,wrong)&&radiant.Generation==2&&radiant.Targets==wrong.Targets);
    assert(!AdvanceQuestRun(radiant,right));
}
''')

history=extract('client/Services/Generic/QuestJournal.cpp','struct QuestReplayState','struct QuestPickup')
run('active quest catch-up from existing campaign history',headers+'''
#include <QuestRunMember.h>
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
    assert(history->at("q").CanRestoreStart(101)&&history->at("q").CanRestoreStart(202));
    assert(!history->at("q").CanRestoreStart(0));
    assert(history->at("q").StartStage==10&&history->at("q").StartType==6);
    ops.push_back({"5","quest-progress-v1","q 1 2 20 6 202"});
    history=ActiveQuestHistory(ops);
    assert(history&&history->at("q").Stopped&&history->at("q").Stages.size()==2);
    assert(!history->at("q").CanRestoreStart(101));
    ops.push_back({"6","quest-progress-v1","q 2 1 10 6 101"});
    ops.push_back({"7","quest-progress-v1","q 2 0 100 6 101"});
    history=ActiveQuestHistory(ops);
    assert(history&&history->at("q").Generation==2&&!history->at("q").Stopped);
    assert(!history->at("q").CanRestoreStart(101));
    ops.push_back({"8","quest-progress-v1","q 4 0 90 6 101"});
    assert(!ActiveQuestHistory(ops));
    ops={{"1","quest-progress-v1","q 1 1 10 6 101"},{"2","quest-progress-v1","q 1 0 80 6 101"},{"3","quest-completed-v1","q 1 2 80 6 101"}};
    history=ActiveQuestHistory(ops);assert(history&&history->at("q").Stopped&&history->at("q").CompletedRunOnce);
    assert(history->at("q").Stages.size()==1&&history->at("q").Stages[0].first==80);
    assert(history->at("q").CanRestoreStart(101)&&history->at("q").CanRestoreStart(202));
    auto incomplete=history->at("q");incomplete.StopStage=99;
    assert(!incomplete.CanRestoreStart(202));
    incomplete=history->at("q");incomplete.StartedBy=0;
    assert(!incomplete.CanRestoreStart(202));
    incomplete=history->at("q");incomplete.Targets="2:r:616263.1;";
    assert(!incomplete.CanRestoreStart(202));
    ops.push_back({"4","quest-progress-v1","q 2 1 10 6 202"});assert(!ActiveQuestHistory(ops));
    ops={{"1","quest-progress-v1","q 1 1 10 6 101 2:r:616263.1;"},
         {"2","quest-progress-v1","q 1 0 20 6 202 2:r:616263.1;"}};
    history=ActiveQuestHistory(ops);assert(history&&history->at("q").Targets=="2:r:616263.1;");
    ops[1].Payload="q 1 0 20 6 202 2:r:616263.2;";assert(!ActiveQuestHistory(ops));
    ops={{"1","quest-progress-v1","q 1 1 10 6 101 2:r:616263.1; @17"},
         {"2","quest-binding-v1","q 1 1 10 6 202 2:r:616263.1; @83"},
         {"3","quest-progress-v1","q 1 0 20 6 202 2:r:616263.1; @83"},
         {"4","quest-progress-v1","q 1 2 20 6 101 2:r:616263.1; @17"},
         {"5","quest-progress-v1","q 2 1 10 6 101 2:r:616263.1; @18"},
         {"6","quest-binding-v1","q 2 1 10 6 202 2:r:616263.1; @84"},
         {"7","quest-progress-v1","q 2 0 20 6 202 2:r:616263.1; @84"}};
    history=ActiveQuestHistory(ops);assert(history&&history->at("q").Generation==2);
    const auto& latest=history->at("q");
    assert(QuestRunMember::Bound(latest.Members,101,2,18));
    assert(QuestRunMember::Bound(latest.Members,202,2,84));
    assert(latest.Prior.size()==1&&latest.Prior[0].Stopped&&latest.Prior[0].Stages.size()==1);
    assert(QuestRunMember::Bound(latest.Members,202,1,83));
    assert(!latest.CanRestoreStart(101)&&!latest.Prior[0].CanRestoreStart(101));
    ops[6].Payload="q 2 0 20 6 202 2:r:616263.1; @83";assert(!ActiveQuestHistory(ops));
}
''', ['-I'+str(code/'common')])

catchup=extract('client/Services/Generic/QuestJournal.cpp',
    'template<class CanDispatch>\nvoid ReconcileRepeatStarts','\n}\n}\n\nuint32_t ExpectedRepeatRun')+'\n}'
run('late completed run starts natively and missing stages remain retryable',headers+'''
#include <QuestRunMember.h>
#include <TradeProperties.h>
#include <sstream>
namespace CampaignJournal {
struct Operation{std::string Id,Kind,Payload;};
struct Journal{std::vector<Operation> rows;const auto& Operations() const{return rows;}};
}
using GameId=uint32_t;
struct NotifyQuestUpdate{
 enum {StageUpdate,Started,Stopped};GameId Id{};uint16_t Stage{};uint8_t ClientQuestType{},Status{};
 uint32_t SharedRun{},Speaker{},ExpectedInstance{};bool CheckInstance{};std::string Targets;
};
struct RequestQuestUpdate{
 enum {StageUpdate,Started,Stopped};GameId Id{};uint16_t Stage{};uint8_t ClientQuestType{},Status{};
 bool BindInstance{};uint32_t Instance{},ExpectedRun{};std::string Targets;
};
struct ScopedQuestOverride{};
struct TESForm{static TESForm* GetById(uint32_t);};
struct TESQuest:TESForm{
 enum Flags{RunOnce=256,Completed=2,Failed=64,AllowRepeatStages=8};
 uint32_t formID=2,currentInstanceID=1;uint16_t flags=RunOnce;
 bool stopped=true;std::string targets;std::set<uint16_t> reached;
 uint16_t currentStage{};int starts{};bool promotion{};
 enum class State{WaitingPromotion,Stopped,Running};
 State getState(){return promotion?State::WaitingPromotion:stopped?State::Stopped:State::Running;}
 bool EnsureQuestStarted(bool& success,bool force){assert(!force);++starts;stopped=false;++currentInstanceID;success=true;reached={0};currentStage=0;return true;}
 bool IsStopped()const{return stopped;}
 bool IsStageReached(uint16_t s)const{return reached.contains(s);}
} quest;
TESForm* TESForm::GetById(uint32_t id){return id==2?&quest:nullptr;}
template<class T>T* Cast(TESForm* p){return static_cast<T*>(p);}
struct PlayerCharacter {
 struct Instance{TESQuest* quest;};struct Objective{Instance* instance;};
 std::vector<Objective> objectives;
 static PlayerCharacter* Get(){static PlayerCharacter p;return &p;}
};
struct Mod{
 bool lite=false,loaded=true;bool IsLoaded(){return loaded;}bool IsLite(){return lite;}
 uint32_t GetFormId(uint32_t local){return local;}
};
struct ModManager{
 Mod mod;static ModManager* Get(){static ModManager m;return &m;}
 Mod* GetByName(const char* name){return std::string(name)=="abc.esp"?&mod:nullptr;}
};
struct World{
 struct Party{bool joined=true;bool IsInParty(){return joined;}uint32_t GetLeaderPlayerId(){return 9;}} party;
 struct Overlay{bool personal=false;bool IsPersonalQuest(uint32_t){return personal;}} overlay;
 struct Mods{bool GetServerModId(uint32_t id,GameId& result){result=id;return true;}} mods;
 struct Transport{std::vector<RequestQuestUpdate> sent;void Send(const RequestQuestUpdate& update){sent.push_back(update);}} transport;
 Transport& GetTransport(){return transport;}
 bool loading=false;bool IsLoadingSave(){return loading;}
 Party& GetPartyService(){return party;}Overlay& GetOverlayService(){return overlay;}Mods& GetModSystem(){return mods;}
};
bool ready=true;bool SaveWorldReady(){return ready;}
struct QuestService{static bool IsNonSyncableQuest(TESQuest*){return false;}
 static std::string CaptureQuestTargets(TESQuest* q){return q->targets;}};
uint32_t LocalQuestForm(const std::string& value){const auto key=TradeProperties::DecodeForm(value);return key?key->Local:0;}
struct RepeatJoin{uint64_t Campaign{},Character{};uint32_t Run{},Before{};
 std::chrono::steady_clock::time_point RetryAt{};bool SawRunning{};};
std::map<uint32_t,RepeatJoin> repeatJoins;
'''+history+catchup+'''
int main(){
    World world;CampaignJournal::Journal journal;
    const auto key=TradeProperties::FormKey{"abc.esp",2}.Encode();
    journal.rows={{"1","quest-progress-v1",key+" 1 1 0 6 101"},
                  {"2","quest-progress-v1",key+" 1 0 40 6 101"},
                  {"3","quest-progress-v1",key+" 1 0 20 6 101"},
                  {"4","quest-completed-v1",key+" 1 2 20 6 101"}};
    bool busy=false;
    auto plan=[&]{return MissingActiveQuestStages(world,journal,202,[&](uint32_t id,uint32_t instance){
        assert(id==2&&instance==quest.currentInstanceID);return !busy;});};
    auto start=plan();assert(start.size()==1&&start[0].Status==NotifyQuestUpdate::Started&&start[0].Stage==0);
    // An issued but unapplied update is not a receipt.
    assert(plan().size()==1);
    busy=true;assert(plan().empty());busy=false;
    quest.stopped=false;quest.reached.insert(0);
    auto stages=plan();assert(stages.size()==2&&stages[0].Stage==40&&stages[1].Stage==20);
    assert(stages[0].Status==NotifyQuestUpdate::StageUpdate&&stages[1].Status==NotifyQuestUpdate::StageUpdate);
    quest.reached.insert(40);stages=plan();assert(stages.size()==1&&stages[0].Stage==20);
    busy=true;assert(plan().empty());busy=false;
    quest.reached.insert(20);assert(plan().empty());
    quest.stopped=true;quest.flags=TESQuest::RunOnce|TESQuest::Completed;assert(plan().empty());
    quest.flags=TESQuest::RunOnce|TESQuest::Failed;assert(plan().empty());
    quest.flags=TESQuest::RunOnce|TESQuest::AllowRepeatStages;assert(plan().empty());
    quest.flags=TESQuest::RunOnce;quest.reached.clear();
    world.overlay.personal=true;assert(plan().empty());world.overlay.personal=false;
    world.loading=true;assert(plan().empty());world.loading=false;
    journal.rows.back().Payload=key+" 1 2 99 6 101";assert(plan().empty());
    journal.rows.pop_back();assert(plan().size()==1);
    journal.rows.push_back({"4","quest-progress-v1",key+" 1 2 20 6 101"});assert(plan().empty());
    journal.rows[0].Payload=key+" 1 1 0 6 101 2:r:616263.1;";
    journal.rows[1].Payload=key+" 1 0 40 6 101 2:r:616263.1;";
    journal.rows[2].Payload=key+" 1 0 20 6 101 2:r:616263.1;";
    journal.rows[3]={"4","quest-completed-v1",key+" 1 2 20 6 101 2:r:616263.1;"};
    assert(plan().empty()); // no fabricated radiant target
    // A late second participant joins a witnessed fixed-target generation.
    quest.flags=0;quest.stopped=true;quest.targets.clear();quest.currentInstanceID=80;quest.reached.clear();
    journal.rows={{"1","quest-progress-v1",key+" 1 1 0 6 101 @17"},
                  {"2","quest-progress-v1",key+" 1 0 40 6 101 @17"}};
    auto join=[&]{ReconcileRepeatStarts(world,journal,55,202,[&](uint32_t,uint32_t){return !busy;});};
    busy=true;join();assert(!quest.starts);busy=false;
    join();assert(quest.starts==1&&quest.currentInstanceID==81&&world.transport.sent.empty());
    assert(plan().empty()); // no stage before an authoritative bind
    join();assert(world.transport.sent.size()==1&&world.transport.sent.back().ExpectedRun==1&&world.transport.sent.back().Instance==81);
    journal.rows.push_back({"3","quest-binding-v1",key+" 1 1 0 6 202 @81"});
    join();assert(repeatJoins.empty());stages=plan();
    assert(stages.size()==1&&stages[0].Stage==40&&stages[0].CheckInstance&&stages[0].ExpectedInstance==81);
    // Closed generation and a new start require a new native instance.
    journal.rows.push_back({"4","quest-progress-v1",key+" 1 2 40 6 101 @17"});
    journal.rows.push_back({"5","quest-progress-v1",key+" 2 1 0 6 101 @18"});
    quest.stopped=true;quest.currentInstanceID=79;join();assert(quest.starts==1);
    quest.currentInstanceID=81;join();assert(quest.starts==2&&quest.currentInstanceID==82);
    join();assert(world.transport.sent.back().ExpectedRun==2&&world.transport.sent.back().Instance==82);
    // A racing stop cannot turn the pending instance into generation three.
    journal.rows.push_back({"6","quest-progress-v1",key+" 2 2 0 6 101 @18"});
    journal.rows.push_back({"7","quest-progress-v1",key+" 3 1 0 6 101 @19"});
    const auto sent=world.transport.sent.size();join();assert(world.transport.sent.size()==sent&&repeatJoins.at(2).Run==2);
    // Nonempty targets must never be filled by a fabricated native start.
    repeatJoins.clear();quest.stopped=true;quest.targets="2:r:616263.1;";
    journal.rows={{"1","quest-progress-v1",key+" 1 1 0 6 101 2:r:616263.1; @17"}};
    join();assert(quest.starts==2);
}
''', ['-I'+str(code/'common')])

run('native instance identity across repeated quest generations',headers+'''
#include <QuestRunMember.h>
#include <sstream>
int main(){
    using namespace QuestRunMember;
    Members m;
    assert(Admit(m,101,1,17,false,true,false,false));
    assert(Remember(m,101,1,17));
    assert(Admit(m,101,1,17,false,false,true,false));
    assert(!Admit(m,101,1,18,false,false,true,false));
    assert(Admit(m,202,1,83,false,false,true,false));
    assert(Remember(m,202,1,83));
    assert(!Admit(m,101,1,17,true,true,false,false));
    assert(Admit(m,101,1,18,true,true,false,false));
    assert(Remember(m,101,2,18));
    assert(!Admit(m,101,2,17,true,true,false,false));
    assert(!Admit(m,202,2,83,false,true,false,false));
    assert(!Admit(m,202,2,84,false,false,true,false));
    assert(Admit(m,202,2,84,false,true,false,false));
    assert(Remember(m,202,2,84)&&Bound(m,202,2,84));
    assert(!Remember(m,202,2,83)&&!Remember(m,202,3,83));
    assert(!Admit(m,303,2,1,false,false,true,false));
    assert(CanAttach(m,303,2,2,1,true,false));
    assert(!CanAttach(m,303,3,2,1,true,false)&&!CanAttach(m,303,2,2,1,true,true));
    assert(!CanAttach(m,303,2,2,1,false,false));
    assert(CanAttach(m,202,2,2,84,true,false)&&!CanAttach(m,202,2,2,83,true,false));
    assert(CanStartMissing(m,303,3,1)&&CanStartMissing(m,202,3,84));
    assert(!CanStartMissing(m,202,3,99)&&!CanStartMissing(m,202,2,84));
    std::string targets;std::optional<uint32_t> id;
    std::istringstream old("2:r:616263.1;");assert(ReadTail(old,targets,id)&&!id);
    std::istringstream current("2:r:616263.1; @0");assert(ReadTail(current,targets,id)&&id==0);
    std::istringstream bare("@42");assert(ReadTail(bare,targets,id)&&targets.empty()&&id==42);
    std::istringstream wrong("@042");assert(!ReadTail(wrong,targets,id));
    std::istringstream invalid("@4294967296");assert(!ReadTail(invalid,targets,id));
    std::istringstream extra("@42 x");assert(!ReadTail(extra,targets,id));
}
''', ['-I'+str(code/'common')])

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
    assert(!CanRecordQuestPickup(initial,true,2));
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
    const std::string second=prefix+"ref item 1 202";
    assert(QuestItemChange(ops,prefix,second,false)==D::Copy);
    assert(QuestItemChange(ops,prefix,second,true)==D::Reject);
    ops.pop_back(); // unspent original
    ops.push_back({"2","quest-copy-v1",second});
    assert(QuestItemChange(ops,prefix,second,false)==D::Duplicate);
    assert(QuestItemChange(ops,prefix,second,true)==D::ConsumeCopy);
    assert(QuestItemChange(ops,prefix,prefix+"ref item 1 303",true)==D::Reject);
    ops.push_back({"3","quest-consumed-v2",second});
    assert(QuestItemChange(ops,prefix,pickup,true)==D::Duplicate);
    assert(QuestItemChange(ops,prefix,second,true)==D::Duplicate);
    assert(QuestItemChange(ops,prefix,prefix+"other item 1 303",false)==D::Reject);
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
    assert(ShouldReconcileSpentPickup(history->at(0),101,1));
    assert(!ShouldReconcileSpentPickup(history->at(0),202,1));
    assert(!ShouldReconcileSpentPickup(history->at(0),101,2));
    assert(!ShouldReconcileSpentPickup(history->at(1),202,1));
    assert(ShouldRestorePickup(history->at(1),202,1,false));
    ops.push_back({"5","quest-consumed-v1","q 1 4 wrong base2 1 202"});
    assert(!QuestPickupHistory(ops));
    ops={{"1","quest-pickup-v1","q 1 3 ref base 1 101"},
         {"2","quest-copy-v1","q 1 3 ref base 1 202"}};
    history=QuestPickupHistory(ops);assert(history&&history->size()==1);
    assert(ShouldRestorePickup(history->at(0),101,1,false));
    assert(!ShouldRestorePickup(history->at(0),202,1,false));
    assert(!ShouldReconcileSpentPickup(history->at(0),202,1));
    ops.push_back({"3","quest-consumed-v2","q 1 3 ref base 1 202"});
    history=QuestPickupHistory(ops);assert(history&&history->at(0).Consumed);
    assert(ShouldReconcileSpentPickup(history->at(0),101,1));
    assert(ShouldReconcileSpentPickup(history->at(0),202,1));
    assert(!ShouldReconcileSpentPickup(history->at(0),303,1));
    assert(!ShouldRestorePickup(history->at(0),101,1,false));
    ops[2].Payload="q 1 3 ref base 1 303";assert(!QuestPickupHistory(ops));
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

run('word acquisition and durable per-character learning',headers+'''
#include <QuestWordReward.h>
int main(){
    using namespace QuestWordReward;
    Sample a{11,100,2,{10,20}},b=a;
    assert(NewWords(a,b).empty());b.Known.insert(30);assert((NewWords(a,b)==std::vector<uint32_t>{30}));
    b.Cell=101;assert(NewWords(a,b).empty());b.Cell=100;b.Peer=3;assert(NewWords(a,b).empty());
    b.Peer=2;b.Character=12;assert(NewWords(a,b).empty());b.Character=11;
    b.Known.erase(10);assert(NewWords(a,b).empty());b.Known.insert(10);
    b.Known.insert(40);assert((NewWords(a,b)==std::vector<uint32_t>{30,40}));
    b.Known.insert(0);assert(NewWords(a,b).empty());b.Known.erase(0);
    const auto first=TradeProperties::FormKey{"skyrim.esm",123}.Encode();
    const auto second=TradeProperties::FormKey{"skyrim.esm",124}.Encode();
    std::set<std::string> batch{first,second};
    assert(DecodeBatch(EncodeBatch(batch))==batch&&DecodeBatch(first)==std::set<std::string>{first});
    assert(!DecodeBatch(first+" "+first)&&!DecodeBatch(second+" "+first));
    assert(!DecodeBatch(first+" ")&&!DecodeBatch(" "+first)&&!DecodeBatch("bad")&&!DecodeBatch(""));
    std::set<std::string> many;
    for (uint32_t n=1;n<=40;++n) many.insert(TradeProperties::FormKey{"skyrim.esm",n}.Encode());
    auto encoded=EncodeBatch(many);
    auto decoded=DecodeBatch(encoded);
    assert(decoded&&decoded->size()==MaxBatch&&encoded.size()<=MaxPayload);
    for (const auto& word:*decoded) assert(many.erase(word)==1);
    assert(!many.empty()&&DecodeBatch(EncodeBatch(many))->size()==MaxBatch);
    many.clear();
    for (uint32_t n=1;n<=16;++n) many.insert(TradeProperties::FormKey{std::string(99,'a')+".esp",n}.Encode());
    encoded=EncodeBatch(many);decoded=DecodeBatch(encoded);
    assert(decoded&&!decoded->empty()&&decoded->size()<MaxBatch&&encoded.size()<=MaxPayload);
    Grant g{"736b7972696d2e65736d.123",11,22};
    assert(g.Valid()&&Decode(g.Encode())&&g.Entitles(11)&&g.Entitles(22)&&!g.Entitles(33));
    assert(!Decode(g.Encode()+" ")&&!Decode("bad 11 22")&&!Decode(g.Word+" 11 11"));
    std::vector<CampaignJournal::Operation> ops={{"1","quest-word-v1",g.Encode()}};
    auto grants=Read(ops);assert(grants&&Entitled(*grants,g.Word,11)&&Entitled(*grants,g.Word,22));
    assert(!Entitled(*grants,g.Word,33));
    ops.push_back({"2","quest-word-v1",g.Word+" 22 33"});assert(!Read(ops));
    ops.pop_back();ops.push_back({"2","quest-word-v1",g.Word+" 11 33"});assert(!Read(ops));
    ops.pop_back();ops.push_back({"2","quest-word-v1",g.Word+" 33 22"});assert(Read(ops));
}
''', ['-I'+str(code/'common')])

run('native dragon absorption receipt identity and duplicates',headers+'''
#include <DragonAbsorption.h>
int main(){
    using namespace DragonAbsorption;
    Receipt a{"736b7972696d2e65736d.123",11},b{a.Dragon,22};
    assert(a.Valid()&&b.Valid()&&Decode(a.Encode())&&Decode(b.Encode()));
    assert(!Decode(a.Encode()+" ")&&!Decode("bad 11")&&!Decode(a.Dragon+" 0"));
    std::vector<CampaignJournal::Operation> ops={{"1","dragon-absorbed-v1",a.Encode()}};
    auto receipts=Read(ops);assert(receipts&&receipts->contains(a.Encode())&&!receipts->contains(b.Encode()));
    ops.push_back({"2","dragon-absorbed-v1",b.Encode()});
    receipts=Read(ops);assert(receipts&&receipts->size()==2);
    ops.push_back({"3","dragon-absorbed-v1",a.Encode()});assert(!Read(ops));
    ops.pop_back();ops[1].Payload="bad 22";assert(!Read(ops));
}
''', ['-I'+str(code/'common')])

run('shared dragon death cohort and one immutable grant',headers+'''
#include <DragonAbsorption.h>
int main(){
    using namespace DragonAbsorption;
    const std::string key="736b7972696d2e65736d.123";
    Kill kill{key,11,22};assert(kill.Valid()&&DecodeKill(kill.Encode()));
    assert(kill.Other(11)==22&&kill.Other(22)==11&&!kill.Other(33));
    assert(!DecodeKill(key+" 22 11")&&!DecodeKill(key+" 11 11"));
    std::vector<CampaignJournal::Operation> ops={{"1","dragon-death-v1",kill.Encode()}};
    auto h=ReadShared(ops);assert(h&&h->Kills.at(key).Revision==1);
    assert(!h->Plan({key,33}));const auto grant=h->Plan({key,11});
    assert(h->Eligible({key,11})&&h->Eligible({key,22})&&!h->Eligible({key,33}));
    assert(grant&&grant->Recipient==22);
    ops.push_back({"2","dragon-shared-v1",grant->Encode()});assert(!ReadShared(ops));
    ops.pop_back();ops.push_back({"2","dragon-absorbed-v1",Receipt{key,11}.Encode()});
    h=ReadShared(ops);assert(h&&MissingGrants(*h,ops).size()==1);
    assert(MissingGrants(*h,ops)[0].Encode()==grant->Encode());
    ops.push_back({"3","dragon-shared-v1",grant->Encode()});
    h=ReadShared(ops);assert(h&&h->Grants.size()==1&&!h->Plan({key,22}));
    assert(MissingGrants(*h,ops).empty());
    ops.push_back({"4","dragon-retired-v1",key});h=ReadShared(ops);
    assert(h&&h->Retired.contains(key)&&h->Grants.size()==1&&!h->Plan({key,11}));
    ops.push_back({"5","dragon-death-v1",kill.Encode()});assert(!ReadShared(ops));
    assert(Mask(0.f)==0&&Mask(16777215.f)==16777215u);
    assert(!Mask(-1.f)&&!Mask(16777216.f)&&!Mask(1.5f));
    assert(ReceiptBase==0x813&&MaxGrants==768);
}
''', ['-I'+str(code/'common')])

run('exclusive native quest pickup permits',headers+'''
#include <QuestPickupPermit.h>
int main(){
    using namespace QuestPickupPermit;
    Claim c{"736b7972696d2e65736d.123","736b7972696d2e65736d.456","736b7972696d2e65736d.789",3};
    assert(c.Valid()&&Decode(c.Encode()));
    assert(!Decode(c.Encode()+" ")&&!Decode("bad 3 ref base"));
    Leases leases;auto now=Leases::Clock::now();
    assert(leases.Acquire(c,101,now));
    assert(leases.Acquire(c,101,now));
    assert(!leases.Acquire(c,202,now));
    auto d=c;d.Reference="736b7972696d2e65736d.457";
    assert(!leases.Acquire(d,202,now));
    d=c;d.Alias=4;assert(!leases.Acquire(d,202,now));
    assert(leases.Acquire(c,202,now+std::chrono::seconds(31)));
    TurnInLeases turnIns;
    assert(turnIns.Acquire(c,101,1,7,now));
    assert(turnIns.Acquire(c,101,1,7,now));
    assert(!turnIns.Acquire(c,101,1,8,now));
    assert(!turnIns.Acquire(c,101,2,7,now));
    assert(!turnIns.Acquire(c,202,2,7,now));
    assert(!turnIns.Acquire(d,202,2,7,now));
    assert(turnIns.Acquire(c,202,2,9,now+std::chrono::seconds(121)));
    assert(turnIns.Allows(c,202,now+std::chrono::seconds(122)));
    assert(!turnIns.Allows(c,101,now+std::chrono::seconds(122)));
    assert(!turnIns.Allows(c,202,now+std::chrono::seconds(242)));
    assert(PositiveSingleItem(0,1.f)&&PositiveSingleItem(3<<5,1.f));
    assert(PositiveSingleItem(1<<5,0.f)&&PositiveSingleItem(2<<5,0.f));
    assert(!PositiveSingleItem(1,1.f)&&!PositiveSingleItem(4,1.f)&&!PositiveSingleItem(0,2.f));
    assert(CompareSingleItem(0,0.f,0)==true&&CompareSingleItem(0,0.f,1)==false);
    assert(CompareSingleItem(1<<5,0.f,0)==false&&CompareSingleItem(1<<5,0.f,1)==true);
    assert(CompareSingleItem(2<<5,0.f,1)==true&&CompareSingleItem(3<<5,1.f,1)==true);
    assert(CompareSingleItem(4<<5,1.f,0)==true&&CompareSingleItem(5<<5,0.f,1)==false);
    assert(!CompareSingleItem(6<<5,1.f,1)&&!CompareSingleItem(4,1.f,1)&&!CompareSingleItem(0,1.f,2));
    assert(SharedSingleItemCount(0,false,false,false)==1);
    assert(SharedSingleItemCount(0,false,true,false)==-1);
    assert(SharedSingleItemCount(1,false,true,true)==1);
    assert(SharedSingleItemCount(1,true,true,true)==0);
    assert(SharedSingleItemCount(1,true,false,false)==-1);
    assert(SharedSingleItemCount(1,true,true,false)==-1);
    assert(SharedSingleItemCount(0,true,false,false)==0);
    assert(SharedSingleItemCount(2,true,true,true)==-1);
    std::vector<CampaignJournal::Operation> ops={{"1","quest-pickup-v1",
        c.Quest+" 1 3 "+c.Reference+" "+c.Item+" 1 101"}};
    assert(AlreadyTaken(ops,c));assert(AlreadyTaken(ops,d));
    Receipt pickup{c,1,true},remove{c,-1,false},shared{c,-1,false,true};
    assert(AvailableOriginal(ops,c)==Payload(c,101));
    assert(DecodeReceipt(shared.Encode())->Shared);
    assert(!DecodeReceipt("turnin "+pickup.Encode()));
    assert(!Acknowledged(ops,shared,202));
    assert(DecodeReceipt(pickup.Encode())&&DecodeReceipt(remove.Encode()));
    assert(!DecodeReceipt(remove.Encode()+" ")&&!DecodeReceipt(c.Encode()+" -1 1"));
    assert(Acknowledged(ops,pickup,101)&&!Acknowledged(ops,pickup,202));
    assert(!Acknowledged(ops,remove,101));
    ops.push_back({"2","quest-copy-v1",Payload(c,202)});
    ops.push_back({"3","quest-consumed-v2",Payload(c,202)});
    assert(Acknowledged(ops,pickup,202)&&Acknowledged(ops,remove,101)&&Acknowledged(ops,remove,202));
    assert(!Acknowledged(ops,remove,303));
    assert(Acknowledged(ops,shared,303)&&!AvailableOriginal(ops,c));
    d.Reference="736b7972696d2e65736d.457";assert(!AlreadyTaken(ops,d));
}
''', ['-I'+str(code/'common')])

removal=extract('client/Games/Skyrim/TESObjectREFR.cpp','static bool IdentifyQuestRemoval','int64_t TESObjectREFR::GetItemCountInInventory')
run('native quest removal without an explicit extra-data stack',headers+'''
using GameId=unsigned;
struct TESForm{};
enum class ExtraDataType{Count};
struct ExtraCount{int count=1;};
struct ExtraDataList{unsigned quest=10,ref=20,alias=3;ExtraCount count;bool hasCount=true;bool questItem=true;bool stopped=false;void* GetByType(ExtraDataType){return hasCount?&count:nullptr;}};
template<class T>T* Cast(void* p){return static_cast<T*>(p);}
struct Inventory{struct Entry{int Count=-1;bool IsQuestItem=false;};};
struct InventoryChangeEvent{unsigned FormId;Inventory::Entry Item;GameId QuestId{},QuestRef{};unsigned QuestAlias{};};
struct ScopedExtraDataOverride{};
void CaptureQuestItem(ExtraDataList* extra,void*,TESForm*,InventoryChangeEvent& e,bool allowStopped=false){if(extra->stopped&&!allowStopped)return;e.QuestId=extra->quest;e.QuestRef=extra->ref;e.QuestAlias=extra->alias;}
struct Entry{TESForm* form;std::vector<ExtraDataList*>* dataList;};
struct Changes{std::vector<Entry*>* entries;};
struct TESObjectREFR{
 int64_t total=1;std::vector<Entry*> entries;Changes changes{&entries};
 const Changes* GetContainerChanges(){return &changes;}
 int64_t GetItemCountInInventory(TESForm*){return total;}
 void GetItemFromExtraData(Inventory::Entry& entry,ExtraDataList* stack){entry.Count=stack->count.count;entry.IsQuestItem=stack->questItem;}
};
'''+removal+'''
int main(){
 TESForm base,other;ExtraDataList exact,ordinary,duplicate;
 ordinary.quest=0;ordinary.questItem=false;
 std::vector<ExtraDataList*> stacks{&exact},otherStacks{&ordinary};
 Entry a{&base,&stacks},b{&other,&otherStacks};TESObjectREFR player;player.entries={&a,&b};
 auto probe=[&](ExtraDataList* stack=nullptr){InventoryChangeEvent result{14,{}};return IdentifyQuestRemoval(player,&base,stack,result);};
 InventoryChangeEvent result{14,{}};
 assert(IdentifyQuestRemoval(player,&base,nullptr,result)&&result.Item.Count==-1&&result.Item.IsQuestItem&&result.QuestRef==20);
 player.total=2;assert(!probe());assert(probe(&exact));
 player.total=0;assert(!probe());player.total=1;
 stacks.push_back(&duplicate);assert(!probe());stacks.pop_back();
 exact.count.count=2;assert(!probe()&&!probe(&exact));exact.count.count=1;
 exact.hasCount=false;assert(probe());
 exact.stopped=true;assert(probe());exact.stopped=false;
 exact.questItem=false;assert(!probe());exact.questItem=true;
 stacks={&ordinary};assert(!probe());stacks.clear();assert(!probe());
 player.changes.entries=nullptr;assert(!probe());
}
''')

run('one unique quest reward and immutable recipient',headers+'''
#include <UniqueQuestReward.h>
int main(){
    using namespace UniqueQuestReward;
    const auto& azura=Catalog[0];const auto& black=Catalog[1];
    assert(FindSource(azura.Quest,"QF_DA01_00028AD6","Fragment_15")==&azura);
    assert(FindSource(black.Quest,"qf_da01_00028ad6","FRAGMENT_17")==&black);
    assert(!FindSource(azura.Quest,"QF_DA01_00028AD6","Fragment_13"));
    assert(!FindSource(azura.Quest+1,"QF_DA01_00028AD6","Fragment_15"));
    assert(!FindSource(azura.Quest,"WrongScript","Fragment_15"));
    const auto key=TradeProperties::FormKey{"skyrim.esm",azura.Quest}.Encode();
    std::vector<CampaignJournal::Operation> ops={{"1","quest-progress-v1",key+" 1 1 0 7 101"},
        {"2","quest-progress-v1",key+" 1 0 100 7 202"}};
    assert(KnownQuest(ops,azura.Quest)&&StageSource(ops,azura)==202&&!StageSource(ops,black));
    assert(!Read(ops,azura.Quest)->Item);
    auto offer=Decode("reward-offer "+std::to_string(azura.Item));assert(offer&&!offer->Choice);
    auto select=Decode("reward-choice "+std::to_string(azura.Item)+" 9");assert(select&&select->Choice&&select->Peer==9);
    assert(!Decode("reward-choice 15 9")&&!Decode("reward-choice "+std::to_string(azura.Item)+" 0"));
    ops.push_back({"3","quest-unique-offer-v1",std::to_string(azura.Item)+" 202"});
    auto state=Read(ops,azura.Quest);assert(state&&state->Source==202&&!state->Recipient);
    ops.push_back({"4","quest-unique-choice-v1",std::to_string(azura.Item)+" 202 101"});
    state=Read(ops,azura.Quest);assert(state&&state->Recipient==101);
    ops.push_back({"5","quest-unique-choice-v1",std::to_string(azura.Item)+" 202 202"});
    assert(!Read(ops,azura.Quest));
    ops.pop_back();ops.push_back({"5","quest-unique-offer-v1",std::to_string(black.Item)+" 101"});
    assert(!Read(ops,azura.Quest));
}
''', ['-I'+str(code/'common')])

# Exercise the exact native argument parser and the handle check independently
# of the Windows VM ABI. The native call itself is never replaced.
nativeRemoval=extract('client/Games/Misc/BSScript.cpp','struct NativeSharedRemoval','BSScript::CallResult TP_MAKE_THISCALL(HookDragonNativeCall')
handleCheck=extract('client/Services/Generic/QuestJournal.cpp','static bool MatchesNativeRemovalHandle','void ObserveSharedQuestRemoval')
run('authorized Papyrus removal evidence and exact object identity',headers+'''
struct TESForm{unsigned formType=30;};
namespace BSScript {
struct Object{uint64_t handle;uint64_t GetHandle(){return handle;}};
struct Variable {
 enum Type:uint64_t{kEmpty,kObject,kString,kInteger,kFloat,kBoolean,kMax=16};
 Type type=kEmpty;
 union {int i;bool b;Object* pObj;} data{};
 Object* GetObject() const {return type==kObject||(type>kMax&&!(type&1))?data.pObj:nullptr;}
};
struct IFunction{unsigned params=4;unsigned GetParamCount(){return params;}};
struct StackFrame {
 void* Parent;
 StackFrame* Previous;
 IFunction* Function;
 Variable Self;
 unsigned Size=4;
 Variable args[4];
 unsigned GetPageForFrame(){return 0;}
 Variable* GetStackFrameVariable(unsigned n,unsigned){return &args[n];}
};
struct IObjectHandlePolicy{
 TESForm* expected{};bool available=true,typed=true;
 bool HandleIsType(unsigned,uint64_t){return typed;}
 bool IsHandleObjectAvailable(uint64_t){return available;}
 void* GetObjectForHandle(unsigned,uint64_t h){return h==10?expected:nullptr;}
};
}
using DragonFrameView=BSScript::StackFrame;
struct DragonStackView{DragonFrameView* Top;};
struct PlayerCharacter{static PlayerCharacter* Get(){static PlayerCharacter p;return &p;}};
PlayerCharacter* DragonFrameActor(const BSScript::Variable* v){return v->GetObject()?PlayerCharacter::Get():nullptr;}
'''+nativeRemoval+handleCheck+'''
int main(){
 using namespace BSScript;
 Object player{1},item{10},caller{20},destination{30};
 IFunction function;int parent=1;
 StackFrame source{},frame{};
 source.Parent=&parent;source.Function=&function;source.Self.type=Variable::kObject;source.Self.data.pObj=&caller;
 frame.Parent=&parent;frame.Previous=&source;frame.Function=&function;frame.Self.type=Variable::kObject;frame.Self.data.pObj=&player;
 frame.args[0].type=Variable::kObject;frame.args[0].data.pObj=&item;
 frame.args[1].type=Variable::kInteger;frame.args[1].data.i=1;
 frame.args[2].type=Variable::kBoolean;frame.args[2].data.b=false;
 DragonStackView stack{&frame},*p=&stack;
 auto read=[&]{return ReadNativeSharedRemoval(&p);};
 assert(read().Item==10&&read().Caller==20);
 frame.args[3].type=Variable::kObject;frame.args[3].data.pObj=&destination;assert(!read().Item);
 frame.args[3].data.pObj=nullptr;assert(read().Item==10);
 frame.args[3].type=static_cast<Variable::Type>(32);assert(read().Item==10);
 frame.args[3].type=static_cast<Variable::Type>(33);assert(!read().Item);
 frame.args[3].type=Variable::kInteger;assert(!read().Item);
 frame.args[3].type=Variable::kEmpty;
 frame.args[1].data.i=2;assert(!read().Item);frame.args[1].data.i=1;
 frame.Size=3;assert(!read().Item);frame.Size=4;
 frame.Self.data.pObj=nullptr;assert(!read().Item);frame.Self.data.pObj=&player;
 source.Self.data.pObj=nullptr;assert(!read().Caller);source.Self.data.pObj=&caller;
 function.params=3;assert(!read().Item);function.params=4;
 frame.Previous=nullptr;assert(!read().Item);frame.Previous=&source;
 TESForm exact,other;IObjectHandlePolicy policy;policy.expected=&exact;
 assert(MatchesNativeRemovalHandle(&policy,10,&exact));
 assert(!MatchesNativeRemovalHandle(&policy,10,&other)&&!MatchesNativeRemovalHandle(&policy,11,&exact));
 policy.available=false;assert(!MatchesNativeRemovalHandle(&policy,10,&exact));
 policy.available=true;policy.typed=false;assert(!MatchesNativeRemovalHandle(&policy,10,&exact));
}
''')
