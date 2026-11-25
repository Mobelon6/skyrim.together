import subprocess
import tempfile
import sys
from pathlib import Path

up=Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory() as d:
    code=Path(d)/"test.cpp"
    code.write_text(r'''
#include "HomeSeatLeases.h"
#include <cassert>
using namespace HomeSeatLeases;
int main()
{
    Table t; const auto now=Clock::time_point{};
    Claim a{1,2,11,1,1,8,10,"seat"}, b{1,3,12,1,2,8,10,"seat"};
    assert(t.Change(1,a,now));
    assert(!t.Change(1,b,now));
    auto other=b;other.Slot=9;
    assert(t.Change(1,other,now)); // same original REFR, different house
    assert(!t.Change(0,b,now));
    auto stale=a;stale.Nonce++;
    assert(!t.Change(0,stale,now));
    assert(t.Change(2,a,now+std::chrono::seconds(5)));
    assert(t.Change(0,a,now+std::chrono::seconds(6)));
    assert(!t.Change(2,a,now+std::chrono::seconds(7)));
    assert(t.Change(0,other,now));
    assert(t.Change(1,b,now));
    t.Prune(now,[](const Claim&){return false;});
    assert(t.Change(1,a,now));
    assert(t.Change(1,b,now+std::chrono::seconds(31)));
    assert(!t.Change(2,a,now+std::chrono::seconds(32)));
}
''',encoding="utf-8")
    exe=Path(d)/"test"
    subprocess.run(["g++","-std=c++20","-Wall","-Wextra","-Werror","-I",str(up/"Code/common"),str(code),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
client=(up/"Code/client/Services/Generic/PartyHome.cpp").read_text()
server=(up/"Code/server/Services/PartyHome.cpp").read_text()
assert "SeatReply(m_world,e)" in client and "UpdateSeat(m_world)" in client
assert "if (!object->Activate(player,0,nullptr,1,0)) ReleaseSeat(world);" in client
assert "HomeVisit::CanEnter(*p,cell.ModId,cell.BaseId)" in server
print("PASS seat contention, distinct scopes, stale release, lease renewal, expiry, disconnect pruning")
# Existing native seat marker is protected only in a registered original home.
interp=(up/"Code/client/Systems/InterpolationSystem.cpp").read_text()
assert "actorExtension->IsRemotePlayer()" in interp
assert "HomeRegistry::Scoped(mod)" in interp
assert "furniture->GetParentCellEx()==cell" in interp
assert "occupiedFurniture.handle.iBits" in interp
assert "|| nativeRemoteSeat) && !cIsTeleport" in interp
assert "if (!sittingInPlace)" in interp
print("PASS scoped native seated interpolation source checks")
