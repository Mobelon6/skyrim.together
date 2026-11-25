"""Classify original references, without generating any game records."""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "obsolete/tools"))
from build_home_instances import stable

GENERATED = {"skyrimcoophomeinstances.esp", "skyrimcoopguesthomes.esp"}
DOOR_FIELDS = {b"EDID", b"NAME", b"DATA", b"XSCL", b"XNDP"}
SEAT_FIELDS = {b"EDID", b"NAME", b"DATA", b"XSCL", b"XESP"}


def classify(reader, unpack):
    cells, bases, locations, cell_locs = set(), {}, set(), {}
    for kind, key, flags, _, resolve, read in reader({b"CELL", b"DOOR", b"FURN", b"NPC_", b"LCTN"}):
        if kind == b"LCTN":
            locations.discard(key)
            keywords = read().get(b"KWDA", b"")
            if not flags & 32 and len(keywords) % 4 == 0 and any(resolve(unpack(keywords[i:i+4])) == ("skyrim.esm", 0xfc1a3) for i in range(0, len(keywords), 4)):
                locations.add(key)
            continue
        if kind == b"CELL":
            cells.discard(key)
            cell_locs.pop(key, None)
            d = read()
            if not flags & 32 and d.get(b"DATA", b"\0")[0] & 1:
                cells.add(key)
                if len(d.get(b"XLCN", b"")) == 4:
                    cell_locs[key] = resolve(unpack(d[b"XLCN"]))
            continue
        bases.pop(key, None)
        if flags & 32:
            continue
        d = read()
        if {b"VMAD", b"DEST", b"DSTD"} & d.keys():
            continue
        if kind == b"DOOR":
            bases[key] = "D"
        elif kind == b"NPC_":
            if b"TPLT" not in d:
                bases[key] = "A"
        elif not flags & ((1 << 7) | (1 << 23)):
            marker = d.get(b"MNAM", b"")
            bench = d.get(b"WBDT", b"")
            spell = d.get(b"NAM1", b"\0" * 4)
            if len(marker) != 4 or len(bench) != 2 or bench[0] or spell != b"\0" * 4:
                continue
            mask = unpack(marker)
            if mask & (1 << 30) and mask & 0xffffff and not mask & ((1 << 31) | (1 << 25) | (1 << 26)):
                bases[key] = "S"
    rows, parents, links = {}, {}, {}
    for kind, key, flags, ctx, resolve, read in reader({b"REFR", b"ACHR"}):
        rows.pop(key, None)
        links.pop(key, None)
        parents.pop(key, None)
        if flags & 32:
            continue
        d = read()
        if len(d.get(b"XESP", b"")) >= 4:
            parents[key] = resolve(unpack(d[b"XESP"]))
        allowed_flags = 0xc00 if kind == b"ACHR" or b"XTEL" in d else 0x800
        if flags & ~allowed_flags or len(d.get(b"NAME", b"")) != 4 or len(d.get(b"DATA", b"")) != 24:
            continue
        root = next((resolve(unpack(g)) for g in reversed(ctx)
                     if unpack(g[4:]) in (6, 8, 9, 10)), None)
        base = resolve(unpack(d[b"NAME"]))
        tag = bases.get(base)
        if not root or not tag or any(k[0] in GENERATED for k in (root, key, base)):
            continue
        if tag != "A" and root not in cells:
            continue
        if (kind == b"ACHR") != (tag == "A"):
            continue
        if tag == "A":
            if key == ("skyrim.esm", 0x14) or base == ("skyrim.esm", 7):
                continue
            if set(d) <= {b"EDID", b"NAME", b"DATA", b"XSCL"} and len(d.get(b"XSCL", bytes(4))) == 4:
                rows[key] = ("A" if root in cells else "M"), root, key, base
        elif tag == "D" and len(d.get(b"XTEL", b"")) == 32 and unpack(d[b"XTEL"][28:]) == 0 and set(d) <= DOOR_FIELDS | {b"XTEL"}:
            target = resolve(unpack(d[b"XTEL"]))
            if target and target != key:
                links[key] = (root, target)
        elif set(d) <= (DOOR_FIELDS if tag == "D" else SEAT_FIELDS):
            rows[key] = tag, root, key, base
    controlled = set(parents.values())
    # Admit only mutually linked doors in different interiors with the same explicit player-house location.
    for key, (root, target) in links.items():
        other = links.get(target)
        loc = cell_locs.get(root)
        if (other and other[1] == key and other[0] != root and loc in locations and
                cell_locs.get(other[0]) == loc and key not in controlled and target not in controlled):
            rows[key] = ("L", root, key, target)
    return sorted(row for k, row in rows.items() if row[0] not in ("D", "A", "M") or k not in controlled)


def build(out, reader, unpack):
    rows = classify(reader, unpack)
    with (out / "CoopNativeHomeObjects.txt").open("x", encoding="utf-8") as f:
        f.write("STM-NATIVE-HOME-OBJECTS 1\n")
        for tag, root, ref, base in rows:
            f.write(f"{tag} {stable(root)} {stable(ref)} {stable(base)}\n")
    print(f"Native capability metadata: {len(rows)} original references")
