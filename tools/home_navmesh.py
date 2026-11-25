"""Skyrim SE NVNM v12 interior remapping and NAVI generation.

Format reference: xEdit Core/wbDefinitionsTES5.pas, NAVM/NAVI and REFR XNDP.
The ordinary NAVI record is Skyrim.esm:00012FB4. Each plugin contributes
its own navigation entries; never copy source-world paths into this layer.
No exterior mesh or edge pointing outside the copied cell is supported.
"""
import math
import struct

CELL_CRC = 0xA5E9A03C
def u(n): return struct.pack("<I", n)
def sub(tag, data):
    if len(data) > 65535:
        return b"XXXX\x04\x00"+u(len(data))+tag+b"\0\0"+data
    return tag+struct.pack("<H",len(data))+data

class Geometry:
    def __init__(self, raw):
        self.raw = bytearray(raw)
        self.pos = 16
        if len(raw)<20 or struct.unpack_from("<III",raw)!=(12,CELL_CRC,0):
            raise ValueError("unsupported NVNM header")
        self.parent = struct.unpack_from("<I",raw,12)[0]
        self.vertices = self.array(12)
        self.triangles = self.array(16)
        self.edges = self.array(10)
        self.doors = self.array(10)
        self.cover = self.array(2)
        self.points = [struct.unpack_from("<3f",raw,p) for p in self.vertices]
        self.faces = [struct.unpack_from("<3H3h2H",raw,p) for p in self.triangles]
        if not self.points or not self.faces or len(self.points)>65535 or len(self.faces)>32767:
            raise ValueError("empty or oversized mesh")
        if any(not math.isfinite(x) for point in self.points for x in point):
            raise ValueError("nonfinite vertex")
        for a,b,c,e0,e1,e2,flags,cover in self.faces:
            if max(a,b,c)>=len(self.points) or len({a,b,c})!=3:
                raise ValueError("bad vertex index")
            for i,edge in enumerate((e0,e1,e2)):
                if flags & (1<<i):
                    if not 0<=edge<len(self.edges): raise ValueError("bad portal index")
                elif not -1<=edge<len(self.faces): raise ValueError("bad triangle index")
        for p in self.doors:
            tri,crc,ref=struct.unpack_from("<hII",raw,p)
            if not 0<=tri<len(self.faces) or not ref or not self.faces[tri][6]&0x400:
                raise ValueError("bad door triangle")
        for p in self.cover:
            if not 0<=struct.unpack_from("<h",raw,p)[0]<len(self.faces):
                raise ValueError("bad cover triangle")
        if self.pos+36>len(raw): raise ValueError("missing grid")
        div,*bounds=struct.unpack_from("<I8f",raw,self.pos)
        if not 0<=div<=12 or any(not math.isfinite(x) for x in bounds):
            raise ValueError("bad grid")
        self.pos+=36
        for _ in range(div*div):
            for p in self.array(2):
                if not 0<=struct.unpack_from("<h",raw,p)[0]<len(self.faces):
                    raise ValueError("bad grid triangle")
        if self.pos!=len(raw): raise ValueError("trailing NVNM data")
    def array(self,width):
        if self.pos+4>len(self.raw): raise ValueError("missing array")
        count=struct.unpack_from("<I",self.raw,self.pos)[0]
        self.pos+=4
        if count>(len(self.raw)-self.pos)//width: raise ValueError("truncated array")
        start=self.pos;self.pos+=count*width
        return range(start,self.pos,width)
    def remap(self,parent,resolve,mesh_ids,mesh_sizes,door_ids,door_links):
        data=bytearray(self.raw)
        struct.pack_into("<I",data,12,parent)
        for p in self.edges:
            _,target,tri=struct.unpack_from("<IIh",data,p)
            key=resolve(target)
            if key not in mesh_ids or not 0<=tri<mesh_sizes[key]:
                raise ValueError("portal leaves isolated cell or invalid target triangle")
            struct.pack_into("<I",data,p+4,mesh_ids[key])
        for p in self.doors:
            tri,_,target=struct.unpack_from("<hII",data,p)
            key=resolve(target)
            if key not in door_ids or door_links.get(key)!=(parent,tri):
                raise ValueError("door association is not reciprocal")
            struct.pack_into("<I",data,p+6,door_ids[key])
        return bytes(data)

def clone_cell(root, records, door_records, cell_id, nav_ids, ref_ids, form):
    """Return all meshes/door associations or reject the entire cell."""
    geom={}
    for key,flags,fields,resolve in records:
        if flags & 0x800 or set(fields)-{b"EDID",b"NVNM",b"ONAM",b"PNAM",b"NNAM"}:
            raise ValueError("unsupported NAVM record")
        m=Geometry(fields.get(b"NVNM",b""))
        if resolve(m.parent)!=root: raise ValueError("wrong original parent cell")
        geom[key]=(m,fields,resolve)
    sizes={k:len(m.faces) for k,(m,_,_) in geom.items()}
    ids={k:nav_ids[k] for k in geom}
    links={}
    for key,(fields,resolve) in door_records.items():
        if b"XNDP" not in fields: continue
        value=fields[b"XNDP"]
        if len(value)!=8: raise ValueError("bad XNDP")
        target,triangle=struct.unpack_from("<Ih",value)
        mesh=resolve(target)
        if mesh not in geom or not 0<=triangle<sizes[mesh]:
            raise ValueError("door points outside isolated cell")
        links[key]=(ids[mesh],triangle)
    output={}
    seen=set()
    for key,(m,fields,resolve) in geom.items():
        # Reciprocal REFR XNDP refers to this mesh, not its CELL.
        local_links={door:(cell_id,tri) for door,(mesh,tri) in links.items() if mesh==ids[key]}
        for p in m.doors:
            tri,_,door=struct.unpack_from("<hII",m.raw,p)
            d=resolve(door)
            if d in seen or local_links.get(d)!=(cell_id,tri):
                raise ValueError("duplicate or unmatched door")
            seen.add(d)
        raw=m.remap(cell_id,resolve,ids,sizes,ref_ids,local_links)
        payload=sub(b"NVNM",raw)
        if b"ONAM" in fields:
            value=fields[b"ONAM"]
            if len(value)%4: raise ValueError("bad ONAM")
            encoded=b""
            for (fid,) in struct.iter_unpack("<I",value):
                base=resolve(fid)
                if not base: raise ValueError("unresolved navmesh base")
                encoded+=form(base)
            payload+=sub(b"ONAM",encoded)
        for tag in (b"PNAM",b"NNAM"):
            if tag in fields:
                value=fields[tag]
                if len(value)%2 or any(v>=len(m.points) for (v,) in struct.iter_unpack("<H",value)):
                    raise ValueError("bad connector list")
                payload+=sub(tag,value)
        output[key]=(payload,raw)
    if seen!=set(links): raise ValueError("XNDP without matching mesh link")
    return output,{key:struct.pack("<IhH",mesh,tri,0) for key,(mesh,tri) in links.items()}

def nav_info(ident,raw):
    """Build an NVMI row for a remapped v12 mesh, not a source-world cache."""
    m=Geometry(raw)
    linked=set();preferred=set();doors=set()
    area=preferred_area=0.0;center=[0.0]*3
    for a,b,c,e0,e1,e2,flags,_ in m.faces:
        p,q,r=(m.points[i] for i in (a,b,c))
        x=[q[i]-p[i] for i in range(3)];y=[r[i]-p[i] for i in range(3)]
        w=math.sqrt(sum((x[i]*y[j]-x[j]*y[i])**2 for i,j in ((0,1),(1,2),(2,0))))
        area+=w
        if flags&64: preferred_area+=w
        for i in range(3): center[i]+=w*(p[i]+q[i]+r[i])/3
        for i,edge in enumerate((e0,e1,e2)):
            if flags&(1<<i):
                target=struct.unpack_from("<I",raw,m.edges[edge]+4)[0]
                linked.add(target)
                if flags&64: preferred.add(target)
    for p in m.doors:
        _,crc,ref=struct.unpack_from("<hII",raw,p)
        doors.add((crc,ref))
    minimum=[min(p[i] for p in m.points) for i in range(3)]
    maximum=[max(p[i] for p in m.points) for i in range(3)]
    island=not linked and not doors
    data=struct.pack("<II4f",ident,0x20 if island else 0,
                     *([v/area for v in center] if area else minimum),
                     preferred_area/area if area else 0)
    for links in (linked,preferred):
        data+=u(len(links))+b"".join(map(u,sorted(links)))
    data+=u(len(doors))+b"".join(struct.pack("<II",*d) for d in sorted(doors))
    data+=bytes([island])
    if island:
        data+=struct.pack("<6f",*minimum,*maximum)+u(len(m.faces))
        data+=b"".join(struct.pack("<3H",*f[:3]) for f in m.faces)
        data+=u(len(m.points))+b"".join(struct.pack("<3f",*p) for p in m.points)
    return data+struct.pack("<III",CELL_CRC,0,m.parent)
