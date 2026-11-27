"""Execute the production party interaction handler with two NPC ids and three players."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(sys.argv[1], "Code")
overlay = (root / "server/Services/OverlayService.cpp").read_text()
talk_handler = overlay[overlay.index("void OverlayService::OnPlayerDialogue("):overlay.index("void OverlayService::OnTeleport(")]
enum_source = (root / "encoding/Structs/CoopInteraction.h").read_text()
enum_source = enum_source[enum_source.index("enum class Interaction"):enum_source.index("struct CoopInteraction")]
party = (root / "server/Services/PartyService.h").read_text()
records = party[party.index("        struct Conversation"):party.index("        Map<GameId, QuestRun>")]
service = (root / "server/Services/CoopInteraction.cpp").read_text().split("namespace\n", 1)[1]
harness = r"""
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <optional>
#include <string>
#include <vector>
namespace glm { struct vec3 {float x{},y{},z{};}; float distance(vec3 a,vec3 b){return std::hypot(a.x-b.x,a.y-b.y,a.z-b.z);} }
struct GameId {uint32_t id{};uint64_t LogFormat()const{return id;} explicit operator bool()const{return id!=0;} bool operator==(const GameId&)const=default;};
struct DialogueAnchor {bool GameInitiated{};unsigned Session{},TargetEntity{},Choice{};GameId TargetForm{},Cell{},WorldSpace{},TopicInfo{},TopicQuest{};glm::vec3 Position{};};
"""
mocks = r"""
struct CoopInteraction {Interaction Kind{};unsigned Speaker{},Session{},Value{};GameId Npc{};};
struct PlayerDialogueRequest{DialogueAnchor Anchor;std::string Text,NpcLine;};struct NotifyPlayerDialogue:PlayerDialogueRequest{unsigned SpeakerId{};std::string Name;};
struct RequestCoopInteraction{CoopInteraction Event;};struct NotifyCoopInteraction{CoopInteraction Event;};
struct MovementComponent{glm::vec3 Position;};
struct Cell{GameId Cell{},WorldSpaceId{};};
struct Player {unsigned id; struct Membership{std::optional<unsigned> JoinedPartyId{1};} membership;Cell cell{{1},{}};
 std::vector<CoopInteraction> messages;auto GetCharacter(){return std::optional<unsigned>(id);}
 std::string GetUsername(){return "actor";}auto& GetParty(){return membership;}unsigned GetId(){return id;}auto& GetCellComponent(){return cell;}
 void Send(const NotifyCoopInteraction& e){messages.push_back(e.Event);}
};
namespace CampaignAccess { uint64_t Character(const Player& p){return p.id+100;} }
template<class T>struct PacketEvent{T Packet;Player* pPlayer;};
struct World; struct PartyService {static bool WithinSyncRange(World&,Player&,Player&) noexcept;struct Party{unsigned LeaderPlayerId{};RECORDS std::vector<Player*> Members; } one;
 std::map<uint64_t,Party::Follower> followers;std::map<uint32_t,Party::Conversation> hireTalks;
 auto& Followers(){return followers;}auto& HireTalks(){return hireTalks;}
 Party* GetPlayerParty(Player* p){return p->GetParty().JoinedPartyId==1?&one:nullptr;}};
struct World {std::map<unsigned,MovementComponent> pos;PartyService parties;std::map<unsigned,Player*> players;
 unsigned ownerCalls{};World& GetCharacterService(){return *this;}void AssignInteractionOwner(Player*, GameId){++ownerCalls;}
 auto& GetPartyService(){return parties;}auto& GetPlayerManager(){return *this;}
 Player* GetById(unsigned id){return players.count(id)?players[id]:nullptr;}
 template<class F>void ForEach(F f){for(auto [id,p]:players)f(p);}
 template<class T>T* try_get(unsigned id){return pos.count(id)?&pos[id]:nullptr;}};
RANGECHECK
struct GameServer{static GameServer* Get(){static GameServer g;return &g;}void SendToParty(const NotifyPlayerDialogue&,Player::Membership&,Player*){}};
struct OverlayService {World& m_world;void OnPlayerDialogue(const PacketEvent<PlayerDialogueRequest>&)const noexcept;void OnInteraction(const PacketEvent<RequestCoopInteraction>& e)noexcept;};
"""
cases = r"""
int main(){
 World w;Player host{0},guest{2},outsider{3};outsider.membership.JoinedPartyId=2;
 w.players={{0,&host},{2,&guest},{3,&outsider}};w.pos={{0,{}},{2,{}},{3,{}}};auto& p=w.parties.one;p.Members={&host,&guest};
 assert(PartyService::WithinSyncRange(w,host,guest));w.pos[2].Position.x=1200;
 assert(PartyService::WithinSyncRange(w,host,guest));w.pos[2].Position.x=1200.1f;
 assert(!PartyService::WithinSyncRange(w,host,guest));w.pos[2].Position.x=0;
 guest.cell.Cell={99};assert(!PartyService::WithinSyncRange(w,host,guest));
 guest.cell.Cell={1};host.cell.WorldSpaceId={13};guest.cell.WorldSpaceId={14};
 assert(!PartyService::WithinSyncRange(w,host,guest));guest.cell.WorldSpaceId={13};guest.cell.Cell={99};
 assert(PartyService::WithinSyncRange(w,host,guest));host.cell.WorldSpaceId=guest.cell.WorldSpaceId={};guest.cell.Cell={1};
 OverlayService s{w};
 auto act=[&](Player& player,Interaction k,unsigned who,unsigned session,unsigned npc,unsigned v=0){
  s.OnInteraction({{{k,who,session,v,GameId{npc}}},&player});};
 auto say=[&](unsigned session,unsigned npc,unsigned choice,unsigned quest,bool open,bool forced){
   PlayerDialogueRequest m{};m.Anchor.Session=session;m.Anchor.Choice=choice;m.Anchor.TopicQuest={quest};m.Anchor.TopicInfo={choice?391u:0u};
   m.Anchor.GameInitiated=forced;m.Anchor.TargetForm={npc};m.Anchor.Cell={open?1u:0u};s.OnPlayerDialogue({m,&host});};
 say(1,81,0,0,true,false);say(1,81,1,700,true,false);auto& first=p.Talks.at(0);
 assert(first.ChoiceListeners.empty());act(guest,Interaction::Join,0,1,81);
 say(1,81,1,700,true,false);assert(first.ChoiceListeners.empty());
 say(1,81,2,701,true,false);auto now=std::chrono::steady_clock::now();
 assert(first.SharesQuest(GameId{701},now)&&!first.SharesQuest(GameId{700},now));auto deadline=first.ChoiceExpires;
 say(1,81,2,701,true,false);assert(first.ChoiceExpires==deadline);
 say(1,81,3,702,false,false);assert(first.Closed&&first.SharesQuest(GameId{702},now));
 act(host,Interaction::Brawl,0,1,81);act(host,Interaction::OpponentState,0,1,81,1);
 say(2,81,0,0,true,false);assert(p.Talks.at(0).Listeners.empty());
 say(3,82,0,0,true,true);assert(p.Talks.at(0).Listeners.empty());
 say(4,81,0,0,true,true);assert(p.Talks.at(0).Listeners.count(2)&&p.Brawls.empty());
 w.pos[2].Position.x=1190;p.Talks[0].Listeners.clear();
 act(host,Interaction::FavorResult,0,4,81,3);assert(guest.messages.back().Kind==Interaction::FavorResult);
 auto n=guest.messages.size();w.pos[2].Position.x=1201;act(host,Interaction::FavorResult,0,4,81,3);
 assert(guest.messages.size()==n);w.pos[2].Position.x=0;
 p.Talks.clear();p.Brawls.clear();host.messages.clear();guest.messages.clear();
 for(unsigned npc:{88u,39003u}){
  auto& talk=p.Talks[0];talk={};talk.Anchor.Session=11;talk.Anchor.TargetForm={npc};talk.Anchor.Cell={1};
  talk.Expires=std::chrono::steady_clock::now()+std::chrono::seconds(8);
  act(outsider,Interaction::Join,0,11,npc);act(guest,Interaction::Join,0,10,npc);assert(talk.Listeners.empty());
  w.pos[2].Position.x=600;act(guest,Interaction::Join,0,11,npc);assert(talk.Listeners.empty());w.pos[2].Position.x=0;
  act(guest,Interaction::Join,0,11,npc);assert(talk.Listeners.count(2));assert(guest.messages.back().Kind==Interaction::Joined);
  assert(host.messages.back().Kind==Interaction::WatchOutcomes);
  talk.Anchor.Choice=1;talk.Anchor.TopicInfo={30};talk.Anchor.TopicQuest={180};talk.ChoiceListeners=talk.Listeners;
  auto now=std::chrono::steady_clock::now();talk.ChoiceExpires=now+std::chrono::seconds(8);
  assert(talk.SharesQuest(GameId{180},now)&&!talk.SharesQuest(GameId{2},now)&&!talk.SharesQuest(GameId{180},talk.ChoiceExpires));
  act(guest,Interaction::Brawl,0,11,npc);assert(p.Brawls.empty());
  act(host,Interaction::Brawl,0,11,npc);assert(p.Brawls.at(0).Listeners.count(2)&&w.ownerCalls);
  act(host,Interaction::OpponentState,0,11,npc,1);assert(guest.messages.back().Value==1);
  auto activeFight=p.Brawls.at(0);
  act(host,Interaction::OpponentState,0,11,npc,2);assert(guest.messages.back().Value==2);
  auto stopped=guest.messages.size();act(host,Interaction::Brawl,0,11,npc);
  act(host,Interaction::OpponentState,0,11,npc,3);act(host,Interaction::OpponentState,0,11,npc,1);
  assert(guest.messages.size()==stopped && p.Brawls.at(0).OpponentFinished);
  p.Brawls.at(0)=activeFight; // independent hire/leave case below
  auto count=guest.messages.size();act(host,Interaction::Outcome,0,11,npc+1,4);assert(guest.messages.size()==count);
  act(host,Interaction::Outcome,0,11,npc,1);assert(guest.messages.back().Kind==Interaction::Outcome);
  act(guest,Interaction::Leave,0,11,npc);act(host,Interaction::Outcome,0,11,npc,2);
  assert(!p.Brawls.at(0).Listeners.count(2)&&guest.messages.back().Value==1);
  w.parties.HireTalks().clear();w.parties.HireTalks()[0]=talk;
  act(guest,Interaction::Hire,0,8,npc,11);assert(guest.messages.back().Kind==Interaction::Denied);
  act(host,Interaction::PrepareHire,0,80,npc,11);assert(host.messages.back().Kind==Interaction::HireReady);
  assert(w.parties.Followers().at(npc).Prepared);auto guestNotices=guest.messages.size();
  act(guest,Interaction::FollowerReport,0,0,0);assert(guest.messages.size()==guestNotices);
  act(host,Interaction::CommitHire,0,81,npc,11);assert(w.parties.Followers().at(npc).Prepared);
  act(host,Interaction::CommitHire,0,80,npc,11);assert(!w.parties.Followers().at(npc).Prepared);
  assert(guest.messages.back().Kind==Interaction::FollowerReserved);act(host,Interaction::CancelHire,0,80,npc);
  p.Talks[2]=talk;w.parties.HireTalks()[2]=talk;act(host,Interaction::PrepareHire,0,89,npc,11);assert(w.parties.Followers().empty());
  act(host,Interaction::Hire,0,7,npc,10);assert(host.messages.back().Kind==Interaction::Denied);act(host,Interaction::Hire,0,7,npc,11);assert(host.messages.back().Kind==Interaction::Granted);
  act(guest,Interaction::Hire,0,8,npc,11);assert(guest.messages.back().Kind==Interaction::Denied);
  act(host,Interaction::Hire,0,7,npc,11);assert(host.messages.back().Kind==Interaction::Granted);
  act(host,Interaction::CancelHire,0,70,npc);assert(w.parties.Followers().size()==1);
  act(host,Interaction::CancelHire,0,7,npc);assert(w.parties.Followers().empty());
  act(host,Interaction::Hire,0,9,npc,11);assert(host.messages.back().Kind==Interaction::Granted);
  act(host,Interaction::FollowerReport,0,0,0);assert(w.parties.Followers().size()==1);
  act(host,Interaction::FollowerReport,0,0,npc);assert(w.parties.Followers().at(npc).Confirmed);
  act(host,Interaction::FollowerReport,0,0,0);assert(guest.messages.back().Kind==Interaction::FollowerReleased);
  act(guest,Interaction::Hire,0,9,npc,11);assert(guest.messages.back().Kind==Interaction::Granted);
  act(guest,Interaction::FollowerReport,0,0,npc);act(guest,Interaction::FollowerReport,0,0,0);
  assert(w.parties.Followers().empty());p.Talks.clear();p.Brawls.clear();
 }
 // No group: report an already hired follower, deliver snapshot to joining peer,
 // deny a competing hire and leave the lease intact across membership changes.
 host.membership.JoinedPartyId.reset();guest.membership.JoinedPartyId.reset();outsider.membership.JoinedPartyId.reset();
 act(host,Interaction::FollowerReport,0,0,81);assert(w.parties.Followers().at(81).Owner==0);
 outsider.messages.clear();act(outsider,Interaction::FollowerReport,0,0,0);
 assert(outsider.messages.back().Kind==Interaction::FollowerReserved);
 PlayerDialogueRequest request{};request.Anchor.Session=99;request.Anchor.Cell={1};request.Anchor.TargetForm={81};
 s.OnPlayerDialogue({request,&guest});act(guest,Interaction::Hire,0,1,81,99);
 assert(guest.messages.back().Kind==Interaction::Denied);
 host.membership.JoinedPartyId=1;host.membership.JoinedPartyId.reset();
 act(host,Interaction::FollowerReport,0,0,81);assert(w.parties.Followers().at(81).Confirmed);
 act(host,Interaction::FollowerReport,0,0,0);assert(w.parties.Followers().empty());
 w.pos[2].Position.x=500;act(guest,Interaction::Hire,0,2,81,99);assert(guest.messages.back().Kind==Interaction::Denied);
 w.pos[2].Position.x=0;act(guest,Interaction::Hire,0,3,81,99);assert(guest.messages.back().Kind==Interaction::Granted);
 act(guest,Interaction::FollowerReport,0,0,81);assert(w.parties.Followers().at(81).Owner==2);
 auto& saved=w.parties.Followers().at(81);saved.Detached=true;saved.Owner=host.id;
 act(host,Interaction::FollowerReport,0,0,81);assert(saved.Detached);
 act(guest,Interaction::FollowerReport,0,0,0);assert(saved.Detached);
 act(guest,Interaction::FollowerReport,0,0,81);assert(!saved.Detached && saved.Owner==guest.id);
 act(guest,Interaction::FollowerReport,0,0,0);assert(!w.parties.Followers().contains(81));
}
"""
range_file = (root / "server/Services/PartyService.cpp").read_text()
range_check = range_file[range_file.index("bool PartyService::WithinSyncRange"):range_file.index("void PartyService::OnUpdate")]
with tempfile.TemporaryDirectory() as temp:
    binary = Path(temp, "interaction")
    subprocess.run(["g++", "-std=c++20", "-x", "c++", "-", "-o", binary],
                   input=harness + enum_source + mocks.replace("RECORDS", records).replace("RANGECHECK",range_check) + "namespace\n" + service + talk_handler + cases,
                   text=True, check=True)
    subprocess.run([binary], check=True)
print("PASS actual server handlers: arbitrary NPCs, opt-in, distance, role, leave, scoped brawl, hire race, release and transfer")

