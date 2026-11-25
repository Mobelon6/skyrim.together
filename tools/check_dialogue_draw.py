"""Render the actual marker into a recording draw list, without Windows, Skyrim or a GPU."""
from pathlib import Path
import subprocess
import sys
import tempfile

client = Path(sys.argv[1], 'Code/client').resolve()
impl = (client / 'Services/Generic/DialogueMarker.cpp').read_text()
body = impl[impl.index('void OverlayService::DrawDialogueMarker()'):]
harness = r"""
#include <cassert>
#include <mutex>
#include <vector>
#include "DialogueMarkerLayout.h"
namespace glm {
struct vec3{float x{},y{},z{};};
vec3 operator+(vec3 a,vec3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
vec3 operator-(vec3 a,vec3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
}
using NiPoint3=glm::vec3;
struct ImVec2{float x{},y{};};
using ImU32=unsigned;
#define IM_COL32(r,g,b,a) ((unsigned)r)
struct Draw {
    std::vector<ImVec2> lineEnds,triangles,rects;
    void AddLine(ImVec2,ImVec2 b,ImU32,float){lineEnds.push_back(b);}
    void AddTriangleFilled(ImVec2 a,ImVec2,ImVec2,ImU32){triangles.push_back(a);}
    void AddRectFilled(ImVec2 a,ImVec2,ImU32,float){rects.push_back(a);}
    void AddCircleFilled(ImVec2,float,ImU32){}
} draws;
namespace MarkerDepth { bool available=true;int calls=0;bool Begin(Draw*,float,float,float,bool){++calls;return available;} void End(Draw*){} }
namespace ImGui {
struct IO{ImVec2 DisplaySize{1280,720};};
IO io; IO GetIO(){return io;}
Draw* GetBackgroundDrawList(){return &draws;}
}
namespace HUDMenuUtils {
float yawOffset{};
bool SceneDepth(glm::vec3 p,float& d){d=.25f*p.z;return p.z>=0;}
bool ProjectForOverlay(glm::vec3 p, NiPoint3& uv){
    if(p.z<0)return false;
    uv={p.x+yawOffset,p.y,1}; return true;
}
}
struct UI{bool blocked{};static UI* Get(){static UI ui;return &ui;}bool BlocksPartyHud()const{return blocked;}};
struct OverlayService {
 struct DialogueMarker{
    glm::vec3 Anchor{},Eye{},Npc{},Speaker{};
    bool Active{},HasNpc{},HasSpeaker{};
 };
 std::mutex m_markerMutex;DialogueMarker m_marker;
 bool observing{};bool ObservingDialogue(){return observing;}
 void DrawDialogueMarker()noexcept;
};
"""
checks = r"""
int main(){
    OverlayService s;
    auto& m=s.m_marker;
    m.Active=m.HasNpc=m.HasSpeaker=true;
    m.Npc=m.Anchor={.3f,.4f,1}; m.Speaker={.5f,.6f,1};
    float prev=0;
    for(int i=0;i<120;i++){
        HUDMenuUtils::yawOffset=i*.001f;
        draws={}; s.DrawDialogueMarker();
        assert(draws.lineEnds.size()==4 && draws.rects.size()==2 && draws.triangles.size()==2);
        auto x=draws.rects[0].x; assert(x>prev);prev=x;
    }
    assert(MarkerDepth::calls==120);
    MarkerDepth::available=false;draws={};s.DrawDialogueMarker();assert(draws.lineEnds.size()==4);
    s.observing=true;draws={};s.DrawDialogueMarker();assert(draws.lineEnds.empty()&&draws.rects.empty()&&draws.triangles.empty());
    s.observing=false;MarkerDepth::available=true;
    HUDMenuUtils::yawOffset=0;
    for (ImVec2 size : {ImVec2{1920,1080},ImVec2{2560,1440},ImVec2{3440,1440},ImVec2{1024,768}}) {
        ImGui::io.DisplaySize=size; draws={}; s.DrawDialogueMarker();
        assert(std::abs(draws.lineEnds[0].x-size.x*.3f)<.1f && std::abs(draws.lineEnds[0].y-size.y*.6f)<.1f);
        assert(std::abs(draws.lineEnds[2].x-size.x*.5f)<.1f && std::abs(draws.lineEnds[2].y-size.y*.4f)<.1f);
    }
    m.Speaker={1.5f,.2f,1};draws={};s.DrawDialogueMarker();assert(draws.lineEnds.size()==2);
    m.Npc=m.Anchor={1.6f,.5f,1};draws={};s.DrawDialogueMarker();
    assert(draws.lineEnds.empty() && draws.triangles.size()==4);
    m.Npc=m.Anchor={.3f,.4f,-1};m.Speaker=m.Npc;draws={};s.DrawDialogueMarker();
    assert(draws.lineEnds.empty() && draws.triangles.size()==4);
    UI::Get()->blocked=true;draws={};s.DrawDialogueMarker();assert(draws.rects.empty());UI::Get()->blocked=false;
    m.Active=false;draws={};s.DrawDialogueMarker();assert(draws.rects.empty() && draws.lineEnds.empty());
}
"""
with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp, 'draw')
    subprocess.run(['g++','-std=c++20','-Wall','-Werror','-I',str(client/'Services'),'-x','c++','-','-o',str(exe)],
                   input=harness+body+checks,text=True,check=True)
    subprocess.run([str(exe)],check=True)
print("PASS real draw callback at 120 camera samples, two/single/no visible actors, behind view and closure")
