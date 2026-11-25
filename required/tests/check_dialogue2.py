"""Exercise native placement with both actors, partial visibility, behind-camera and changing camera samples."""
from pathlib import Path
from tempfile import TemporaryDirectory
import subprocess
import sys
source = Path(sys.argv[1]).resolve() / 'Code/client/Services'
with TemporaryDirectory() as tmp:
    exe = Path(tmp, 'geometry')
    test = r"""
#include <cassert>
#include "DialogueMarkerLayout.h"
using namespace DialogueMarkerLayout;
int main() {
  Projection npc{{500,300},true}, player{{700,340},true}, missing{};
  for(int i=0; i<120; ++i) {
    npc.p.x+=1; player.p.x+=1;
    auto l=Place(1280,720,1,npc,npc,player);
    assert(!l.edge && l.npcLine && l.speakerLine && l.center.x==601+i && l.center.y<300);
  }
  Projection left{{-300,240},true};
  auto one=Place(1280,720,1,left,left,player);
  assert(!one.edge && !one.npcLine && one.speakerLine && one.center.x==player.p.x);
  auto none=Place(1280,720,1,left,left,missing);
  assert(none.edge && !none.npcLine && !none.speakerLine && none.direction.x<0);
  for(float width : {640.f,1280.f,3440.f}) {
    Projection rear{{width*.5f,360.f},false};
    auto l=Place(width,720,1,rear,rear,missing);
    assert(l.edge && l.direction.y>0 && l.center.y==680);
    auto fallback=Place(width,720,1,{{210,210},true},missing,missing);
    assert(!fallback.edge && !fallback.npcLine && !fallback.speakerLine);
  }
}
"""
    subprocess.run(['g++','-std=c++20','-Wall','-Werror','-I',str(source),'-x','c++','-','-o',str(exe)],
                   input=test, text=True, check=True)
    subprocess.run([str(exe)], check=True)
print('PASS native marker geometry at 120 camera positions and three aspect ratios')
