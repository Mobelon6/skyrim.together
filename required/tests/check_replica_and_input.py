"""Run the actual NPC death gate and observer input filter against independent client states."""
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(sys.argv[1], 'Code/client')
life = (root / 'Services/Generic/CoopInteraction.cpp').read_text()
inputs = (root / 'Games/Skyrim/AI/Movement/PlayerControls.cpp').read_text()
defs = inputs[inputs.index('struct InputEvent\n'):inputs.index('PlayerControls* PlayerControls::GetInstance')]
native = inputs[inputs.index('static uint32_t TP_MAKE_THISCALL(HookProcessInputEvent'):inputs.index('static TiltedPhoques::Initializer s_playerControlsHooks')]
harness = r"""
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <utility>
using Clock=std::chrono::steady_clock;
std::atomic_uint32_t confirmedOpponent{};std::atomic_int64_t confirmedUntil{};
struct Role{bool remote{},player{};bool IsRemote(){return remote;}bool IsPlayer(){return player;}};
struct Actor{unsigned formID=1;Role role;Role*GetExtension(){return &role;}};
thread_local Actor* applyingCoopDeath{};
struct UI{bool modal=false;static UI*Get(){static UI ui;return &ui;}static bool ShouldUnpauseMenus(){return true;}bool IsUnpausedMenuOpen(){return modal;}};
struct PlayerControls{struct {float MoveInputVec,PrevMoveVec;bool bAutoMove;} Data;};
struct OverlayService{static inline bool watch=false;static bool ObservingDialogue(){return watch;}};
#define TP_MAKE_THISCALL(F,C,...) F(C* apThis,__VA_ARGS__)
namespace TiltedPhoques { template<typename F,typename... A>auto ThisCall(F f,A&&... args){return f(static_cast<A&&>(args)...);} }
"""
fake = r"""
std::vector<InputEvent*> received;
unsigned RealProcessInputEvent(PlayerControls*,InputEvent* const* head,void*){
    received.clear();
    for(auto* event=head?*head:nullptr;event;event=event->next)received.push_back(event);
    return 19;
}
"""
tests = r"""
int main(){
 static int total=0;
 originalDeath=[](Actor*,Actor*,float,bool,bool){++total;};
 Actor npc,other;npc.role.remote=true;
 ReplicaDeath(&npc,nullptr,2,true,true);assert(total==0);
 applyingCoopDeath=&other;ReplicaDeath(&npc,nullptr,2,true,true);assert(total==0);
 applyingCoopDeath=&npc;ReplicaDeath(&npc,nullptr,2,true,true);assert(total==1);
 confirmedOpponent=1;confirmedUntil=(Clock::now()+std::chrono::seconds(2)).time_since_epoch().count();
 ReplicaDeath(&npc,nullptr,2,true,true);assert(total==1);
 confirmedUntil=0;applyingCoopDeath=nullptr;npc.role.remote=false;
 ReplicaDeath(&npc,nullptr,2,true,true);assert(total==2);
 npc.role.player=true;npc.role.remote=true;ReplicaDeath(&npc,nullptr,2,true,true);assert(total==3);
 PlayerControls c{{1,1,true}};InputEvent button{},mouse{};mouse.type=1;
 ObserverThumbstick left{},right{};left.type=right.type=3;left.id=11;right.id=12;
 button.next=&mouse;mouse.next=&left;left.next=&right;InputEvent* head=&button;
 OverlayService::watch=true;assert(HookProcessInputEvent(&c,&head,nullptr)==19);
 assert(received.size()==2&&received[0]==&mouse&&received[1]==&right);
 assert(button.next==&mouse&&mouse.next==&left&&left.next==&right&&!right.next);
 assert(!c.Data.MoveInputVec&&!c.Data.PrevMoveVec&&!c.Data.bAutoMove);
 OverlayService::watch=false;HookProcessInputEvent(&c,&head,nullptr);assert(received.size()==4);
 UI::Get()->modal=true;HookProcessInputEvent(&c,&head,nullptr);assert(received.empty());
}
"""
death = life[life.index('using DeathFunction'):life.index('void GuardReplica')]
with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp, 'replica')
    subprocess.run(['g++','-std=c++20','-Wno-invalid-offsetof','-x','c++','-','-o',str(exe)],
                   input=harness+death+defs+fake+native+tests,text=True,check=True)
    subprocess.run([str(exe)],check=True)
print('PASS native death ownership, real death permission and brawl terminal guard; keyboard, mouse and two controller sticks')
