"""Run the production encounter reconciliation with independent actor and combat-group targets."""
import subprocess, sys, tempfile
from pathlib import Path
src = Path(sys.argv[1], 'Code/client/Services/Generic/EncounterCombat.cpp').read_text()
body = src[src.index('namespace\n{'):]
fixture = r"""
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <map>
#include <vector>
struct Actor;
static std::map<uint32_t,Actor*> byID;
struct TESForm {static Actor* GetById(uint32_t i) {return byID[i];}};
struct TESObjectREFR {static Actor* GetByHandle(uint32_t i) {return byID[i];}};
template<class T> T* Cast(Actor* a){return static_cast<T*>(a);}
struct Group {
 struct Lock {int readers{}; void LockForRead(){++readers;}void UnlockRead(){--readers;}} lock;
 struct Target{uint32_t targetHandle{};};
 struct {unsigned length{};Target items[4];Target& operator[](unsigned i){return items[i];}} targets;
};
struct Actor {
 uint32_t id; explicit Actor(uint32_t i):id(i){byID[i]=this;}
 Actor* target{};Group group;bool fight{},dead{},loaded=true;unsigned stops{},delayed{};
 struct Handle{struct {uint32_t iBits;} handle;};
 Handle GetHandle(){return {{id}};}Actor* GetCombatTarget(){return target;}
 Group* GetCombatGroup(){return &group;}bool IsDead(){return dead;}bool IsDisabled(){return false;}bool IsInCombat(){return fight;}
 Actor* GetNiNode(){return loaded?this:nullptr;}
 void StopCombat(){assert(!group.lock.readers);++stops;if(stops<=delayed)return;target=nullptr;fight=false;group.targets.length=0;}
};
struct PlayerCharacter{static Actor* Get(){return byID[1];}};
struct PlayerComponent {uint32_t Id;};struct FormIdComponent {uint32_t Id;};
struct World{
 struct Party{std::vector<unsigned> members{22};auto& GetPartyMembers(){return members;}} party;
 Party& GetPartyService(){return party;}
 struct View:std::vector<unsigned>{ template<class T>T get(unsigned)const{
 if constexpr(std::is_same_v<T,PlayerComponent>)return {22};else return {2};}};
 template<class... T> View view(){View v;v.push_back(22);return v;}
};
struct ProcessLists{std::vector<uint32_t> highActorHandleArray;
 static ProcessLists* Get(){static ProcessLists p;return &p;}};
struct OverlayService {static void BeginCombatSettlement(uint32_t)noexcept;static void CancelCombatSettlement(uint32_t)noexcept;
 static void ObservePartyCombat(uint32_t)noexcept;static void ClearCombatSettlements()noexcept;static void UpdateCombatSettlements(World&)noexcept;};
namespace spdlog {template<class ...T> void info(T...){}}
"""
checks=r"""
int main(){
 World w; Actor visitor(1), speaker(2), opponent(42),guard(88);
 auto t=Clock::now(); auto& svc=OverlayService::UpdateCombatSettlements;
 visitor.fight=opponent.fight=true;opponent.target=&visitor;
 visitor.group.targets.length=1;visitor.group.targets[0]={42};
 OverlayService::ObservePartyCombat(42);
 // The compass can have lost its current target before the settlement arrives.
 visitor.target=nullptr;visitor.group.targets.length=0;opponent.target=&speaker;opponent.delayed=1;
 OverlayService::BeginCombatSettlement(42);auto& s=settling.at(42);
 assert(!Settle(w,&opponent,s,t) && opponent.stops==1 && visitor.stops==1 && !visitor.fight);
 assert(!Settle(w,&opponent,s,t+100ms) && opponent.stops==1);
 assert(!Settle(w,&opponent,s,t+450ms) && opponent.stops==2 && !opponent.fight);
 assert(!Settle(w,&opponent,s,t+600ms));assert(Settle(w,&opponent,s,t+3200ms));
 OverlayService::ClearCombatSettlements();opponent.fight=visitor.fight=true;
 opponent.target=&visitor;visitor.target=&opponent;guard.fight=true;guard.target=&visitor;
 ProcessLists::Get()->highActorHandleArray={88};
 Settlement two{};Settle(w,&opponent,two,t);
 assert(!opponent.fight && visitor.fight && visitor.stops==1 && guard.stops==0);
 // A subsequent assault and an opponent currently fighting a bystander end the old intervention.
 opponent.fight=true;opponent.target=&guard;Settlement third{};Settle(w,&opponent,third,t);
 assert(opponent.fight && opponent.stops==3);
 OverlayService::BeginCombatSettlement(42);OverlayService::CancelCombatSettlement(42);svc(w);
 assert(settling.empty() && guard.fight);
}
"""
with tempfile.TemporaryDirectory() as w:
    exe=Path(w,'combat')
    subprocess.run(['g++','-std=c++20','-Wall','-Werror','-x','c++','-','-o',str(exe)],input=fixture+body+checks,text=True,check=True)
    subprocess.run([str(exe)],check=True)
print('PASS both ends of combat, vanished HUD target, delayed native stop, actual second attacker and subsequent assault')
