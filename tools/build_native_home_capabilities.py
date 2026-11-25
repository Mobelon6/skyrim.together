"""Classify original references, without generating any game records."""
from build_home_instances import stable

GENERATED = {"skyrimcoophomeinstances.esp", "skyrimcoopguesthomes.esp"}
DOOR_FIELDS = {b"EDID", b"NAME", b"DATA", b"XSCL", b"XNDP"}
SEAT_FIELDS = {b"EDID", b"NAME", b"DATA", b"XSCL", b"XESP"}


def classify(reader, unpack):
    cells, bases = set(), {}
    for kind, key, flags, _, resolve, read in reader({b"CELL", b"DOOR", b"FURN", b"NPC_"}):
        if kind == b"CELL":
            cells.discard(key)
            if not flags & 32 and read().get(b"DATA", b"\0")[0] & 1:
                cells.add(key)
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
    rows, parents = {}, {}
    for kind, key, flags, ctx, resolve, read in reader({b"REFR", b"ACHR"}):
        rows.pop(key, None)
        parents.pop(key, None)
        if flags & 32:
            continue
        d = read()
        if len(d.get(b"XESP", b"")) >= 4:
            parents[key] = resolve(unpack(d[b"XESP"]))
        if flags & ~0x800 or len(d.get(b"NAME", b"")) != 4 or len(d.get(b"DATA", b"")) != 24:
            continue
        root = next((resolve(unpack(g)) for g in reversed(ctx)
                     if unpack(g[4:]) in (6, 8, 9, 10)), None)
        base = resolve(unpack(d[b"NAME"]))
        tag = bases.get(base)
        if root not in cells or not tag or any(k[0] in GENERATED for k in (root, key, base)):
            continue
        if (kind == b"ACHR") != (tag == "A"):
            continue
        if set(d) <= (DOOR_FIELDS if tag in ("D", "A") else SEAT_FIELDS):
            rows[key] = tag, root, key, base
    controlled = set(parents.values())
    return sorted(row for k, row in rows.items() if row[0] not in ("D", "A") or k not in controlled)


def build(out, reader, unpack):
    rows = classify(reader, unpack)
    with (out / "CoopNativeHomeObjects.txt").open("x", encoding="utf-8") as f:
        f.write("STM-NATIVE-HOME-OBJECTS 1\n")
        for tag, root, ref, base in rows:
            f.write(f"{tag} {stable(root)} {stable(ref)} {stable(base)}\n")
    print(f"Native capability metadata: {len(rows)} original references")
