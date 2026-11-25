"""Static integration checks. Native gameplay still requires the game."""
from pathlib import Path
import sys
r=Path(sys.argv[1])
home=(r/"Code/client/Services/Generic/PartyHome.cpp").read_text()
overlay=(r/"Code/client/Services/Generic/HomeOriginalOverlay.cpp").read_text()
trade=(r/"Code/client/Services/Generic/PartyTrade.cpp").read_text()
actors=(r/"Code/client/Services/Generic/CharacterService.cpp").read_text()
save=(r/"Code/client/Games/SaveLoad.cpp").read_text()
assert 'std::getenv("STM_NATIVE_HOME_VISITS")' in home
assert "visit.Native=chosen.Native" in home
assert home.index("originalGuestCell.store(visit.Copy);")<home.index("MoveAt(*p,*target,visit.Pose,0);")
assert "visit.Prepared=HomeOriginalOverlay::Prepare(" in home
assert "HomeOriginalOverlay::Switch(m_world,true)" in home
assert home.index("HomeOriginalOverlay::Switch(m_world,false)")<home.index("MoveAt(*p,*out,visit.Pose,6)")
assert "if (!HomeOriginalOverlay::Release())" in home
assert "current->formID==visit.ReturnCell && current->IsAttached()" in home
assert "if (NativeGuestCell(cell->formID))" in home
assert "no owner observation, registration or inventory recovery" in home
assert "HomeInstance::NativeGuestCell(cell->formID)" in trade
assert "HomeInstance::GuestReference(object->formID,source)" in trade
assert "HomeInstance::NativeGuestCell(pActor->parentCell->formID)" in actors
assert "HomeInstance::NativeGuestActive()" in save
assert "if (!rows.count(ref->formID)) return false;" in overlay
assert "HomeInstance::ForgetOriginalVisit();" in overlay
print("PASS original-cell route wiring (static), quarantine, receipt proxy and exit guard")
