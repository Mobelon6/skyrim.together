"""Read-only Hearthfire evidence catalog. No entry authorizes runtime mutation."""
import json
import math
import struct
from collections import defaultdict

from build_home_instances import stable

HF = "hearthfires.esm"
# Stable vanilla entry points, not a list of individual construction objects.
ESTATES = (
    ("lakeview", 0x305e, 0x308a, 0x3092, 0x2fec, 0x4849, 0x3063),
    ("windstad", 0xbe09, 0x5209, 0xbe0a, 0x11cb3, 0x15154, 0xa535),
    ("heljarchen", 0x10bee, 0x520a, 0xbe1d, 0x16a56, 0x16a55, 0xc3b1),
)
BASE_TYPES = {b"STAT", b"MSTT", b"SCOL", b"DOOR", b"FURN", b"CONT", b"ACTI",
              b"FLOR", b"TREE", b"NPC_", b"LIGH", b"MISC", b"IDLM", b"TACT",
              b"ASPC", b"SOUN", b"XMRK", b"BOOK", b"ALCH", b"INGR", b"WEAP", b"ARMO"}


def key_text(key):
    return stable(key) if key else None


def form(data, resolve, field):
    value = data.get(field, b"")
    return resolve(struct.unpack_from("<I", value)[0]) if len(value) >= 4 else None


def text(data, field):
    return data.get(field, b"").rstrip(b"\0").decode("utf-8", "replace")


def floats(data, count):
    if len(data) != 4 * count:
        return None
    values = struct.unpack("<" + "f" * count, data)
    return list(values) if all(map(math.isfinite, values)) else None


def group_key(ctx, resolve, unpack, types):
    return next((resolve(unpack(g)) for g in reversed(ctx) if unpack(g[4:]) in types), None)


def object_properties(raw, resolve):
    """Read VMAD script table only (QUST fragment/alias tail is not interpreted)."""
    pos = 0

    def take(fmt):
        nonlocal pos
        result = struct.unpack_from("<" + fmt, raw, pos)
        pos += struct.calcsize("<" + fmt)
        return result[0] if len(result) == 1 else result

    def string():
        nonlocal pos
        length = take("H")
        if pos + length > len(raw):
            raise ValueError("truncated VMAD string")
        result = raw[pos:pos + length].decode("utf-8")
        pos += length
        return result

    def scalar(kind):
        if kind == 1:
            if fmt == 2:
                _, alias, fid = take("HHI")
            else:
                fid, alias, _ = take("IHH")
            # Preserve aliases, null and unresolved IDs rather than treating them as REFRs.
            return {"key": key_text(resolve(fid)), "alias": alias, "unresolved": bool(fid and not resolve(fid))}
        if kind == 2:
            return string()
        if kind in (3, 4, 5):
            return take({3: "i", 4: "f", 5: "B"}[kind])
        raise ValueError(f"unsupported VMAD property type {kind}")

    version, fmt, scripts = take("HHH")
    if version not in (4, 5) or fmt not in (1, 2):
        raise ValueError("unsupported VMAD header")
    result = {}
    for _ in range(scripts):
        name = string().casefold()
        _, count = take("BH")
        if name in result:
            raise ValueError("duplicate VMAD script")
        props = result[name] = {}
        seen = set()
        for _ in range(count):
            prop = string()
            kind, _ = take("BB")
            if prop.casefold() in seen:
                raise ValueError("duplicate VMAD property")
            seen.add(prop.casefold())
            if 11 <= kind <= 15:
                n = take("I")
                if n > len(raw) - pos:
                    raise ValueError("truncated VMAD array")
                value = [scalar(kind - 10) for _ in range(n)]
            else:
                value = scalar(kind)
            if kind in (1, 11):
                props[prop] = value if kind == 11 else [value]
    return result


def collect(reader, unpack):
    quests = {(HF, e[1]) for e in ESTATES}
    cells, locations, quest_rows = {}, {}, {}
    for kind, key, flags, ctx, resolve, read in reader({b"CELL", b"LCTN", b"QUST"}):
        if kind == b"QUST" and key not in quests:
            continue
        dest = {b"CELL": cells, b"LCTN": locations, b"QUST": quest_rows}[kind]
        dest.pop(key, None)
        if flags & 32:
            continue
        d = read()
        if kind == b"CELL":
            grid = d.get(b"XCLC", b"")
            dest[key] = {"editor_id": text(d, b"EDID"), "location": key_text(form(d, resolve, b"XLCN")),
                         "world": key_text(group_key(ctx, resolve, unpack, {1})),
                         "interior": bool(d.get(b"DATA", b"\0")[0] & 1),
                         "grid": list(struct.unpack_from("<ii", grid)) if len(grid) >= 8 else None}
        elif kind == b"LCTN":
            dest[key] = {"editor_id": text(d, b"EDID"), "parent": key_text(form(d, resolve, b"PNAM"))}
        else:
            props = object_properties(d[b"VMAD"], resolve).get("byohhousescript")
            if not props:
                raise ValueError(f"missing BYOHHouseScript: {key}")
            dest[key] = {"editor_id": text(d, b"EDID"), "properties": props}
    def belongs(location, root):
        seen = set()
        while location and location not in seen:
            if location == root:
                return True
            seen.add(location)
            location = location_by_text.get(location, {}).get("parent")
        return False

    location_by_text = {stable(k): v for k, v in locations.items()}
    cell_by_text = {stable(k): v for k, v in cells.items()}
    seed_keys, interior_keys, estates = set(), set(), []
    for name, q, outer, inner, house, basement, table in ESTATES:
        q, outer, inner, house, basement, table = [(HF, x) for x in (q, outer, inner, house, basement, table)]
        if q not in quest_rows or outer not in locations or inner not in locations:
            raise ValueError(f"missing estate anchors: {name}")
        props = quest_rows[q]["properties"]
        direct = {k: v for k, v in props.items() if "enablelist" in k.casefold() or "disablelist" in k.casefold()}
        if not direct or "SiteEnableMarker" not in props:
            raise ValueError(f"missing construction properties: {name}")
        seeds = {x["key"] for v in direct.values() for x in v if x["alias"] == 65535 and x["key"]}
        # Other direct quest links are evidence, not construction instructions.
        support = {x["key"] for v in props.values() for x in v if x["alias"] == 65535 and x["key"]}
        marker = props["SiteEnableMarker"]
        if len(marker) != 1 or marker[0]["alias"] != 65535 or not marker[0]["key"]:
            raise ValueError(f"invalid site marker: {name}")
        interiors = {stable(k) for k, v in cells.items() if v["interior"] and belongs(v["location"], stable(inner))}
        if not {stable(house), stable(basement)} <= interiors:
            raise ValueError(f"missing or reassigned main/basement cell: {name}")
        named_exterior = {stable(k) for k, v in cells.items() if not v["interior"] and belongs(v["location"], stable(outer))}
        seed_keys.update(support | {stable(table)})
        interior_keys.update(interiors)
        estates.append({"name": name, "key": stable(q), "outer_location": stable(outer), "inner_location": stable(inner),
                        "house": stable(house), "basement": stable(basement), "drafting_table": stable(table),
                        "site_marker": marker[0]["key"], "construction": sorted(seeds), "quest_links": sorted(support),
                        "properties": props, "interiors": sorted(interiors), "named_exterior_cells": sorted(named_exterior),
                        "approved_boundary": None, "approved_arrival": None})

    refs, parents = {}, {}

    def ref_row(kind, flags, ctx, resolve, d):
        cell = key_text(group_key(ctx, resolve, unpack, {6, 8, 9, 10}))
        world = key_text(group_key(ctx, resolve, unpack, {1}))
        pose = floats(d.get(b"DATA", b""), 6)
        xp = d.get(b"XESP", b"")
        xt = d.get(b"XTEL", b"")
        return {"kind": kind.decode(), "cell": cell, "world": world, "base": key_text(form(d, resolve, b"NAME")),
                "editor_id": text(d, b"EDID"), "flags": flags, "pose": pose,
                "scale": floats(d.get(b"XSCL", struct.pack("<f", 1.0)), 1),
                "parent": key_text(form(d, resolve, b"XESP")),
                "parent_flags": unpack(xp[4:]) if len(xp) == 8 else None,
                "teleport": key_text(form(d, resolve, b"XTEL")),
                "teleport_pose": floats(xt[4:28], 6) if len(xt) == 32 else None,
                "teleport_flags": unpack(xt[28:]) if len(xt) == 32 else None,
                "fields": sorted(k.decode() for k in d)}

    # Last record wins, including deletion and relocation. Keep all parent edges,
    # but only retain detailed rows for seeds and estate-interior teleport doors.
    for kind, key, flags, ctx, resolve, read in reader({b"REFR", b"ACHR"}):
        k = stable(key)
        refs.pop(k, None)
        parents.pop(k, None)
        if flags & 32:
            continue
        d = read()
        parent = key_text(form(d, resolve, b"XESP"))
        if parent:
            parents[k] = parent
        cell = key_text(group_key(ctx, resolve, unpack, {6, 8, 9, 10}))
        if k in seed_keys or (cell in interior_keys and b"XTEL" in d):
            refs[k] = ref_row(kind, flags, ctx, resolve, d)
    initial = set(refs)
    extra = {v[f] for v in refs.values() for f in ("parent", "teleport") if v[f]}
    extra.update(k for k, parent in parents.items() if parent in initial)
    extra -= initial
    for kind, key, flags, ctx, resolve, read in reader({b"REFR", b"ACHR"}):
        k = stable(key)
        if k not in extra:
            continue
        refs.pop(k, None)
        if not flags & 32:
            refs[k] = ref_row(kind, flags, ctx, resolve, read())
    children = defaultdict(list)
    for child, parent in parents.items():
        if parent in refs:
            children[parent].append(child)
    bases = {}
    wanted = {r["base"] for r in refs.values() if r["base"]}
    for kind, key, flags, _, resolve, read in reader(BASE_TYPES):
        k = stable(key)
        if k not in wanted:
            continue
        bases.pop(k, None)
        if not flags & 32:
            d = read()
            bases[k] = {"kind": kind.decode(), "editor_id": text(d, b"EDID"),
                        "model": text(d, b"MODL"), "flags": flags, "scripted": b"VMAD" in d}
    memberships = defaultdict(set)
    for e in estates:
        for k in e["construction"] + e["interiors"]:
            memberships[k].add(e["name"])
    for e in estates:
        table, marker = refs.get(e["drafting_table"]), refs.get(e["site_marker"])
        if (not table or not marker or table["parent"] != e["site_marker"] or not table["world"] or not table["pose"] or
                marker["world"] != table["world"] or bases.get(table["base"], {}).get("kind") != "FURN"):
            raise ValueError(f"unresolved or changed drafting table binding: {e['name']}")
        e["construction_missing"] = sorted(set(e["construction"]) - refs.keys())
        e["construction_shared"] = sorted(k for k in e["construction"] if len(memberships[k]) > 1)
        e["construction_foreign_interior"] = sorted(k for k in e["construction"] if k in refs and
            refs[k]["cell"] in interior_keys and refs[k]["cell"] not in e["interiors"])
        pairs = []
        for k, row in sorted(refs.items()):
            if row["cell"] not in e["interiors"] or not row["teleport"]:
                continue
            target = refs.get(row["teleport"])
            if (target and target["teleport"] == k and row["teleport_pose"] and target["teleport_pose"] and
                    row["teleport_flags"] == target["teleport_flags"] == 0 and
                    bases.get(row["base"], {}).get("kind") == bases.get(target["base"], {}).get("kind") == "DOOR"):
                # Directional entries retain distinct arrival poses.
                pairs.append([k, row["teleport"]])
        e["reciprocal_door_candidates"] = pairs
    used_cells = set(v["cell"] for v in refs.values())
    for e in estates:
        used_cells.update(e["interiors"] + e["named_exterior_cells"])
    return {"schema": "STM-ESTATE-EVIDENCE-1", "runtime_ready": False, "estates": estates,
            "refs": refs, "bases": bases, "cells": {k: v for k, v in cell_by_text.items() if k in used_cells},
            "enable_children": {k: sorted(v) for k, v in children.items()}}


def compact_index(catalog):
    """One owner-independent estate identity, with no approved boundary."""
    rows = ["STM-ESTATE-INDEX 1"]
    seen = set()
    for e in sorted(catalog["estates"], key=lambda v: v["key"]):
        table = catalog["refs"][e["drafting_table"]]
        rooms = e["interiors"]
        if e["house"] not in rooms or len(rooms) > 128 or seen.intersection(rooms):
            raise ValueError("ambiguous estate interior membership")
        seen.update(rooms)
        rows.append(" ".join(("E", e["key"], e["house"], table["world"],
                              e["drafting_table"], e["site_marker"])))
        rows.extend(" ".join(("I", e["key"], room)) for room in sorted(rooms))
        # Diagnostic bounds of original anchor points, NOT terrain/mesh bounds.
        poses = [table["pose"]]
        poses.extend(catalog["refs"][k]["pose"] for k in e["construction"]
                     if k in catalog["refs"] and catalog["refs"][k]["world"] == table["world"]
                     and catalog["refs"][k]["pose"])
        e["anchor_extent_xy"] = [min(p[0] for p in poses), min(p[1] for p in poses),
                                 max(p[0] for p in poses), max(p[1] for p in poses)]
    return "\n".join(rows) + "\n"


def build(out, reader, unpack, order):
    catalog = collect(reader, unpack)
    catalog["plugin_order"] = [n.casefold() for n in order]
    index = compact_index(catalog)
    if any((out / name).exists() for name in ("CoopEstateEvidence.json", "CoopEstateIndex.txt")):
        raise FileExistsError("estate output already exists")
    with (out / "CoopEstateEvidence.json").open("x", encoding="utf-8") as f:
        json.dump(catalog, f, ensure_ascii=False, sort_keys=True, indent=2, allow_nan=False)
        f.write("\n")
    with (out / "CoopEstateIndex.txt").open("x", encoding="utf-8", newline="\n") as f:
        f.write(index)
    print(f"Estate evidence: {len(catalog['estates'])} estates, {len(catalog['refs'])} reference rows; not runtime-ready")
