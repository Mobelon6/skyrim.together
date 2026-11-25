import struct
import subprocess
import tempfile
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "reusable/tools"))
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
    ref(10,2),ref(11,3),ref(12,4),ref(13,5),ref(14,2,XTEL=bytes(32)),
    ref(15,2),ref(16,3,XESP=pack(15)+pack(0)),
    ref(17,3),ref(17,3,VMAD=b"x"),
]
rows=classify(lambda kinds:(r for r in records if r[0] in kinds),unpack)
assert {(r[0],r[2][1]) for r in rows}=={("D",10),("S",11),("S",16)},rows
source=(Path(sys.argv[1])/"Code/client/Services/Generic/PartyHome.cpp").read_text()
assert "NativeSeat(m_world,*object,*player,visit.Root)" in source
assert "!NativeStanding(world,*p)" in source
assert "!NativeStanding(m_world,*p)" in source
assert "ProjectionForm(local) || PrivateCell(local) || NativeGuestCell(local)" in source
objects=(Path(sys.argv[1])/"Code/client/Services/Generic/ObjectService.cpp").read_text()
activate=objects.split("void ObjectService::OnActivate(",1)[1].split("void ObjectService::OnActivateNotify(",1)[0]
assert activate.index("HomeInstance::ManagedCell(cell->formID)")<activate.index("m_transport.Send(request)")
notify=objects.split("void ObjectService::OnActivateNotify(",1)[1].split("void ObjectService::OnLockChange(",1)[0]
assert notify.index("HomeInstance::ManagedCell(cell->formID)")<notify.index("pObject->Activate(")
print("PASS original door/seat classification, winning override, exclusion of scripts, teleport, workbench, beds and enable parents")
# Compile and exercise the production metadata parser with actual common headers.
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp)/"native.cpp"
    src.write_text(r'''
#include "HomeNativeCapabilities.h"
#include <cassert>
int main()
{
    using namespace HomeNativeCapabilities;
    const std::string root="736b7972696d2e65736d.1", ref="736b7972696d2e65736d.11", base="736b7972696d2e65736d.3";
    const auto row="S "+root+" "+ref+" "+base+"\n";
    std::istringstream good("STM-NATIVE-HOME-OBJECTS 1\n"+row);
    const auto parsed=Read(good);
    assert(parsed && parsed->count(Row{'S',root,ref,base}));
    assert(!parsed->count(Row{'D',root,ref,base}));
    std::istringstream duplicate("STM-NATIVE-HOME-OBJECTS 1\n"+row+row);
    assert(!Read(duplicate));
    std::istringstream invalid("STM-NATIVE-HOME-OBJECTS 1\nX "+root+" "+ref+" "+base+"\n");
    assert(!Read(invalid));
}
''',encoding="utf-8")
    exe=Path(tmp)/"native"
    subprocess.run(["g++","-std=c++20","-Wall","-Wextra","-Werror","-I",str((Path(sys.argv[1])/"Code/common").resolve()),str(src),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print("PASS native capability parser and activation isolation source checks")
