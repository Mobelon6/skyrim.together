"""Exercise the production probe, owner reply and replica correction with two NPC simulations."""
from pathlib import Path
import subprocess,sys,tempfile
root=Path(sys.argv[1],'Code')
client=(root/'client/Services/Generic/ActorValueService.cpp').read_text()
server=(root/'server/Services/ActorValueService.cpp').read_text()
actor=(root/'client/Games/Skyrim/Actor.cpp').read_text()
def body(src,signature,upper=None):
    start=src.index(signature)
    return src[start:src.index(upper,start) if upper else len(src)]
pre=r"""
#include <cassert>
#include <cstdint>
#include <chrono>
#include <optional>
#include <vector>
using namespace std::chrono_literals;
struct RequestNpcLifeCheck{unsigned Id{},OwnershipEpoch{};};
struct NotifyNpcLifeCheck{unsigned Id{},OwnershipEpoch{};};
struct RequestDeathStateChange{unsigned Id{},OwnershipEpoch{};bool IsDead{};};
struct NotifyDeathStateChange{unsigned Id{},OwnershipEpoch{};bool IsDead{};};
"""
sv=r"""
namespace entt{using entity=unsigned;}
struct CellIdComponent{unsigned world{};bool IsInRange(const CellIdComponent& other,bool)const{return world==other.world;}};
struct Player{CellIdComponent cell{1};std::vector<NotifyNpcLifeCheck> sent;
 auto&GetCellComponent(){return cell;}void Send(NotifyNpcLifeCheck n){sent.push_back(n);}};
struct OwnerComponent{Player*p{};unsigned OwnershipEpoch=4;auto*GetOwner()const{return p;}};
struct World{OwnerComponent owner;CellIdComponent cell{1};bool valid(unsigned id){return id==8;}
 template<class T>T*try_get(unsigned){if constexpr(std::is_same_v<T,OwnerComponent>)return &owner;else return &cell;}};
template<class T>struct PacketEvent{Player*pPlayer;T Packet;};
struct ActorValueService{World&m_world;void OnLifeCheck(const PacketEvent<RequestNpcLifeCheck>&)const noexcept;};
"""
svtest=r"""
int main(){Player owner,guest;World w;w.owner.p=&owner;ActorValueService s{w};
 auto query=[&](Player&p,unsigned id,unsigned epoch){s.OnLifeCheck({&p,{id,epoch}});};
 query(owner,8,4);query(guest,8,3);query(guest,8,0);query(guest,7,4);
 guest.cell.world=2;query(guest,8,4);assert(owner.sent.empty());
 guest.cell.world=1;query(guest,8,4);assert(owner.sent.size()==1&&owner.sent[0].Id==8&&owner.sent[0].OwnershipEpoch==4);
}
"""
cl=r"""
namespace entt{using entity=unsigned;}
struct OverlayService{static bool NonlethalOpponent(unsigned){return false;}static void CheckRevivedReplica(unsigned){}};
struct ActorExtension{bool IsRemote(){return !player;}bool player{};bool IsPlayer(){return player;}};
struct Actor{ActorExtension ex;bool dead=true,loaded=true;int resets{},resurrects{},positionUpdates{},kills{};
 auto*GetExtension(){return &ex;}bool IsDead(){return dead;}bool GetNiNode(){return loaded;}
 void Resurrect(bool resetInv){assert(!resetInv);dead=false;++resurrects;}void Update3DPosition(bool warp){assert(warp);++positionUpdates;}
 void Respawn(){++resets;}void Kill(){++kills;dead=true;}void RestoreRemoteLife()noexcept;
 unsigned formID=44;
};
Actor actor;
struct TESForm{static Actor*GetById(unsigned id){return id==44?&actor:nullptr;}};
template<class T>T*Cast(Actor*a){return a;}
struct FormIdComponent{unsigned Id=44;};struct PlayerComponent{};
struct RemoteComponent{unsigned ConfirmedCorpseEpoch{},OwnershipEpoch=4,Id=8;std::chrono::steady_clock::time_point NextLifeCheck{};};
struct WaitingFor3D{std::optional<NotifyDeathStateChange> PendingDeathState;};
struct World{FormIdComponent f;RemoteComponent r;WaitingFor3D wait;bool waiting{},player{},owner{};
 template<class T>T*try_get(unsigned){if constexpr(std::is_same_v<T,FormIdComponent>)return &f;else return waiting?&wait:nullptr;}
 template<class T>T&get(unsigned){return r;}
 template<class T>bool all_of(unsigned){return false;}
 template<class... T>bool any_of(unsigned){return waiting||player;}
};
namespace Utils{std::optional<unsigned>FindEntityByServerId(unsigned id){return id==8?std::optional<unsigned>(id):std::nullopt;}}
struct OwnershipSystem{
 static std::optional<unsigned>AcceptOwnerUpdate(World&w,unsigned id,unsigned epoch){return w.owner&&epoch==w.r.OwnershipEpoch?Utils::FindEntityByServerId(id):std::nullopt;}
 static std::optional<unsigned>AcceptRemoteUpdate(World&w,unsigned id,unsigned epoch){return !w.owner&&epoch==w.r.OwnershipEpoch?Utils::FindEntityByServerId(id):std::nullopt;}
};
namespace spdlog{template<class...T>void info(T...){}}
struct Transport{std::vector<RequestNpcLifeCheck> probes;std::vector<RequestDeathStateChange> replies;
 void Send(RequestDeathStateChange n){replies.push_back(n);}void Send(RequestNpcLifeCheck n){probes.push_back(n);}};
struct ActorValueService{World&m_world;Transport&m_transport;
 void OnLifeCheck(const NotifyNpcLifeCheck&)const noexcept;void OnDeathStateChange(const NotifyDeathStateChange&)const noexcept;
 void Observe(bool dead,std::chrono::steady_clock::time_point now){
 auto&remote=m_world.r;unsigned entity=8;
"""
probe=body(client,'        if (!dead)\n        {\n            remote.ConfirmedCorpseEpoch','        if (dead != remote.ObservedDead')
cltest=r"""
int main(){
 World w;Transport network;ActorValueService s{w,network};
 auto t=std::chrono::steady_clock::time_point{}+1s;
 s.Observe(true,t);s.Observe(true,t+500ms);assert(network.probes.empty());
 s.Observe(true,t+1s);s.Observe(true,t+2s);assert(network.probes.size()==1);
 w.owner=true;actor.dead=false;s.OnLifeCheck({8,3});assert(network.replies.empty());
 s.OnLifeCheck({8,4});assert(network.replies.size()==1&&!network.replies.back().IsDead);
 w.owner=false;actor.dead=true;s.OnDeathStateChange({8,3,false});assert(actor.dead);
 s.OnDeathStateChange({8,4,false});s.OnDeathStateChange({8,4,false});
 assert(!actor.dead&&actor.resurrects==1&&actor.positionUpdates==1&&!actor.resets);
 s.OnDeathStateChange({8,4,true});assert(actor.dead&&w.r.ConfirmedCorpseEpoch==4);
 s.Observe(true,t+10s);assert(network.probes.size()==1);
 w.r.OwnershipEpoch=5;s.Observe(true,t+11s);assert(network.probes.size()==2);
 s.Observe(false,t+12s);w.waiting=true;s.Observe(true,t+14s);assert(network.probes.size()==2);
 s.OnDeathStateChange({8,5,false});assert(w.wait.PendingDeathState.has_value()&&actor.dead);
 w.waiting=false;actor.ex.player=true;s.OnDeathStateChange({8,5,false});assert(actor.dead);
}
"""
tests={'server':sv+body(server,'void ActorValueService::OnLifeCheck(')+svtest,
       'client':cl+probe+'}};\n'+body(actor,'void Actor::RestoreRemoteLife()','void Actor::Respawn()')+
                body(client,'void ActorValueService::OnDeathStateChange(')+cltest}
with tempfile.TemporaryDirectory() as td:
    for side,src in tests.items():
        exe=Path(td,side);subprocess.run(['g++','-std=c++20','-I',str(Path(sys.argv[1],'Code/client')),'-Wall','-Werror','-x','c++','-','-o',str(exe)],input='#include "Games/ReplicaPose.h"\n'+pre+src,text=True,check=True)
        subprocess.run([str(exe)],check=True)
print('PASS observed corpse debounce, ownership/range, authoritative living/dead reply, engine reset exclusion and unloaded actors')
