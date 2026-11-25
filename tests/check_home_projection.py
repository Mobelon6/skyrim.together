"""Exercise real projection mapping and persistent per-character permissions."""
from pathlib import Path
import subprocess,sys,tempfile
root=Path(sys.argv[1])
common=(root/"Code/common").resolve()
source=r'''
#include "HomeProjectionPlan.h"
#include "HomePermissions.h"
#include <cassert>
#include <filesystem>
int main(int argc,char** argv)
{
    using namespace HomePermissions;
    CampaignJournal::Journal j;
    const auto path=std::filesystem::path(argv[1]);
    assert(j.Open(path,71,"profile",true));
    TradeProperties::FormKey cell{"house.esl",0x900};
    const auto a=HomeRegistry::Register(j,101,cell),b=HomeRegistry::Register(j,202,cell);
    assert(a&&b);
    assert(Effective(j,a->Slot,101)==Right::Modify);
    assert(Effective(j,a->Slot,202)==Right::None);
    assert(!Set(j,202,a->Slot,303,Right::Modify,0));
    assert(Set(j,101,a->Slot,202,Right::Visit,0));
    assert(Effective(j,a->Slot,202)==Right::Visit);
    assert(Effective(j,b->Slot,202)==Right::Modify);
    assert(Effective(j,b->Slot,101)==Right::None);
    assert(!Set(j,101,a->Slot,202,Right::Modify,0));
    assert(Set(j,101,a->Slot,202,Right::Modify,1));
    assert(Set(j,101,a->Slot,202,Right::Modify,2));
    assert(Read(j)->Revision==2);
    assert(Set(j,101,a->Slot,202,Right::None,2));
    CampaignJournal::Journal restart;
    assert(restart.Open(path,71,"profile",false));
    assert(Effective(restart,a->Slot,202)==Right::None&&Read(restart)->Revision==3);
    HomeSnapshot::State state;
    HomeSnapshot::Ref r;
    r.Key={"house.esl",0x901};r.Base={"skyrim.esm",0x123};r.Type=28;r.Inventory=1;r.Scale=100;
    state.Refs.push_back(r);
    HomeProjection::Binding bind{r.Key,{"skyrimcoophomeinstances.esp",0x800},
        {"skyrimcoophomeinstances.esp",0x801},28};
    const auto plugin="skyrimcoophomeinstances.esp";
    assert(HomeProjection::Plan(state,{bind},plugin));
    auto wrong=bind;wrong.Target=r.Key;
    assert(!HomeProjection::Plan(state,{wrong},plugin));
    wrong=bind;wrong.Type=34;assert(!HomeProjection::Plan(state,{wrong},plugin));
    assert(!HomeProjection::Plan(state,{},plugin));
    auto bad=state;bad.Refs[0].Inventory=2;assert(!HomeProjection::Plan(bad,{bind},plugin));
    bad=state;bad.Omitted=1;assert(!HomeProjection::Plan(bad,{bind},plugin));
    bad=state;bad.Refs[0].Flags=2;assert(!HomeProjection::Plan(bad,{bind},plugin));
    auto second=r;second.Key.Local++;state.Refs.push_back(second);
    auto duplicate=bind;duplicate.Source=second.Key;
    assert(!HomeProjection::Plan(state,{bind,duplicate},plugin));
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp,"home.cpp");src.write_text(source)
    exe=Path(tmp,"home")
    subprocess.run(["g++","-std=c++20","-Wall","-Wextra","-Werror","-Wno-unused-parameter",
                    "-I",str(common),str(common/"CampaignJournal.cpp"),str(common/"QuestJournalDisk.cpp"),
                    str(src),"-o",str(exe)],check=True)
    subprocess.run([str(exe),str(Path(tmp,"journal"))],check=True)
trade=(root/"Code/client/Services/Generic/PartyTrade.cpp").read_text()
assert "cellKey->Plugin!=ProjectionPlugin" in trade
assert "ref->GetParentCellEx()!=&cell" in trade
assert "if (!current || !current->empty()) return false;" in trade
assert "Never create a second spendable copy" in trade
server=(root/"Code/server/Services/PartyHome.cpp").read_text()
assert "HomePermissions::Set(link.Journal,character" in server
print("PASS complete isolated reference mapping, fail-closed unsupported state and durable owner-only access")
