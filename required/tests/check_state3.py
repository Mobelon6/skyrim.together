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
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
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
    // Repeated removals of the last available item cannot underflow the
    // snapshot or consume another item. No claim about native grants/CTDs.
    change(sword,1);change(fire,2);change(sword,-1);change(sword,-1);
    assert(inv.Entries.size()==1&&inv.Entries.front().CanBeMerged(fire)&&inv.Entries.front().Count==2);
    change(fire,-2);assert(inv.Entries.empty());
    auto other=frost;other.EnchantData.Effects.front().Duration=25;assert(!other.CanBeMerged(frost));
    other=frost;other.EnchantData.IsWeapon=true;assert(!other.CanBeMerged(frost));
}
''', ['-I'+str(code/'encoding')])

qs='client/Services/Generic/QuestService.cpp'
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
# Exercise the real adapter to Papyrus, not a replacement ScriptSetStage.
bodies += extract('client/Games/Skyrim/Forms/TESQuest.cpp',
    'bool TESQuest::ScriptSetStage(', 'void TESQuest::SetStopped(')
bodies += extract(qs,
    'BSTEventResult QuestService::OnEvent(const TESQuestStageItemDoneEvent*',
    'BSTEventResult QuestService::OnEvent(const TESEnterBleedoutEvent*')
# The simulator supplies the standard map in place of TiltedCore's map adapter.
bodies=bodies.replace('it.value()', 'it->second').replace('deferred.value()', 'deferred->second').replace('retry.value()', 'retry->second')
# Q-11: join the actual server transitions, disk journal, client history
# parser and native stage adapter in the existing scene simulator.
branch_server=(extract('server/Services/QuestService.cpp','bool ValidQuestTargets(', 'std::string QuestKey(')
    +extract('server/Services/QuestService.cpp','bool RestoreQuestRun(', 'bool CanRecordQuestPickup('))
branch_history=extract('client/Services/Generic/QuestJournal.cpp','struct QuestReplayState','struct QuestPickup')
run('actual client quest handlers with simulated Skyrim scenes',headers+'''
#include <CampaignJournal.h>
#include <QuestRunMember.h>
#include <TradeProperties.h>
#include <filesystem>
#include <sstream>
#define __FUNCTION__ "test"
struct GameId{uint32_t BaseId;uint32_t ModId=0;GameId(uint32_t x=0):BaseId(x){}operator uint32_t()const{return BaseId;} uint32_t LogFormat()const{return BaseId;}};
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
std::function<void(uint16_t)> nativeFragment;
bool ScriptSetStage(uint16_t);
void NativeSetCurrentStageID(int s){
    if(reject.contains(s)||!reached.insert(s).second)return;
    stopped=false;actions.push_back(s);currentStage=s;
    if(nativeFragment)nativeFragment(s);
}
unsigned GetStageItemCount(uint16_t stage){return stage==100||(nativeFragment&&stage==20)?1:0;}
bool IsStopped(){return stopped;}void SetActive(bool){}
void SetStopped(){actions.push_back(-1);stopped=true;reached.clear();}
} quest;
#define PAPYRUS_FUNCTION(result, type, name, arg) auto s_p##name = [](type* self, arg s) -> result {self->NativeSetCurrentStageID(s);}
enum class BSTEventResult{kOk};
template<class T> struct EventDispatcher{};
struct TESQuestStageItemDoneEvent{uint32_t formId;uint16_t stageId;};
void QuestRewardStageDone(uint32_t,uint16_t) noexcept {}
TESForm* TESForm::GetById(uint32_t id){return id==2?&quest:nullptr;}
struct ModSystem{uint32_t GetGameId(uint32_t id){return id;}};
struct Party{bool leader=true;bool IsLeader(){return leader;}};
struct World{static World& Get(){static World w;return w;}ModSystem mods;Party party;ModSystem& GetModSystem(){return mods;}Party& GetPartyService(){return party;}
 struct Runner{template<class F> void Queue(F f){f();}} runner;
 Runner& GetRunner(){return runner;}
};
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
BSTEventResult OnEvent(const TESQuestStageItemDoneEvent*,const EventDispatcher<TESQuestStageItemDoneEvent>*);
};
void FlushPhysicalQuestStages(World&){}
void FinishPhysicalQuestStage(World&,uint32_t,uint16_t){}
void ClearPhysicalQuestStages(){}
using HeldStages=QuestService::HeldStages;
static bool MatchesQuestTargets(TESQuest* q,const NotifyQuestUpdate& u){return q&&(!u.CheckInstance||(!q->stopped&&q->currentInstanceID==u.ExpectedInstance))&&(u.Targets.empty()||(!q->stopped&&q->currentInstanceID==u.LocalInstance&&q->targets==u.Targets));}
static void ReleaseStagesForWaitingPhases(TESQuest*,HeldStages&) noexcept;
static void ReleaseUncheckedStages(TESQuest*,HeldStages&) noexcept;
static bool IsCheckedByPlayingScene(TESQuest* q,uint16_t){return !q->scenes.empty()&&q->scenes[0]->isPlaying;}
static bool IsCheckedByStage(const Conditions& c,TESQuest*,uint16_t s,bool){return s==c.wants;}
static bool IsPhaseWaitingOnConditions(BGSScene* s,int){return s->ready;}
struct QuestRun{
    unsigned Generation{1};
    std::set<uint16_t> Stages;
    std::vector<uint16_t> OrderedStages;
    bool Stopped=false,Started=false,CompletedRunOnce=false;
    std::string Targets;
    QuestRunMember::Members Members;
};
'''+bodies+branch_server+branch_history+'''
int main(int argc,char** argv){
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

    // Restore two missed reward-bearing stages in the accepted journal order.
    // The test VM is deliberately asynchronous. The real ScriptSetStage calls
    // its SetCurrentStageID adapter; rewards occur only in native fragments,
    // never from the STM journal. These are representative effects, not actual
    // Skyrim scripts or an assertion about a particular quest's conditions.
    service.OnPartyLeft({});
    quest.flags=0;quest.stopped=false;quest.reached.clear();quest.actions.clear();
    quest.stages.clear();quest.targets.clear();scene.isPlaying=false;
    struct Personal{std::set<int> words,spells;int skill=15,souls=0;};
    Personal personal;
    std::vector<uint16_t> fragments;
    quest.nativeFragment=[&](uint16_t stage){fragments.push_back(stage);};
    auto finish=[&](uint16_t stage){
        assert(!fragments.empty()&&fragments.front()==stage);
        fragments.erase(fragments.begin());
        if(stage==100){personal.words.insert(1);personal.spells.insert(2);++personal.souls;}
        if(stage==20)personal.skill+=5;
        TESQuestStageItemDoneEvent event{2,stage};
        assert(service.OnEvent(&event,nullptr)==BSTEventResult::kOk);
    };
    auto restore=[&]{send(100,0);send(20,0);};
    restore();restore();
    assert((quest.actions==Vector<int>{100})&&(fragments==Vector<uint16_t>{100}));
    assert(personal.words.empty()&&personal.spells.empty()&&personal.skill==15&&personal.souls==0);
    service.OnUpdate({});assert((quest.actions==Vector<int>{100}));
    finish(100);service.OnUpdate({});
    assert((quest.actions==Vector<int>{100,20})&&(fragments==Vector<uint16_t>{20}));
    assert(personal.words.contains(1)&&personal.spells.contains(2)&&personal.souls==1&&personal.skill==15);
    finish(20);service.OnUpdate({});restore();service.OnUpdate({});
    assert(fragments.empty()&&personal.skill==20&&personal.souls==1);
    // Reconnect after spending the soul: an already executed stage must not
    // be executed again just because its personal resource was consumed.
    --personal.souls;service.OnDisconnected({});restore();service.OnUpdate({});
    assert(fragments.empty()&&personal.souls==0&&personal.skill==20);
    assert((quest.actions==Vector<int>{100,20}));
    // Load an older ESS in which stage 100 and its rewards are present but
    // stage 20 and its skill award are absent. Only the missing effect runs.
    service.OnDisconnected({});quest.reached={100};quest.currentStage=100;
    quest.actions.clear();personal.skill=15;
    restore();assert((fragments==Vector<uint16_t>{20}));
    finish(20);service.OnUpdate({});restore();service.OnUpdate({});
    assert((quest.actions==Vector<int>{20})&&fragments.empty());
    assert(personal.skill==20&&personal.souls==0&&personal.words.contains(1)&&personal.spells.contains(2));
    // Load an ESS from before both stages, including its original rewards.
    service.OnDisconnected({});quest.reached.clear();quest.currentStage=0;
    quest.actions.clear();personal=Personal{};
    restore();finish(100);service.OnUpdate({});finish(20);service.OnUpdate({});
    assert((quest.actions==Vector<int>{100,20})&&fragments.empty());
    assert(personal.words.contains(1)&&personal.spells.contains(2)&&personal.skill==20&&personal.souls==1);
    quest.nativeFragment={};
    std::puts("PASS native stage reward fragments: replay, ordering, duplicate, spent soul, older ESS");

    // Same cooperative campaign: either player can select either branch.
    // The branch definitions are synthetic, but use the real GetStageDone
    // exclusion guard and native adapter. No Skyrim quest PEX is executed.
    const auto key=TradeProperties::FormKey{"skyrim.esm",2}.Encode();
    const auto dir=std::filesystem::path(argv[0]).parent_path();
    TESConditionItem excludes30,excludes40;
    excludes30.params[0]=&quest;excludes30.params[1]=reinterpret_cast<void*>(40);
    excludes40.params[0]=&quest;excludes40.params[1]=reinterpret_cast<void*>(30);
    TESCondition entry30{&excludes30},entry40{&excludes40};
    TESQuest::Stage branch30{30,{&entry30}},branch40{40,{&entry40}};
    quest.stages={&branch30,&branch40};quest.flags=TESQuest::RunOnce;
    quest.scenes.clear();quest.targets.clear();
    auto reset=[&](std::set<uint16_t> reached){
        service.OnDisconnected({});
        quest.stopped=false;quest.reached=std::move(reached);quest.actions.clear();
        ++quest.currentInstanceID;quest.currentStage=0;
    };
    auto publish=[&](CampaignJournal::Journal& j,uint64_t author,uint16_t stage,uint8_t status,bool completed=false){
        QuestRun accepted;assert(RestoreQuestRun(j,key,accepted));
        NotifyQuestUpdate update{2,stage,status,6};
        if(!AdvanceQuestRun(accepted,update,completed)) return false;
        return StoreQuestRun(j,key,author,accepted,update,std::nullopt);
    };
    auto sync=[&](CampaignJournal::Journal& h,CampaignJournal::Journal& g){
        while(g.Current().Revision<h.Current().Revision){
            const auto prior=g.Current();const auto next=prior.Revision+1;
            assert(g.ImportExpected(prior,*h.At(next),*h.Export(next))==CampaignJournal::Result::Committed);
        }
        assert(g.Current()==h.Current()&&g.Operations()==h.Operations());
    };
    auto replay=[&](const CampaignJournal::Journal& replica){
        const auto history=ActiveQuestHistory(replica.Operations());
        assert(history&&history->contains(key));
        const auto& run=history->at(key);
        for(const auto& [stage,type]:run.Stages){
            NotifyQuestUpdate notice{2,stage,NotifyQuestUpdate::StageUpdate,type};
            notice.LocalInstance=quest.currentInstanceID;
            service.OnQuestUpdate(notice);
            service.OnUpdate({});
        }
    };
    // Two author roles x two valid branches, same ordered journal format.
    unsigned scenario=0;
    for(uint64_t author:{101u,202u}) for(uint16_t chosen:{30u,40u}){
        const uint16_t opposite=chosen==30?40:30;
        const uint64_t other=author==101?202:101;
        const auto hostPath=dir/("q11-host-"+std::to_string(++scenario));
        const auto peerPath=dir/("q11-peer-"+std::to_string(scenario));
        CampaignJournal::Journal h,g;
        assert(h.Open(hostPath,71,"profile",true)&&g.Open(peerPath,71,"profile",true));
        assert(publish(h,other,10,NotifyQuestUpdate::Started));
        sync(h,g); // peer goes offline before the choice
        const auto old=g.Current();
        reset({10});
        World::Get().party.leader=author==101;
        quest.NativeSetCurrentStageID(chosen); // local, already happened in Skyrim
        assert(publish(h,author,chosen,NotifyQuestUpdate::StageUpdate));
        assert(!publish(h,other,chosen,NotifyQuestUpdate::StageUpdate));
        assert(publish(h,other,70,NotifyQuestUpdate::StageUpdate)); // either can continue
        assert(publish(h,author,70,NotifyQuestUpdate::Stopped,true));
        assert(!publish(h,other,opposite,NotifyQuestUpdate::StageUpdate)); // completed run cannot accept a new stage
        CampaignJournal::Journal reopened,peer;
        assert(reopened.Open(hostPath,71,"profile",false));
        assert(peer.Open(peerPath,71,"profile",false)&&peer.Current()==old);
        QuestRun restored;assert(RestoreQuestRun(reopened,key,restored));
        assert(restored.CompletedRunOnce&&(restored.OrderedStages==Vector<uint16_t>{chosen,70}));
        sync(reopened,peer);
        const auto outcome=ActiveQuestHistory(peer.Operations());
        assert(outcome&&outcome->at(key).CanRestoreStart(other));
        reset({10});World::Get().party.leader=other==101;
        unsigned choiceEffects=0,continuationEffects=0;
        quest.nativeFragment=[&](uint16_t stage){
            if(stage==chosen)++choiceEffects;
            else if(stage==70)++continuationEffects;
            else assert(false);
        };
        replay(peer);replay(peer);
        assert((quest.actions==Vector<int>{chosen,70})&&choiceEffects==1&&continuationEffects==1);
        assert(StageExcludedByLocalBranch(&quest,opposite));
        // Old cooperative save with choice already applied but next stage missing.
        reset({10,chosen});choiceEffects=1;continuationEffects=0;
        replay(peer);replay(peer);
        assert((quest.actions==Vector<int>{70})&&choiceEffects==1&&continuationEffects==1);
        // Old cooperative save before choice; recover both, once.
        reset({10});choiceEffects=continuationEffects=0;
        replay(peer);assert((quest.actions==Vector<int>{chosen,70})&&choiceEffects==1&&continuationEffects==1);
        // A contradictory received stage cannot run its fragments.
        send(opposite,NotifyQuestUpdate::StageUpdate);
        assert((quest.actions==Vector<int>{chosen,70})&&service.m_branchConflicts.count(2));
        quest.nativeFragment={};
    }
    std::puts("PASS Q-11 either author/branch, authoritative disk replay, old cooperative saves, duplicate and incoming exclusion");
    // Characterize the remaining race, do not claim first-choice-wins:
    // two different native stages can arrive before either client replays
    // the other. The server has no quest condition data and accepts both.
    CampaignJournal::Journal race;
    assert(race.Open(dir/"q11-race",71,"profile",true));
    assert(publish(race,101,10,NotifyQuestUpdate::Started));
    assert(publish(race,101,30,NotifyQuestUpdate::StageUpdate));
    assert(publish(race,202,40,NotifyQuestUpdate::StageUpdate));
    QuestRun racing;assert(RestoreQuestRun(race,key,racing));
    assert((racing.OrderedStages==Vector<uint16_t>{30,40}));
    reset({10,30});send(40,NotifyQuestUpdate::StageUpdate);
    assert(quest.actions.empty()&&service.m_branchConflicts.count(2));
    reset({10,40});send(30,NotifyQuestUpdate::StageUpdate);
    assert(quest.actions.empty()&&service.m_branchConflicts.count(2));
    std::puts("PASS Q-11 known race characterization: no second native branch replay, server does not arbitrate different stages");
}
''', ['-I'+str(code/'common'),str(code/'common/CampaignJournal.cpp'),str(code/'common/QuestJournalDisk.cpp')])

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
    history=ActiveQuestHistory(ops);assert(history&&history->at("q~legacy").Targets=="2:r:616263.1;");
    ops[1].Payload="q 1 0 20 6 202 2:r:616263.2;";assert(!ActiveQuestHistory(ops));
    ops={{"1","quest-progress-v1","q 1 1 10 6 101 2:r:616263.1; @17"},
         {"2","quest-binding-v1","q 1 1 10 6 202 2:r:616263.1; @83"},
         {"3","quest-progress-v1","q 1 0 20 6 202 2:r:616263.1; @83"},
         {"4","quest-progress-v1","q 1 2 20 6 101 2:r:616263.1; @17"},
         {"5","quest-progress-v1","q 2 1 10 6 101 2:r:616263.1; @18"},
         {"6","quest-binding-v1","q 2 1 10 6 202 2:r:616263.1; @84"},
         {"7","quest-progress-v1","q 2 0 20 6 202 2:r:616263.1; @84"}};
    history=ActiveQuestHistory(ops);assert(history&&history->at("q~legacy").Generation==2);
    const auto& latest=history->at("q~legacy");
    assert(QuestRunMember::Bound(latest.Members,101,2,18));
    assert(QuestRunMember::Bound(latest.Members,202,2,84));
    assert(latest.Prior.size()==1&&latest.Prior[0].Stopped&&latest.Prior[0].Stages.size()==1);
    assert(QuestRunMember::Bound(latest.Members,202,1,83));
    assert(!latest.CanRestoreStart(101)&&!latest.Prior[0].CanRestoreStart(101));
    ops[6].Payload="q 2 0 20 6 202 2:r:616263.1; @83";assert(!ActiveQuestHistory(ops));
    ops={{"1","quest-progress-v1","q~ 1 1 10 6 101 2:r:616263.1; @17"},
         {"2","quest-progress-v1","q~ 1 1 10 6 202 2:r:616263.2; @83"},
         {"3","quest-progress-v1","q~ 1 0 20 6 101 2:r:616263.1; @17"},
         {"4","quest-progress-v1","q~ 1 0 20 6 202 2:r:616263.2; @83"},
         {"5","quest-progress-v1","q~ 1 2 20 6 101 2:r:616263.1; @17"},
         {"6","quest-progress-v1","q~ 1 0 30 6 202 2:r:616263.2; @83"}};
    history=ActiveQuestHistory(ops);assert(history&&history->size()==2);
    const auto& left=history->at("q~2:r:616263.1;");
    const auto& right=history->at("q~2:r:616263.2;");
    assert(left.Stopped&&!right.Stopped&&left.Stages.size()==1&&right.Stages.size()==2);
    assert(QuestRunMember::Bound(left.Members,101,1,17)&&!QuestRunMember::Instance(left.Members,202,1));
    assert(QuestRunMember::Bound(right.Members,202,1,83)&&!QuestRunMember::Instance(right.Members,101,1));
    assert(QuestRunMember::BaseKey("q~2:r:616263.1;")=="q");
    ops.push_back({"7","quest-progress-v1","q~ 2 1 10 6 202 2:r:616263.1; @84"});
    history=ActiveQuestHistory(ops);assert(history&&history->at("q~2:r:616263.1;").Generation==2&&history->at("q~2:r:616263.2;").Generation==1);
}
''', ['-I'+str(code/'common')])

catchup=extract('client/Services/Generic/QuestJournal.cpp',
    'template<class CanDispatch>\nvoid ReconcileRepeatStarts','\n}\n}\n\nuint32_t ExpectedRepeatRun')+'\n}'
run('late completed run starts natively and missing stages remain retryable',headers+'''
#include <QuestRunMember.h>
#include <QuestPhysicalStage.h>
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
bool physicalPending=false;
bool QuestPhysicalStagePending(uint32_t,uint32_t,uint16_t){return physicalPending;}
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
    journal.rows.push_back({"physical","quest-physical-stage-v1",QuestPhysicalStage::Payload(key,1,40,101)});
    assert(plan().empty()); // cannot replay the physical stage or its successors
    quest.reached.insert(40);physicalPending=true;assert(plan().empty());
    physicalPending=false;stages=plan();assert(stages.size()==1&&stages[0].Stage==20);
    journal.rows.push_back({"arrival","quest-physical-stage-v1",QuestPhysicalStage::Payload(key,1,40,202)});
    quest.reached.erase(40);assert(plan().empty()); // old ESS overrides historical receipt
    journal.rows.pop_back();journal.rows.pop_back();
    quest.reached.insert(40);stages=plan();assert(stages.size()==1&&stages[0].Stage==20);
    busy=true;assert(plan().empty());busy=false;
    quest.reached.insert(20);assert(plan().empty());
    // An older ESS can contain a later stage but not an earlier reward
    // fragment. Select the absent recorded stage rather than the max stage.
    quest.reached.erase(40);stages=plan();
    assert(stages.size()==1&&stages[0].Stage==40&&stages[0].Status==NotifyQuestUpdate::StageUpdate);
    quest.reached.insert(40);assert(plan().empty());
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
    assert(Admit(m,303,2,1,true,true,false,false,true));
    assert(!Admit(m,101,2,17,true,true,false,false,true));
    assert(ScopeKey("q","2:r:616263.1;")!=ScopeKey("q","2:r:616263.2;"));
    assert(RecordKey("q","")=="q"&&RecordKey("q","2:r:616263.1;")=="q~");
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
#include <QuestPickupInventoryScope.h>
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
void QuestRewardInventoryTouched(Actor*,uint32_t) noexcept {}
void QueueActorInventoryChange(InventoryChangeEvent e){sent.push_back(e);}
void* RealPickUpObject(Actor* actor,TESObjectREFR* object,int32_t,bool,float){
 if(actor==PlayerCharacter::Get()&&object->extra.quest)
    assert(QuestPickupInventory::IsNestedAdd(actor->formID,object->baseForm->formID));
 ++actor->calls;actor->count+=actor->actual;return actor;
}
namespace TiltedPhoques{template<class F,class... A>auto ThisCall(F f,A... a){return f(a...);}}
namespace spdlog{template<class... A>void debug(A...){}}
'''+pickup+'''
int main(){
 Base base;TESObjectREFR object{&base};auto* player=PlayerCharacter::Get();
 player->actual=0;assert(HookPickUpObject(player,&object,1,false,0)==player);
 assert(sent.empty()&&player->calls==1&&!QuestPickupInventory::IsNestedAdd(player->formID,base.formID));
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
''', ['-I'+str(code/'common')])

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
    assert(MinimumRequiredCount(0,3.f)==3);
    assert(MinimumRequiredCount(2u<<5,2.f)==3);
    assert(MinimumRequiredCount(3u<<5,2.1f)==3);
    assert(MinimumRequiredCount(3u<<5,9999.f)==9999);
    assert(!MinimumRequiredCount(0,2.5f));
    assert(!MinimumRequiredCount(1u<<5,0.f));
    assert(!MinimumRequiredCount(4u<<5,4.f));
    assert(!MinimumRequiredCount(1,3.f)); // OR
    assert(!MinimumRequiredCount(4,3.f)); // non-literal flags
    assert(!MinimumRequiredCount(2u<<5,9999.f));
    assert(CompareItemCount(3u<<5,3.f,2)==false);
    assert(CompareItemCount(3u<<5,3.f,3)==true);
    assert(!CompareSingleItem(3u<<5,3.f,3)); // previous exact path unchanged

    std::vector<CampaignJournal::Operation> ops={{"1","quest-pickup-v1",
        c.Quest+" 1 3 "+c.Reference+" "+c.Item+" 1 101"}};
    assert(AlreadyTaken(ops,c));assert(AlreadyTaken(ops,d));
    Receipt pickup{c,1,true},remove{c,-1,false},shared{c,-1,false,true};
    assert(AvailableOriginal(ops,c)==Payload(c,101));
    assert(HasAvailableHolder(ops,c,101)&&!HasAvailableHolder(ops,c,202));
    assert(!HasAvailableHolder(ops,c,0));
    assert(PhysicalHolder(101,101,false,1));
    assert(!PhysicalHolder(101,101,false,0));
    assert(!PhysicalHolder(202,101,false,1));
    assert(!PhysicalHolder(202,101,false,0));
    assert(PhysicalHolder(202,101,true,1));
    assert(!PhysicalHolder(202,101,true,2));
    assert(DecodeReceipt(shared.Encode())->Shared);
    assert(!DecodeReceipt("turnin "+pickup.Encode()));
    assert(!Acknowledged(ops,shared,202));
    assert(DecodeReceipt(pickup.Encode())&&DecodeReceipt(remove.Encode()));
    assert(!DecodeReceipt(remove.Encode()+" ")&&!DecodeReceipt(c.Encode()+" -1 1"));
    assert(Acknowledged(ops,pickup,101)&&!Acknowledged(ops,pickup,202));
    assert(!Acknowledged(ops,remove,101));
    ops.push_back({"2","quest-copy-v1",Payload(c,202)});
    assert(HasAvailableHolder(ops,c,101)&&HasAvailableHolder(ops,c,202));
    assert(!HasAvailableHolder(ops,c,303));
    ops.push_back({"3","quest-consumed-v2",Payload(c,202)});
    assert(!HasAvailableHolder(ops,c,101)&&!HasAvailableHolder(ops,c,202));
    assert(Acknowledged(ops,pickup,202)&&Acknowledged(ops,remove,101)&&Acknowledged(ops,remove,202));
    assert(!Acknowledged(ops,remove,303));
    assert(Acknowledged(ops,shared,303)&&!AvailableOriginal(ops,c));
    d.Reference="736b7972696d2e65736d.457";assert(!AlreadyTaken(ops,d));
}
''', ['-I'+str(code/'common')])

removal=extract('client/Games/Skyrim/TESObjectREFR.cpp','bool IdentifyQuestRemoval','int64_t TESObjectREFR::GetItemCountInInventory')
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

# Native-first acceptance: the previous supplements remain readable but must
# not observe, issue, replay or suppress personal rewards in this build.
run('native-first personal reward policy', '''
#include <CampaignRewardPolicy.h>
static_assert(!CampaignRewardPolicy::SupplementWords);
static_assert(!CampaignRewardPolicy::SupplementDragonSouls);
int main() {}
''', ['-I'+str(code/'common')])
for file, marker, guard in [
    ('client/Services/Generic/QuestJournal.cpp', 'void ObserveLearnedWord(', 'if (!CampaignRewardPolicy::SupplementWords) return;'),
    ('client/Services/Generic/QuestJournal.cpp', 'void RestoreLearnedWords(', 'if (!CampaignRewardPolicy::SupplementWords) return;'),
    ('client/Services/Generic/QuestJournal.cpp', 'void SendDragonAbsorption(', 'if (!CampaignRewardPolicy::SupplementDragonSouls) return;'),
    ('client/Services/Generic/QuestJournal.cpp', 'void ApplySharedDragonGrants(', 'if (!CampaignRewardPolicy::SupplementDragonSouls) return;'),
    ('client/Services/Generic/QuestJournal.cpp', 'void RecordNativeDragonAbsorption(', 'if (!CampaignRewardPolicy::SupplementDragonSouls) return;'),
    ('server/Services/QuestJournal.cpp', 'void RecordCampaignDragonLife(', 'if (!CampaignRewardPolicy::SupplementDragonSouls) return;'),
    ('client/Games/Misc/BSScript.cpp', 'bool TeachCampaignWord(', 'if (!CampaignRewardPolicy::SupplementWords) return false;'),
]:
    source = (code / file).read_text()
    start = source.index('{', source.index(marker))
    assert source[start+1:].lstrip().startswith(guard), (file, marker)
server = (code / 'server/Services/QuestJournal.cpp').read_text()
assert 'if (CampaignRewardPolicy::SupplementWords&&!p.LearnedWord.empty()' in server
assert 'if (CampaignRewardPolicy::SupplementDragonSouls&&!p.AbsorbedDragon.empty()' in server
assert 'if (CampaignRewardPolicy::SupplementDragonSouls)\n            if (const auto history=DragonAbsorption::ReadShared' in server
native = (code / 'client/Games/Misc/BSScript.cpp').read_text()
assert 'if (CampaignRewardPolicy::SupplementDragonSouls&&\n        apThis==modActorValueNative.load' in native
assert 'return CampaignRewardPolicy::SupplementDragonSouls&&realDragonNativeCall' in native
print('PASS native-first personal reward entry points', flush=True)

# Native-first acceptance: execute the real ordinary STR object handlers,
# not a second implementation of locks, doors, or personal jail stashes.
objects = 'client/Services/Generic/ObjectService.cpp'
stash = extract(objects, 'Set<const TESObjectREFR*> GetPlayerStashContainers()', 'void ObjectService::OnDisconnected(')
activate = extract(objects, 'void ObjectService::OnActivateNotify(', 'void ObjectService::OnLockChange(')
lock = extract(objects, 'void ObjectService::OnLockChangeNotify(', 'void ObjectService::OnScriptAnimationEvent(')
run('campaign physical doors, locks and activators stay local; legacy STR and confiscated inventory unchanged', headers+'''
template<class T> using Set=std::set<T>;
enum class FormType{Door=0x1D,Container=0x1C,Activator=0x18,TalkingActivator=0x19,Static=0x22,StaticCollection=0x23,MovableStatic=0x24,Furniture=0x28,Book=27,Character=62};
struct TESObjectCELL{uint32_t formID=10;};
struct TESForm{uint32_t formID{};FormType formType{};static TESForm* GetById(uint32_t);};
template<class T>T* Cast(TESForm* p){return static_cast<T*>(p);}
struct Lock{bool locked=false;int lockLevel=0;bool IsLocked(){return locked;}void SetLock(bool b){locked=b;}};
struct Actor{};
struct TESObjectREFR:TESForm{
 enum OpenState{kClosed,kOpen};
 TESForm base{};TESForm* baseForm=&base;TESObjectCELL cell;Lock lock;
 bool hasLock=false;int lockChanges=0,activations=0;OpenState state=kClosed;Actor* activator{};
 TESObjectCELL* GetParentCellEx(){return &cell;}
 Lock* GetLock(){return hasLock?&lock:nullptr;}
 Lock* CreateLock(){hasLock=true;return &lock;}
 void LockChange(){++lockChanges;}
 OpenState GetOpenState(){return state;}
 void Activate(Actor* a,int,void*,int,int){activator=a;++activations;if(base.formType==FormType::Door)state=state==kClosed?kOpen:kClosed;}
} door,stashA,stashB,other,questChestA,questChestB,proxy;
TESForm* TESForm::GetById(uint32_t id){return id==1?&door:nullptr;}
struct TESFaction{struct{TESObjectREFR* playerInventoryContainer{};TESObjectREFR* stolenGoodsContainer{};}crimeData;};
struct ModManager{std::vector<TESFaction*> factions;static ModManager* Get(){static ModManager m;return &m;}};
namespace NativeTrade{bool IsProxy(const TESObjectREFR* p){return p==&proxy;}}
namespace CampaignReplication{struct Link{enum class Mode{Ready,Disabled,Faulted};};}
namespace CampaignAccess{auto mode=CampaignReplication::Link::Mode::Disabled;auto Mode(){return mode;}}
namespace HomeInstance{bool ManagedCell(uint32_t id){return id==99;}}
namespace Workbench{bool owned=false,ordinary=false;bool Owns(const TESObjectREFR*){return owned;}bool OrdinaryRoute(const TESObjectREFR*){return ordinary;}}
struct World{struct ModSystem{uint32_t GetGameId(uint32_t id){return id;}} mods;static World& Get(){static World w;return w;}ModSystem& GetModSystem(){return mods;}};
Actor remote;namespace Utils{template<class T>T* GetByServerId(uint32_t id){return id==2?&remote:nullptr;}}
namespace spdlog{template<class...T>void error(T&&...){}}
struct NotifyActivate{uint32_t Id,ActivatorId;int PreActivationOpenState;};
struct NotifyLockChange{uint32_t Id;bool IsLocked;int LockLevel;};
struct ObjectService{void OnActivateNotify(const NotifyActivate&) noexcept;void OnLockChangeNotify(const NotifyLockChange&) noexcept;};
'''+stash+activate+lock+'''
int main(){
 TESFaction faction{{&stashA,&stashB}};
 ModManager::Get()->factions={nullptr,&faction};
 questChestA.formID=0x39CF1;questChestB.formID=0x3EF03;
 auto exclusions=GetPlayerStashContainers();
 assert(exclusions.size()==2&&!ShouldSyncObject(&stashA,exclusions)&&!ShouldSyncObject(&stashB,exclusions));
 assert(!ShouldSyncObject(&questChestA,exclusions)&&!ShouldSyncObject(&questChestB,exclusions));
 assert(!ShouldSyncObject(&proxy,exclusions)&&!ShouldSyncObject(nullptr,exclusions));
 assert(ShouldSyncObject(&other,exclusions)&&ShouldSyncObject(&door,exclusions));
 ObjectService svc;door.base.formType=FormType::Door;
 svc.OnActivateNotify({1,2,TESObjectREFR::kClosed});
 assert(door.state==TESObjectREFR::kOpen&&door.activations==1&&door.activator==&remote);
 svc.OnActivateNotify({1,2,TESObjectREFR::kClosed}); // repeated opening must not close it
 assert(door.state==TESObjectREFR::kOpen&&door.activations==1);
 svc.OnActivateNotify({1,2,TESObjectREFR::kOpen});
 assert(door.state==TESObjectREFR::kClosed&&door.activations==2);
 svc.OnActivateNotify({1,3,TESObjectREFR::kClosed});assert(door.activations==2);
 svc.OnLockChangeNotify({1,true,50});
 assert(door.hasLock&&door.lock.locked&&door.lock.lockLevel==50&&door.lockChanges==1);
 svc.OnLockChangeNotify({1,false,50});assert(!door.lock.locked&&door.lockChanges==2);
 svc.OnLockChangeNotify({1,false,50});assert(door.lockChanges==2);
 door.cell.formID=99;svc.OnActivateNotify({1,2,TESObjectREFR::kClosed});svc.OnLockChangeNotify({1,true,50});
 assert(door.activations==2&&!door.lock.locked&&door.lockChanges==2); // managed objects stay on their existing route
 door.base.formType=FormType::Activator;Workbench::ordinary=true;
 svc.OnActivateNotify({1,2,TESObjectREFR::kClosed});assert(door.activations==3);
 Workbench::owned=true;svc.OnActivateNotify({1,2,TESObjectREFR::kClosed});assert(door.activations==3);
 Workbench::owned=false;Workbench::ordinary=false;
 // The actual incoming handlers must not open/close or unlock a peer's
 // geometry in campaign mode, including stale legacy packets.
 CampaignAccess::mode=CampaignReplication::Link::Mode::Ready;
 door.cell.formID=10;door.base.formType=FormType::Door;
 svc.OnActivateNotify({1,2,TESObjectREFR::kClosed});
 svc.OnLockChangeNotify({1,true,50});
 assert(door.activations==3&&!door.lock.locked&&door.lockChanges==2);
 for(auto t:{FormType::Activator,FormType::TalkingActivator,FormType::Container,
             FormType::Static,FormType::StaticCollection,FormType::MovableStatic}){
    door.base.formType=t;assert(LocalPhysicalActivation(&door));
    svc.OnActivateNotify({1,2,TESObjectREFR::kClosed});assert(door.activations==3);
 }
 for(auto t:{FormType::Book,FormType::Character,FormType::Furniture}){
    door.base.formType=t;assert(!LocalPhysicalActivation(&door));
 }
 CampaignAccess::mode=CampaignReplication::Link::Mode::Disabled;
 door.base.formType=FormType::Door;
 assert(!LocalPhysicalActivation(&door));
 svc.OnActivateNotify({1,2,TESObjectREFR::kClosed});assert(door.activations==4);
}
''')
physical_objects=(code/objects).read_text()
assert 'if (!m_transport.IsConnected() || LocalPhysicalActivation(acEvent.pObject))' in physical_objects
assert 'if (!m_transport.IsConnected() || LocalPhysicalWorld())' in physical_objects
assert 'if (!LocalPhysicalWorld() && objectData.CurrentLockData != LockData{})' in physical_objects
assert 'if (!LocalPhysicalWorld())\n            if (Lock* pLock = pObject->GetLock())' in physical_objects
assert 'objectData.CurrentInventory = pObject->GetInventory();' in physical_objects
assert 'pObject->SetInventoryRetainingQuestItems(currentInventory, objectData.CurrentInventory);' in physical_objects
assert 'pObject->SetInventory(objectData.CurrentInventory);' in physical_objects
assert physical_objects.index('acEvent.pObject->Activate(acEvent.pActivator') < physical_objects.index('LocalPhysicalActivation(acEvent.pObject)')
print('PASS campaign local physical state preserves native activation, object inventory snapshots and existing leases',flush=True)

# The original server still relays weather and optional combat XP; test the
# actual network handler branches. An in-memory transport represents recipients.
serverObjects=extract('server/Services/ObjectService.cpp','void ObjectService::OnActivate(', 'void ObjectService::OnLockChange(')
weather=extract('server/Services/WeatherService.cpp','void WeatherService::OnWeatherChange(', 'void WeatherService::OnRequestCurrentWeather(')
weather_source=(code/'server/Services/WeatherService.cpp').read_text()
weather += weather_source[weather_source.index('void WeatherService::OnRequestCurrentWeather('):]
xp=extract('server/Services/CharacterService.cpp','void CharacterService::OnSyncExperienceRequest(', 'void CharacterService::OnDialogueRequest(')
run('ordinary object routing, weather cache and configurable native combat XP',headers+'''
struct GameId{uint32_t ModId{},BaseId{};bool operator==(const GameId&)const=default;};
struct ActivateRequest{GameId CellId,Id;uint32_t ActivatorId;int PreActivationOpenState;};
struct NotifyActivate{GameId Id;uint32_t ActivatorId{};int PreActivationOpenState{};};
struct RequestWeatherChange{GameId Id;};struct RequestCurrentWeather{};struct NotifyWeatherChange{GameId Id;};
struct SyncExperienceRequest{float Experience;};struct NotifySyncExperience{float Experience;};
struct Cell{GameId Cell;bool estate=false;bool HasExteriorEstate()const{return estate;}};
struct Player{
 Cell location;uint32_t character=1;int party=1;std::vector<NotifyActivate> acts;std::vector<NotifyWeatherChange> weather;
 Cell GetCellComponent()const{return location;}
 const uint32_t* GetCharacter()const{return character?&character:nullptr;}
 int GetParty()const{return party;}
 void Send(const NotifyActivate& n){acts.push_back(n);}
 void Send(const NotifyWeatherChange& n){weather.push_back(n);}
};
template<class T>struct PacketEvent{Player* pPlayer;T Packet;Player* GetSender()const{return pPlayer;}};
namespace HomeScope{bool Private(uint32_t id){return id==99;}}
struct Party{GameId CachedWeather;};
struct World{
 std::vector<Player*> players;Party party;
 World& GetPartyService(){return *this;}
 Party* GetPlayerParty(Player* p){return p->party?&party:nullptr;}
 const auto& GetPlayerManager()const{return players;}
};
struct GameServer{
 static GameServer* Get(){static GameServer s;return &s;}
 int weatherRelays=0,xpRelays=0;float xp=0;Player* sender{};int group=0;
 void SendToPartyInRange(const NotifyWeatherChange&,int p,uint32_t,Player* s){++weatherRelays;group=p;sender=s;}
 void SendToParty(const NotifySyncExperience& n,int p,Player* s){++xpRelays;xp=n.Experience;group=p;sender=s;}
};
bool bEnableXpSync=false;
struct ObjectService{World& m_world;void OnActivate(const PacketEvent<ActivateRequest>&)const noexcept;};
struct WeatherService{World& m_world;void OnWeatherChange(const PacketEvent<RequestWeatherChange>&)const noexcept;void OnRequestCurrentWeather(const PacketEvent<RequestCurrentWeather>&)const noexcept;};
struct CharacterService{void OnSyncExperienceRequest(const PacketEvent<SyncExperienceRequest>&)const noexcept;};
'''+serverObjects+weather+xp+'''
int main(){
 Player a,b,c; a.location.Cell=b.location.Cell={0,10};c.location.Cell={0,11};
 World world{{&a,&b,&c}};ObjectService objects{world};
 objects.OnActivate({&a,{{0,10},{0,5},1,2}});
 assert(a.acts.empty()&&c.acts.empty()&&b.acts.size()==1);
 assert(b.acts[0].Id==(GameId{0,5})&&b.acts[0].ActivatorId==1&&b.acts[0].PreActivationOpenState==2);
 b.location.estate=true;objects.OnActivate({&a,{{0,10},{0,5},1,2}});assert(b.acts.size()==1);
 auto* game=GameServer::Get();WeatherService svc{world};svc.OnWeatherChange({&a,{{0,42}}});
 assert(game->weatherRelays==1&&world.party.CachedWeather==(GameId{0,42}));
 svc.OnRequestCurrentWeather({&b,{}});assert(b.weather.size()==1&&b.weather[0].Id==(GameId{0,42}));
 a.party=0;svc.OnWeatherChange({&a,{{0,43}}});assert(game->weatherRelays==1&&world.party.CachedWeather==(GameId{0,42}));
 a.party=1;CharacterService cs;cs.OnSyncExperienceRequest({&a,{10.f}});assert(game->xpRelays==0);
 bEnableXpSync=true;cs.OnSyncExperienceRequest({&a,{10.f}});assert(game->xpRelays==1&&game->xp==10.f&&game->sender==&a&&game->group==1);
}
''')

rest=extract('server/Services/CalendarService.cpp','void CalendarService::OnRest(', 'void CalendarService::OnUpdate(')
days=extract('common/DateTime.cpp','const uint32_t cDayLengthArray','void DateTime::Update(')
run('existing shared sleep and wait arrival without a second time increment',headers+'''
#include <cmath>
struct TimeModel{float TimeScale=20.f,Time=12.f;uint32_t Year=201,Month=0,Day=1;};
struct DateTime{TimeModel m_timeModel;static uint32_t GetNumberOfDaysByMonthIndex(int);};
struct RequestRestTime{TimeModel Arrival;};
struct Player{bool party=true,character=true;bool GetCharacter()const{return character;}};
template<class T>struct PacketEvent{Player* pPlayer;T Packet;};
struct World{World& GetPartyService(){return *this;}bool GetPlayerParty(Player* p){return p->party;}};
struct CalendarService{
 World m_world;DateTime m_dateTime;int broadcasts=0;void SendTimeResync(bool rest){assert(rest);++broadcasts;}
 void OnRest(const PacketEvent<RequestRestTime>&)noexcept;
};
'''+days+rest+'''
int main(){
 CalendarService s;Player a,b;
 s.m_dateTime.m_timeModel={20.f,23.f,201,11,31};
 TimeModel arrival{99.f,7.f,202,0,1};
 s.OnRest({&a,{arrival}});
 assert(s.m_dateTime.m_timeModel.Day==1&&s.m_dateTime.m_timeModel.Year==202&&s.m_dateTime.m_timeModel.Time==7.f);
 assert(s.m_dateTime.m_timeModel.TimeScale==20.f&&s.broadcasts==1);
 s.OnRest({&b,{arrival}});assert(s.m_dateTime.m_timeModel.Time==7.f&&s.broadcasts==2);
 s.OnRest({&b,{{1.f,8.f,202,0,3}}});assert(s.m_dateTime.m_timeModel.Time==7.f&&s.m_dateTime.m_timeModel.Day==1);
 s.OnRest({&a,{{1.f,8.f,202,1,30}}});assert(s.broadcasts==3);
 b.party=false;s.OnRest({&b,{{1.f,8.f,202,0,1}}});assert(s.broadcasts==3);
 b.party=true;s.OnRest({&b,{{1.f,9.f,202,0,1}}});assert(s.m_dateTime.m_timeModel.Time==9.f&&s.broadcasts==4);
}
''')


# Verify the approved weather policy against real client handlers, including
# party leadership changes and a delayed response after leaving the loaded area.
weather_client = (code/'client/Services/Generic/WeatherService.cpp').read_text()
weather_client = weather_client[weather_client.index('void WeatherService::OnDisconnected('):]
run('regional weather when apart; existing party weather on approach and handoff',headers+'''
namespace entt{struct registry{};using entity=int;}
struct GameId{uint32_t ModId{},BaseId{};};
struct TESForm{uint32_t formID{};static TESForm* GetById(uint32_t);};
struct TESWeather:TESForm{} regional{{10}},remoteWeather{{20}},mapWeather{{0xA6858}};
TESForm* TESForm::GetById(uint32_t id){return id==10?&regional:id==20?&remoteWeather:id==0xA6858?&mapWeather:nullptr;}
template<class T>T* Cast(TESForm* p){return static_cast<T*>(p);}
struct Sky{
 TESWeather* current=&regional;bool forced=false;int releases=0;
 static Sky* Get(){static Sky s;return &s;}
 TESWeather* GetWeather(){return current;}
 void ForceWeather(TESWeather* weather){current=weather;forced=true;}
 void ReleaseWeatherOverride(){current=&regional;forced=false;++releases;}
};
struct PlayerComponent{uint32_t Id{};};
struct PartyService{bool in=true,leader=false;uint32_t leaderId=2;bool IsInParty()const{return in;}bool IsLeader()const{return leader;}uint32_t GetLeaderPlayerId()const{return leaderId;}};
struct World:entt::registry{
 PartyService party;std::map<int,PlayerComponent> players;
 PartyService& GetPartyService(){return party;}
 struct ModSystem{uint32_t GetGameId(GameId id){return id.BaseId;}bool GetServerModId(uint32_t id,GameId& out){out={0,id};return true;}} mods;
 ModSystem& GetModSystem(){return mods;}
 template<class T>T* try_get(int id){auto it=players.find(id);return it==players.end()?nullptr:&it->second;}
 template<class T>T& get(int id){return players.at(id);}
 struct View{World& world;std::vector<int> ids;auto begin(){return ids.begin();}auto end(){return ids.end();}
  template<class T>T& get(int id){return world.get<T>(id);}
 };
 template<class T>View view(){View v{*this,{}};for(auto& [id,p]:players)v.ids.push_back(id);return v;}
};
struct RequestCurrentWeather{};struct RequestWeatherChange{GameId Id;};struct NotifyWeatherChange{GameId Id;};
struct TransportService{bool online=true;int requests=0;std::vector<uint32_t> published;
 bool IsOnline(){return online;}void Send(RequestCurrentWeather){++requests;}void Send(RequestWeatherChange p){published.push_back(p.Id.BaseId);}
};
struct DisconnectedEvent{};struct PartyJoinedEvent{bool IsLeader;};struct PartyLeftEvent{};
namespace spdlog{template<class...T>void error(T&&...){}}
struct WeatherService{
 World& m_world;TransportService& m_transport;uint32_t m_cachedWeatherId{};bool m_followingWeather{};
 void RefreshPartyWeather()noexcept;void OnDisconnected(const DisconnectedEvent&)noexcept;
 void OnPartyJoinedEvent(const PartyJoinedEvent&)noexcept;void OnPartyLeftEvent(const PartyLeftEvent&)noexcept;
 void OnWaitingFor3DRemoved(entt::registry&,entt::entity)noexcept;void OnPlayerComponentRemoved(entt::registry&,entt::entity)noexcept;
 void OnWeatherChange(const NotifyWeatherChange&)noexcept;void RunWeatherUpdates(double)noexcept;
 void ToggleGameWeatherSystem(bool)noexcept;void SetCachedWeather()noexcept;
};
'''+weather_client+'''
int main(){
 World world;TransportService transport;WeatherService service{world,transport};auto* sky=Sky::Get();
 service.OnPartyJoinedEvent({false});
 assert(!sky->forced&&!service.m_followingWeather&&transport.requests==0);
 service.OnWeatherChange({{0,20}});assert(!sky->forced);
 world.players[2]={2};
 service.OnWaitingFor3DRemoved(world,2);assert(service.m_followingWeather&&transport.requests==1);
 service.OnWeatherChange({{0,20}});assert(sky->forced&&sky->current==&remoteWeather);
 sky->current=&regional;service.RunWeatherUpdates(0);assert(sky->current==&remoteWeather);
 service.OnPlayerComponentRemoved(world,2);world.players.erase(2);
 assert(!sky->forced&&!service.m_followingWeather&&sky->current==&regional);
 service.OnWeatherChange({{0,20}});service.RunWeatherUpdates(0);assert(!sky->forced);
 world.players[2]={2};service.OnWaitingFor3DRemoved(world,2);
 service.OnWeatherChange({{0,20}});assert(sky->forced);
 world.party.leader=true;world.party.leaderId=1;service.RefreshPartyWeather();
 assert(!sky->forced&&!service.m_followingWeather&&transport.published==std::vector<uint32_t>{10});
 service.OnWeatherChange({{0,20}});assert(!sky->forced);
 world.party.leader=false;world.party.leaderId=2;service.RefreshPartyWeather();
 assert(service.m_followingWeather&&transport.requests==3);
 service.OnWeatherChange({{0,20}});assert(sky->forced);
 world.party.in=false;service.OnPartyLeftEvent({});service.OnWeatherChange({{0,20}});assert(!sky->forced);
 transport.online=false;service.OnDisconnected({});assert(!service.m_followingWeather);
}
''')
party_info=extract('client/Services/Generic/PartyService.cpp','void PartyService::OnPartyInfo(', 'void PartyService::OnPartyInvite(')
assert 'm_world.ctx().at<WeatherService>().RefreshPartyWeather();' in party_info
assert 'if (leaderChanged)' in party_info
assert 'm_leaderPlayerId = acPartyInfo.LeaderPlayerId;' in party_info
assert 'Console::Setting bEnableXpSync{"Gameplay:bEnableXpSync", "Syncs combat XP within the party", false};' in (code/'server/Services/CharacterService.cpp').read_text()
print('PASS individual combat XP default and party weather handoff integration',flush=True)


# Verify actual transient cell lifecycle, not native ESS persistence or reset.
leave_cell=extract('server/Services/ObjectService.cpp','void ObjectService::OnPlayerLeaveCellEvent(', '// NOTE: this whole system')
run('ordinary object lifecycle: one player remains versus empty cell',headers+'''
namespace entt{using entity=uint32_t;}
template<class T>using Vector=std::vector<T>;
struct CellIdComponent{uint32_t Cell;};
struct ObjectComponent{};
struct Player{CellIdComponent location;const CellIdComponent& GetCellComponent()const{return location;}};
struct PlayerLeaveCellEvent{uint32_t OldCell;};
struct World{
 std::vector<Player*> players;
 std::map<entt::entity,CellIdComponent> objects;
 const auto& GetPlayerManager()const{return players;}
 struct View{
  World& w;std::vector<entt::entity> ids;
  auto begin(){return ids.begin();}auto end(){return ids.end();}
  template<class T>const T& get(entt::entity id){return w.objects.at(id);}
 };
 template<class... T>View view(){View v{*this,{}};for(auto& [id,cell]:objects)v.ids.push_back(id);return v;}
 void destroy(entt::entity id){assert(objects.erase(id)==1);}
};
struct ObjectService{World& m_world;void OnPlayerLeaveCellEvent(const PlayerLeaveCellEvent&) noexcept;};
'''+leave_cell+'''
int main(){
 Player a{{10}},b{{10}};World world{{&a,&b},{{1,{10}},{2,{10}},{3,{11}}}};
 ObjectService service{world};
 a.location.Cell=11;
 service.OnPlayerLeaveCellEvent({10});assert(world.objects.size()==3);
 b.location.Cell=11;
 service.OnPlayerLeaveCellEvent({10});
 assert(world.objects.size()==1&&world.objects.contains(3));
 service.OnPlayerLeaveCellEvent({10});assert(world.objects.size()==1);
 service.OnPlayerLeaveCellEvent({11});assert(world.objects.size()==1);
}
''')


# Native additions/consumption: exercise the changed actual hooks. The
# identity resolver is independently tested above; this mock controls whether
# a native stack is exact and how much the engine actually changes.
adds=extract('client/Games/Skyrim/TESObjectREFR.cpp','void TP_MAKE_THISCALL(HookAddInventoryItem,','// A native QF RemoveItem')
removes=extract('client/Games/Skyrim/TESObjectREFR.cpp','BSPointerHandle<TESObjectREFR>*\nTP_MAKE_THISCALL(HookRemoveInventoryItem,','void TP_MAKE_THISCALL(HookRotateX,')
run('confirmed native inventory deltas and nested pickup guard',headers+'''
#include <QuestPickupInventoryScope.h>
#define TP_MAKE_THISCALL(name,cls,...) name(cls* apThis,__VA_ARGS__)
struct TESBoundObject{uint32_t formID=40;};
struct ExtraDataList{bool quest{};};
struct Inventory{struct Entry{uint32_t BaseId{};int32_t Count{};bool IsQuestItem{};};};
struct InventoryChangeEvent{uint32_t FormId;Inventory::Entry Item;uint32_t QuestId{},QuestRef{},QuestAlias{};};
struct TESObjectREFR{
 uint32_t formID=14;int64_t count{};int add{},remove{},calls{};bool exact{};
 int64_t GetItemCountInInventory(TESBoundObject*){return count;}
 void GetItemFromExtraData(Inventory::Entry& e,ExtraDataList* data){e.IsQuestItem=data->quest;}
};
struct PlayerCharacter:TESObjectREFR{static PlayerCharacter* Get(){static PlayerCharacter p;return &p;}};
template<class T>struct BSPointerHandle{struct{uint32_t iBits{};}handle;};
struct NiPoint3{};
enum class ITEM_REMOVE_REASON{kRemove,kDropping,kTransfer,kStoreInContainer};
struct ScopedInventoryOverride{static bool IsOverriden(){return false;}};
struct ScopedExtraDataOverride{};struct ScopedEquipOverride{};
namespace NativeTrade{bool BlockAdd(TESObjectREFR*,TESObjectREFR*){return false;}
bool BlockRemove(TESObjectREFR*,TESObjectREFR*,TESBoundObject*,int32_t,ExtraDataList*){return false;}}
namespace HomePlacementClient{bool InterceptDrop(TESObjectREFR*,TESBoundObject*,ExtraDataList*,int32_t,const NiPoint3*,const NiPoint3*){return false;}}
namespace spdlog{template<class...T>void debug(T&&...){}}
struct ModSystem{void GetServerModId(uint32_t a,uint32_t& b){b=a;}};
struct World{ModSystem mods;static World& Get(){static World w;return w;}ModSystem& GetModSystem(){return mods;}};
struct QuestRewardAddObservation{QuestRewardAddObservation(TESObjectREFR*,TESBoundObject*,ExtraDataList*,int32_t,TESObjectREFR*){}};
void QuestRewardInventoryTouched(TESObjectREFR*,uint32_t) noexcept {}
bool NativeQuestTransfer(TESObjectREFR*,ITEM_REMOVE_REASON,const InventoryChangeEvent&){return false;}
bool IdentifyQuestRemoval(TESObjectREFR& owner,TESBoundObject*,ExtraDataList*,InventoryChangeEvent& e){
 if(!owner.exact||owner.count!=1)return false;
 e.Item.IsQuestItem=true;e.Item.Count=-1;e.QuestId=100;e.QuestRef=200;e.QuestAlias=3;return true;
}
std::vector<InventoryChangeEvent> sent;
void QueueReferenceInventoryChange(InventoryChangeEvent e,TESObjectREFR*){sent.push_back(e);}
void RealAddInventoryItem(TESObjectREFR* o,TESBoundObject*,ExtraDataList*,int32_t,TESObjectREFR*){o->count+=o->add;++o->calls;}
BSPointerHandle<TESObjectREFR>* RealRemoveInventoryItem(TESObjectREFR* o,BSPointerHandle<TESObjectREFR>* out,TESBoundObject*,int32_t,ITEM_REMOVE_REASON,ExtraDataList*,TESObjectREFR*,const NiPoint3*,const NiPoint3*){o->count-=std::min<int64_t>(o->count,o->remove);++o->calls;return out;}
namespace TiltedPhoques{template<class F,class...A>auto ThisCall(F f,A... a){return f(a...);}}
'''+adds+removes+'''
int main(){
 auto* player=PlayerCharacter::Get();TESBoundObject item;BSPointerHandle<TESObjectREFR> out;
 auto add=[&]{HookAddInventoryItem(player,&item,nullptr,1,nullptr);};
 auto remove=[&](int count){return HookRemoveInventoryItem(player,&out,&item,count,ITEM_REMOVE_REASON::kRemove,nullptr,nullptr,nullptr,nullptr);};
 player->add=0;add();assert(sent.empty());
 player->add=1;player->exact=true;add();assert(sent.size()==1&&sent.back().Item.Count==1&&sent.back().QuestRef==200);
 player->remove=1;assert(remove(1)==&out);assert(sent.size()==2&&sent.back().Item.Count==-1&&sent.back().QuestRef==200);
 player->exact=false;add();assert(sent.size()==3&&!sent.back().QuestRef);
 player->remove=1;remove(3);assert(sent.size()==4&&sent.back().Item.Count==-1);
 remove(3);assert(sent.size()==4);
 {QuestPickupInventory::Scope scope(player->formID,item.formID);add();assert(sent.size()==4);
   {QuestPickupInventory::Scope inner(17,99);assert(!QuestPickupInventory::IsNestedAdd(player->formID,item.formID));}
   assert(QuestPickupInventory::IsNestedAdd(player->formID,item.formID));}
 assert(!QuestPickupInventory::IsNestedAdd(player->formID,item.formID));
 player->remove=0;remove(2);assert(sent.size()==4&&player->count==1);
}
''',['-I'+str(code/'common')])
server_inventory=(code/'server/Services/InventoryService.cpp').read_text()
assert 'sharedOriginal=CampaignAccess::RecordQuestItemChange' in server_inventory
assert 'if (sharedOriginal&&message.Item.Count>0)' in server_inventory
assert 'snapshot.CurrentInventory=inventoryComponent.Content;' in server_inventory
quest_recovery=(code/'client/Services/Generic/QuestJournal.cpp').read_text()
assert quest_recovery.count('PublishReconciledQuestInventory(world,*player);')==2
print('PASS confirmed originals use existing snapshot route after acquisition and reconciliation',flush=True)

# Verify the old virtual command has no live write path. The historical
# row decoder is retained so old campaign journals remain readable.
physical_server=(code/'server/Services/QuestService.cpp').read_text()
assert 'if (receipt->Shared) return;' in physical_server
assert 'HasAvailableHolder(authority.Journal.Operations(),*claim,character)' in physical_server
physical_client=(code/'client/Services/Generic/QuestJournal.cpp').read_text()
assert 'QuestPickupPermit::PhysicalHolder(character,item.Character,item.Copies.contains(character),count)' in physical_client
assert 'ObserveSharedQuestRemoval' not in physical_client
print('PASS physical-holder quest turn-in policy wiring',flush=True)

# Historical named grants may be recovered, but cannot intercept new native rewards.
legacy_reward_capture=extract('client/Services/Generic/QuestJournal.cpp','bool CaptureUniqueQuestAward(', 'bool DeferSharedQuestTurnIn(')
assert 'return false;' in legacy_reward_capture and 'return true;' not in legacy_reward_capture
assert 'GetRunner().Queue' not in legacy_reward_capture
print('PASS superseded named reward capture stays disabled',flush=True)


# Dynamic keyword selection with arbitrary plugin forms and authoritative
# stage source. Compile the actual server handler with the real journal.
reward_handler=extract('server/Services/QuestService.cpp','void ApplyKeywordRewardCommand(', 'void CampaignAccess::ReplayQuestReceipt(')
run('Q-13 keyword policy and authenticated native-copy choice',headers+'''
#include <KeywordQuestReward.h>
#include <filesystem>
struct Player{uint32_t Id;uint64_t Character;uint32_t GetId()const{return Id;}};
struct Party{std::vector<Player*> Members;};
struct World{Party party;World& GetPartyService(){return *this;}
 Party* GetPlayerParty(Player* p){return std::find(party.Members.begin(),party.Members.end(),p)!=party.Members.end()?&party:nullptr;}
};
namespace CampaignReplication{struct Link{enum class Mode{Ready,Disabled};Mode mode=Mode::Ready;CampaignJournal::Journal Journal;Mode State()const{return mode;}};}
namespace CampaignAccess{
 CampaignReplication::Link link;
 auto& Authority(){return link;}uint64_t Character(Player& p){return p.Character;}
}
namespace spdlog{template<class...T>void error(T&&...){}}
'''+reward_handler+'''
int main(){
 namespace R=KeywordQuestReward;using namespace CampaignJournal;
 assert(R::Classify(true,false,true,false,false)==R::Policy::Choice);
 assert(R::Classify(true,false,false,true,false)==R::Policy::Choice);
 assert(R::Classify(true,false,false,false,true)==R::Policy::Choice);
 assert(R::Classify(true,false,false,false,false)==R::Policy::Native);
 assert(R::Classify(true,true,true,true,true)==R::Policy::Protected);
 assert(R::Classify(false,false,true,true,true)==R::Policy::Native);
 R::Key k{TradeProperties::FormKey{"testquest.esm",0x999}.Encode(),TradeProperties::FormKey{"reward.esl",0xabc}.Encode(),55};
 auto root=std::filesystem::temp_directory_path()/("stm-keyword-reward-"+std::to_string(
   std::chrono::steady_clock::now().time_since_epoch().count()));
 assert(!std::filesystem::exists(root));
 auto& j=CampaignAccess::link.Journal;
 assert(j.Open(root,42,"test",true));
 Player a{1,101},b{2,202},outside{3,303};World w{{{&a,&b}}};
 auto key=k.Encode();
 auto ready="reward-keyword-ready "+key,choice="reward-keyword-choice "+key+" 2",
      keep="reward-keyword-keep "+key,removed="reward-keyword-removed "+key;
 ApplyKeywordRewardCommand(w,a,ready);assert(j.Current().Revision==0);
 assert(j.Append(j.Current(),{"stage","quest-progress-v1",k.Quest+" 1 0 55 8 101"})==Result::Committed);
 auto before=j.Current();ApplyKeywordRewardCommand(w,outside,ready);assert(j.Current()==before);
 ApplyKeywordRewardCommand(w,b,ready);
 assert(!R::Read(j.Operations(),k)->BothReady());
 before=j.Current();ApplyKeywordRewardCommand(w,a,choice);assert(j.Current()==before);
 ApplyKeywordRewardCommand(w,a,ready);
 auto s=R::Read(j.Operations(),k);assert(s&&s->BothReady());
 before=j.Current();ApplyKeywordRewardCommand(w,a,ready);ApplyKeywordRewardCommand(w,b,choice);assert(j.Current()==before);
 ApplyKeywordRewardCommand(w,a,choice);
 s=R::Read(j.Operations(),k);assert(s&&s->Recipient==202&&!s->Kept);
 assert(!R::CanRemove(*s,101,0,true));
 before=j.Current();ApplyKeywordRewardCommand(w,a,keep);ApplyKeywordRewardCommand(w,a,removed);assert(j.Current()==before);
 ApplyKeywordRewardCommand(w,b,keep);
 s=R::Read(j.Operations(),k);assert(s&&s->Kept&&R::CanRemove(*s,101,0,true));
 assert(!R::CanRemove(*s,202,0,true)&&!R::CanRemove(*s,101,0,false));
 assert(!R::CanRemove(*s,101,s->ChoiceRevision,true));
 ApplyKeywordRewardCommand(w,a,removed);
 s=R::Read(j.Operations(),k);assert(s&&s->Removed&&!R::CanRemove(*s,101,0,true));
 before=j.Current();ApplyKeywordRewardCommand(w,a,removed);assert(j.Current()==before);
 CampaignJournal::Journal reopened;assert(reopened.Open(root,42,"test",false)&&reopened.Current()==before);
 assert(R::Read(reopened.Operations(),k)->Recipient==202);
 assert(R::Read(reopened.Operations(),k)->Removed);
 auto ops=j.Operations();ops.push_back({"restart","quest-progress-v1",k.Quest+" 2 0 55 8 202"});
 assert(!R::Read(ops,k));
 assert(!R::Decode("reward-keyword-choice "+key+" 0"));
 assert(!R::Decode("reward-keyword-ready "+key+" extra"));
 std::filesystem::remove_all(root);
}
''',['-I'+str(code/'common'),str(code/'common/CampaignJournal.cpp'),str(code/'common/QuestJournalDisk.cpp')])
native=(code/'client/Games/Misc/BSScript.cpp').read_text()
assert 'UniqueQuestReward::FindSource' not in native
assert 'nativeUniqueRewardQuest=quest->formID;' in native
client=(code/'client/Services/Generic/QuestJournal.cpp').read_text()
assert 'QuestRewardAddObservation reward' in (code/'client/Games/Skyrim/Actor.cpp').read_text()
assert 'QuestRewardAddObservation reward' in (code/'client/Games/Skyrim/TESObjectREFR.cpp').read_text()
assert 'QuestRewardStageDone(questFormId,stage);' in (code/'client/Services/Generic/QuestService.cpp').read_text()
assert 'e->IsQuestObject()' in extract('client/Services/Generic/QuestJournal.cpp','bool CleanReward(', 'bool LiveReward(')
assert 'p->RemoveItem(item,1,ITEM_REMOVE_REASON::kRemove,nullptr,nullptr);' in extract('client/Services/Generic/QuestJournal.cpp','void UpdateKeywordRewards(', 'struct PendingQuestReceipt')
assert 'AddObjectToContainer' not in extract('client/Services/Generic/QuestJournal.cpp','void UpdateKeywordRewards(', 'struct PendingQuestReceipt')
print('PASS keyword reward native observation, stage barrier, protected/modified copy exclusion and no artificial grant',flush=True)


# Actor insertion owns a nested REFR insertion and publishes the actual
# alias-bearing unit only after Skyrim returns. Compile the production body.
actor_add=extract('client/Games/Skyrim/Actor.cpp','void TP_MAKE_THISCALL(HookAddInventoryItem, Actor','namespace\n{\n// Native SE layouts')
run('Q-04 actor direct insertion, nested pickup and exact inventory observation',headers+'''
#include <QuestPickupInventoryScope.h>
#define TP_MAKE_THISCALL(name,cls,...) name(cls* apThis,__VA_ARGS__)
struct TESForm{uint32_t formID=40;};struct TESBoundObject:TESForm{};
struct ExtraDataList{};
struct Inventory{struct Entry{uint32_t BaseId{};int32_t Count{};bool IsQuestItem{};};};
struct InventoryChangeEvent{uint32_t FormId;Inventory::Entry Item;uint32_t QuestId{},QuestRef{},QuestAlias{};};
struct TESObjectREFR{
 uint32_t formID=14;int64_t count{};int add{},calls{};bool exact{};
 int64_t GetItemCountInInventory(TESForm*){return count;}
 void GetItemFromExtraData(Inventory::Entry&,ExtraDataList*){}
};
struct Actor:TESObjectREFR{};struct PlayerCharacter:Actor{static PlayerCharacter* Get(){static PlayerCharacter p;return &p;}};
struct QuestRewardAddObservation{QuestRewardAddObservation(TESObjectREFR*,TESBoundObject*,ExtraDataList*,int32_t,TESObjectREFR*){}};
struct ScopedInventoryOverride{static bool IsOverriden(){return false;}};
namespace NativeTrade{bool BlockAdd(TESObjectREFR*,TESObjectREFR*){return false;}}
struct ModSystem{void GetServerModId(uint32_t a,uint32_t& b){b=a;}};
struct World{ModSystem mods;static World& Get(){static World w;return w;}ModSystem& GetModSystem(){return mods;}};
bool IdentifyQuestRemoval(TESObjectREFR& owner,TESForm*,ExtraDataList*,InventoryChangeEvent& e){
 if(!owner.exact||owner.count!=1)return false;
 e.Item.IsQuestItem=true;e.Item.Count=-1;e.QuestId=100;e.QuestRef=200;e.QuestAlias=3;return true;
}
std::vector<InventoryChangeEvent> sent;
void QueueActorInventoryChange(InventoryChangeEvent e,TESObjectREFR*){sent.push_back(e);}
void RealAddInventoryItem(Actor* o,TESBoundObject* item,ExtraDataList*,int32_t count,TESObjectREFR*){
 if(o==PlayerCharacter::Get()&&o->count==0&&count==1) assert(QuestPickupInventory::IsNestedAdd(o->formID,item->formID));
 o->count+=o->add;++o->calls;
}
namespace TiltedPhoques{template<class F,class...A>auto ThisCall(F f,A... a){return f(a...);}}
'''+actor_add+'''
int main(){
 auto* p=PlayerCharacter::Get();TESBoundObject item;
 auto add=[&]{HookAddInventoryItem(p,&item,nullptr,1,nullptr);};
 p->add=0;add();assert(sent.empty()&&p->calls==1);
 p->add=1;p->exact=true;add();assert(sent.size()==1&&sent.back().Item.Count==1&&sent.back().QuestRef==200);
 p->count=0;p->exact=false;add();assert(sent.size()==2&&!sent.back().QuestRef);
 p->count=0;
 {QuestPickupInventory::Scope scope(p->formID,item.formID);add();assert(sent.size()==2);}
 assert(!QuestPickupInventory::IsNestedAdd(p->formID,item.formID));
 Actor npc;npc.add=1;HookAddInventoryItem(&npc,&item,nullptr,1,nullptr);
 assert(sent.size()==3&&!sent.back().QuestRef);
}
''',['-I'+str(code/'common')])
reference=(code/'client/Games/Skyrim/TESObjectREFR.cpp').read_text()
assert 'CampaignAccess::Mode()!=CampaignReplication::Link::Mode::Ready' in reference
assert 'nativeQuestRemovalQuest=quest->formID' in (code/'client/Games/Misc/BSScript.cpp').read_text()
assert 'NativeQuestTransfer(apMoveToRef,aReason,change)' in reference
assert 'destinationBefore+1' in reference
assert 'QuestPickupPermit::AlreadyTaken(authority.Journal.Operations(),' in physical_server
receipt=extract('client/Services/Generic/QuestJournal.cpp','void QueueQuestItemReceipt(', 'void QuestRewardInventoryTouched(')
assert 'CampaignAccess::LocalIdentity(world)' in receipt
assert 'GetTransport().IsOnline()' not in receipt
send_receipt=extract('client/Services/Generic/QuestJournal.cpp','void SendQuestReceipt(', 'std::optional<DeferredQuestPickup> DescribeQuestPickup')
assert 'Acknowledged(' in send_receipt and 'Deadline' not in send_receipt
event=extract('client/Services/Generic/InventoryService.cpp','void InventoryService::OnInventoryChangeEvent(', 'void InventoryService::OnEquipmentChangeEvent(')
assert event.index('QueueQuestItemReceipt(m_world,request);')<event.index('if (!m_transport.IsConnected())')
print('PASS Q-04/09/10 receipt before transport lookup, native transfer evidence, no remote base-only quest copy',flush=True)


# TRAVEL-01: actual native sleep event record, first run and active stage,
# per-recipient acknowledgement, no stale/duplicate dispatch.
run('scripted travel native sleep trigger policy and durable arrival',headers+'''
#include <ScriptedTravel.h>
#include <filesystem>
int main(){
 namespace T=ScriptedTravel;using namespace CampaignJournal;
 T::Move m{TradeProperties::FormKey{"example.esm",0x123}.Encode(),
   TradeProperties::FormKey{"example.esm",0x456}.Encode(),"ExampleQuestScript",30,
   std::bit_cast<uint32_t>(12.f),std::bit_cast<uint32_t>(12.25f)};
 assert(m.Valid()&&T::Decode(m.Encode()));
 auto bad=m;bad.Script="Other Method";assert(!bad.Valid());
 bad=m;bad.EndBits=std::bit_cast<uint32_t>(-1.f);assert(!bad.Valid());
 assert(!T::Decode(m.Encode()+" extra"));
 auto root=std::filesystem::temp_directory_path()/("stm-sleep-travel-"+std::to_string(
   std::chrono::steady_clock::now().time_since_epoch().count()));
 assert(!std::filesystem::exists(root));
 Journal journal;assert(journal.Open(root,42,"test",true));
 assert(!T::CurrentStage(journal.Operations(),m));
 assert(journal.Append(journal.Current(),{"stage","quest-progress-v1",m.Quest+" 1 0 30 4 101"})==Result::Committed);
 assert(T::CurrentStage(journal.Operations(),m));
 assert(T::MayPublish(true,101,202,nullptr,true));
 assert(!T::MayPublish(false,202,101,nullptr,true));
 assert(!T::MayPublish(true,101,101,nullptr,true));
 assert(journal.Append(journal.Current(),{"move","quest-sleep-move-v1",T::Payload(m,101,202)})==Result::Committed);
 auto records=T::Read(journal.Operations());assert(records&&records->size()==1);
 auto r=records->at(0);assert(r.Revision==2&&!r.Arrived);
 assert(!T::MayPublish(true,101,202,&r,true));
 assert(T::MayDispatch(r,202,true,false,false,false));
 assert(!T::MayDispatch(r,101,true,false,false,false));
 assert(!T::MayDispatch(r,202,true,true,false,false));
 assert(!T::MayDispatch(r,202,true,false,true,false));
 assert(!T::MayDispatch(r,202,true,false,false,true));
 assert(!T::MayDispatch(r,202,false,false,false,false));
 assert(!T::MayAck(&r,101,m)&&T::MayAck(&r,202,m));
 assert(journal.Append(journal.Current(),{"ack","quest-sleep-arrived-v1",T::Payload(m,101,202)})==Result::Committed);
 records=T::Read(journal.Operations());assert(records&&records->at(0).Arrived);
 assert(!T::MayDispatch(records->at(0),202,true,false,false,false));
 assert(!T::MayAck(&records->at(0),202,m));
 auto old=journal.Current();Journal reopened;
 assert(reopened.Open(root,42,"test",false)&&reopened.Current()==old);
 assert(T::Read(reopened.Operations())->at(0).Arrived);
 auto ops=journal.Operations();ops.push_back({"later","quest-progress-v1",m.Quest+" 1 0 40 4 101"});
 assert(!T::CurrentStage(ops,m));
 ops=journal.Operations();ops.push_back({"repeat","quest-progress-v1",m.Quest+" 2 0 30 4 101"});
 assert(!T::CurrentStage(ops,m));
 ops=journal.Operations();ops.push_back({"duplicate","quest-sleep-move-v1",T::Payload(m,101,202)});
 assert(!T::Read(ops));
 std::filesystem::remove_all(root);
}
''',['-I'+str(code/'common'),str(code/'common/CampaignJournal.cpp'),str(code/'common/QuestJournalDisk.cpp')])
travel_native=(code/'client/Games/Misc/BSScript.cpp').read_text()
assert 'DragonFrameActor(&frame->Self)!=PlayerCharacter::Get()' in travel_native
assert 'BSInputEnableManager::IsMovementControlsEnabled()' in travel_native
assert 'DispatchMethodCall2(handle,scope,method,args,callback)' in travel_native
assert 'method("OnSleepStart")' in travel_native
assert 'ObserveScriptedTravel(std::move(*move))' in travel_native
travel_client=extract('client/Services/Generic/QuestJournal.cpp','struct SleepTravelObservation','struct DeferredQuestTurnIn')
assert 'AtScriptedTravelTarget(target)' in travel_client
assert 'DispatchScriptedSleep(questId,r.Event.Script,start,end)' in travel_client
assert '->MoveTo(' not in travel_client
assert 'sleepAttempts.insert(r.Event.Key())' in travel_client
assert 'quest->currentStage!=r.Event.Stage' in travel_client
assert 'ApplyScriptedTravel(m_world,*stamp,canDispatch)' in (code/'client/Services/Generic/QuestJournal.cpp').read_text()
assert 'ScriptedTravel::MayPublish(party->LeaderPlayerId==player.GetId()' in (code/'server/Services/QuestService.cpp').read_text()
print('PASS TRAVEL-01 native trigger integration, no coordinate fallback',flush=True)


run('Q-14 physical fragment policy and journal barriers',headers+'''
#include <QuestPhysicalStage.h>
struct Row{std::string Kind,Payload;};
int main(){
 using namespace QuestPhysicalStage;
 Observation o{2};o.Seen(Effect::Geometry);o.Done();
 assert(o.Remaining==1&&o.Individual());
 o.Seen(Effect::PlayerTravel);o.Done();assert(!o.Remaining&&!o.Individual());
 Observation n{1};n.Done();assert(!n.Individual());
 std::vector<Row> rows{{"quest-progress-v1","irrelevant"},{"quest-physical-stage-v1","quest 1 40 101"}};
 auto r=Read(rows);assert(r&&Any(*r,"quest",1)&&!Any(*r,"quest",2));
 assert(Has(*r,"quest",1,40)&&!Has(*r,"quest",1,20));
 assert(Reached(*r,"quest",1,40,101)&&!Reached(*r,"quest",1,40,202));
 assert(Blocks(*r,"quest",1,40,false,false)&&Blocks(*r,"quest",1,40,true,true));
 assert(!Blocks(*r,"quest",1,40,true,false)&&!Blocks(*r,"quest",2,40,false,false));
 rows.push_back({"quest-physical-stage-v1",Payload("quest",1,40,202)});
 r=Read(rows);assert(r&&Reached(*r,"quest",1,40,202));
 assert(Blocks(*r,"quest",1,40,false,false));
 rows.push_back({"quest-physical-stage-v1","quest 1 99999 101"});
 assert(!Read(rows));
}
''',['-I'+str(code/'common')])
physical_native=(code/'client/Games/Misc/BSScript.cpp').read_text()
assert 'caller=caller->Previous' in physical_native and 'ObserveQuestPhysicalEffect(physical->Form' in physical_native
physical_client=(code/'client/Services/Generic/QuestService.cpp').read_text()
assert 'FinishPhysicalQuestStage(m_world,questFormId,stage)' in physical_client
assert 'QueuePhysicalQuestProgress(m_world,formId,instance,std::move(update),itemCount)' in physical_client
assert 'if(quest->GetStageItemCount(message.Stage)&&!classifiedStages.contains(key)) return;' in physical_client
physical_server=(code/'server/Services/QuestService.cpp').read_text()
assert physical_server.index('!RecordPhysicalStage(authority.Journal,key,next.Generation,message.Stage,character)')<physical_server.index('if (durable && !StoreQuestRun(authority.Journal, key, character, next')
assert 'if(durable&&HasPhysicalStages(authority.Journal,key,run.Generation)) return;' in physical_server
assert 'SubmitObservedQuestStage(m_world,' in (code/'client/Services/Generic/InventoryService.cpp').read_text()
print('PASS Q-14 marker before stage, journal planner barrier, native source and alternate report wiring',flush=True)


# HOME-01: do not replay HousePurchase's shared, repeatable location-
# conditional stage as an ordinary quest. The native local purchase remains
# unmodified. This does not implement ownership or charge reconciliation.
house_filter=extract('client/Services/Generic/QuestService.cpp',
    'static constexpr std::array kNonSyncableQuestIds', 'void QuestService::DebugDumpQuests(')
run('city house purchases excluded from generic campaign stage replay',headers+'''
namespace CampaignReplication{struct Link{enum class Mode{Disabled,Ready};};}
namespace CampaignAccess{
 CampaignReplication::Link::Mode current=CampaignReplication::Link::Mode::Disabled;
 auto Mode(){return current;}
}
struct TESQuest{uint32_t formID;struct Stages{bool empty=false;bool Empty()const{return empty;}} stages;};
struct QuestService{static bool IsNonSyncableQuest(TESQuest*);};
'''+house_filter+'''
int main(){
 TESQuest house{0xA7B33},other{0x010A7B33},ordinary{0x1234},follower{0x750BA},empty{0x5678,{true}};
 assert(!QuestService::IsNonSyncableQuest(&house));
 CampaignAccess::current=CampaignReplication::Link::Mode::Ready;
 assert(QuestService::IsNonSyncableQuest(&house));
 assert(!QuestService::IsNonSyncableQuest(&other)&&!QuestService::IsNonSyncableQuest(&ordinary));
 assert(QuestService::IsNonSyncableQuest(&follower)&&QuestService::IsNonSyncableQuest(&empty));
 CampaignAccess::current=CampaignReplication::Link::Mode::Disabled;
 assert(!QuestService::IsNonSyncableQuest(&house));
}
''')
home_server=extract('server/Services/QuestService.cpp',
    'void QuestService::OnQuestChanges(', 'auto* pPlayer = acMessage.pPlayer;')
assert 'message.Id.BaseId==0xA7B33' in home_server
assert 'TradeProperties::FormKey{"skyrim.esm",0xA7B33}.Encode()' in home_server
assert 'CampaignAccess::Authority().State()!=CampaignReplication::Link::Mode::Disabled' in home_server
