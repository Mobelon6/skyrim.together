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
}
''')

server=extract('server/Services/QuestService.cpp','bool AdvanceQuestRun(', 'std::string QuestKey(')
run('party quest lifecycle and ordered catch-up history',headers+'''
struct NotifyQuestUpdate{enum {StageUpdate,Started,Stopped};uint16_t Stage;uint8_t Status;};
struct QuestRun{unsigned Generation{1};std::set<uint16_t> Stages;std::vector<uint16_t> OrderedStages; bool Stopped=false;bool Started=false;};
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
