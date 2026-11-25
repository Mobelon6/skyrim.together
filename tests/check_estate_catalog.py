"""Exercise the evidence collector against a compact winning-record stream."""
import struct
import sys
import tempfile
import json
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import build_estate_catalog as ec

u = lambda n: struct.pack("<I", n)
unpack = lambda b: struct.unpack_from("<I", b)[0]
key = lambda n: (ec.HF, n) if n else None
stable = lambda n: ec.stable(key(n))
ctx = lambda n: (u(n) + u(6),)
world = (u(90) + u(1),) + ctx(91)
s = lambda v: struct.pack("<H", len(v)) + v.encode()
obj = lambda fid, alias=65535: struct.pack("<HHI", 0, alias, fid)

def vmad(ids):
    return (struct.pack("<HHH", 5, 2, 1) + s("BYOHHouseScript") + struct.pack("<BH", 0, 2) +
            s("SiteEnableMarker") + bytes([1, 1]) + obj(7) + s("EnableList") + bytes([11, 1]) +
            u(len(ids) + 2) + b"".join(obj(n) for n in ids) + obj(0) + obj(99, 4))

def rec(kind, n, d, flags=0, context=()):
    return kind, key(n), flags, context, key, lambda: d

def ref(n, base, context, flags=0, **extra):
    return rec(b"REFR", n, {b"NAME": u(base), b"DATA": bytes(24),
                              **{k.encode(): v for k, v in extra.items()}}, flags, context)

def catalog(rows):
    return ec.collect(lambda kinds: (r for r in rows if r[0] in kinds), unpack)

# Use a single fixture estate; the production entry points remain unchanged.
ec.ESTATES = (("fixture", 1, 2, 3, 4, 5, 6),)
rows = [rec(b"LCTN", 2, {}), rec(b"LCTN", 3, {}),
        rec(b"CELL", 4, {b"DATA": b"\1", b"XLCN": u(3)}),
        rec(b"CELL", 5, {b"DATA": b"\1", b"XLCN": u(3)}),
        rec(b"CELL", 91, {b"DATA": b"\0"}, context=(u(90)+u(1),)),
        rec(b"QUST", 1, {b"VMAD": vmad([10, 11, 12])}),
        rec(b"QUST", 1, {b"VMAD": vmad([10, 11, 13])}),
        rec(b"FURN", 20, {}), rec(b"STAT", 21, {b"MODL": b"model.nif\0"}), rec(b"DOOR", 22, {}),
        ref(6, 20, world, XESP=u(7)+u(0)), ref(7, 21, world, flags=0xc00),
        ref(10, 21, world), ref(11, 21, ctx(4)), ref(11, 21, ctx(5)),
        ref(13, 21, world), ref(13, 21, world, flags=32), ref(14, 21, world, XESP=u(10)+u(1)),
        ref(15, 22, ctx(4), XTEL=u(16)+bytes(28)), ref(16, 22, world, XTEL=u(15)+bytes(28))]
a = catalog(rows)
e = a["estates"][0]
assert not a["runtime_ready"] and e["approved_boundary"] is None and e["approved_arrival"] is None
assert e["construction"] == sorted(stable(n) for n in (10, 11, 13))
assert e["construction_missing"] == [stable(13)]
complete = catalog(rows + [ref(13, 21, world)])
ci = ec.compact_index(complete).splitlines()
assert {line for line in ci if line.startswith("C ")} == {
    " ".join(("C", stable(1), stable(n), stable(b))) for n,b in ((6,20),(7,21),(10,21),(13,21))}
assert len(complete["estates"][0]["construction_observation"]) == 4
assert a["refs"][stable(11)]["cell"] == stable(5)
assert a["refs"][stable(6)]["world"] == stable(90)
assert a["refs"][stable(14)]["parent_flags"] == 1
assert stable(14) not in e["construction"] and stable(14) in a["enable_children"][stable(10)]
assert e["reciprocal_door_candidates"] == [[stable(15), stable(16)]]
assert not catalog(rows + [ref(16, 22, world, XTEL=u(15)+bytes(24)+u(1))])["estates"][0]["reciprocal_door_candidates"]
assert not catalog(rows + [ref(16, 22, world, flags=32)])["estates"][0]["reciprocal_door_candidates"]
assert not catalog(rows + [ref(16, 22, world, XTEL=u(15)+bytes(24))])["estates"][0]["reciprocal_door_candidates"]
with tempfile.TemporaryDirectory() as tmp:
    p = Path(tmp)
    ec.build(p, lambda kinds: (r for r in rows if r[0] in kinds), unpack, [ec.HF])
    assert json.loads((p / "CoopEstateEvidence.json").read_text())["plugin_order"] == [ec.HF]
    try:
        ec.build(p, lambda kinds: (r for r in rows if r[0] in kinds), unpack, [ec.HF])
    except FileExistsError:
        pass
    else:
        raise AssertionError("overwrote evidence")
    assert sorted(x.name for x in p.iterdir()) == ["CoopEstateEvidence.json", "CoopEstateIndex.txt"]
    index = (p / "CoopEstateIndex.txt").read_text().splitlines()
    assert index[0] == "STM-ESTATE-INDEX 1"
    assert index[1] == " ".join(("E", stable(1), stable(4), stable(90), stable(6), stable(7)))
    assert set(index[2:4]) == {"I "+stable(1)+" "+stable(4), "I "+stable(1)+" "+stable(5)}
    assert index[4] == "B "+stable(1)+" main 4 -128 -128 128 -128 128 128 -128 128"
    candidate = json.loads((p / "CoopEstateEvidence.json").read_text())["estates"][0]["spatial_candidate"]
    assert candidate["safe_arrival"] is None and candidate["status"] == "provisional"
    assert candidate["switch_anchor"] == stable(6)
    assert candidate["door_arrivals"][0]["source"] == stable(15)
    assert ec.hull([(0,0),(1,0),(2,0),(2,2),(0,2),(1,1)]) == [(0,0),(2,0),(2,2),(0,2)]
    assert json.loads((p / "CoopEstateEvidence.json").read_text())["estates"][0]["anchor_extent_xy"] == [0,0,0,0]
for bad in (vmad([10])[:-1], b"\0" * 6):
    try:
        ec.object_properties(bad, key)
    except (ValueError, struct.error):
        pass
    else:
        raise AssertionError("invalid VMAD accepted")
print("PASS estate winning references, VMAD, dependency evidence, XTEL, no authorization or overwrite")
