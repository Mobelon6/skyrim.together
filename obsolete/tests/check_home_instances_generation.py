"""Synthetic non-vanilla house, inherited location and actual container projection."""
from pathlib import Path
import tempfile,sys,struct,subprocess
from zipfile import ZipFile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/"required/tools"))
from build_trade_plugin import record,sub,group
from check_home_instances_plugin import validate
def u(n):return struct.pack("<I",n)
def header(masters=()):
    return record(b"TES4",0,0,sub(b"HEDR",struct.pack("<fII",1.7,0,0x1000))+
                  b"".join(sub(b"MAST",n.encode()+b"\0")+sub(b"DATA",bytes(8)) for n in masters))
def cell(ident,loc):
    return record(b"CELL",0,ident,sub(b"DATA",b"\x01\x00")+sub(b"XLCN",u(loc)))
def ref(ident,base):
    return record(b"REFR",0,ident,sub(b"NAME",u(base))+sub(b"DATA",bytes(24)))
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp)
    sk=header()+record(b"LCTN",0,0x100,sub(b"KWDA",u(0xfc1a3)))
    sk+=record(b"STAT",0,0x101,sub(b"MODL",b"fixture.nif\0"))
    sk+=record(b"DOOR",0,0x102,sub(b"MODL",b"door.nif\0"))
    sk+=record(b"NPC_",0,0x103,sub(b"EDID",b"FixtureResident\0"))
    sk+=record(b"NPC_",0,0x104,sub(b"VMAD",b"script"))
    mod=header(("Skyrim.esm",))
    mod+=record(b"LCTN",0,0x01000800,sub(b"PNAM",u(0x100)))
    mod+=record(b"CONT",0,0x01000801,sub(b"OBND",bytes(12))+sub(b"COCT",u(1))+
                sub(b"CNTO",u(0x101)+u(5))+sub(b"DATA",bytes(5)))
    mod+=record(b"ACTI",0,0x01000802,sub(b"VMAD",b"script"))
    mod+=cell(0x01000900,0x01000800)+group(6,0x01000900,group(9,0x01000900,
        ref(0x01000901,0x101)+ref(0x01000902,0x01000801)+ref(0x01000903,0x01000802)+
        record(b"ACHR",0,0x01000904,b"")+record(b"NAVM",0,0x01000905,b"")+
        record(b"ACHR",0,0x01000907,sub(b"NAME",u(0x103))+sub(b"DATA",bytes(24)))+
        record(b"ACHR",0,0x01000908,sub(b"NAME",u(0x104))+sub(b"DATA",bytes(24)))+
        ref(0x01000914,0x102)+
        record(b"REFR",0,0x01000915,sub(b"NAME",u(0x102))+sub(b"DATA",bytes(24))+sub(b"VMAD",b"script"))))
    # Non-house interior must not appear.
    mod+=cell(0x01000910,0)
    mod+=group(6,0x01000910,group(9,0x01000910,
        record(b"ACHR",0,0x01000912,sub(b"NAME",u(0x103))+sub(b"DATA",bytes(24)))+
        record(b"ACHR",0,0x01000913,sub(b"NAME",u(0x104))+sub(b"DATA",bytes(24)))))
    def teleport(ident,other):
        return record(b"REFR",0x400,ident,sub(b"NAME",u(0x102))+sub(b"DATA",bytes(24))+
                      sub(b"XTEL",u(other)+struct.pack("<6f",1,2,3,0,0,0)))
    mod+=group(6,0x01000900,group(8,0x01000900,teleport(0x01000906,0x01000911)))
    mod+=group(6,0x01000910,group(8,0x01000910,teleport(0x01000911,0x01000906)))
    with ZipFile(p/"profile.zip","w") as z:
        z.writestr("Skyrim.esm",sk);z.writestr("ExampleHouse.esp",mod)
    (p/"loadorder.txt").write_text("Skyrim.esm\nExampleHouse.esp\n")
    (p/"plugins.txt").write_text("*Skyrim.esm\n*ExampleHouse.esp\n")
    command=[sys.executable,str(ROOT/"reusable/tools/build_guest_homes.py"),str(p/"profile.zip"),
             str(p/"loadorder.txt"),str(p/"plugins.txt")]
    for n in ("first","second"):
        subprocess.run(command+[str(p/n),"--instances"],check=True)
        assert validate(p/n)==(1,5,4)
        manifest=(p/n/"CoopHomeInstances.txt").read_text()
        assert "\nD " in manifest
        doors=[line.split() for line in manifest.splitlines() if line.startswith("T ")]
        assert doors==[["T","6578616d706c65686f7573652e657370.2304","6578616d706c65686f7573652e657370.2324"]]
        assert manifest.startswith("STM-HOME-INSTANCES 3\n")
        pool={tuple(line.split()[1:]) for line in manifest.splitlines() if line.startswith("M ")}
        assert ("6578616d706c65686f7573652e657370.2322","736b7972696d2e65736d.259") in pool
        assert not any(key.endswith(".2323") for key,_ in pool)
        rows=[line.split() for line in manifest.splitlines() if line.startswith("A ")]
        assert rows==[["A","6578616d706c65686f7573652e657370.2304",
                       "6578616d706c65686f7573652e657370.2311","736b7972696d2e65736d.259"]]
        assert any(line.endswith("6578616d706c65686f7573652e657370.2312 ACHR") for line in manifest.splitlines())
    for name in ("SkyrimCoopHomeInstances.esp","CoopHomeInstances.txt"):
        assert (p/"first"/name).read_bytes()==(p/"second"/name).read_bytes()
    assert subprocess.run(command+[str(p/"first"),"--instances"],capture_output=True).returncode!=0
print("PASS automatic modded interior, empty isolated container, explicit unsupported records and deterministic layer")
