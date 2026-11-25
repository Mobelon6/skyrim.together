"""Check guest actor and projection item hook integration, not engine simulation."""
from pathlib import Path
import sys
base=Path(sys.argv[1])/"Code/client"
actor=(base/"Games/Skyrim/Actor.cpp").read_text()
player=(base/"Games/Skyrim/PlayerCharacter.cpp").read_text()
ref=(base/"Games/Skyrim/TESObjectREFR.cpp").read_text()
trade=(base/"Services/Generic/PartyTrade.cpp").read_text()
inv=(base/"Services/Generic/InventoryService.cpp").read_text()
policy=(base/"Services/HomeActorProtection.h").read_text()
assert "!ext->IsPlayer()" in policy and "HomeInstance::ProjectionForm(cell->formID)" in policy
assert "if (HomeActorProtection::Resident(apThis) || HomeActorProtection::Resident(apHitter)) return false;" in actor
assert "if (NativeTrade::BlockAdd(apThis,apOldOwner)) return;" in actor
assert "if (HomeActorProtection::Resident(apThis) || HomeActorProtection::PickupBlocked(apObject)) return nullptr;" in actor
assert "if (HomeActorProtection::Resident(this)) return;" in actor
assert "if (HomeActorProtection::PickupBlocked(apObject))" in player
assert "World::Get().GetOverlayService().TryTrade(apObject->formID);" in player
assert ref.index("if (HomeActorProtection::Resident(apThis)) return false;") < ref.index("if (service.TryJoinConversation(apThis->formID))")
assert "#include <Services/HomeInstance.h>" in trade
assert "ScopedInventoryOverride::IsOverriden() && !from && HomeActorProtection::Resident(to)" in trade
assert "ScopedInventoryOverride::IsOverriden() && !to && HomeActorProtection::Resident(from)" in trade
assert "if (acMessage.Drop && !HomeActorProtection::Resident(pActor))" in inv
print("PASS projection actor inventory, pickup, drop, activation and damage hook integration")
