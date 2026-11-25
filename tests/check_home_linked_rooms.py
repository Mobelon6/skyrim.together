import struct
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from build_native_home_capabilities import classify
u=lambda n:struct.pack("<I",n)
unpack=lambda d:struct.unpack_from("<I",d)[0]
key=lambda n:("skyrim.esm",n)
def rec(kind,n,d,flags=0,ctx=()):
    return kind,key(n),flags,ctx,key,lambda:d
def cell(n,loc):
    return rec(b"CELL",n,{b"DATA":b"\x01",b"XLCN":u(loc)})
def door(n,room,dest,**extra):
    return rec(b"REFR",n,{b"NAME":u(50),b"DATA":bytes(24),b"XTEL":u(dest)+bytes(24),**{k.encode():v for k,v in extra.items()}},0x400,(u(room)+u(6),))
rows=[rec(b"LCTN",100,{b"KWDA":u(0xfc1a3)}),rec(b"LCTN",200,{b"KWDA":u(0xfc1a3)}),
      cell(1,100),cell(2,100),cell(3,200),rec(b"DOOR",50,{}),
      door(10,1,11),door(11,2,10),door(12,1,13),door(13,3,12),
      door(14,1,15),door(15,2,16),door(16,1,15,VMAD=b"x")]
def run(rows):
    return {(t,r[1],b[1]) for t,c,r,b in classify(lambda kinds:(x for x in rows if x[0] in kinds),unpack) if t=="L"}
assert run(rows)=={("L",10,11),("L",11,10)}
assert not run(rows+[door(11,2,10,VMAD=b"x")])
assert not run(rows+[rec(b"LCTN",100,{},32)])
src=(Path(sys.argv[1])/"Code/client/Services/Generic/PartyHome.cpp").read_text()
assert "candidate!=owner||slot==visit.Slot" in src
assert "HomeOriginalOverlay::Release()" in src
assert src.index("if (!HomeOriginalOverlay::Release())")<src.index("SendAccess(m_world,*identity,3,next.Slot")
assert "previousGuestCell.store(visit.Copy);originalGuestCell.store(moved.Copy)" in src
assert "visit.Copy!=visit.EntryCell" in src
print("PASS reciprocal linked-room metadata, location boundary, winning overrides and source transition guards")
