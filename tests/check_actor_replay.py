"""Play deliberately delayed actions through the production client playback routine."""
from pathlib import Path
import subprocess, sys, tempfile
root=Path(sys.argv[1],"Code/client")
code=(root/"Systems/AnimationSystem.cpp").read_text()
update=code[code.index("void AnimationSystem::Update("):code.index("void AnimationSystem::Setup(")]
fixture=r"""
#include "Games/ReplicaPose.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <deque>
#include <string>
struct Actor;
struct TESForm{static TESForm* GetById(unsigned){return nullptr;}};
struct BGSAction:TESForm{};struct TESObjectREFR:TESForm{};struct TESIdleForm:TESForm{};
template<class T>T* Cast(TESForm* f){return static_cast<T*>(f);}
struct BSFixedString{BSFixedString(const char*){}};
struct Data{uint64_t Tick{};unsigned ActionId{},TargetId{},IdleId{},State1{},State2{},Type{};int Variables{};std::string EventName;};
struct Actor{struct{unsigned flags1{},flags2{};}actorState;struct {ReplicaPose Pose;bool player=false;bool IsPlayer(){return player;}} extension;
 struct {bool ready=true;unsigned reverts=0;bool IsReady(){return ready;}void RevertAnimationGraphManager(){++reverts;}} animationGraphHolder;
 auto* GetExtension(){return &extension;}int graph=5;void LoadAnimationVariables(int v){graph=v;}
 bool IsDead(){return ((actorState.flags1>>21)&15)==2;}};
struct RemoteAnimationComponent{std::deque<Data> TimePoints;Data LastRanAction;unsigned ReplayCount{};bool ResetAnimationGraphForReplay{};};
struct TESActionData{TESActionData(unsigned,Actor*,BGSAction*,TESObjectREFR*){}BSFixedString eventName{""};TESIdleForm* idleForm{};unsigned someFlag{};};
struct ActorMediator{unsigned count{};static auto Get()->ActorMediator*{static ActorMediator m;return &m;}bool ForceAction(TESActionData*){++count;return true;}};
struct World{uint64_t tick=1110;uint64_t GetTick(){return tick;}};
struct AnimationSystem{static void Update(World&,Actor*,RemoteAnimationComponent&,uint64_t)noexcept;};
"""
checks=r"""
int main(){
 Actor guest;World w;RemoteAnimationComponent c;auto& pose=guest.extension.Pose;
 auto play=[&](uint64_t t,unsigned action,unsigned life,unsigned knock,int vars){
  Data e{};e.Tick=t;e.ActionId=action;e.State1=(life<<21)|(knock<<25)|17u;e.State2=812;e.Variables=vars;
  c.TimePoints.push_back(e);AnimationSystem::Update(w,&guest,c,w.tick);assert(c.TimePoints.empty());};
 pose.Observe(ReplicaPose::Kneeling,1000);
 pose.Observe(ReplicaPose::Standing,1100);
 c.ReplayCount=1;c.ResetAnimationGraphForReplay=true;
 play(0,ReplicaPose::Death,2,7,41);
 #ifdef PREVIOUS
 assert(guest.IsDead() && ActorMediator::Get()->count==1);
 return 0;
 #endif
 assert(!guest.IsDead() && !ActorMediator::Get()->count && !c.ReplayCount && !guest.animationGraphHolder.reverts);
 play(1050,ReplicaPose::BleedStart,7,3,42);assert(guest.graph==5 && !ActorMediator::Get()->count);
 play(1050,308,2,7,43);assert(!guest.IsDead() && guest.graph==5 && ActorMediator::Get()->count==1);
 play(1105,309,2,7,44);assert(guest.actorState.flags1==17 && guest.graph==44);
 assert(!pose.AllowsVariables(1080,w.tick) && pose.AllowsVariables(1105,w.tick));
 pose.Observe(ReplicaPose::Kneeling,1120);assert(!pose.AllowsVariables(1130,1140));
 Actor other;assert(other.extension.Pose.AllowsVariables(1080,1140));
 guest.actorState.flags1=(2u<<21);pose={};
 play(1108,ReplicaPose::Death,0,0,47);assert(guest.IsDead() && guest.graph==47);
 // Normal knockdowns still work on a living NPC outside a managed fight.
 guest.actorState.flags1=0;play(1109,ReplicaPose::KnockDown,0,7,9);
 assert(guest.actorState.flags1==(17u|(7u<<25)));
 guest.extension.player=true;play(1109,309,7,3,71);
 assert(guest.actorState.flags1==(17u|(7u<<21)|(3u<<25)));
}
"""
with tempfile.TemporaryDirectory() as tmp:
    binary=Path(tmp,'playback')
    previous=len(sys.argv)>2
    include=Path(sys.argv[2],'Code/client') if previous else root
    subprocess.run(['g++','-std=c++20','-I',str(include),*(['-DPREVIOUS'] if previous else []),'-x','c++','-','-o',str(binary)],
                   input=fixture+update+checks,text=True,check=True)
    subprocess.run([str(binary)],check=True)
print("Reproduced living actor overwritten as dead by delayed action" if previous else "PASS delayed death, bleedout, standing transition, movement and valid corpse/knockdown/player playback")
if not previous:
    memory=(root/'Games/Memory.cpp').read_text()
    allocate=memory[memory.index('void* TP_MAKE_THISCALL(HookFormAllocate'):memory.index('void* Memory::Allocate')]
    mock=r"""
#include <cassert>
#include <cstdlib>
#include <new>
#include <cstddef>
#include <cstdint>
struct Actor{uint64_t data[8];};struct PlayerCharacter:Actor{uint64_t more[10];};
struct ActorExtension{inline static int constructed=0;uint64_t extra[3];ActorExtension(){++constructed;}};
struct ExActor:Actor,ActorExtension{};struct ExPlayerCharacter:PlayerCharacter,ActorExtension{};
struct GameHeap{};int RealFormAllocate{};size_t bytes{};
#define TP_MAKE_THISCALL(name,type,...) name(type* apThis,__VA_ARGS__)
namespace TiltedPhoques{void* ThisCall(int,GameHeap*,size_t n,size_t,bool){bytes=n;return std::malloc(n);}}
"""
    tests=r"""
int main(){GameHeap heap;
 for (size_t size : {sizeof(Actor),sizeof(PlayerCharacter),sizeof(ExActor),sizeof(ExPlayerCharacter),size_t(200)}) {
  auto before=ActorExtension::constructed;auto* p=HookFormAllocate(&heap,size,0,false);
  if(size==sizeof(Actor))assert(bytes==sizeof(ExActor) && ActorExtension::constructed==before+1);
  else if(size==sizeof(PlayerCharacter))assert(bytes==sizeof(ExPlayerCharacter) && ActorExtension::constructed==before+1);
  else assert(size==bytes && ActorExtension::constructed==before);
  std::free(p);
 }
}
"""
    with tempfile.TemporaryDirectory() as tmp:
        binary=Path(tmp,'actor_alloc')
        subprocess.run(['g++','-std=c++20','-x','c++','-','-o',str(binary)],input='#include <initializer_list>\n'+mock+allocate+tests,text=True,check=True)
        subprocess.run([str(binary)],check=True)
    print("PASS only genuine actor allocations receive the animation extension")
