"""Compile the production scope policy and cell range implementation."""
from pathlib import Path
import subprocess,sys,tempfile
root=Path(sys.argv[1])
common=(root/"Code/common").resolve()
cell=(root/"Code/server/Components/CellIdComponent.h").read_text()
# Use the actual component body, stubbing only the engine-independent grid helper and GameId.
cell=cell[cell.index("struct CellIdComponent"):].strip()
source=r'''
#include <cassert>
#include <cstdint>
#include "HomeScope.h"
struct GameId {
    uint32_t ModId{}, BaseId{};
    bool operator==(const GameId&) const=default;
    explicit operator bool()const{return ModId||BaseId;}
};
struct GridCellCoords {
    int x{},y{};
    static bool IsCellInGridCell(GridCellCoords a,GridCellCoords b,bool){return a.x==b.x && a.y==b.y;}
};
'''+cell+r'''
int main(){
    using namespace HomeScope;
    GameId empty{},ordinary{0,100},publicOther{1,100},world{0,60};
    GameId a{Registered|1,100},b{Registered|2,100},pending{Pending|1,100},wrongCell{Registered|1,101};
    GameId ref{5,0x345};
    assert(ReferenceMatches(ref,a,ref,a));
    assert(!ReferenceMatches(ref,a,ref,b));
    assert(!ReferenceMatches(ref,a,ref,pending));
    assert(!ReferenceMatches(ref,a,ref,ordinary));
    assert(!ReferenceMatches(ref,ordinary,ref,a));
    assert(!ReferenceMatches(ref,a,ref,wrongCell));
    assert(ReferenceMatches(ref,ordinary,ref,publicOther));
    assert(!ReferenceMatches(ref,a,GameId{5,0x346},a));
    assert(AdmittedCell(a,a,empty));
    assert(!AdmittedCell(a,a,world));
    assert(!AdmittedCell(a,b,empty));
    assert(!AdmittedCell(a,ordinary,empty));
    assert(!AdmittedCell(ordinary,a,empty));
    assert(!AdmittedCell(pending,a,empty));
    assert(AdmittedCell(pending,pending,empty));
    assert(SpawnAllowed(a,a,true));
    assert(!SpawnAllowed(a,b,true));
    assert(!SpawnAllowed(a,wrongCell,true));
    assert(!SpawnAllowed(a,ordinary,true));
    assert(!SpawnAllowed(pending,pending,true));
    assert(!SpawnAllowed(ordinary,ordinary,true));
    assert(!SpawnAllowed(ordinary,a,false));
    assert(!SpawnAllowed(a,ordinary,false));
    assert(SpawnAllowed(ordinary,publicOther,false));
    CellIdComponent ca(a),cb(b),cp(pending),co(ordinary),cx(ordinary,world,{0,0}),bad(a,world,{0,0});
    assert(ca.IsInRange(ca,false));
    assert(!ca.IsInRange(cb,false)&&!cb.IsInRange(ca,true));
    assert(!ca.IsInRange(cp,true)&&!co.IsInRange(ca,false)&&!ca.IsInRange(co,false));
    assert(!cx.IsInRange(ca,true)&&!ca.IsInRange(cx,true));
    assert(!ca.IsInRange(bad,true)&&!bad.IsInRange(ca,true));
    assert(cx.IsInRange(cx,false));
    CellIdComponent estateA(ordinary,world,{0,0}),estateB(ordinary,world,{0,0}),guest(ordinary,world,{0,0});
    estateA.Estate={71,101,"plotA"};estateB.Estate={71,202,"plotA"};guest.Estate={71,101,"plotA"};
    assert(estateA.IsInRange(guest,false));
    assert(!estateA.IsInRange(estateB,false)&&!estateB.IsInRange(estateA,true));
    assert(!estateA.IsInRange(cx,false)&&!cx.IsInRange(estateA,false));
    guest.Estate={72,101,"plotA"};assert(!estateA.IsInRange(guest,false));
    guest.Estate={71,101,"plotB"};assert(!estateA.IsInRange(guest,false));
    guest.Estate={0,101,"plotA"};assert(!guest.IsInRange(guest,false));
    guest.Estate={71,101,""};assert(!guest.IsInRange(guest,false));
    guest.Estate={71,101,"plotA"};guest.CenterCoords={1,0};
    assert(!estateA.IsInRange(guest,false)); // physical range is still required
    assert(estateA.HasExteriorEstate()&&!cx.HasExteriorEstate());
    assert(!estateA.SamePresence(estateB)&&!estateA.SamePresence(cx));
    CellIdComponent ca2(a);
    ca.Estate={71,101,"plotA"};
    assert(ca.SamePresence(ca2)&&ca.IsInRange(ca2,false));
    assert(!ca.HasExteriorEstate()&&!ca.SamePresence(estateA));
    assert(!ca.SamePresence(cb)); // distinct physical interior slot
    assert(!estateA.SamePresence(ca));
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp,"scope.cpp");src.write_text(source)
    exe=Path(tmp,"scope")
    subprocess.run(["g++","-std=c++20","-Wall","-Wextra","-Werror","-I",str(common),str(src),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
server=root/"Code/server/Services"
actor=(server/"CharacterService.cpp").read_text()
objects=(server/"ObjectService.cpp").read_text()
inventory=(server/"InventoryService.cpp").read_text()
player=(server/"PlayerService.cpp").read_text()
assert "HomeScope::ReferenceMatches(formIdComponent.Id,location.Cell,refId,cell)" in actor
assert "HomeScope::AdmittedCell(acMessage.pPlayer->GetCellComponent().Cell,message.CellId,message.WorldSpaceId)" in actor
assert "HomeScope::Private(senderCell.ModId)" in actor
assert "HomeScope::Compatible(cellIdComponent.Cell,movement.CellId)" in actor
assert "HomeScope::ReferenceMatches(formIdComponent.Id,view.get<CellIdComponent>(entity).Cell,id,cell)" in objects
assert "HomeScope::AdmittedCell(acMessage.pPlayer->GetCellComponent().Cell,object.CellId,object.WorldSpaceId)" in objects
assert inventory.count("!HomeScope::Compatible(acMessage.pPlayer->GetCellComponent().Cell,cell->Cell)")==3
assert "if (HomeScope::Private(message.PlayerCell.ModId) || HomeScope::Private(message.WorldSpaceId.ModId)) return;" in player
client=(root/"Code/client/Services/Generic/CharacterService.cpp").read_text()
assert "if (!AcceptHomeSpawn(m_world,acMessage.CellId)) return;" in client
assert "if (projectedNpc && acMessage.FormId!=GameId{})" in client
assert "pActor=CreateProjectedActor(m_world,acMessage);" in client
assert "if (!projectedNpc) pNpc->Deserialize" in client
assert "m_world.emplace_or_replace<HomeProjectedActor>" in client
assert "if (isLocalOwner && m_world.all_of<HomeProjectedActor>" in client
assert "HomeRegistry::OwnsCell(link.Journal,CampaignAccess::Character(*acMessage.pPlayer)" in actor
assert "HomeRegistry::OwnsCell(link.Journal,CampaignAccess::Character(*apPlayer)" in actor
assert "location->IsInRange(pPlayer->GetCellComponent(),false)" in actor
assert "senderLocation.HasExteriorEstate()||cellIdComponent.HasExteriorEstate()" in actor
assert "cell.Estate=HomeVisit::EstateFor" in player
assert "cellIdComponent.Estate=acMessage.pPlayer->GetCellComponent().Estate" in actor
assert "location.Estate=acMessage.pPlayer->GetCellComponent().Estate" in objects
assert "sender->GetCellComponent().SamePresence" in objects
assert inventory.count("cell->HasExteriorEstate()||acMessage.pPlayer->GetCellComponent().HasExteriorEstate()")==3
print("PASS scoped reference matching, private admission, spawn routing and simulation gates")
