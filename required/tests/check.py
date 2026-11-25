from pathlib import Path
from tempfile import TemporaryDirectory
import sys
import subprocess as sp
if len(sys.argv) != 2:
 raise SystemExit('Usage: python check.py <patched TiltedEvolution checkout>')
root=Path(sys.argv[1]).resolve()/'Code'
def body(path,begin,after):
 t=(root/path).read_text(); i=t.index(begin); return t[i:t.index(after,i)]
def check(code,label):
 with TemporaryDirectory() as tmp:
  cpp=Path(tmp)/'verify.cpp';cpp.write_text(code)
  exe=Path(tmp)/'verify';sp.run(['g++','-std=c++20','-Wall','-Werror','-Wno-unused-parameter','-Wno-reorder',str(cpp),'-o',str(exe)],check=True);sp.run([str(exe)],check=True)
 print('PASS',label)
hook=body('client/Games/Skyrim/Actor.cpp','bool TP_MAKE_THISCALL(HookDamageActor','TP_THIS_FUNCTION(TApplyActorEffect')
event=(root/'client/Events/HealthChangeEvent.h').read_text().replace('#pragma once','')
check('''
#include <cassert>
#include <cstdint>
#include <algorithm>
#include <vector>
'''+event+'''
struct HitEvent { int a,b; };
struct Runner { std::vector<HealthChangeEvent> hp; void Trigger(HitEvent) {} void Trigger(HealthChangeEvent e){ hp.push_back(e); } };
struct World { static World& Get(){ static World w;return w;} Runner rs; auto& GetRunner(){return rs;}
struct Settings{bool PvpEnabled=false;} s; auto& GetServerSettings(){return s;} };
struct Ext { int role; bool IsLocalPlayer()const{return role==2;} bool IsRemotePlayer()const{return role==3;} bool IsLocal()const{return role==0||role==2;} };
struct ActorValueInfo {static constexpr int kHealth=24;};
struct Actor {int formID; Ext role; float hp; bool protects; bool projected=false; struct State {bool IsBleedingOut()const{return false;}} actorState{}; bool IsEssential()const{return protects;} Ext* GetExtension(){return &role;} float GetActorValue(int){return hp;}};
namespace HomeActorProtection { bool Resident(const Actor* a){return a&&a->projected;} }
struct GameplayFormulas {static float CalculateRealDamage(Actor*,float d,bool){return d;}};
static bool RealDamageActor(Actor*a,float d,Actor*,bool){a->hp=a->protects&&d>=a->hp?1.0f:a->hp-d;return a->hp<=0;}
struct TiltedPhoques {static bool ThisCall(decltype(&RealDamageActor) fn,Actor*a,float d,Actor*h,bool k){return fn(a,d,h,k);}};
#define TP_MAKE_THISCALL(name,obj,...) name(obj* apThis,__VA_ARGS__)
'''+hook+'''
int main(){
 for (int target=0;target<4;++target) for(int hitter=-1;hitter<4;++hitter)
 for(bool protects: {false,true}) for(float damage: {4.f,12.f}) {
 Actor t{1,{target},10,protects}, h{2,{hitter},30,false};
 auto& changes=World::Get().rs.hp;changes.clear();
 bool result=HookDamageActor(&t,damage,hitter<0?nullptr:&h,false);
 if (target==0 && hitter!=3) {
   assert(changes.size()==1&&changes[0].SampleOwnerHealth&&changes[0].DeltaHealth==0);
   assert(t.hp==(protects&&damage>=10?1.f:10.f-damage));assert(result==(t.hp<=0));
 }else if(target==2 && hitter==3){assert(changes.empty()&&!result&&t.hp==10);}
 else if(target==2){assert(changes.size()==1&&!changes[0].SampleOwnerHealth&&changes[0].DeltaHealth==-damage);}
 else if(target==1 && hitter==2){
   assert(changes.size()==1&&!changes[0].SampleOwnerHealth);
   assert(changes[0].KeepAlive==(protects||damage<10));
   assert(changes[0].DeltaHealth==(protects&&damage>=10?-9.f:-damage));
   assert(result==(t.hp<=0));
 }
 else {assert(changes.empty()&&t.hp==10&&result==(target==3 && damage>=10));}
 }
 for (bool targetProjected: {false,true}) for(bool hitterProjected: {false,true}) {
   if (!targetProjected && !hitterProjected) continue;
   Actor t{1,{0},10,false}, h{2,{2},30,false};
   t.projected=targetProjected;h.projected=hitterProjected;
   auto& changes=World::Get().rs.hp;changes.clear();
   assert(!HookDamageActor(&t,12.f,&h,false)&&t.hp==10&&changes.empty());
 }
 Actor projected{1,{1},10,false};projected.projected=true;
 auto& changes=World::Get().rs.hp;changes.clear();
 assert(!HookDamageActor(&projected,12.f,nullptr,false)&&projected.hp==10&&changes.empty());
}''','80 ordinary hit cases plus 4 projected actor cases; original hook body with simulated engine')
rank=body('client/Games/Skyrim/Actor.cpp','void Actor::SetFactions(','void Actor::SetFactionRank')
check('''
#include <cstdint>
#include <map>
#include <vector>
#include <cassert>
using GameId=int;
template<class K,class V> using Map=std::map<K,V>;
struct Faction{int Id;int8_t Rank;}; struct Factions{std::vector<Faction> NpcFactions, ExtraFactions;};
struct TESFaction{int id;};template<class T>T* Cast(T*p){return p;}
struct World{static World&Get(){static World w;return w;}World&GetModSystem(){return *this;}World&GetPartyService(){return *this;}World&GetTransport(){return *this;}bool party=true,online=true;bool IsInParty(){return party;}bool IsOnline(){return online;}int GetGameId(int id){return id;}};
struct Actor{Factions factions;std::vector<Faction> writes; Factions GetFactions(){return factions;}
 TESFaction* GetById(int id){static TESFaction f;f.id=id;return &f;}void SetFactionRank(TESFaction*f,int8_t rank){writes.push_back({f->id,rank});factions.ExtraFactions.push_back({f->id,rank});}
 void SetFactions(const Factions&) noexcept;};
'''+rank+'''
int main(){Actor a;
Factions personal;personal.ExtraFactions={{0x5C84E,0}};a.SetFactions(personal);assert(a.writes.empty());
World::Get().party=false;a.SetFactions(personal);assert(a.writes.empty());
World::Get().online=false;a.SetFactions(personal);assert(a.writes.size()==1&&a.writes.back().Id==0x5C84E);
World::Get().online=true;World::Get().party=true;a.writes.clear();a.factions.ExtraFactions.clear();
a.factions.NpcFactions={{1,0},{2,-1},{3,1}};
Factions incoming{{{1,0},{2,-1},{3,1}},{{2,0},{3,-1},{4,5}}};
a.SetFactions(incoming);assert(a.writes.size()==3);
a.writes.clear();a.SetFactions(incoming);assert(a.writes.empty());
Factions leave{{{1,0},{2,-1},{3,1}},{}};
a.SetFactions(leave);assert(a.writes.size()==3);
a.writes.clear();a.SetFactions(leave);assert(a.writes.empty());}''','join, dismiss, per-reference override precedence and idempotent rank reapplication')
template='''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>
#include <stdexcept>
#include <functional>
namespace TiltedPhoques {
template<class T>using Vector=std::vector<T>;
template<class K,class V>using Map=std::map<K,V>;
struct Buffer {std::vector<bool> bits;
struct Writer {Buffer&b; void WriteBits(uint64_t v,unsigned n){for(unsigned i=0;i<n;++i)b.bits.push_back((v>>i)&1);}};
struct Reader {Buffer&b;size_t at=0;void ReadBits(uint64_t&v,unsigned n){v=0; for(unsigned i=0;i<n;++i)v|=uint64_t(b.bits.at(at++))<<i;}};
};
struct Serialization{static void WriteVarInt(Buffer::Writer&w,uint64_t v){while(v>=128){w.WriteBits((v&127)|128,8);v>>=7;}w.WriteBits(v,8);}
static uint64_t ReadVarInt(Buffer::Reader&r){uint64_t n=0,x;unsigned sh=0;do{r.ReadBits(x,8);n|=(x&127)<<sh;sh+=7;}while(x&128);return n;}};
} using TiltedPhoques::Serialization;
using ClientOpcode=int;using ServerOpcode=int;enum {kRequestFactionsChanges,kNotifyFactionsChanges};
struct ClientMessage {int op;ClientMessage(int n):op(n){}virtual ~ClientMessage()=default;
int GetOpcode()const{return op;}virtual void SerializeRaw(TiltedPhoques::Buffer::Writer&)const noexcept {}
virtual void DeserializeRaw(TiltedPhoques::Buffer::Reader&)noexcept {}};
using ServerMessage=ClientMessage;
'''
def stripped(file):
 return '\n'.join(l for l in (root/'encoding'/file).read_text().splitlines() if not l.startswith(('#include','#pragma')))
for path in ['Structs/GameId','Structs/Faction','Structs/Factions','Messages/RequestFactionsChanges','Messages/NotifyFactionsChanges']:
 # Each translation unit in the project uses internal linkage for its private constants.
 template += '\n'+stripped(path+'.h')+'\n'+stripped(path+'.cpp')+'\n'
check(template+'''
template<typename Msg> void roundtrip(size_t n) {
 Msg sent{},got{};
 for (size_t i=0;i<n;++i){auto &entry=sent.Changes[static_cast<uint32_t>(i)];entry.OwnershipEpoch=i+1;Faction f{};
 f.Id=GameId(8,i+700);f.Rank=2;entry.Content.NpcFactions.push_back(f);f.Rank=-1;entry.Content.ExtraFactions.push_back(f);}
 TiltedPhoques::Buffer buf; TiltedPhoques::Buffer::Writer w{buf};sent.SerializeRaw(w);w.WriteBits(0xAD,8);
 TiltedPhoques::Buffer::Reader r{buf};got.DeserializeRaw(r);uint64_t end=0;r.ReadBits(end,8);assert(got==sent&&end==0xAD&&r.at==buf.bits.size());
} int main(){roundtrip<RequestFactionsChanges>(0);roundtrip<RequestFactionsChanges>(1);roundtrip<RequestFactionsChanges>(255);
roundtrip<NotifyFactionsChanges>(0);roundtrip<NotifyFactionsChanges>(1);roundtrip<NotifyFactionsChanges>(1023);}''','6 real faction codec round trips using an isolated bit buffer')
