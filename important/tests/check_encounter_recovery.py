"""Exercise the shipping actor reconciliation and server quest filter with deterministic frames."""
from pathlib import Path
import tempfile
import subprocess
import sys

root = Path(sys.argv[1], "Code")
actor = (root / "client/Services/Generic/CoopInteraction.cpp").read_text()
server = (root / "server/Services/QuestService.cpp").read_text()
reconcile = actor[actor.index("struct PeaceSnapshot"):actor.index("void ApplyPeace")]
scope = server[server.index("    if (!pParty) return;"):server.index("    if (message.Status == RequestQuestUpdate::Started")]
party = (root / "server/Services/PartyService.h").read_text()
conversation = party[party.index("        struct Conversation"):party.index("        struct Follower")]
fixture = r"""
#include "Games/ReplicaPose.h"
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <vector>
#include <type_traits>
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace glm {
struct vec3 { float x,y,z; };
vec3 operator-(vec3 a,vec3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
float length(vec3 a){return std::sqrt(a.x*a.x+a.y*a.y+a.z*a.z);}
}
namespace spdlog { template<class... A> void info(A...) {} }
struct BSFixedString { const char* text; BSFixedString(const char* t):text(t){} };
struct Joint { glm::vec3 worldPosition; };
struct Skeleton { Joint hips{{0,0,0}}, head{{0,0,45}}; bool present=true;
 Joint* GetByName(const BSFixedString& s){ return !present ? nullptr : (strstr(s.text,"Head") ? &head : &hips); } };
struct Role { ReplicaPose Pose; bool player=false,remote=true;bool IsRemote(){return remote;}bool IsPlayer(){return player;} };
struct Actor {
 struct Graph { bool ready=true,bleeding=false;bool IsReady(){return ready;}
 bool GetVariableBool(BSFixedString*,bool* result){*result=bleeding;return true;} } animationGraphHolder;
 unsigned formID=123;Skeleton root;Role role;
 struct {unsigned flags1{};bool bleed=false;bool IsBleedingOut(){return bleed;}} actorState;
 bool loaded=true,disabled=false,dead=false,fighting=false,rag=false,rebuildWorks=true;
 int rebuilds{},getups{},stand{},stops{},revives{},ragFixes{};
 Actor* target{};
 Skeleton* GetNiNode(){return loaded?&root:nullptr;} Role* GetExtension(){return &role;}
 bool IsInRagdollState(){return rag;}bool IsDisabled(){return disabled;}bool IsDead(){return dead;}
 Actor* GetCombatTarget(){return target;}bool IsInCombat(){return fighting;}
 void RestoreRemoteLife(){++revives;dead=false;}
 void StopCombat(){++stops;fighting=false;target=nullptr;}
 void RefreshReplicaModel(){++rebuilds;if(rebuildWorks){root.head.worldPosition={0,0,45};rag=false;actorState.flags1=0;}}
 void PotentiallyFixRagdollState(){++ragFixes;}void InitiateGetUpPackage(){++getups;}
 bool SendAnimationEvent(const BSFixedString*){++stand;animationGraphHolder.bleeding=false;return true;}
};
struct PlayerCharacter : Actor {static Actor* Get(){static PlayerCharacter player;return &player;}};
struct PlayerComponent{unsigned Id;};struct FormIdComponent{unsigned Id;};
struct World{uint64_t GetTick(){return 10000;} struct View:std::vector<int>{template<class T>T get(int)const{return {99};}};
 template<class...T>View view(){return {};}};
"""
checks = r"""
int main(){
 World world;auto start=Clock::now();auto state=[&]{PeaceSnapshot s{};s.encounter=true;s.expires=start+15s;return s;};
 auto frame=[&](Actor& npc, PeaceSnapshot& s,int elapsed){return VerifyStanding(world,&npc,s,start+std::chrono::milliseconds(elapsed));};
 // A living biped is lying flat even though the engine reports normal knock, life and ragdoll flags.
 Actor model;auto s=state();model.root.head.worldPosition={40,0,2};
 bool complete=false;for(int ms=0;ms<=6000;ms+=100)if(frame(model,s,ms)){complete=true;break;}
 assert(complete && model.rebuilds==1 && !model.revives && model.stand>0);
 // A game-supplied get-up animation can finish on its own before any fallback is needed.
 Actor recovery;auto t=state();recovery.rag=true;recovery.actorState.flags1=6u<<25;
 assert(!frame(recovery,t,0));assert(!frame(recovery,t,1200));assert(recovery.getups==0);
 recovery.rag=false;recovery.actorState.flags1=0;assert(!frame(recovery,t,1800));assert(frame(recovery,t,2800));
 assert(!recovery.rebuilds);
 Actor late;auto u=state();late.loaded=false;late.dead=true;assert(!frame(late,u,0));
 late.loaded=true;late.rag=true;assert(!frame(late,u,900));assert(late.revives==1 && late.ragFixes==1 && late.getups==1);
 late.dead=true;assert(frame(late,u,1100));assert(late.revives==1);
 Actor knee;auto b=state();knee.animationGraphHolder.bleeding=true;knee.animationGraphHolder.ready=false;
 assert(!frame(knee,b,0) && !knee.stand);knee.animationGraphHolder.ready=true;
 assert(!frame(knee,b,200) && knee.stand==1 && !knee.animationGraphHolder.bleeding);
 assert(frame(knee,b,1400) && !knee.rebuilds);
 Actor chair;auto v=state();chair.actorState.flags1=3u<<14;chair.root.head.worldPosition={30,0,1};
 assert(frame(chair,v,0) && !chair.stand && !chair.rebuilds);
 Actor other,adversary;auto p=state();other.target=&adversary;other.fighting=true;assert(frame(other,p,0)&&!other.stops);
 Actor permanent;auto q=state();permanent.rebuildWorks=false;permanent.rag=true;
 for(int ms=0;ms<15000;ms+=100)assert(!frame(permanent,q,ms));
 assert(frame(permanent,q,15000) && permanent.rebuilds==1);
 Actor alone;auto r=state();r.encounter=false;alone.role.remote=false;assert(frame(alone,r,0));
 auto k=state();alone.role.remote=true;alone.target=PlayerCharacter::Get();alone.fighting=true;
 assert(!frame(alone,k,0) && alone.stops==0);alone.fighting=true;alone.target=&adversary;
 assert(frame(alone,k,200) && alone.stops==0);
}
"""
quest_fixture = r"""
using GameId=unsigned;
struct DialogueAnchor {unsigned Choice{},TopicInfo{},TopicQuest{},TargetForm{};};
struct Party {
CONVERSATION
unsigned LeaderPlayerId=1;
std::map<unsigned,Conversation> Talks,Brawls;
} data;
struct Player {unsigned id=1;unsigned GetId(){return id;}} owner;
struct RequestQuestUpdate {enum {StageUpdate,Started,Stopped};unsigned Id{}, Status{};bool JournalObjective{},CompletedRunOnce{};};
struct Notice {unsigned ClientQuestType{},Subject{},Encounter{};};
bool bEnableMiscQuestSync=false;
void route(Party* pParty, Player* pPlayer, RequestQuestUpdate message, Notice& notify, int& sent,bool knownSharedQuest=false){
 const Party::Conversation* participants=nullptr;
FILTER
 ++sent;
}
int main(){
 auto& talk=data.Talks[1];talk.Expires=Clock::now()+10s;
 talk.Anchor={1,91,100,81};talk.ChoiceListeners={2};talk.ChoiceExpires=talk.Expires;talk.Encounter=11;
 auto sends=[&](unsigned id,unsigned status,bool inJournal,unsigned& encounter,unsigned type=6){Notice n{type};int sent=0;
 route(&data,&owner,{id,status,inJournal},n,sent);encounter=n.Encounter;return sent;};unsigned e{};
 assert(sends(100,0,true,e)==1 && e==11);
 assert(!sends(200,0,true,e)&&!sends(201,1,true,e)&&!sends(202,2,true,e));
 talk.ChoiceListeners.clear();assert(!sends(100,0,true,e));
 data.Brawls[1]=talk;data.Talks.clear();assert(!sends(200,0,true,e));
 data.Brawls.clear();assert(sends(200,0,true,e)==1 && !e);assert(!sends(200,0,false,e));
 assert(!sends(200,1,true,e)&&!sends(200,2,true,e));
 owner.id=2;
 auto guest=[&](unsigned status,bool inJournal,bool known,bool completed=false){Notice n{6};int sent=0;route(&data,&owner,{200,status,inJournal,completed},n,sent,known);return sent;};
 assert(!guest(0,true,false));assert(guest(0,true,true));assert(!guest(0,false,true));
 assert(!guest(1,true,true)&&!guest(2,true,true));
 assert(guest(2,false,true,true)&&!guest(2,false,false,true));
 data.Brawls[2]=talk;assert(!guest(0,true,true));
}
"""
with tempfile.TemporaryDirectory() as tmp:
    for name, source in [('actor',fixture+reconcile+checks),
                         ('scope',fixture+quest_fixture.replace('CONVERSATION',conversation).replace('FILTER',scope))]:
        exe=Path(tmp,name)
        subprocess.run(['g++','-std=c++20','-I',str(root/'client'),'-x','c++','-','-o',str(exe)],input=source,text=True,check=True)
        subprocess.run([str(exe)],check=True)
print('PASS prone living replica, native get-up, unloaded model, true subsequent death, seating, combat and rebuild limit')
print('PASS witnessed topic, unrelated stages, fight gap, background start/stop and independent journal progress')
