"""Run the production camera lifecycle using Skyrim camera/input stand-ins."""
from pathlib import Path
import subprocess,sys,tempfile
root=Path(sys.argv[1],'Code/client')
source=(root/'Services/Generic/DialogueObserver.cpp').read_text()
harness=r"""
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <memory>
#include <string>
namespace glm {
struct vec3{float x{},y{},z{};};
vec3 operator+(vec3 a,vec3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
vec3 operator-(vec3 a,vec3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
vec3 operator*(vec3 a,float s){return {a.x*s,a.y*s,a.z*s};}
float distance(vec3 a,vec3 b){auto d=a-b;return std::hypot(d.x,d.y,d.z);}
}
using NiPoint3=glm::vec3;struct NiPoint2{float x{},y{};};
struct TESCameraState{enum Id:unsigned{kFirstPerson=0,kFree=3,kThirdPerson=9,kOther=11};char pad[0x18];Id id;};
struct NiCamera{NiPoint3 worldPosition;};
struct PlayerCamera{
 TESCameraState* currentState;TESCameraState* normal;TESCameraState* freeview;
 NiCamera eye{};bool cameraRoot=true;int toggles{};
 static PlayerCamera* Get(){static PlayerCamera c;return &c;}auto* GetNiCamera(){return &eye;}
 TESCameraState first{},*savedThird{};
 void ForceFirstPerson(){savedThird=normal;first.id=TESCameraState::kFirstPerson;normal=currentState=&first;}
 void ForceThirdPerson(){normal=currentState=savedThird;}
 void ToggleFreeCameraWithoutPause(){++toggles;currentState=currentState==freeview?normal:freeview;}
};
struct PlayerCharacter{bool dead{},combat{};
 static PlayerCharacter* Get(){static PlayerCharacter p;return &p;}bool IsDead(){return dead;}bool IsInCombat(){return combat;}};
namespace BSGraphics{struct Win{bool active=true;bool IsForeground(){return active;}};Win* GetMainWindow(){static Win w;return &w;}}
struct BSInputEnableManager{static bool IsMovementControlsEnabled(){return true;}};
bool backspace{};constexpr int VK_BACK=8;int GetAsyncKeyState(int){return backspace?0x8000:0;}
namespace Utils{int hints=0;void ShowHudMessage(const char*){++hints;}}
struct OverlayService{std::string m_sampledCaption;struct DialogueMarker{glm::vec3 Anchor{},Eye{},Npc{},Speaker{};bool Active{},HasNpc{},HasSpeaker{};};
 static void RequestObserverExit() noexcept;
 std::chrono::steady_clock::time_point m_rejoinUntil{};
 bool ObserverInCombat(unsigned)const noexcept{return PlayerCharacter::Get()->combat;}bool m_restoreThird{};void ObserverHud(bool open){if(open) ++Utils::hints;}
 static bool ObservingDialogue() noexcept;bool CaptionForObserver(unsigned,const char*,const char*){return true;}
 void UpdateObserver(const DialogueMarker&,uint32_t,uint32_t,float)noexcept;void EndObserver()noexcept;void LeaveConversation(uint64_t){}
 uint64_t m_observedTalk{},m_dismissedTalk{},m_requestedTalk{};TESCameraState* m_observerCamera{};
 glm::vec3 m_observerEye{};float m_appliedPitch{},m_appliedYaw{},m_offsetPitch{},m_offsetYaw{};
};
"""
checks=r"""
int main(){
 OverlayService s;auto* c=PlayerCamera::Get();
 FreeView freeState{};freeState.id=TESCameraState::kFree;TESCameraState third{},script{};
 third.id=TESCameraState::kThirdPerson;script.id=TESCameraState::kOther;
 c->currentState=c->normal=&third;c->freeview=&freeState;c->eye.worldPosition={0,0,70};
 OverlayService::DialogueMarker frame{};frame.Eye={0,0,70};frame.Npc={-20,200,80};frame.Speaker={20,200,80};frame.HasNpc=frame.HasSpeaker=true;
 auto update=[&](unsigned session,float distance=220.f){s.UpdateObserver(frame,2,session,distance);if(c->currentState==&c->first)s.UpdateObserver(frame,2,session,distance);};
 update(1,301);assert(c->toggles==0);frame.HasNpc=false;update(1);assert(c->toggles==0);
 frame.HasNpc=true;update(1);assert(c->currentState==&third);auto join=[&](unsigned session){s.m_requestedTalk=(uint64_t(2)<<32)|session;};join(1);update(1);assert(c->currentState==&freeState && Utils::hints==1);
 assert(s.ObservingDialogue());assert(freeState.rotation.y==0&&freeState.rotation.x==0);
 freeState.rotation.y=.1f;freeState.translation={0,100,70};update(1);
 assert(std::abs(freeState.rotation.y-.03f)<.005f&&freeState.translation.y==0);
 frame.Npc.x=20;frame.Speaker.x=60;update(1);
 assert(std::abs(freeState.rotation.y-(std::atan2(40.f,200.f)+.03f))<.005f);
 backspace=true;update(1);backspace=false;update(1);assert(c->currentState==&third && c->toggles==2 && !s.ObservingDialogue());
 join(2);update(2);assert(c->currentState==&freeState);PlayerCharacter::Get()->combat=true;update(2);
 assert(c->currentState==&third);PlayerCharacter::Get()->combat=false;join(3);update(3);c->currentState=&script;
 update(3);s.EndObserver();assert(c->currentState==&script&&c->toggles==5);
 c->normal=c->currentState=&third;update(3);assert(c->currentState==&third);
 join(4);update(4);assert(c->currentState==&freeState);s.EndObserver();assert(c->currentState==&third);
 c->currentState=&freeState;update(5);s.EndObserver();assert(c->currentState==&freeState);
}
"""
with tempfile.TemporaryDirectory() as tmp:
    app=Path(tmp,'observer')
    subprocess.run(['g++','-std=c++20','-Wno-invalid-offsetof','-x','c++','-','-o',str(app)],
                   input=harness+source[source.index('namespace\n{'):]+checks,text=True,check=True)
    subprocess.run([str(app)],check=True)
print('PASS observer entry, camera aim, native look offset, fixed position, combat, manual exit and script takeover')
