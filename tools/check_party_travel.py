"""Execute the two production carriage adapters with simulated Skyrim, transport and time."""
import subprocess, sys, tempfile
from pathlib import Path

root = Path(sys.argv[1], 'Code')
shared = r"""
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace glm {
struct vec3 { float x{}, y{}, z{}; };
vec3 operator-(vec3 a, vec3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
float dot(vec3 a,vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
float distance(vec3 a,vec3 b){return std::sqrt(dot(a-b,a-b));}
}
using GameId=unsigned;
struct UpdateEvent{};
enum TravelAction:unsigned{BoardCarriage,ArriveByCarriage,AcceptCarriage,DeclineCarriage,CancelCarriage,OpenTravelMap,ArriveByMarker};
enum TravelNotice:unsigned{InvitePassenger,CancelPassenger,TransportPassenger,InviteMarkerPassenger};
struct NotifyPartyTravel{unsigned Rider{},Trip{},Kind{},Cell{},World{};glm::vec3 Position{};std::string Name;};
struct PartyTravelRequest{unsigned Action{},Rider{},Trip{},Cell{},World{};glm::vec3 Position{};};
"""
server = r"""
struct Party{std::optional<unsigned> JoinedPartyId;};
struct Cell{unsigned WorldSpaceId=1;};
struct Player{
 unsigned id;Party party{{5}};Cell cell;std::vector<NotifyPartyTravel> events;
 auto& GetParty(){return party;} unsigned GetId(){return id;}
 auto GetCharacter(){return std::optional<unsigned>(id);}
 auto& GetCellComponent(){return cell;}std::string GetUsername(){return "Rider";}
 void Send(const NotifyPartyTravel& n){events.push_back(n);}
};
struct MovementComponent{glm::vec3 Position;};
struct Players:std::vector<Player*>{
 Player* GetById(unsigned id){for(auto *p:*this)if(p->id==id)return p;return nullptr;}
};
struct World{Players players;std::map<unsigned,MovementComponent> movements;
 auto& GetPlayerManager(){return players;}
 template<class T>T* try_get(unsigned id){return movements.count(id)?&movements.at(id):nullptr;}
};
template<class T>struct PacketEvent{Player*pPlayer;T Packet;};
struct OverlayService{World& m_world;
"""
header = (root/'server/Services/OverlayService.h').read_text()
server += header[header.index('    struct Ride'):header.index('    entt::scoped_connection m_rideConnection')]
server += r"""
};
int main();
"""
server_cases = r"""
int main(){
 Player rider{1}, yes{2}, no{3}, far{4}, stranger{5}, otherWorld{6};
 stranger.party.JoinedPartyId=27;otherWorld.cell.WorldSpaceId=2;
 World w;w.players={};for(auto* p:{&rider,&yes,&no,&far,&stranger,&otherWorld}){w.players.push_back(p);w.movements[p->id]={};}
 w.movements[2].Position.x=1200;w.movements[4].Position.x=1201;
 OverlayService service{w};auto act=[&](Player& p,unsigned action,unsigned n,unsigned pos=0,unsigned world=1){
   service.OnPartyTravel({&p,{action,1,n,14,world,{float(pos),256,0}}});};
 act(rider,BoardCarriage,1);act(rider,BoardCarriage,1);
 assert(yes.events.size()==1&&no.events.size()==1&&far.events.empty()&&stranger.events.empty()&&otherWorld.events.empty());
 act(no,DeclineCarriage,1);act(no,AcceptCarriage,1);act(yes,AcceptCarriage,1);act(yes,AcceptCarriage,1);
 act(far,AcceptCarriage,1);act(yes,ArriveByCarriage,1,10000);service.UpdateRides({});
 act(rider,ArriveByCarriage,1,500);act(rider,ArriveByCarriage,1,10000,2);service.UpdateRides({});
 assert(yes.events.size()==1);
 act(rider,ArriveByCarriage,1,10000);act(rider,ArriveByCarriage,1,30000);service.UpdateRides({});service.UpdateRides({});
 assert(yes.events.size()==2&&yes.events.back().Kind==TransportPassenger&&yes.events.back().Position.x==10000&&no.events.size()==1);
 act(rider,BoardCarriage,2);act(rider,ArriveByCarriage,2,11000);act(yes,AcceptCarriage,1);service.UpdateRides({});
 assert(yes.events.back().Kind==InvitePassenger);
 act(yes,AcceptCarriage,2);yes.party.JoinedPartyId.reset();service.UpdateRides({});
 assert(yes.events.back().Kind==CancelPassenger);
 act(no,AcceptCarriage,2);service.UpdateRides({});assert(no.events.back().Position.x==11000);
 yes.party.JoinedPartyId=5;act(rider,BoardCarriage,3);act(rider,CancelCarriage,3);service.UpdateRides({});
 assert(yes.events.back().Kind==CancelPassenger);
 act(rider,BoardCarriage,4);w.players.erase(w.players.begin());service.UpdateRides({});
 assert(no.events.back().Kind==CancelPassenger&&service.m_rides.empty());
 auto before=yes.events.size();w.players.push_back(&rider);
 act(rider,OpenTravelMap,10);act(rider,OpenTravelMap,10);service.UpdateRides({});assert(yes.events.size()==before);
 act(rider,CancelCarriage,10);service.UpdateRides({});assert(yes.events.size()==before);
 act(rider,OpenTravelMap,11);act(yes,AcceptCarriage,11);act(rider,ArriveByCarriage,11,12000);service.UpdateRides({});assert(yes.events.size()==before);
 act(rider,ArriveByMarker,11,300);service.UpdateRides({});assert(yes.events.size()==before+1 && yes.events.back().Kind==InviteMarkerPassenger);
 act(yes,AcceptCarriage,11);act(no,DeclineCarriage,11);service.UpdateRides({});assert(yes.events.back().Kind==TransportPassenger);
 act(rider,BoardCarriage,5);service.m_rides.at(1).expires={};
 act(yes,AcceptCarriage,5);service.UpdateRides({});assert(yes.events.back().Kind==CancelPassenger&&service.m_rides.empty());
}
"""
client = r"""
struct TESForm{unsigned formID{};static std::map<unsigned,TESForm*> forms;
 static TESForm* GetById(unsigned id){return forms[id];}virtual ~TESForm()=default;};
std::map<unsigned,TESForm*> TESForm::forms;
template<class T>T* Cast(TESForm*p){return dynamic_cast<T*>(p);}
struct TESObjectREFR:TESForm{glm::vec3 position{};TESForm* space{};auto GetWorldSpace(){return space;}};
struct TESQuest:TESForm{TESObjectREFR* alias{};auto GetAliasedRef(unsigned i){assert(i==14);return alias;}};
struct PlayerCharacter:TESObjectREFR{TESForm* parentCell{};bool combat{};
 static PlayerCharacter*Get(){static PlayerCharacter p;return &p;} auto GetParentCellEx(){return parentCell;}
 bool IsInCombat(){return combat;}};
using BSFixedString=std::string;struct UI{bool blocked{},open{},loading{};bool GetMenuOpen(const BSFixedString& n){return n=="MapMenu"?open:loading;}static UI*Get(){static UI ui;return &ui;}bool BlocksPartyHud(){return blocked;}};
struct ActivateEvent{TESObjectREFR*pObject,*pActivator;};
struct Party{bool joined=true;bool IsInParty(){return joined;}};
struct Mods{bool GetServerModId(unsigned id,unsigned&out){out=id;return id!=0;}};
struct World{Party p;Mods m;auto& GetPartyService(){return p;}auto& GetModSystem(){return m;}};
struct Transport{bool connected=true;std::vector<PartyTravelRequest> sent;
 bool IsConnected(){return connected;}unsigned GetLocalPlayerId(){return 7;}void Send(PartyTravelRequest p){sent.push_back(p);}};
struct NotifyTeleport{unsigned CellId{},WorldSpaceId{};glm::vec3 Position{};};
struct CefListValue{static auto Create(){return std::make_shared<CefListValue>();}
 void SetInt(int,unsigned){}void SetString(int,const char*){}};
struct OverlayClient{bool active{};void SetUIVisible(bool flag){active=flag;}};
struct Overlay{std::vector<std::string> events;template<class... T>void ExecuteAsync(std::string e,T...){events.push_back(e);}};
struct OverlayService{World m_world;Transport m_transport;Overlay ui;OverlayClient client;
 bool m_showingProgress{};Overlay* m_pOverlay=&ui;
 bool GetInGame(){return true;}bool GetActive(){return client.active;}
 std::vector<NotifyTeleport> jumps;void OnNotifyTeleport(NotifyTeleport p){jumps.push_back(p);}
 void AnswerCarriage(unsigned,unsigned,bool)noexcept;void OnBoardCarriage(const ActivateEvent&)noexcept;
 void OnTravelNotice(const NotifyPartyTravel&)noexcept;void UpdateCarriage()noexcept;void ClearCarriage()noexcept;
 void CloseTravelPrompt()noexcept;void UpdateMapTravel()noexcept;void ConfirmMarkerTravel()noexcept;
"""
header = (root/'client/Services/OverlayService.h').read_text()
client += header[header.index('    uint32_t m_tripCounter'):header.index('    entt::scoped_connection m_boardConnection')]
client += '};\n'
client_cases = r"""
int main(){
 OverlayService s;s.m_gameOverlayClient=&s.client;
 auto* player=PlayerCharacter::Get();TESForm cell,world;cell.formID=12;world.formID=1;
 player->parentCell=&cell;player->space=&world;
 TESObjectREFR tavern,seat,marker;seat.formID=0x1070F0;marker.space=&world;marker.position.x=15000;
 TESForm::forms[0x108D8D]=&marker;
 s.OnBoardCarriage({&tavern,player});s.OnBoardCarriage({&seat,&tavern});assert(s.m_transport.sent.empty());
 s.OnBoardCarriage({&seat,player});s.OnBoardCarriage({&seat,player});assert(s.m_transport.sent.size()==1);
 s.UpdateCarriage();assert(s.m_transport.sent.size()==1);
 player->position.x=9000;s.UpdateCarriage();assert(s.m_transport.sent.size()==1);
 player->position=marker.position;s.UpdateCarriage();s.UpdateCarriage();
 assert(s.m_transport.sent.size()==2&&s.m_transport.sent.back().Action==ArriveByCarriage&&s.m_transport.sent.back().Cell==12);
 NotifyPartyTravel notice;notice.Rider=9;notice.Trip=20;notice.Name="Other";s.OnTravelNotice(notice);
 UI::Get()->blocked=true;s.UpdateCarriage();assert(!s.GetActive());UI::Get()->blocked=false;
 s.UpdateCarriage();s.UpdateCarriage();assert(s.GetActive()&&s.ui.events.size()==1);
 s.AnswerCarriage(9,99,true);assert(s.GetActive());s.AnswerCarriage(9,20,false);assert(!s.GetActive());
 notice.Kind=TransportPassenger;notice.Cell=12;notice.World=1;s.OnTravelNotice(notice);s.UpdateCarriage();assert(s.jumps.empty());
 notice.Kind=InvitePassenger;notice.Trip++;s.OnTravelNotice(notice);s.UpdateCarriage();s.AnswerCarriage(9,21,true);
 s.AnswerCarriage(9,21,true);notice.Kind=TransportPassenger;s.OnTravelNotice(notice);
 player->combat=true;s.UpdateCarriage();assert(s.jumps.empty());player->combat=false;s.UpdateCarriage();
 s.OnTravelNotice(notice);s.UpdateCarriage();assert(s.jumps.size()==1&&s.jumps[0].CellId==12);
 notice.Kind=InvitePassenger;notice.Trip++;s.OnTravelNotice(notice);s.UpdateCarriage();s.client.active=false;
 s.UpdateCarriage();assert(s.m_transport.sent.back().Action==DeclineCarriage);
 s.client.active=true;notice.Trip++;s.OnTravelNotice(notice);s.UpdateCarriage();s.AnswerCarriage(9,23,false);assert(s.client.active);
 notice.Trip++;s.client.active=false;s.OnTravelNotice(notice);s.UpdateCarriage();s.ClearCarriage();assert(!s.client.active&&!s.m_offer);
 s.m_shareNpc=false;size_t count=s.m_transport.sent.size();s.OnBoardCarriage({&seat,player});assert(s.m_transport.sent.size()==count);
 s.m_shareMap=false;UI::Get()->open=true;s.UpdateMapTravel();assert(s.m_transport.sent.size()==count);
 s.m_shareMap=true;s.UpdateMapTravel();assert(s.m_transport.sent.back().Action==OpenTravelMap);
 UI::Get()->open=false;s.UpdateMapTravel();assert(s.m_transport.sent.size()==count+1);
 s.ConfirmMarkerTravel();player->position.x+=100;s.UpdateMapTravel();assert(s.m_transport.sent.back().Action==ArriveByMarker);
 s.UpdateMapTravel();assert(s.m_transport.sent.size()==count+2);
 UI::Get()->open=true;s.UpdateMapTravel();UI::Get()->open=false;s.UpdateMapTravel();
 s.m_mapExpires={};s.UpdateMapTravel();assert(s.m_transport.sent.back().Action==CancelCarriage);
}
"""
with tempfile.TemporaryDirectory() as td:
    for side, mocks, tests, path in [('server',server,server_cases,'server/Services/PartyTravel.cpp'),
                                     ('client',client,client_cases,'client/Services/Generic/PartyTravel.cpp')]:
        source=(root/path).read_text()
        cpp=Path(td,side+'.cpp')
        cpp.write_text(shared+mocks+source[source.index('namespace\n{'):]+tests)
        exe=Path(td,side)
        subprocess.run(['g++','-std=c++20','-Wall','-Werror',str(cpp),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
        print('PASS',side,'carriage production handlers')
