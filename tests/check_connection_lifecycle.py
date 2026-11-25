"""Compile selected production connection routines with deterministic DNS/Steam substitutes."""
from pathlib import Path
import subprocess, tempfile, sys
connect=Path(sys.argv[1],"Code/connect/src/Client.cpp").read_text()
transport=Path(sys.argv[2],"Code/client/Services/Generic/TransportService.cpp").read_text()
def section(src,start,end):
    return src[src.index(start):src.index(end,src.index(start))]
dns=section(connect,"    bool Client::Connect(const std::string&","    bool Client::Connect(const SteamNetworkingIPAddr&")
close=section(connect,"    void Client::Close()","    void Client::Update()")
status=section(connect,"    void Client::OnSteamNetConnectionStatusChanged(","    void Client::HandleMessage(")
disconnect=section(transport,"void TransportService::OnDisconnected(","void TransportService::OnUpdate()")
harness=r"""
#include <string>
#include <cassert>
#include <cstdlib>
#include <utility>
#include <netdb.h>
#include <netinet/in.h>
#include <cstdint>
constexpr int k_HSteamNetConnection_Invalid=0, UV_ECANCELED=-125;
struct uv_loop_t {void* data;};
struct uv_getaddrinfo_t {uv_loop_t* loop;};
using uv_req_t=uv_getaddrinfo_t;
using Callback=void(*)(uv_getaddrinfo_t*,int,addrinfo*);
uv_getaddrinfo_t* pending{};
Callback cb{};
int submitError{}, cancelled{}, addressFrees{}, allocations{};
int uv_getaddrinfo(uv_loop_t* loop,uv_getaddrinfo_t* h,Callback callback,const char*,const char*,void*) {
 if(submitError)return submitError;
 assert(!pending);h->loop=loop;pending=h;cb=callback;return 0;
}
int uv_cancel(uv_req_t*){++cancelled;return -16;} // UV_EBUSY
void uv_freeaddrinfo(addrinfo*){++addressFrees;}
struct Allocator {
 static Allocator* GetDefault(){static Allocator a;return &a;}
 void* Allocate(size_t n){++allocations;return std::malloc(n);}
 void Free(void* p){--allocations;std::free(p);}
};
struct SteamNetworkingIPAddr {
 void SetIPv4(uint32_t,uint16_t){}
 void SetIPv6(const unsigned char*,uint16_t){}
};
enum {k_ESteamNetworkingConnectionState_None,k_ESteamNetworkingConnectionState_ClosedByPeer,
 k_ESteamNetworkingConnectionState_ProblemDetectedLocally,k_ESteamNetworkingConnectionState_Connecting,
 k_ESteamNetworkingConnectionState_Connected};
struct SteamNetConnectionStatusChangedCallback_t{int m_hConn; struct{int m_eState;} m_info; int m_eOldState;};
struct Steam {int closes{};void CloseConnection(int,int,const char*,bool){++closes;}};
struct Clock {void Reset(){}};
struct Client {
 enum EDisconnectReason{kTimeout,kLocalProblem,kKicked,kCannotResolve,kAborted,kNormal};
 uv_loop_t loop{this}; void* m_pLoop=&loop; void* m_pHandle{};bool m_dnsCancelled{};
 int m_connection{};Steam steam;Steam* m_pInterface=&steam;Clock m_clock;
 int connects{}, disconnects{};EDisconnectReason reason{};
 bool Connect(const std::string&) noexcept;
 bool Connect(const SteamNetworkingIPAddr&) noexcept {++connects;m_connection=7;return true;}
 void Close() noexcept;
 void OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t*);
 void OnDisconnected(EDisconnectReason r){++disconnects;reason=r;}
};
void deliver(int status,addrinfo* address){
 auto* h=std::exchange(pending,nullptr);assert(h);cb(h,status,address);
}
struct DisconnectedEvent{};
struct ConnectionErrorEvent{std::string ErrorDetail;};
struct Dispatcher{int game{},errors{};void trigger(DisconnectedEvent){++game;}void trigger(ConnectionErrorEvent){++errors;}};
struct Runner{int invalidations{};void InvalidateWorld(){++invalidations;}};
struct Overlay{int calls{};void ExecuteAsync(const char*){++calls;}};
struct OverlayService{Overlay overlay;Overlay* GetOverlayApp(){return &overlay;}};
struct World {Runner runner;OverlayService ui;Runner& GetRunner(){return runner;}OverlayService& GetOverlayService(){return ui;}};
namespace spdlog {template<class... T>void warn(const char*,T...){}}
struct TransportService {
 using EDisconnectReason=Client::EDisconnectReason;
 static constexpr auto kAborted=Client::kAborted;
 bool m_connected{},m_connectionRequested{},title{};
 std::string m_serverPassword="secret";
 unsigned m_localPlayerId=42;
 World m_world;Dispatcher m_dispatcher;
 bool AtTitle(){return title;}
 void OnDisconnected(EDisconnectReason);
};
"""
main=r"""
int main(){
 Client c;
 sockaddr_in ip{};addrinfo address{};address.ai_family=AF_INET;
 address.ai_addr=reinterpret_cast<sockaddr*>(&ip);
 assert(c.Connect(std::string("host:1234")));
 assert(!c.Connect(std::string("other")));
 c.Close();assert(cancelled==1&&c.m_pHandle&&c.m_dnsCancelled);
 assert(!c.Connect(std::string("other")));
 deliver(0,&address); // DNS completes successfully despite uv_cancel failure
 assert(!c.m_pHandle&&!c.connects&&c.disconnects==1&&c.reason==Client::kAborted);
 assert(!allocations&&addressFrees==1);
 submitError=-1;assert(!c.Connect(std::string("host")));assert(!allocations&&!c.m_pHandle);
 submitError=0;assert(c.Connect(std::string("host")));deliver(-1,nullptr);
 assert(c.reason==Client::kCannotResolve&&!allocations);
 assert(c.Connect(std::string("host")));deliver(0,&address);assert(c.connects==1&&c.m_connection==7);
 SteamNetConnectionStatusChangedCallback_t late{6,{k_ESteamNetworkingConnectionState_ClosedByPeer},k_ESteamNetworkingConnectionState_Connected};
 c.OnSteamNetConnectionStatusChanged(&late);assert(c.m_connection==7&&!c.steam.closes);
 late.m_hConn=7;c.OnSteamNetConnectionStatusChanged(&late);assert(!c.m_connection&&c.steam.closes==1);
 TransportService t;t.m_connectionRequested=true;t.OnDisconnected(Client::kCannotResolve);
 assert(!t.m_dispatcher.game&&t.m_dispatcher.errors==1&&t.m_world.ui.overlay.calls==1);
 assert(!t.m_connectionRequested&&t.m_serverPassword.empty()&&!t.m_localPlayerId);
 t.m_connected=true;t.m_connectionRequested=true;t.OnDisconnected(Client::kKicked);
 assert(t.m_dispatcher.game==1&&t.m_dispatcher.errors==1);
 t.OnDisconnected(Client::kAborted);assert(t.m_dispatcher.game==1);
}
"""
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp,"test.cpp");src.write_text(harness+dns+close+status+disconnect+main)
    exe=Path(tmp,"test")
    subprocess.run(["g++","-std=c++20","-Wall","-Wextra","-Werror",str(src),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print("PASS cancelled DNS completion, duplicate request, submission failure, stale connection status and unauthenticated disconnect")
