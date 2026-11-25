"""Static integration gates; native save and movement require game validation."""
from pathlib import Path
import sys
r=Path(sys.argv[1])
save=(r/"Code/client/Games/SaveLoad.cpp").read_text()
home=(r/"Code/client/Services/Generic/PartyHome.cpp").read_text()
world=(r/"Code/client/World.cpp").read_text()
trade=(r/"Code/client/Services/Generic/PartyTrade.cpp").read_text()
assert "HomeGuestSave::Pump(*this)" in world
assert "HomeGuestSave::Cancel();" in world
assert "manager!=SaveManager()" in save
pump=save[save.index("void HomeGuestSave::Pump("):]
assert pump.index("HomeOriginalOverlay::Dirty()")<pump.index("WriteWithCheckpoint(")
assert pump.index("!AtReturn(request.Visit)")<pump.index("WriteWithCheckpoint(")
assert pump.index("!InventoryReady(world)")<pump.index("WriteWithCheckpoint(")
assert pump.index("WriteWithCheckpoint(")<pump.index("Resume(world,record)")
assert "visit.Ready=false;visit.Leaving=true;" in home
assert "record->Entrance=*entrance" in home
assert "HomeGuestSave::HooksReady()" in home
assert "next.ResumePose=record.Pose" in home
assert "SendAccess(world,*identity,3,next.Slot" in home
assert "record->Digest!=lastResumeDigest" in home
assert "HomeSnapshot::Digest(visit.Shape)!=visit.ResumeShape" in home
assert "bool HomeGuestSave::InventoryReady" in trade
print("PASS deferred personal ESS and exact guest checkpoint wiring (static)")
