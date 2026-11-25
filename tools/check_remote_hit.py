"""Run the receiving client's actual health handler with divergent local/owner HP and repeat delivery."""
from pathlib import Path
import subprocess
import sys
import tempfile

src = Path(sys.argv[1], 'Code/client/Services/Generic/ActorValueService.cpp').read_text()
start = src.index('void ActorValueService::OnHealthChangeBroadcast(')
finish = src.index('void ActorValueService::OnActorValueChanges(', start)
handler = src[start:finish]
mock = r"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <optional>
struct NotifyHealthChangeBroadcast {unsigned Id, OwnershipEpoch;float DeltaHealth; bool KeepAlive;};
struct OverlayService {static inline bool brawl{};static bool NonlethalOpponent(unsigned){return brawl;}};
struct ActorExtension {
  bool player{}, remote{};
  bool IsRemote()const{return remote;}
  bool IsPlayer()const{return player;}
};
struct ActorValueInfo {enum {kHealth};};
struct ActorValueOwner {enum class ForceMode {DAMAGE};};
struct Actor {
  unsigned formID=7;ActorExtension ext;
  float hp=3;
  bool dead=false;
  int kills=0,writes=0;
  ActorExtension* GetExtension(){return &ext;}
  float GetActorValue(int)const{return hp;}
  void ForceActorValue(ActorValueOwner::ForceMode,int,float val){hp=val;++writes;}
  bool IsDead()const{return dead;}
  void Kill(){++kills;dead=true;}
};
Actor npc;
namespace Utils {
std::optional<int> FindEntityByServerId(unsigned id){return id==9?std::optional<int>(0):std::nullopt;}
template<typename T>T* GetByServerId(unsigned){return &npc;}
}
struct OwnershipSystem {static unsigned GetOwnershipEpoch(int,int){return 7;}};
struct ActorValueService {int m_world=0;void OnHealthChangeBroadcast(const NotifyHealthChangeBroadcast&)const noexcept;};
"""
cases = r"""
int main(){
    ActorValueService receiver;
    const auto hit=[&](float damage,bool keepAlive,unsigned epoch=7){receiver.OnHealthChangeBroadcast({9,epoch,damage,keepAlive});};
    OverlayService::brawl=true;hit(-20,false);assert(npc.hp==1&&!npc.dead);OverlayService::brawl=false;
    for(bool remote : {true,false}){
        npc=Actor{};npc.ext.remote=remote;
        hit(-12,true,6); assert(npc.hp==3 && !npc.writes);
        hit(-12,true); assert(npc.hp==1 && !npc.kills);
        hit(-12,true); assert(npc.hp==1 && !npc.kills);
        npc.hp=.3f;hit(-12,true); assert(npc.hp==.3f);
        npc.dead=true;int before=npc.writes;hit(-12,true);assert(npc.writes==before);
    }
    npc=Actor{};hit(-12,false);assert(npc.kills==1 && npc.dead);
    npc=Actor{};npc.ext.remote=true;hit(-12,false);assert(!npc.kills && npc.hp==3);
    npc=Actor{};npc.ext.player=true;hit(-12,false);assert(!npc.kills);
}
"""
with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp, 'receive')
    subprocess.run(['g++','-std=c++20','-Wall','-Werror','-x','c++','-','-o',str(exe)],
                   input=mock+handler+cases,text=True,check=True)
    subprocess.run([str(exe)],check=True)
print("PASS receiving actor health after a nonlethal hit, stale ownership and ordinary death")
