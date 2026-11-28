"""Exercise the actual World load-boundary and discovery implementations with engine stubs."""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

root=Path(sys.argv[1])/"Code/client"
def function(path,signature):
    s=(root/path).read_text()
    i=s.index(signature)
    a=s.index("{",i)
    depth=1
    j=a+1
    while depth:
        depth+=(s[j]=="{")-(s[j]=="}")
        j+=1
    return s[i:j]+"\n"

world="World.cpp"
source=(root/world).read_text()
assert "if (!lock.owns_lock() || IsLoadingSave() || m_inWorldFrame) return;" in source
assert source.index("if (!lock.owns_lock() || IsLoadingSave()")<source.index("m_dispatcher.trigger(PreUpdateEvent")
assert source.index("m_transport.CloseWorld();\n        ResetWorldTracking();")<source.index('spdlog::info("STM save load begin')
engine=(root/"Games/SaveLoad.cpp").read_text()
assert "0x610480" in engine and "0x605F90" in engine and '1.6.1170.0' in engine
assert engine.index("BeginSaveLoad()")<engine.index("scope.Success=original(args...)")
assert "WithSaveLoadBoundary(false,realLoadRequest" in engine and "WithSaveLoadBoundary(true,LoadWorldWithCheckpoint" in engine
assert source.index("GetOverlayService().RecheckTradeAfterLoad();")<source.index("m_dispatcher.trigger(PreUpdateEvent")
discovery=function("Services/Generic/DiscoveryService.cpp","BSTEventResult DiscoveryService::OnEvent(")
assert "m_loadEventPending.store(true" in discovery and "VisitCell(" not in discovery and "ResetSessionProgress" not in discovery
# Assert the same real handlers are used for ordinary disconnect and save change.
weather="Services/Generic/WeatherService.cpp"
for name in ("OnWaitingFor3DRemoved","OnPlayerComponentRemoved"):
    assert "if (!m_transport.IsOnline()) return;" in function(weather,"void WeatherService::"+name+"(")
assert "if (auto* sky=Sky::Get())" in function(weather,"void WeatherService::ToggleGameWeatherSystem(")
calendar="Services/Generic/CalendarService.cpp"
calendar_disconnect=function(calendar,"void CalendarService::OnDisconnected(")
assert "TimeData::Get()" not in calendar_disconnect and "ToggleGameClock(" not in calendar_disconnect
magic="Services/Generic/MagicService.cpp"
assert "ResetWorldTracking();" in function(magic,"void MagicService::OnDisconnected(")
assert "m_transport.IsConnected()" not in (root/magic).read_text()
assert "bool m_isDeathSystemEnabled = false;" in (root/"Services/PlayerService.h").read_text()
trade="Services/Generic/PartyTrade.cpp"
trade_reset=function(trade,"void OverlayService::RecheckTradeAfterLoad(")
# Include the actual pending shared-container upload state and cancellation,
# rather than stubbing away the new load-boundary operation.
trade_source=(root/trade).read_text()
shared_state=trade_source[trade_source.index("uint32_t sharedOpening{},innTarget{};"):
                          trade_source.index("void CancelSharedOpen(){")]
shared_cancel=function(trade,"void CancelSharedOpen(")
assert "walletReady=false;readyStream=0;readyCursor=0;" in function(trade,"void OverlayService::ClearTrade(")
assert "!receiptFailure && walletReady" in function(trade,"bool Ready(")
code=r"""
#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <thread>
#include <chrono>
#include <string>
using Clock=std::chrono::steady_clock;
struct GameId{uint32_t ModId{},BaseId{};};
namespace spdlog { template<class... T> void info(const char*,T...){} template<class... T> void warn(const char*,T...){} }
namespace CampaignCheckpointClient { void ForgetLoaded() noexcept {} }
namespace HomeGuestSave { int cancels{}; void Cancel() noexcept {++cancels;} }
thread_local uint32_t threadId=1;
uint32_t GetCurrentThreadId(){return threadId;}
struct DisconnectedEvent{};
struct CalendarService {
    static inline bool s_gameClockLocked=true, s_restMenuOpen=false;
    float m_fadeTimer=4.f;
    uint32_t m_generation=5;bool restReset=false;
    void ResetRestTracking() noexcept {restReset=true;}
    uint64_t m_lastTick=9000,m_lastLogTick=8000;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
};
bool receiptFailure{},receiptFailureCampaign{},walletReady{};
uint64_t readyStream{};
uint32_t readyCursor{},lastConflict{};
std::chrono::steady_clock::time_point pollAt{},cleanupRetry{};
bool shadowCleanupPending{};
int nativeResets{},placementForgets{};
namespace HomePlacementClient { void Forget() noexcept {++placementForgets;} }
void ResetNativeIntents(){++nativeResets;}
struct OverlayService { void RecheckTradeAfterLoad() noexcept; };
struct Transport {
    bool session{}; bool suspended{}; int closes{};
    bool HasWorldSession() const {return session;}
    void SuspendForSaveLoad(){suspended=true;}
    void CloseWorld(){assert(suspended); ++closes; session=false;}
};
struct World {
    std::recursive_mutex m_saveLoadMutex;
    std::atomic<unsigned> m_saveLoadDepth{};
    std::atomic<bool> m_disconnectBeforeLoad{};
    uint32_t m_gameThreadId{1};
    bool m_inWorldFrame{},m_preparingLoad{},m_resetAfterLoad{};
    bool m_loadedWorld{},m_loadFailed{},m_successfulLoadPending{};
    Transport m_transport; int resets{};
    bool IsLoadingSave()const{return m_saveLoadDepth.load()!=0;}
    bool BeginSaveLoad() noexcept;
    void EndSaveLoad(bool success,bool actualWorldLoad) noexcept;
    void ResetWorldTracking(){assert(m_transport.suspended&&!m_transport.session);++resets;}
};
"""
code+=function(world,"bool World::BeginSaveLoad() noexcept")
code+=function(world,"void World::EndSaveLoad(")
code+=shared_state
code+=shared_cancel
code+=trade_reset
code+=calendar_disconnect
code+=r"""
int main(){
    CalendarService calendar;
    calendar.OnDisconnected({});
    assert(!calendar.s_gameClockLocked&&!calendar.m_lastTick&&!calendar.m_lastLogTick&&calendar.m_fadeTimer==0.f&&!calendar.m_generation&&calendar.restReset);
    World w;w.m_transport.session=true;
    assert(w.BeginSaveLoad());
    assert(HomeGuestSave::cancels==1);
    assert(w.IsLoadingSave()&&w.m_transport.closes==1&&w.resets==1);
    std::thread worker([&]{threadId=2;assert(w.BeginSaveLoad());w.EndSaveLoad(true,true);});
    worker.join();assert(w.IsLoadingSave()&&w.resets==1);
    w.EndSaveLoad(true,false);assert(!w.IsLoadingSave()&&w.m_successfulLoadPending);
    // A failed native load also exits the same scope; no permanent depth gate.
    assert(w.BeginSaveLoad());w.EndSaveLoad(false,true);assert(!w.IsLoadingSave()&&w.resets==2&&!w.m_successfulLoadPending);
    assert(w.BeginSaveLoad());w.EndSaveLoad(true,false);assert(!w.m_successfulLoadPending);
    assert(w.BeginSaveLoad());assert(w.BeginSaveLoad());w.EndSaveLoad(true,true);w.EndSaveLoad(false,false);
    assert(!w.m_successfulLoadPending);
    OverlayService overlay;
    receiptFailure=receiptFailureCampaign=walletReady=true;readyStream=7;readyCursor=9;
    sharedOpening=42;innPending=true;innTarget=43;sharedOpenUntil=Clock::now()+std::chrono::seconds(15);
    sharedPrompt=sharedSwitchPending=true;sharedControlsBlocked=true;sharedRevision=9;hudToken=42;hudPhase=3;
    sharedSeed={{3,42},2,9,7,123,"pending snapshot bytes"};
    overlay.RecheckTradeAfterLoad();
    assert(!receiptFailure&&!receiptFailureCampaign&&!walletReady&&!readyStream&&!readyCursor);
    assert(!sharedOpening&&!innPending&&!innTarget&&sharedOpenUntil==Clock::time_point{});
    assert(!sharedPrompt&&!sharedSwitchPending&&!sharedControlsBlocked&&!sharedRevision&&!hudToken&&!hudPhase);
    assert(!sharedSeed.Item.ModId&&!sharedSeed.Item.BaseId&&!sharedSeed.Next&&!sharedSeed.Cursor);
    assert(!sharedSeed.Stream&&!sharedSeed.Digest&&sharedSeed.Bytes.empty());
    assert(shadowCleanupPending && nativeResets==1 && placementForgets==1);
    receiptFailure=true;receiptFailureCampaign=false;walletReady=true;
    overlay.RecheckTradeAfterLoad();
    assert(receiptFailure&&!walletReady && placementForgets==2);
    assert(!sharedOpening&&sharedSeed.Bytes.empty());
    World active;active.m_transport.session=true;
    std::thread wrong([&]{threadId=2;assert(!active.BeginSaveLoad());});
    wrong.join();assert(active.m_disconnectBeforeLoad&&!active.IsLoadingSave()&&active.m_transport.session&&active.resets==0);
    World offline;
    std::thread other([&]{threadId=2;assert(offline.BeginSaveLoad());assert(offline.IsLoadingSave());offline.EndSaveLoad(true,true);});
    other.join();assert(!offline.IsLoadingSave()&&offline.m_resetAfterLoad&&offline.resets==0&&offline.m_transport.suspended);
    World busy;busy.m_inWorldFrame=true;assert(!busy.BeginSaveLoad()&&busy.m_disconnectBeforeLoad&&!busy.IsLoadingSave());
    World preparing;preparing.m_preparingLoad=true;assert(!preparing.BeginSaveLoad());
}
"""
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp,"test.cpp");src.write_text(code)
    exe=Path(tmp,"test")
    subprocess.run(["g++","-std=c++20","-pthread","-Wall","-Wextra","-Werror",str(src),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print("PASS early save-load boundary, nested worker, failed load release, off-thread active refusal, deferred offline cleanup, shared stock upload cancellation and post-load discovery hint")
