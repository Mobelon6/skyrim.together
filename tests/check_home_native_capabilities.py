import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from build_native_home_capabilities import classify

pack=lambda n:struct.pack("<I",n)
unpack=lambda b:struct.unpack_from("<I",b)[0]
resolve=lambda n:("skyrim.esm",n) if n else None
ctx=(pack(1)+pack(6),)
def rec(t,n,d,flags=0,context=()):
    return t,resolve(n),flags,context,resolve,lambda:d
def ref(n,b,**extra):
    return rec(b"REFR",n,{b"NAME":pack(b),b"DATA":bytes(24),**{k.encode():v for k,v in extra.items()}},context=ctx)
records=[
    rec(b"CELL",1,{b"DATA":b"\1"}),
    rec(b"DOOR",2,{}),
    rec(b"FURN",3,{b"MNAM":pack((1<<30)|1),b"WBDT":bytes(2)}),
    rec(b"FURN",4,{b"MNAM":pack((1<<31)|1),b"WBDT":bytes(2)}),
    rec(b"FURN",5,{b"MNAM":pack((1<<30)|1),b"WBDT":b"\1\0"}),
    ref(10,2),ref(11,3),ref(12,4),ref(13,5),ref(14,2,XTEL=bytes(28)),
    ref(15,2),ref(16,3,XESP=pack(15)+pack(0)),
    ref(17,3),ref(17,3,VMAD=b"x"),
]
rows=classify(lambda kinds:(r for r in records if r[0] in kinds),unpack)
assert {(r[0],r[2][1]) for r in rows}=={("D",10),("S",11),("S",16)},rows
source=(Path(sys.argv[1])/"Code/client/Services/Generic/PartyHome.cpp").read_text()
assert "NativeSeat(m_world,*object,*player,visit.Root)" in source
assert "!NativeStanding(world,*p)" in source
assert "!NativeStanding(m_world,*p)" in source
print("PASS original door/seat classification, winning override, exclusion of scripts, teleport, workbench, beds and enable parents")
