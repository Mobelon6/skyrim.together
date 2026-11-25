"""Generate two linked interior NAVMs and a door, verify remapped binary IDs."""
from pathlib import Path
import tempfile,sys,struct,subprocess
from zipfile import ZipFile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/"required/tools"))
sys.path.insert(0,str(ROOT/"obsolete/tools"))
from build_trade_plugin import record,sub,group
from home_navmesh import CELL_CRC,Geometry
from check_home_instances_plugin import validate
u=lambda n:struct.pack("<I",n)
def header(masters=()):
    return record(b"TES4",0,0,sub(b"HEDR",struct.pack("<fII",1.7,0,0x2000))+
                  b"".join(sub(b"MAST",n.encode()+b"\0")+sub(b"DATA",bytes(8)) for n in masters))
def mesh(cell,other,door=0):
    vertices=struct.pack("<9f",0,0,0,100,0,0,0,100,0)
    triangle=struct.pack("<3H3h2H",0,1,2,0,-1,-1,1|(0x400 if door else 0),0)
    return (struct.pack("<4I",12,CELL_CRC,0,cell)+u(3)+vertices+u(1)+triangle+
            u(1)+struct.pack("<IIh",0,other,0)+u(bool(door))+
            (struct.pack("<hII",0,0x12345678,door) if door else b"")+
            u(0)+u(1)+struct.pack("<8f",100,100,0,0,0,100,100,0)+u(1)+struct.pack("<h",0))
def fixture(bad=False):
    cell=0x01000900;d=0x01000901;a=0x01000902;b=0x01000903
    sk=header()+record(b"LCTN",0,0x100,sub(b"KWDA",u(0xfc1a3)))+record(b"DOOR",0,0x102,b"")
    mod=header(("Skyrim.esm",))
    mod+=record(b"CELL",0,cell,sub(b"DATA",b"\1\0")+sub(b"XLCN",u(0x100)))
    door=record(b"REFR",0,d,sub(b"NAME",u(0x102))+sub(b"XNDP",struct.pack("<IhH",a,0,0))+sub(b"DATA",bytes(24)))
    ma=record(b"NAVM",0,a,sub(b"NVNM",mesh(cell,b,d)))
    mb=record(b"NAVM",0,b,sub(b"NVNM",mesh(cell,0x01009999 if bad else a)))
    mod+=group(6,cell,group(9,cell,door+ma+mb))
    return sk,mod
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp)
    (p/"loadorder.txt").write_text("Skyrim.esm\nExampleHouse.esp\n")
    (p/"plugins.txt").write_text("*Skyrim.esm\n*ExampleHouse.esp\n")
    for bad in (False,True):
        sk,mod=fixture(bad)
        with ZipFile(p/"profile.zip","w") as z:
            z.writestr("Skyrim.esm",sk);z.writestr("ExampleHouse.esp",mod)
        output=p/str(bad)
        subprocess.run([sys.executable,str(ROOT/"reusable/tools/build_guest_homes.py"),str(p/"profile.zip"),
                        str(p/"loadorder.txt"),str(p/"plugins.txt"),str(output),"--instances"],check=True)
        assert validate(output)==(1,1,2 if bad else 0)
        rows=(output/"CoopHomeInstances.txt").read_text().splitlines()
        assert sum(r.startswith("N ") for r in rows)==(0 if bad else 2)
        assert sum(r.startswith("U ") for r in rows)==(2 if bad else 0)
print("PASS isolated navigation, remapped internal portals, reciprocal XNDP, NAVI and rejection of external portal")
