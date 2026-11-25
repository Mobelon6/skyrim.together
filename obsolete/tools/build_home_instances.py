"""Generate an isolated, deterministic static projection layer from the active profile.

Called by build_guest_homes.py --instances with its existing effective-record
reader. No original records are overridden. Unknown references are listed,
not fabricated or silently discarded. No quest aliases, scripts, ACHR records or worldspaces are copied.
Conservative stable NPC bindings are manifest metadata for runtime projection only.
Supported interior navigation is remapped to the generated CELL. This is a projection resource,
not a complete visitor implementation.
"""
from collections import defaultdict
import itertools
import struct
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "required/tools"))
from build_trade_plugin import record
from home_navmesh import clone_cell, nav_info, sub

PLUGIN = "skyrimcoophomeinstances.esp"
# Only static-safe base records are shared. Script-bearing bases are rejected.
# Actual containers get an empty dedicated base so replay cannot affect the
# source object's inventory or trigger its scripts.
TYPES = {b"STAT", b"MSTT", b"LIGH", b"DOOR", b"CONT", b"FURN",
         b"TREE", b"FLOR", b"ACTI", b"TACT", b"MISC", b"BOOK", b"INGR",
         b"ARMO", b"WEAP", b"AMMO", b"ALCH", b"KEYM", b"SLGM", b"APPA",
         b"SCRL", b"SOUN", b"ASPC", b"IDLM", b"SCOL", b"HAZD"}
# Engine-specific or linked reference fields are not copied; reject when they
# affect the representation. XESP and VMAD intentionally stay in the original.
UNSUPPORTED_REF = {b"XPRM", b"XTRI", b"XMBR", b"XMRK", b"XLOD", b"XAPD",
                   b"XAPR", b"XLRL", b"XEMI", b"XLYR", b"XLOC", b"XACT",
                   b"XRGD", b"XRGB", b"XALP"}
CELL_LINKS = (b"LTMP", b"XCIM", b"XCAS", b"XCWT")
CELL_BYTES = (b"XCLL", b"LNAM", b"XCLW")
def u(n):
    return struct.pack("<I", n)

def stable(key):
    plugin, local = key
    name = plugin.encode("utf-8")
    if not 0 < len(name) <= 103 or not 0 < local <= 0xffffff:
        raise ValueError("Unrepresentable stable form key")
    return name.hex() + "." + str(local)

def build(out, order, reader, grp, unpack):
    if PLUGIN in {n.casefold() for n in order}:
        raise ValueError("Exclude the previous generated layer from the input profile")
    locs, cells = {}, {}
    for kind, key, flags, _, resolve, read in reader({b"LCTN", b"CELL"}):
        if flags & 32:
            (locs if kind == b"LCTN" else cells).pop(key, None)
            continue
        data = read()
        if kind == b"LCTN":
            keywords = {resolve(n) for (n,) in struct.iter_unpack("<I", data.get(b"KWDA", b""))}
            parent = resolve(unpack(data.get(b"PNAM", bytes(4))))
            locs[key] = (("skyrim.esm", 0xfc1a3) in keywords, parent)
        elif data.get(b"DATA", b"\0")[0] & 1:
            cells[key] = (data, resolve)
        else:
            cells.pop(key, None)
    def house_location(key):
        seen = set()
        while key in locs and key not in seen:
            seen.add(key)
            yes, key = locs[key]
            if yes:
                return True
        return False
    candidates = {}
    for key, (data, resolve) in cells.items():
        loc = resolve(unpack(data.get(b"XLCN", bytes(4))))
        zone = resolve(unpack(data.get(b"XEZN", bytes(4))))
        if key != ("skyrim.esm", 0xeec55) and (house_location(loc) or zone == ("skyrim.esm", 0xf90b1)):
            candidates[key] = (data, resolve)
    if not candidates:
        raise ValueError("No recognized player-house interiors in active profile")
    # Winning references, including moves/deletions out of candidate cells.
    placed = {}
    stable_actors = {}
    def plain_actor_ref(data):
        return (set(data) <= {b"EDID", b"NAME", b"DATA", b"XSCL"} and
                len(data.get(b"NAME", b"")) == 4 and len(data.get(b"DATA", b"")) == 24 and
                len(data.get(b"XSCL", bytes(4))) == 4)
    for kind, key, flags, ctx, resolve, read in reader({b"REFR", b"ACHR", b"NAVM", b"PGRE", b"PHZD"}):
        data = None
        if kind == b"ACHR":
            if flags & 32:
                stable_actors.pop(key, None)
            else:
                data = read()
                if plain_actor_ref(data):
                    stable_actors[key] = (data, resolve)
                else:
                    stable_actors.pop(key, None)
        cell = next((resolve(unpack(g)) for g in reversed(ctx) if unpack(g[4:]) in (6,8,9,10)), None)
        if cell not in candidates or flags & 32:
            placed.pop(key, None)
            continue
        placed[key] = (cell, kind, flags, data if data is not None else read(), resolve)
    destinations = {resolve(unpack(d[b"XTEL"])) for _, kind, _, d, resolve in placed.values()
                    if kind == b"REFR" and len(d.get(b"XTEL", b"")) == 28}
    exits = {}
    for _, key, flags, ctx, resolve, read in reader({b"REFR"}):
        if key not in destinations:
            continue
        data = read()
        parent = next((resolve(unpack(g)) for g in reversed(ctx) if unpack(g[4:]) in (6,8,9,10)), None)
        if flags & 32 or not parent or parent in candidates or len(data.get(b"XTEL", b"")) != 28:
            exits.pop(key, None)
        else:
            exits[key] = (data, resolve, parent)
    wanted = {resolve(unpack(data[b"NAME"])) for _, kind, _, data, resolve in placed.values()
              if kind in (b"REFR", b"ACHR") and len(data.get(b"NAME", b"")) == 4}
    wanted.update(resolve(unpack(data[b"NAME"])) for data, resolve in stable_actors.values())
    bases = {}
    for kind, key, flags, _, resolve, read in reader(TYPES | {b"NPC_"}):
        if key not in wanted:
            continue
        if flags & 32:
            bases.pop(key, None)
        else:
            bases[key] = (kind, read(), resolve)
    def resident_base(key, data, resolve):
        if key == ("skyrim.esm", 0x14) or not plain_actor_ref(data):
            return None
        base = resolve(unpack(data[b"NAME"]))
        if (base in bases and base != ("skyrim.esm", 7) and bases[base][0] == b"NPC_" and
                not ({b"VMAD", b"TPLT"} & bases[base][1].keys())):
            return base
        return None
    # Profile-wide identity pool: metadata only, no new ACHR or NPC_ forms.
    movable = {}
    for key, (data, resolve) in stable_actors.items():
        base = resident_base(key, data, resolve)
        if base:
            try:
                movable[stable(key)] = stable(base)
            except ValueError:
                continue
    # Each source ref has a dedicated target; order is reproducible for a
    # frozen profile. Never regenerate this layer over an existing save.
    sequence = itertools.count(0x800)
    cell_ids = {k: next(sequence) for k in sorted(candidates)}
    base_ids = {}
    ref_ids = {}
    good = defaultdict(list)
    unsupported = defaultdict(list)
    residents = defaultdict(list)
    entry_links = {}
    navmeshes = defaultdict(list)
    nav_ids = {}
    masters = {"skyrim.esm"}
    for key, (cell, kind, flags, data, resolve) in sorted(placed.items()):
        if kind == b"NAVM":
            navmeshes[cell].append((key,flags,data,resolve))
            nav_ids[key]=next(sequence)
            # ONAM references shared base objects, not placed refs.
            value=data.get(b"ONAM",b"")
            if len(value)%4==0:
                for (fid,) in struct.iter_unpack("<I",value):
                    base=resolve(fid)
                    if base: masters.add(base[0])
            continue
        if kind == b"ACHR":
            # Only an explicit NPC_ base and a plain placed reference can be
            # represented. Do not copy scripts, aliases, enable parents,
            # templates, linked refs or native actor records.
            base = resident_base(key, data, resolve)
            if base and len(residents[cell]) < 128:
                residents[cell].append((key, base))
            else:
                unsupported[cell].append((key, "ACHR"))
            continue
        if kind != b"REFR":
            unsupported[cell].append((key, kind.decode("ascii")))
            continue
        base = resolve(unpack(data[b"NAME"])) if len(data.get(b"NAME", b"")) == 4 else None
        if base not in bases or bases[base][0] not in TYPES or len(data.get(b"DATA", b"")) != 24:
            unsupported[cell].append((key, "unresolved-base-or-transform"))
            continue
        typ, bd, br = bases[base]
        if b"VMAD" in bd or (set(data) & UNSUPPORTED_REF) or (typ == b"CONT" and
                (b"MODS" in bd or b"MODD" in bd or len(bd.get(b"DATA", b"")) != 5)):
            unsupported[cell].append((key, "unsupported-base-or-reference"))
            continue
        if typ == b"CONT":
            if base not in base_ids:
                base_ids[base] = next(sequence)
            for tag in (b"SNAM", b"QNAM"):
                if len(bd.get(tag, b"")) == 4:
                    k = br(unpack(bd[tag]))
                    if k: masters.add(k[0])
        else:
            masters.add(base[0])
        if b"XTEL" in data:
            other = resolve(unpack(data[b"XTEL"])) if len(data[b"XTEL"]) == 28 else None
            if typ != b"DOOR" or other not in exits:
                unsupported[cell].append((key, "unpaired-or-internal-teleport"))
                continue
            exterior, ext_resolve, parent = exits[other]
            if ext_resolve(unpack(exterior[b"XTEL"])) != key:
                unsupported[cell].append((key, "nonreciprocal-teleport"))
                continue
            masters.add(other[0]); masters.add(parent[0])
            entry_links[key] = (other, parent, exterior[b"XTEL"][4:], data[b"XTEL"][4:])
        ref_ids[key] = next(sequence)
        good[cell].append((key, base, typ, flags, data, resolve))
    for data, resolve in candidates.values():
        for tag in CELL_LINKS:
            if len(data.get(tag, b"")) == 4:
                key = resolve(unpack(data[tag]))
                if key: masters.add(key[0])
    masterlist = [n for n in order if n.casefold() in masters]
    if len(masterlist) != len(masters) or len(masterlist) > 253:
        raise ValueError("Missing master or too many dependencies")
    index = {n.casefold(): i for i, n in enumerate(masterlist)}
    own = len(masterlist) << 24
    def form(key):
        return u((index[key[0]] << 24) | key[1])
    # Keep original models and safe sound links, but no base inventory,
    # respawn, owner, scripts, keywords or unknown linked fields.
    containers = b""
    for base, ident in sorted(base_ids.items()):
        _, bd, resolve = bases[base]
        data = sub(b"EDID", f"STMHomeContainer{ident:X}\0".encode())
        data += sub(b"OBND", bd.get(b"OBND", bytes(12)))
        data += sub(b"FULL", b"Container\0")
        for tag in (b"MODL", b"MODT"):
            if tag in bd: data += sub(tag, bd[tag])
        data += sub(b"COCT", bytes(4))
        data += sub(b"DATA", bytes([bd[b"DATA"][0] & ~2]) + bd[b"DATA"][1:])
        for tag in (b"SNAM", b"QNAM"):
            if len(bd.get(tag, b"")) == 4:
                k = resolve(unpack(bd[tag]))
                if k: data += sub(tag, form(k))
        containers += record(b"CONT", 0, own | ident, data)
    groups = defaultdict(list)
    manifest = ["STM-HOME-INSTANCES 3"]
    manifest.extend(f"M {key} {base}" for key, base in sorted(movable.items()))
    nav_entries = {}
    for root, ident in sorted(cell_ids.items()):
        data, resolve = candidates[root]
        manifest.append(f"H {stable(root)} {ident}")
        cell = sub(b"EDID", f"STMHomeCell{ident:X}\0".encode())
        cell += sub(b"FULL", b"Co-op home projection\0") + sub(b"DATA", b"\x01\x00")
        for tag in CELL_BYTES:
            if tag in data: cell += sub(tag, data[tag])
        for tag in CELL_LINKS:
            if len(data.get(tag, b"")) == 4:
                k = resolve(unpack(data[tag]))
                if k: cell += sub(tag, form(k))
        temp = b""; persistent = b""
        geometry={};door_nav={}
        records=navmeshes[root]
        try:
            geometry,door_nav=clone_cell(root,records,
                {k:(d,r) for k,_,_,_,d,r in good[root] if b"XNDP" in d},
                own|ident,{k:own|v for k,v in nav_ids.items()},
                {k:own|v for k,v in ref_ids.items()},form)
            # Build NAVI before accepting any of this cell's geometry.
            entries={own|nav_ids[k]:nav_info(own|nav_ids[k],raw) for k,(_,raw) in geometry.items()}
            nav_entries.update(entries)
        except (ValueError,struct.error,KeyError,OverflowError):
            geometry={};door_nav={}
            for k,_,_,_ in records: unsupported[root].append((k,"unsupported-navmesh"))
            if not records:
                # XNDP without any matching NAVM cannot be represented.
                invalid={k for k,_,_,_,d,_ in good[root] if b"XNDP" in d}
                for k in sorted(invalid): unsupported[root].append((k,"missing-navmesh"))
                good[root]=[r for r in good[root] if r[0] not in invalid]
                for k in invalid: ref_ids.pop(k)
        for key, base, typ, flags, rd, _ in good[root]:
            newbase = (PLUGIN, base_ids[base]) if typ == b"CONT" else base
            ref = sub(b"NAME", u(own | base_ids[base]) if typ == b"CONT" else form(base))
            ref += sub(b"DATA", rd[b"DATA"])
            for tag in (b"XSCL", b"XLIG"):
                if tag in rd: ref += sub(tag, rd[tag])
            # No persistent aliases, owner, activation parents, VMAD,
            # or cross-cell linked refs except reciprocal exits. Default enable bit is
            # a starting value; owner snapshot is required before projection.
            target = ref_ids[key]
            if key in door_nav: ref += sub(b"XNDP",door_nav[key])
            if key in entry_links:
                other, parent, inside, outside = entry_links[key]
                ref += sub(b"XTEL", form(other) + outside)
                persistent += record(b"REFR", 0x400, own | target, ref)
            else:
                temp += record(b"REFR", flags & 0x800, own | target, ref)
            manifest.append(f"R {stable(root)} {stable(key)} {stable(base)} {target} {stable(newbase)}")
            # Only plain, non-persistent, non-teleport DOOR refs are advertised
            # for guest changes. Script-bearing bases were already rejected.
            if (typ == b"DOOR" and key not in entry_links and not (flags & ~0x800) and
                set(rd) <= {b"EDID", b"NAME", b"DATA", b"XSCL", b"XNDP"}):
                manifest.append(f"T {stable(root)} {stable(key)}")
            if key in entry_links:
                values = " ".join(str(n) for n in struct.unpack("<12I", inside + outside))
                manifest.append(f"D {stable(root)} {stable(other)} {stable(parent)} {target} {values}")
        for key,(payload,_) in sorted(geometry.items()):
            target=nav_ids[key]
            temp+=record(b"NAVM",0,own|target,payload)
            manifest.append(f"N {stable(root)} {stable(key)} {target}")
        for key, base in residents[root]:
            manifest.append(f"A {stable(root)} {stable(key)} {stable(base)}")
        for key, reason in unsupported[root]:
            manifest.append(f"U {stable(root)} {stable(key)} {reason}")
        cid = own | ident
        content = record(b"CELL", 0, cid, cell)
        children = (grp(8,cid,persistent) if persistent else b'') + (grp(9,cid,temp) if temp else b'')
        if children: content += grp(6, cid, children)
        groups[(ident % 10, (ident // 10) % 10)].append(content)
    blocks = b""
    for block in sorted({b for b, _ in groups}):
        blocks += grp(2, block, b"".join(grp(3, s, b"".join(rows))
                      for (b, s), rows in sorted(groups.items()) if b == block))
    data = (grp(0, b"CONT", containers) if containers else b"") + grp(0, b"CELL", blocks)
    if nav_entries:
        # NAVI has special merge semantics, shared form ID 00012FB4.
        # Only this layer's NVMI entries are emitted; no original mesh overrides.
        info=sub(b"NVER",u(12))+b"".join(sub(b"NVMI",v) for _,v in sorted(nav_entries.items()))
        info+=sub(b"NVPP",bytes(8))
        data+=grp(0,b"NAVI",record(b"NAVI",0,(index["skyrim.esm"]<<24)|0x12fb4,info))
    def count(raw):
        off = n = 0
        while off < len(raw):
            kind, size = struct.unpack_from("<4sI", raw, off)
            n += 1
            if kind == b"GRUP":
                n += count(raw[off+24:off+size])
                off += size
            else: off += 24 + size
        if off != len(raw): raise ValueError("Malformed generated record stream")
        return n
    nextid = next(sequence)
    if nextid > 0xffffff: raise ValueError("Projection layer exceeds FormID space")
    header = sub(b"HEDR", struct.pack("<fII", 1.7, count(data), nextid)) + sub(b"CNAM", b"Skyrim Together\0")
    for name in masterlist:
        header += sub(b"MAST", name.encode() + b"\0") + sub(b"DATA", bytes(8))
    targets = [out / "SkyrimCoopHomeInstances.esp", out / "CoopHomeInstances.txt"]
    if any(p.exists() for p in targets): raise FileExistsError("Refusing to replace an existing home layer")
    targets[0].write_bytes(record(b"TES4", 0, 0, header) + data)
    targets[1].write_text("\n".join(manifest) + "\n", encoding="utf-8")
    print(f"{len(cell_ids)} candidate cells, {len(ref_ids)} isolated references, "
          f"{sum(map(len,residents.values()))} resident bindings, "
          f"{sum(map(len,unsupported.values()))} unsupported records (including unrepresentable navigation/actors)")
