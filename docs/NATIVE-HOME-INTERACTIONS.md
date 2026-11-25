# Campaign33: native home interactions (experimental)
Generate metadata from the identical effective MO2 plugin profile on both clients:
```
python tools/build_guest_homes.py SNAPSHOT.zip loadorder.txt plugins.txt OUTPUT --native-capabilities
```
Install only OUTPUT/CoopNativeHomeObjects.txt into Data/SkyrimTogetherReborn/config.
No ESP or geometry is generated. Regenerate after profile changes and restart both clients.
Metadata is conservative: ordinary doors without teleports, scripts or enable dependencies; simple non-scripted sit-only furniture, not beds or crafting stations. Runtime also checks the original reference/base/cell and native occupancy.
Doors use existing owner publication and authoritative state. Seating uses native local activation only; remote occupancy, simultaneous seat use and late join are not solved. Guest exit and save wait for standing. These paths need game tests. Profile fingerprint enforcement and runtime script attachment detection are not implemented.

Campaign33 repair: corrected patch context, classify the active original guest cell as managed even during route teardown, suppress outbound unscoped activation traffic in managed interiors, keep the existing inbound guard, and use the existing actor-state sit/sleep field before save/restore. Exit from seating requests standing once. Added parser and activation isolation checks; remote seat reservation and replay are still not implemented. CI/runtime status must be checked against the current revision.
