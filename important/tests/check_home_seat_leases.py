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
    Table publicSeats;
    Claim x{1,1,101,1,0,WorkbenchScope,10,"skyrim.esm:123",0,7};
    Claim y{1,2,102,1,1,WorkbenchScope,10,"skyrim.esm:123",2,3};
    assert(publicSeats.Change(1,x,now));
    assert(!publicSeats.Change(1,y,now)); // same physical REFR, different player CELL
    auto different=y;different.Ref="skyrim.esm:124";
    assert(publicSeats.Change(1,different,now));
    auto staleEpoch=x;staleEpoch.Location++;
    assert(!publicSeats.Change(0,staleEpoch,now));
    publicSeats.Prune(now,[&](const Claim& c){return c.Connection!=101;});
    assert(publicSeats.Change(0,different,now));
    assert(publicSeats.Change(1,y,now));
    assert(!publicSeats.Change(2,x,now));
    Usage use;use.Grace=now+std::chrono::seconds(10);
    assert(use.Keep(false,now)); // engine entry animation
    assert(use.Keep(true,now+std::chrono::seconds(2)));
    assert(use.Keep(true,now+std::chrono::hours(1))); // long crafting
    assert(!use.Keep(false,now+std::chrono::hours(1))); // menu and marker released
    Usage failed;failed.Grace=now+std::chrono::seconds(10);
    assert(!failed.Keep(false,failed.Grace));
    assert(!IsWorkbenchType(0)&&!IsWorkbenchType(8)&&!IsWorkbenchType(255));
    for (uint8_t type=1;type<=7;++type) assert(IsWorkbenchType(type));
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

# Wiring of the public lease is independent of home admission and journal;
# it defers the local activation while NPC and non-workbench paths stay native.
assert "if (e.AccessOp==HomeSeatLeases::WorkbenchOperation)" in server
assert server.index("PublicWorkbench(m_world,*owner,e)") < server.index("auto& link=CampaignAccess::Authority();", server.index("void OverlayService::OnHome"))
public=server[server.index("void PublicWorkbench("):server.index("bool HomeVisit::CanEnter(")]
assert "seats.Prune" in public and "p->LocationEpoch()!=c.Location" in public
assert "loc.Cell!=*cell" in public and "seats.Change" in public and "Journal.Append" not in public
assert "Workbench::Defer(apThis,aUnk1,aDefaultProcessing)" in (up/"Code/client/Games/Skyrim/TESObjectREFR.cpp").read_text()
req=client[client.index("bool RequestWorkbench("):client.index("std::string lastResumeDigest;")]
assert "IsFurnitureInUse" not in req
assert "object->Activate(player,workbench.Unk,nullptr,1,workbench.DefaultProcessing)" in req
assert "world.GetRunner().Trigger(ActivateEvent" in req
assert "Utils::ShowHudMessage(\"Станок пока занят\")" in req
assert "workbench.Use.Keep(ObservedWorkbenchUse" in req
assert "UpdateWorkbench(m_world)" in client and "ReleaseWorkbench(m_world)" in client
assert "Workbench::OrdinaryRoute(furniture)" in interp
print("PASS ordinary workbench contention, usage lifecycle, canonical routing and native activation wiring")
