import struct
import sys
import subprocess
import tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from build_native_home_capabilities import classify
u=lambda n:struct.pack("<I",n)
unpack=lambda b:struct.unpack_from("<I",b)[0]
key=lambda n:("skyrim.esm",n)
ctx=(u(1)+u(6),)
def rec(kind,n,d,flags=0,context=()):
    return kind,key(n),flags,context,key,lambda:d
def actor(n,b,**extra):
    return rec(b"ACHR",n,{b"NAME":u(b),b"DATA":bytes(24),**{k.encode():v for k,v in extra.items()}},context=ctx)
rows=[rec(b"CELL",1,{b"DATA":b"\x01"}),rec(b"NPC_",2,{}),
      rec(b"NPC_",3,{b"VMAD":b"x"}),rec(b"NPC_",4,{b"TPLT":u(2)}),
      actor(5,2),actor(6,3),actor(7,4),actor(8,2,XESP=u(9)),actor(9,2),actor(10,2),
      actor(10,2,VMAD=b"x")]
outside=(u(20)+u(6),)
rows += [rec(b"CELL",20,{b"DATA":b"\x00"}),
         rec(b"ACHR",21,{b"NAME":u(2),b"DATA":bytes(24)},flags=0x400,context=outside),
         rec(b"ACHR",22,{b"NAME":u(2),b"DATA":bytes(24),b"VMAD":b"x"},context=outside)]
assert {(t,r[1]) for t,c,r,b in classify(lambda kinds:(x for x in rows if x[0] in kinds),unpack)}=={("A",5),("M",21)}
up=Path(sys.argv[1]).resolve()
home=(up/"Code/client/Services/Generic/PartyHome.cpp").read_text()
overlay=(up/"Code/client/Services/Generic/HomeOriginalOverlay.cpp").read_text()
actors=(up/"Code/client/Services/Generic/CharacterService.cpp").read_text()
assert "ApplyNativeResidents(m_world,*current,visit.Campaign,visit.Slot,visit.Root)" in home
assert "d.Guest.Disabled=true" in overlay
assert "if (row.Type==43) return f;" in overlay
assert "HomeNativeCapabilities::Covers" in overlay
assert "NativeGuestResident(m_world.GetModSystem().GetGameId(acMessage.FormId)" in actors
with tempfile.TemporaryDirectory() as d:
    src=Path(d)/"native.cpp"
    src.write_text(r'''
#include "HomeNativeCapabilities.h"
#include <cassert>
int main()
{
    TradeProperties::FormKey ref{"skyrim.esm",5},base{"skyrim.esm",2};
    HomeSnapshot::State state; state.ResidentKeys.insert(ref.Encode());
    HomeResidents::Record rec;rec.Revision=1;rec.Value.Reference=ref;rec.Value.Base=base;rec.Value.Scale=100;
    HomeResidents::Residents records{{ref.Encode(),rec}};
    std::map<std::string,TradeProperties::FormKey> bases{{ref.Encode(),base}};
    assert(HomeNativeCapabilities::Covers(bases,state,records));
    records.at(ref.Encode()).Value.Flags=8;
    assert(HomeNativeCapabilities::Covers(bases,state,records));
    records.at(ref.Encode()).Value.Flags=2;
    assert(!HomeNativeCapabilities::Covers(bases,state,records));
    records.at(ref.Encode()).Value.Flags=0;
    TradeProperties::FormKey moved{"skyrim.esm",21};
    auto arrival=rec;arrival.Value.Reference=moved;
    records.emplace(moved.Encode(),arrival);
    assert(!HomeNativeCapabilities::Covers(bases,state,records));
    auto pool=bases;pool.emplace(moved.Encode(),base);
    assert(HomeNativeCapabilities::Covers(bases,state,records,&pool));
    records.at(moved.Encode()).Value.Flags=8;
    assert(HomeNativeCapabilities::Covers(bases,state,records,&pool));
    records.at(moved.Encode()).Value.Base.Local++;
    assert(!HomeNativeCapabilities::Covers(bases,state,records,&pool));
    records.clear();assert(!HomeNativeCapabilities::Covers(bases,state,records,&pool));
    std::set<HomeNativeCapabilities::Row> rows{
      {'A',ref.Encode(),ref.Encode(),base.Encode()},
      {'M',moved.Encode(),moved.Encode(),base.Encode()}};
    const auto indexed=HomeNativeCapabilities::Index(rows);
    assert(indexed.Placed.size()==1&&indexed.Movable->size()==2);
}
''',encoding="utf-8")
    exe=Path(d)/"native"
    subprocess.run(["g++","-std=c++20","-Wall","-Wextra","-Werror","-I",str(up/"Code/common"),str(src),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
assert "catalog.Movable=HomeNativeCapabilities::Movable()" in home
assert "HomeInstance::NativeResidentBinding(actor->formID,base)" in overlay
server=(up/"Code/server/Services/CharacterService.cpp").read_text()
assert "A guest's ephemeral actor is not an authoritative incarnation" in server
assert "no eligible owner remains" in server
print("PASS native resident classification, static and moved-in coverage, authority and integration source guards")
