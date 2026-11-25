"""Exercise atomic linked-room permission changes and historical checkpoint access."""
from pathlib import Path
import subprocess, sys, tempfile
up=Path(sys.argv[1]).resolve()
common=up/"Code/common"
source=r'''
#include "HomeLinkedAccess.h"
#include "CampaignCheckpoint.h"
#include <cassert>
int main(int argc,char** argv)
{
    using namespace HomePermissions;
    using namespace HomeLinkedAccess;
    CampaignJournal::Journal j;
    assert(j.Open(argv[1],71,"profile",true));
    const auto initial=j.Current();
    TradeProperties::FormKey a{"skyrim.esm",1},b{"skyrim.esm",2},c{"skyrim.esm",3};
    TradeProperties::FormKey x{"skyrim.esm",10},y{"skyrim.esm",11};
    const auto first=HomeRegistry::Register(j,101,a),second=HomeRegistry::Register(j,101,b),
        foreign=HomeRegistry::Register(j,202,b),unrelated=HomeRegistry::Register(j,101,c);
    assert(first&&second&&foreign&&unrelated);
    std::set<HomeNativeCapabilities::Row> rows{{'L',a.Encode(),x.Encode(),y.Encode()},{'L',b.Encode(),y.Encode(),x.Encode()}};
    const auto graph=BuildGraph(rows);assert(graph);
    auto slots=RegisteredRooms(j,*first,*graph);
    assert((slots==std::vector<uint32_t>{first->Slot,second->Slot}));
    assert(RegisteredRooms(j,*foreign,*graph).size()==1);
    assert(RegisteredRooms(j,*unrelated,*graph).size()==1);
    auto bad=rows;bad.erase(bad.begin());assert(!BuildGraph(bad));
    assert(Set(j,101,first->Slot,303,Right::Visit,0));
    assert(Effective(j,second->Slot,303)==Right::None);
    assert(!SetGroup(j,101,{first->Slot,foreign->Slot},303,Right::Visit,1));
    assert(!SetGroup(j,101,slots,303,Right::Modify,0));
    const auto before=j.Current();
    assert(SetGroup(j,101,slots,303,Right::Modify,1));
    assert(j.Current().Revision==before.Revision+1&&Read(j)->Revision==2);
    assert(Effective(j,first->Slot,303)==Right::Modify&&Effective(j,second->Slot,303)==Right::Modify);
    assert(Effective(j,foreign->Slot,303)==Right::None&&Effective(j,unrelated->Slot,303)==Right::None);
    CampaignCheckpoint::Record save;
    save.Save={71,303,initial};save.Observed=j.Current();save.Digest=std::string(64,'a');
    save.Cell=b;save.HomeSlot=second->Slot;
    assert(CampaignCheckpoint::HadHomeAccess(j,save));
    assert(CampaignCheckpoint::CanResume(j,71,save));
    assert(SetGroup(j,101,slots,303,Right::Modify,2)&&Read(j)->Revision==2);
    assert(SetGroup(j,101,slots,303,Right::None,2));
    assert(CampaignCheckpoint::HadHomeAccess(j,save));
    assert(!CampaignCheckpoint::CanResume(j,71,save));
    auto after=save;after.Observed=j.Current();assert(!CampaignCheckpoint::HadHomeAccess(j,after));
    CampaignJournal::Journal restart;
    assert(restart.Open(argv[1],71,"profile",false));
    assert(Read(restart)->Revision==3&&CampaignCheckpoint::HadHomeAccess(restart,save));
    assert(Effective(restart,second->Slot,303)==Right::None);
    assert(!DecodeGroup("101 303 2 2 2 1"));
    assert(!DecodeGroup("101 303 2 2 1 1"));
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d)/"test.cpp";p.write_text(source);exe=Path(d)/"test"
    subprocess.run(["g++","-std=c++20","-Wall","-Wextra","-Werror","-Wno-unused-parameter","-I",str(common),
        str(common/"CampaignIdentity.cpp"),str(common/"CampaignJournal.cpp"),str(common/"QuestJournalDisk.cpp"),
        str(p),"-o",str(exe)],check=True)
    subprocess.run([str(exe),str(Path(d)/"journal")],check=True)
print("PASS linked-room graph, owner isolation, atomic ACL, mixed grants, historical checkpoint and restart")
