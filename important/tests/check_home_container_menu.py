"""Check home session integration, not the engine ABI."""
from pathlib import Path
import sys
r=Path(sys.argv[1])
server=(r/"Code/server/Services/PartyTrade.cpp").read_text()
client=(r/"Code/client/Services/Generic/PartyTrade.cpp").read_text()
wire=(r/"Code/encoding/Structs/PartyTrade.h").read_text()
assert "HomeOpen" in wire and "r.ReadBits(raw,5)" in wire
assert "ledger.CommitHome(h.Character,h.Slot,h.Ref" in server
assert "HomeVisit::CanEnter(player,cell.Cell.ModId,cell.Cell.BaseId)" in server
assert "ShowHomeStock(p,e.Session,*updated)" in server
assert "ProtectHome(from) || ProtectHome(to)" in client
assert "ProtectHome(to) || ProtectHome(from)" in client
assert "PartyTrade request{TradeOp::HomeOpen" in client
assert "CampaignAccess::LocalIdentity(world)" in client
assert "HomeInstance::GuestReference(object->formID,source)" in client
# Protection must include guest shells as well as journal-backed temporary stacks.
protect=client.split("bool ProtectHome(const TESObjectREFR* object) noexcept",1)[1].split("catch (...)",1)[0]
assert "HomeInstance::ProjectionForm(object->formID)" in protect
assert "HomePlacementClient::Tracked(object)" in protect
assert "HomePlacementClient::LegacyBlocked(object)" in protect
assert "return true;" in protect
print("PASS home container session integration points")
