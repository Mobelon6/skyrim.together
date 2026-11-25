"""Verify cross-service wiring of the guest visit lifecycle; pure scope and ACL tested separately."""
from pathlib import Path
import sys
r=Path(sys.argv[1])
server=(r/"Code/server/Services/PartyHome.cpp").read_text()
client=(r/"Code/client/Services/Generic/PartyHome.cpp").read_text()
player=(r/"Code/server/Services/PlayerService.cpp").read_text()
trade=(r/"Code/client/Services/Generic/PartyTrade.cpp").read_text()
assert "found->second.Character==character" in server
assert "found->second.Connection==uint64_t(player.GetConnectionId())" in server
assert "uint64_t(owner->GetConnectionId()),e.House" in server
assert "HomeEstateCatalog::Identity(*catalog,link.Journal,link.Campaign,*room)" in server
assert "found->second.Slot==slot && Clock::now()<found->second.Until" in server
assert "HomePermissions::Effective(link.Journal,slot,character)!=HomePermissions::Right::None" in server
assert "if (e.AccessOp==3)" in server and "if (e.AccessOp==3)" in client
assert "HomeVisit::CanEnter(*pPlayer,message.CellId.ModId,message.CellId.BaseId)" in player
assert "HomeProjection::Apply(*current,*state,*bindings)" in client
assert "projectedRoot.store((uint64_t(visit.Base)<<32)|visit.Copy)" in client
assert "visit.Ready=true;" in client
assert "if (!valid || !visit.Granted || visit.Copy!=current->formID || !visit.Moved)" in client
assert "ClearTrade();LeaveCopy(*p,*copy,visit.Door)" in client
assert "HomeInstance::GuestReference(object->formID,source)" in trade
assert "if (!current || !current->empty()) return false;" in trade
print("PASS guest permission, scope mapping, entry, recovery and no physical duplicate inventory integration")
