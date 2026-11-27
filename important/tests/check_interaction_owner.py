"""Exercise the production character owner policy for leader, challenger and follower-owner roles."""
from pathlib import Path
import subprocess, sys, tempfile
root=Path(sys.argv[1],"Code/server")
source=(root/"Services/CharacterService.cpp").read_text()
start=source.index("bool CharacterService::CanClaimOwnership")
end=source.index("void CharacterService::AssignInteractionOwner",start)
rule=source[start:end]
mocks=r"""
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>
#include <type_traits>
namespace entt{using entity=unsigned;}
namespace spdlog {template<class...T>void debug(T&&...){}}
struct GameId{unsigned n{};auto LogFormat()const{return uint64_t(n);}};
struct FormIdComponent{GameId Id{81};};
struct CellIdComponent{bool near=true;bool IsInRange(const CellIdComponent&,bool)const{return near;}};
struct Player{unsigned id{};CellIdComponent cell;auto& GetCellComponent(){return cell;}auto GetId(){return id;}};
struct OwnerComponent{Player* old{};unsigned OwnershipEpoch{6};auto* GetOwner()const{return old;}};
struct CharacterComponent{bool mount{},player{};bool IsMount()const{return mount;}bool IsPlayer()const{return player;}bool IsDragon()const{return false;}};
struct PartyService{struct Party {struct Lease{unsigned Owner;bool Confirmed;std::chrono::steady_clock::time_point Expires;bool Detached{};};
 struct Fight{struct {GameId TargetForm;}Anchor;std::chrono::steady_clock::time_point Expires;bool Detached{};};
 std::map<uint64_t,Lease> Followers;std::map<unsigned,Fight>Brawls;std::vector<Player*>Members;};
 Party party;auto& Followers(){return party.Followers;}bool IsPlayerInParty(Player*p){return std::find(party.Members.begin(),party.Members.end(),p)!=party.Members.end();}
 bool IsPlayerLeader(Player*p){return p->id==0;}Party* GetPlayerParty(Player*){return &party;}};
struct Registry{OwnerComponent o;CharacterComponent c;CellIdComponent loc;FormIdComponent f;PartyService ps;
 template<class...Ts> auto& view(){return *this;}const unsigned*find(unsigned)const{return &token;}const unsigned*end()const{return nullptr;}unsigned token{};
 template<class T>const T& get(unsigned)const{if constexpr(std::is_same_v<T,OwnerComponent>)return o;else if constexpr(std::is_same_v<T,CellIdComponent>)return loc;else if constexpr(std::is_same_v<T,FormIdComponent>)return f;else return c;}
 PartyService& GetPartyService(){return ps;}};
struct World{static unsigned ToInteger(unsigned n){return n;}};
struct CharacterService{Registry&m_world;enum class OwnershipTransferReason{LeaderAssignment,LeaderClaim,PartyInteraction};
 static const char*GetOwnershipTransferReasonName(OwnershipTransferReason){return "";}
 bool CanClaimOwnership(Player*,entt::entity,uint32_t,OwnershipTransferReason)const noexcept;};
"""
checks=r"""
int main(){Registry registry;Player leader{0},guest{2},stranger{3};registry.ps.party.Members={&leader,&guest};
 auto& party=registry.ps.party;CharacterService s{registry};using Why=CharacterService::OwnershipTransferReason;
 auto can=[&](Player* p,Why why,unsigned epoch=6){return s.CanClaimOwnership(p,42,epoch,why);};
 registry.o.old=&guest;assert(can(&leader,Why::LeaderClaim));assert(!can(&leader,Why::PartyInteraction));
 registry.o.old=&leader;assert(!can(&guest,Why::LeaderClaim));
 party.Followers[81]={2,true,{}};assert(can(&guest,Why::PartyInteraction));assert(!can(&guest,Why::PartyInteraction,5));
 guest.cell.near=false;assert(!can(&guest,Why::PartyInteraction));guest.cell.near=true;
 registry.o.old=&guest;assert(!can(&leader,Why::LeaderClaim));
 party.Followers.clear();assert(can(&leader,Why::LeaderClaim));
 party.Brawls[2]={{GameId{81}},std::chrono::steady_clock::now()+std::chrono::minutes(1)};
 assert(!can(&leader,Why::LeaderClaim));registry.o.old=&leader;assert(can(&guest,Why::PartyInteraction));
 registry.c.player=true;assert(!can(&guest,Why::PartyInteraction));registry.c.player=false;
 registry.o.old=&stranger;assert(!can(&guest,Why::PartyInteraction));
 party.Followers[81]={2,true,{}};party.Members.clear();assert(can(&guest,Why::PartyInteraction));assert(!can(&leader,Why::LeaderClaim));
 party.Followers.clear();party.Members={&leader,&guest};
 registry.o.old=&leader;party.Brawls[2].Expires={};assert(!can(&guest,Why::PartyInteraction));
}
"""
with tempfile.TemporaryDirectory() as temp:
    binary=Path(temp,"owner")
    subprocess.run(["g++","-std=c++20","-x","c++","-","-o",binary],input=mocks+rule+checks,text=True,check=True)
    subprocess.run([binary],check=True)
print("PASS production ownership: leader default, follower owner, challenger, expiry, scope, player exclusion and stale epochs")
