"""Validate every generated projection binding against the actual plugin."""
from pathlib import Path
import struct
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from home_navmesh import Geometry, nav_info

def validate(root):
    records = {}
    counts = 0
    def walk(raw, cell=None):
        nonlocal counts
        off = 0
        while off < len(raw):
            tag, size, flags, fid = struct.unpack_from("<4sIII", raw, off)
            counts += 1
            if tag == b"GRUP":
                assert 24 <= size <= len(raw)-off
                walk(raw[off+24:off+size], flags if fid in (6,8,9,10) else cell)
                off += size
                continue
            end = off+24+size
            assert end <= len(raw) and fid not in records
            fields = []
            p = off+24
            while p < end:
                t, n = struct.unpack_from("<4sH", raw, p)
                p += 6
                if t==b"XXXX":
                    assert n==4 and p+10<=end
                    n=struct.unpack_from("<I",raw,p)[0];t=raw[p+4:p+8];p+=10
                assert p+n <= end
                fields.append((t, raw[p:p+n]))
                p += n
            records[fid] = tag, fields, cell
            off = end
        assert off == len(raw)
    walk((root/"SkyrimCoopHomeInstances.esp").read_bytes())
    masters = [x.rstrip(b"\0").decode().casefold() for t,x in records[0][1] if t==b"MAST"]
    assert masters[0] == "skyrim.esm"
    assert struct.unpack_from("<I",dict(records[0][1])[b"HEDR"],4)[0] == counts-1
    own = len(masters)<<24
    assert all(fid>>24 == len(masters) or (fid==0x12fb4 and records[fid][0]==b"NAVI") for fid in records if fid)
    assert {r[0] for r in records.values()} <= {b"TES4",b"CELL",b"CONT",b"REFR",b"NAVM",b"NAVI"}
    def key(s):
        name, ident = s.split(".")
        return bytes.fromhex(name).decode(), int(ident)
    cells, refs, sources, unknown = {}, set(), set(), set()
    exits, entrances, navigation, residents = set(), set(), set(), set()
    lines = (root/"CoopHomeInstances.txt").read_text().splitlines()
    version=lines.pop(0)
    assert version in ("STM-HOME-INSTANCES 2", "STM-HOME-INSTANCES 3")
    movable={}
    source_refs={}
    guest_doors=set()
    for line in lines:
        t,*f = line.split()
        if t=="M":
            assert version=="STM-HOME-INSTANCES 3"
            source, base = f
            assert source not in movable
            for encoded in (source,base):
                plugin, ident=key(encoded)
                assert plugin!="skyrimcoophomeinstances.esp" and 0<ident<=0xffffff
            movable[source]=base
        elif t=="H":
            r, c = f
            assert r not in cells and records[own|int(c)][0]==b"CELL"
            cells[r] = own|int(c)
        elif t=="R":
            r, source, original, dst, base = f
            refid = own|int(dst)
            typ, fields, cell = records[refid]
            fields = dict(fields)
            assert typ==b"REFR" and cell==cells[r] and refid not in refs and source not in sources
            assert set(fields) <= {b"NAME",b"DATA",b"XSCL",b"XLIG",b"XTEL",b"XNDP"}
            name, ident = key(base)
            fid = (own if name=="skyrimcoophomeinstances.esp" else masters.index(name)<<24)|ident
            assert struct.unpack("<I",fields[b"NAME"])[0]==fid
            if fid>>24==len(masters):
                kind, fields, _ = records[fid]
                fields = dict(fields)
                assert kind==b"CONT" and fields[b"COCT"]==bytes(4)
                assert not ({b"VMAD",b"CNTO",b"COED"}&fields.keys())
            else: assert key(original)==key(base)
            refs.add(refid);sources.add(source);source_refs[source]=(r,refid)
        elif t=="T":
            assert version=="STM-HOME-INSTANCES 3"
            r,source=f
            assert source not in guest_doors and source in source_refs
            parent,target=source_refs[source]
            assert parent==r and b"XTEL" not in dict(records[target][1])
            guest_doors.add(source)
        elif t=="N":
            r,source,target=f
            fid=own|int(target)
            assert source not in sources and fid not in navigation
            kind,fields,cell=records[fid]
            assert kind==b"NAVM" and cell==cells[r]
            assert Geometry(dict(fields)[b"NVNM"]).parent==cell
            navigation.add(fid);sources.add(source)
        elif t=="D":
            root, entrance, outside, target, *bits = f
            assert len(bits)==12 and entrance not in entrances
            dest = own|int(target)
            assert dest in refs and dest not in exits
            kind, fields, cell = records[dest]
            assert kind==b"REFR" and cell==cells[root]
            name, ident = key(entrance)
            expected = (masters.index(name)<<24)|ident
            assert dict(fields)[b"XTEL"]==struct.pack("<I6I",expected,*map(int,bits[6:]))
            key(outside)
            exits.add(dest);entrances.add(entrance)
        elif t=="A":
            r, source, base = f
            assert r in cells and source not in sources and source not in unknown
            for encoded in (source,base):
                plugin, ident=key(encoded)
                assert plugin!="skyrimcoophomeinstances.esp" and 0<ident<=0xffffff
            if version=="STM-HOME-INSTANCES 3": assert movable.get(source)==base
            sources.add(source);residents.add(source)
        else:
            assert t=="U"
            r, source, reason = f
            assert r in cells and source not in unknown and source not in sources
            unknown.add(source)
    assert set(cells.values())=={fid for fid,r in records.items() if r[0]==b"CELL"}
    assert refs=={fid for fid,r in records.items() if r[0]==b"REFR"}
    assert exits=={fid for fid in refs if b"XTEL" in dict(records[fid][1])}
    assert navigation=={fid for fid,r in records.items() if r[0]==b"NAVM"}
    if navigation:
        meshes={fid:Geometry(dict(records[fid][1])[b"NVNM"]) for fid in navigation}
        doors={}
        for fid,m in meshes.items():
            for p in m.edges:
                _,dest,tri=struct.unpack_from("<IIh",m.raw,p)
                assert dest in meshes and meshes[dest].parent==m.parent and 0<=tri<len(meshes[dest].faces)
            for p in m.doors:
                tri,_,ref=struct.unpack_from("<hII",m.raw,p)
                assert ref in refs and records[ref][2]==m.parent and ref not in doors
                assert struct.unpack("<IhH",dict(records[ref][1])[b"XNDP"])==(fid,tri,0)
                doors[ref]=fid
        assert set(doors)=={r for r in refs if b"XNDP" in dict(records[r][1])}
        info=records[0x12fb4]
        assert info[0]==b"NAVI"
        assert [v for t,v in info[1] if t==b"NVER"]==[struct.pack("<I",12)]
        assert [v for t,v in info[1] if t==b"NVMI"]==[
            nav_info(fid,bytes(meshes[fid].raw)) for fid in sorted(navigation)]
    else: assert 0x12fb4 not in records
    return len(cells),len(refs),len(unknown)

if __name__=="__main__":
    print("PASS projection layer:",validate(Path(sys.argv[1])))
