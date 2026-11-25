"""Production coverage/binding validation for stored residents, without the game."""
from pathlib import Path
import subprocess,sys,tempfile
root=Path(sys.argv[1]);common=(root/"Code/common").resolve()
source=r'''
#include "HomeResidentPlan.h"
#include <cassert>
int main()
{
    using TradeProperties::FormKey;
    const FormKey root{"house.esp",100},src{"house.esp",101},base{"skyrim.esm",202},
        door{"house.esp",102},outside{"skyrim.esm",301},resident{"house.esp",103},npc{"skyrim.esm",203};
    const std::string header="STM-HOME-INSTANCES 2\nH "+root.Encode()+" 2048\n";
    const std::string row="R "+root.Encode()+" "+src.Encode()+" "+base.Encode()+" 2049 "+base.Encode()+"\n";
    const std::string exit="D "+root.Encode()+" "+door.Encode()+" "+outside.Encode()+" 2049 0 0 0 0 0 0 0 0 0 0 0 0\n";
    const std::string actor="A "+root.Encode()+" "+resident.Encode()+" "+npc.Encode()+"\n";
    std::istringstream input(header+row+exit+actor);
    auto catalog=HomeCatalog::Read(input);assert(catalog);
    auto cell=catalog->at(root.Encode());
    HomeSnapshot::State baseline;HomeSnapshot::Ref r;r.Key=src;r.Base=base;r.Type=29;r.Scale=100;
    baseline.Refs.push_back(r);
    HomeResidents::State observed;observed.Reference=resident;observed.Base=npc;observed.Scale=100;
    HomeResidents::Residents records{{resident.Encode(),{1,0,observed}}};
    assert(!HomeResidentPlan::Build(cell,baseline,records));
    auto legacy=HomeSnapshot::Decode(baseline.Encode());assert(legacy&&legacy->ResidentKeys.empty());
    baseline.ResidentKeys.insert(resident.Encode());
    assert(baseline.Encode().starts_with("static-v2 "));
    auto restored=HomeSnapshot::Decode(baseline.Encode());
    assert(restored&&restored->ResidentKeys==baseline.ResidentKeys);
    assert(HomeResidentPlan::Build(cell,*restored,records));
    assert(!HomeResidentPlan::Build(cell,*restored,{}));
    auto other=records;other.begin()->second.Value.Base.Local++;
    assert(!HomeResidentPlan::Build(cell,*restored,other));
    other=records;other.begin()->second.Revision=0;
    assert(!HomeResidentPlan::Build(cell,*restored,other));
    other=records;other.begin()->second.Value.Flags=2;
    assert(HomeResidentPlan::Build(cell,*restored,other));
    other=records;other.begin()->second.Value.Flags=5;
    assert(HomeResidentPlan::Build(cell,*restored,other));
    restored->Omitted=1;assert(!HomeResidentPlan::Build(cell,*restored,records));restored->Omitted=0;
    cell.Unsupported.insert(resident.Encode());assert(!HomeResidentPlan::Build(cell,*restored,records));
    for (const auto& bad:{header+row+exit+actor+actor,header+row+exit+actor+"U "+root.Encode()+" "+resident.Encode()+" ACHR\n"})
    {std::istringstream stream(bad);assert(!HomeCatalog::Read(stream));}
    baseline.ResidentKeys.insert(src.Encode());assert(!baseline.Valid());
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp,"plan.cpp");p.write_text(source);exe=Path(tmp,"plan")
    subprocess.run(["g++","-std=c++20","-Wall","-Wextra","-Werror","-I",str(common),str(p),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
client=(root/"Code/client/Services/Generic/PartyHome.cpp").read_text()
actor=(root/"Code/client/Services/Generic/CharacterService.cpp").read_text()
assert "HomeResidentPlan::Build(cell,state,*records)" in client
assert "HomeResidentProjection::Sync(world,target,slot,entries)" in client
assert "state.ResidentKeys.insert(encoded)" in client
assert "HomeResidentProjection::Clear();residentDisplayNext={};" in client
assert "if (LiveResident(world,slot,reference) || (s.Flags&5u))" in actor
assert "actor->GetHandle().handle.iBits!=display.Handle" in actor
assert "if (actor) DropResidentDisplay" in actor
print("PASS resident coverage, legacy incompatibility, explicit catalog bindings, replica projection integration")
