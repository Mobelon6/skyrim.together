"""Execute the real reversible-overlay transaction and verify adapter wiring."""
from pathlib import Path
import sys, tempfile, subprocess
root=Path(sys.argv[1])
source=r"""
#include "HomeOriginalOverlay.h"
#include <cassert>
#include <map>
using namespace HomeOriginalOverlay;
int main()
{
    Fields a,b;b.Disabled=true;b.Pose[0]=std::bit_cast<uint32_t>(10.f);
    Delta parent{1,3,34,100,a,b,false},child{2,4,34,100,a,b,true};
    Transaction t;
    std::map<uint32_t,Fields> current{{1,a},{2,a}},queued;
    int writes=0;
    auto read=[&](const Delta& d)->std::optional<Fields>{
        auto i=current.find(d.Ref);return i==current.end()?std::nullopt:std::optional<Fields>(i->second);
    };
    auto write=[&](const Delta& d,const Fields& f){++writes;queued[d.Ref]=f;return true;};
    assert(t.Stage({parent,child})&&!t.Dirty());
    assert(!t.Stage({parent}));
    assert(t.Switch(true,read,write)==Result::Pending&&t.Dirty()&&writes==2);
    assert(!t.Release());
    assert(t.Switch(true,read,write)==Result::Pending&&writes==2);
    assert(t.Switch(false,read,write)==Result::Pending&&writes==2);
    current=queued;
    assert(t.Switch(true,read,write)==Result::Ready&&t.Dirty());
    assert(t.Switch(false,read,write)==Result::Pending&&t.Dirty()&&writes==4);
    current=queued;
    assert(t.Switch(false,read,write)==Result::Ready&&!t.Dirty()&&t.Release());
    assert(current[1].Same(a)&&current[2].Same(a));
    assert(t.Stage({parent,child}));
    current[2].LockState=1;
    assert(t.Switch(true,read,write)==Result::Unavailable&&!t.Dirty()&&writes==4);
    current[2]=a;
    assert(t.Switch(true,read,[&](const Delta& d,const Fields& f){
        if(d.Ref==2)return false;
        current[d.Ref]=f;return true;
    })==Result::Unavailable&&t.Dirty());
    assert(!t.Release());
    assert(t.Switch(false,read,write)==Result::Unavailable&&t.Dirty());
    t.ForgetReplacedWorld();assert(!t.Dirty()&&!t.Staged());
    assert(t.Stage({}));
    assert(t.Switch(true,read,write)==Result::Ready&&t.Dirty());
    assert(t.Switch(false,read,write)==Result::Ready&&t.Release());
    // Identical personal/guest loose shell must still be tracked; pickup
    // changes only its confirmed guest target, never the personal backup.
    Delta loose{3,5,41,100,a,a,false,true,true};
    current={{3,a}};queued.clear();
    assert(t.Stage({loose}));
    assert(t.Switch(true,read,write)==Result::Ready&&t.CanTake(3));
    auto before=writes;
    assert(t.RefreshHidden({{3,true}},read,write)==Result::Pending&&!t.CanTake(3));
    assert(writes==before+1);
    assert(t.Switch(true,read,write)==Result::Pending&&writes==before+1);
    current=queued;
    assert(t.Switch(true,read,write)==Result::Ready&&!t.CanTake(3));
    assert(t.RefreshHidden({{3,true}},read,write)==Result::Ready&&writes==before+1);
    assert(t.RefreshHidden({{3,false}},read,write)==Result::Unavailable);
    assert(t.Switch(false,read,write)==Result::Pending);
    current=queued;
    assert(t.Switch(false,read,write)==Result::Ready&&t.Release()&&current[3].Same(a));
    // A parent-controlled loose shell cannot be independently hidden.
    loose.LiveLoose=false;
    assert(t.Stage({loose})&&t.Switch(true,read,write)==Result::Ready&&!t.CanTake(3));
    before=writes;
    assert(t.RefreshHidden({{3,true}},read,write)==Result::Unavailable&&writes==before);
    assert(t.Switch(false,read,write)==Result::Ready&&t.Release());
    Fields closed=a,opened=a,moving=a;
    closed.Open=3;opened.Open=1;moving.Open=2;
    Delta door{4,6,29,100,closed,closed,false,false,false,true};
    current={{4,closed}};queued.clear();
    assert(t.Stage({door})&&t.Switch(true,read,write)==Result::Ready&&t.DoorState(4)==3);
    before=writes;
    assert(t.RefreshDoors({{4,1}},read,write)==Result::Pending&&t.DoorState(4)==0);
    current[4]=moving;
    assert(t.Switch(true,read,write)==Result::Pending&&writes==before+1);
    current=queued;
    assert(t.Switch(true,read,write)==Result::Ready&&t.DoorState(4)==1);
    assert(t.RefreshDoors({{4,1}},read,write)==Result::Ready&&writes==before+1);
    assert(t.RefreshDoors({{4,2}},read,write)==Result::Unavailable);
    assert(t.Switch(false,read,write)==Result::Pending);
    current[4].Open=4;
    assert(t.Switch(false,read,write)==Result::Pending&&writes==before+2);
    current=queued;
    assert(t.Switch(false,read,write)==Result::Ready&&t.Release()&&current[4].Same(closed));
    door.Personal=moving;assert(!t.Stage({door}));
    door.Personal=closed;door.Guest=opened;
    assert(t.Stage({door})&&t.Switch(true,read,write)==Result::Pending);
    current=queued;
    assert(t.Switch(true,read,write)==Result::Ready&&t.DoorState(4)==1);
    assert(t.Switch(false,read,write)==Result::Pending);
    current=queued;
    assert(t.Switch(false,read,write)==Result::Ready&&t.Release()&&current[4].Same(closed));
}
"""
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp,"overlay.cpp");p.write_text(source);exe=Path(tmp,"overlay")
    subprocess.run(["g++","-std=c++20","-Wall","-Wextra","-Werror","-I",str(root/"Code/client/Services"),str(p),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
native=(root/"Code/client/Services/Generic/HomeOriginalOverlay.cpp").read_text()
save=(root/"Code/client/Games/SaveLoad.cpp").read_text()
world=(root/"Code/client/World.cpp").read_text()
assert "CampaignAccess::HomeBaseline(campaign,slot)" in native
assert "CampaignAccess::HomeObjectView(campaign,slot)" in native
assert "Parent(*ref)!=std::optional<uint32_t>(id)" in native
assert "if (!row.ParentDriven&&current->Disabled!=target.Disabled)" in native
assert 'Get("ObjectReference","SetPosition")' in native and "object->MoveTo(" not in native
assert "SetInventory(" not in native and "CreateLock(" not in native
assert "HomeOriginalOverlay::Dirty()" in save
assert "HomeOriginalOverlay::ForgetReplacedWorld();" in world
home=(root/"Code/client/Services/Generic/PartyHome.cpp").read_text()
trade=(root/"Code/client/Services/Generic/PartyTrade.cpp").read_text()
assert "HomeOriginalOverlay::CanTake(local)" in home
assert "HomeOriginalOverlay::RefreshLoose(m_world,visit.Campaign,visit.Slot)" in home
assert "if (HomeInstance::NativeGuestCell(cell->formID) && loose) return {};" not in trade
assert "d.LiveLoose=object&&CanChange" in native
assert "HomeOriginalOverlay::RefreshDoors(m_world,visit.Campaign,visit.Slot)" in home
assert "HomeOriginalOverlay::DoorState(form)" in home
assert 'Get("ObjectReference","SetOpen")' in native
print("PASS real overlay transaction: readback, pending transition, failed mutation guard, restore")
