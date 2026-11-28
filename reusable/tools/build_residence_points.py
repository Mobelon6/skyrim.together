"""Derive personal lodging access candidates from winning records, not place names."""
import math
import struct
from collections import defaultdict

PLAYER = ('skyrim.esm', 7)
PLAYER_BED = ('skyrim.esm', 991347)  # PlayerBedOwnership FACT
HOUSE_BED = ('skyrim.esm', 988118)   # HouseBedRefType LCRT
HOUSE_CONTAINER = ('skyrim.esm', 462945)  # HouseContainerRefType LCRT
MERCHANT_CONTAINER = ('skyrim.esm', 817941)
CITY_HOMES = {('skyrim.esm', n) for n in (91560, 92678, 93149, 93690, 92024)}

def stable(key):
    return '-' if key is None else key[0].encode('utf-8').hex() + '.' + str(key[1])

def form(d, tag, resolve, unpack):
    v = d.get(tag, b'')
    return resolve(unpack(v)) if len(v) == 4 else None

def loc_types(d, resolve, unpack):
    v = d.get(b'XLRT', b'')
    return {resolve(unpack(v[i:i+4])) for i in range(0, len(v), 4)} if len(v) % 4 == 0 else set()

def rent_bed(vmad, resolve, unpack):
    """VMAD script object properties; never scan for an arbitrary FormID."""
    if len(vmad) < 6:
        return None
    try:
        _, fmt, count = struct.unpack_from('<HHH', vmad)
        if fmt not in (1, 2):
            return None
        p = 6
        def take(n):
            nonlocal p
            if n < 0 or p+n > len(vmad):
                raise ValueError('truncated VMAD')
            b = vmad[p:p+n]; p += n
            return b
        def string():
            return take(struct.unpack('<H', take(2))[0]).decode('utf-8').casefold()
        def value(t):
            if t == 1:
                b = take(8)
                return resolve(unpack(b[4:] if fmt == 2 else b[:4]))
            if t == 2: return string()
            if t in (3, 4): return take(4)
            if t == 5: return take(1)
            if 11 <= t <= 15:
                n = unpack(take(4))
                if n > 65536: raise ValueError('array')
                for _ in range(n): value(t-10)
                return None
            raise ValueError('unknown VMAD type')
        found = set()
        for _ in range(count):
            name = string(); take(1)
            for _ in range(struct.unpack('<H', take(2))[0]):
                prop = string(); t = take(1)[0]; take(1)
                val = value(t)
                if name == 'rentroomscript' and prop == 'bed' and t == 1 and val:
                    found.add(val)
        return next(iter(found)) if len(found) == 1 else None
    except (ValueError, UnicodeError, struct.error):
        return None

def classify(reader, unpack):
    cells, bases, keepers, factions, excluded, quest_refs = {}, {}, {}, set(), {}, set()
    for kind, key, flags, _, resolve, read in reader({b'CELL', b'FURN', b'CONT', b'NPC_', b'FACT', b'QUST'}):
        d = read()
        if kind == b'QUST':
            for raw in d.get(b'ALFR_ALL', [d.get(b'ALFR', b'')]):
                if len(raw) == 4: quest_refs.add(resolve(unpack(raw)))
            continue
        if kind == b'FACT':
            factions.discard(key); excluded.pop(key, None)
            if not flags & 32:
                factions.add(key)
                excluded[key] = {form(d, t, resolve, unpack) for t in (b'VENC', b'STOL', b'PLCN')}
            continue
        if kind == b'CELL':
            cells.pop(key, None)
            if not flags & 32 and d.get(b'DATA', b'\0')[0] & 1:
                cells[key] = form(d, b'XOWN', resolve, unpack)
            continue
        if kind == b'NPC_':
            keepers.pop(key, None)
            if not flags & 32:
                bed = rent_bed(d.get(b'VMAD', b''), resolve, unpack)
                if bed: keepers[key] = bed
            continue
        bases.pop(key, None)
        if flags & 32: continue
        if kind == b'FURN':
            marker = d.get(b'MNAM', b'')
            if len(marker) == 4 and unpack(marker) & 0x80000000:
                bases[key] = 'bed'
        elif not {b'VMAD', b'DEST', b'DSTD'} & d.keys():
            name = d.get(b'EDID', b'').lower()
            if any(x in name for x in (b'merchant', b'quest', b'confiscat', b'playercontainernoresetold', b'boss')): continue
            if b'chest' in name and b'drawers' not in name: bases[key] = 'chest'
            elif any(x in name for x in (b'dresser', b'wardrobe', b'cupboard', b'drawers', b'nighttable', b'endtable')):
                bases[key] = 'furniture'
    excluded_refs = set().union(*excluded.values()) if excluded else set()
    refs = {}
    for _, key, flags, ctx, resolve, read in reader({b'REFR'}):
        refs.pop(key, None)
        if flags & 32: continue
        cell = next((resolve(unpack(g)) for g in reversed(ctx) if unpack(g[4:]) in (6,8,9,10)), None)
        if cell not in cells: continue
        d = read(); base = form(d, b'NAME', resolve, unpack)
        if base not in bases or len(d.get(b'DATA', b'')) != 24: continue
        pos = struct.unpack_from('<3f', d[b'DATA'])
        if not all(map(math.isfinite, pos)): continue
        tags = loc_types(d, resolve, unpack)
        refs[key] = (cell, base, bases[base], pos, form(d, b'XOWN', resolve, unpack), tags,
                     not bool({b'VMAD',b'XLOC',b'XLKR',b'XLRL',b'XACT',b'XSPC'} & d.keys()))
    beds, stores = defaultdict(list), defaultdict(list)
    for key, r in refs.items():
        if r[2] == 'bed': beds[r[0]].append((key, r))
        elif r[6] and key not in excluded_refs and key not in quest_refs and MERCHANT_CONTAINER not in r[5]:
            stores[r[0]].append((key, r))
    rental = set(keepers.values())
    rows = []
    for cell, group in beds.items():
        if cell in CITY_HOMES or cell[0] == 'hearthfires.esm': continue
        used = set()
        for bed, b in sorted(group):
            if bed in used: continue
            alternatives = [(k, r) for k, r in group if k != bed and r[4] == b[4] and math.dist(b[3], r[3]) <= 16]
            if len(alternatives) > 1: continue
            alt = alternatives[0][0] if alternatives else None
            used.update((bed, alt))
            is_rental = bed in rental or alt in rental
            owner = b[4]
            explicit_player = owner in (PLAYER, PLAYER_BED)
            tagged = HOUSE_BED in b[5]
            role = owner in factions and owner != cells[cell]
            if not (is_rental or explicit_player or tagged or role): continue
            options = []
            for ref, c in stores[cell]:
                distance = math.dist(b[3], c[3])
                if distance > 300 or abs(b[3][2]-c[3][2]) > 96: continue
                if any(k not in (bed,alt) and math.dist(r[3],c[3])+32 < distance for k,r in group): continue
                if any(k not in (bed,alt) and abs(math.dist(r[3],c[3])-distance)<32 for k,r in group): continue
                co=c[4]
                same=owner is not None and co==owner
                if is_rental:
                    if co not in (None, PLAYER, cells[cell]): continue
                elif not (same or (tagged and HOUSE_CONTAINER in c[5]) or (explicit_player and co is None)):
                    continue
                if co and not (same or co==PLAYER or (is_rental and co==cells[cell])):continue
                if role and not explicit_player and not tagged and any(k not in (bed,alt) and r[4]==owner for k,r in group):continue
                priority=0 if HOUSE_CONTAINER in c[5] else 1 if c[2]=='chest' else 2
                options.append((priority,distance,ref,c))
            options.sort(key=lambda x:(x[0],x[1],x[2]))
            if not options: continue
            # All candidates passed competing-bed and ownership checks;
            # choose one suitable furnishing in the same allocated room.
            _,_,ref,c=options[0]
            rows.append(('R' if is_rental else 'G',cell,bed,alt,ref,c[1],c[3]))
    counts=defaultdict(int)
    for row in rows: counts[row[4]]+=1
    return sorted(r for r in rows if counts[r[4]]==1),sorted(keepers)

def encode(rows, keepers):
    lines=['STM-RESIDENCE-POINTS 1']
    lines += ['K '+stable(k) for k in keepers]
    for tag,cell,bed,alt,ref,base,pos in rows:
        lines.append(' '.join([tag,*map(stable,(cell,bed,alt,ref,base)),*(format(x,'.9g') for x in pos)]))
    return '\n'.join(lines)+'\n'

def build(out,reader,unpack):
    rows,keepers=classify(reader,unpack)
    with (out/'CoopResidenceIndex.txt').open('x',encoding='utf-8') as f:
        f.write(encode(rows,keepers))
    print(f'Residence evidence: {len(rows)} storage points, {len(keepers)} rental NPC bases')
