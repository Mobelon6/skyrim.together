"""Exercise house transactions through the same production trade receipt ledger."""
from pathlib import Path
import sys,tempfile,subprocess
common=Path(sys.argv[1],"Code/common").resolve()
source=r'''
#include "TradeLedger.h"
#include <cassert>
int main(int argc,char** argv)
{
    using namespace HomePermissions;
    CampaignJournal::Journal host;
    assert(host.Open(std::filesystem::path(argv[1]),71,"profile",true));
    TradeProperties::FormKey root{"house.esp",100},chest{"house.esp",101},item{"skyrim.esm",42};
    auto a=HomeRegistry::Register(host,101,root),b=HomeRegistry::Register(host,202,root);
    assert(a&&b);
    HomeSnapshot::State s;
    HomeSnapshot::Ref r;r.Key=chest;r.Base={"skyrim.esm",999};r.Type=28;r.Inventory=1;r.Scale=100;
    TradeProperties::Values owned;owned.Mask=TradeProperties::Values::HasOwnership;owned.Owner={"skyrim.esm",123};
    r.Items={{item,5,{}},{item,2,owned.Encode()}};
    s.Refs.push_back(r);
    assert(HomeSnapshot::Publish(host,a->Slot,s.Encode()));
    assert(HomeSnapshot::Publish(host,b->Slot,s.Encode()));
    assert(Set(host,101,a->Slot,202,Right::Visit,0));
    auto count=[&](TradeLedger& l,uint32_t slot,std::string variant={}) {
        auto state=l.HomeState(slot);assert(state);
        for (const auto& i:state->Refs.at(0).Items) if (i.Variant==variant) return i.Count;
        return 0u;
    };
    {
        TradeLedger l(host,71);assert(l.Stream()==71);
        assert(!l.CommitHome(202,a->Slot,chest,item,{},1,0,0));
        assert(Set(host,101,a->Slot,202,Right::Modify,1));
        assert(!l.CommitHome(202,a->Slot,chest,item,{},6,0,0));
        assert(!l.Commit({"202",TradeLedger::HomeAccount(a->Slot,chest),"skyrim.esm",42,-1,1},0));
        assert(l.CommitHome(202,a->Slot,chest,item,{},3,0,0));
        assert(count(l,a->Slot)==2&&count(l,b->Slot)==5&&count(l,a->Slot,owned.Encode())==2);
        assert(!l.CommitHome(101,a->Slot,chest,item,{},3,0,0));
        assert(l.MatchesReceipt("202",71,0,1,"skyrim.esm",42,3,0));
        assert(!l.CommitHome(202,a->Slot,chest,item,{},-2,3,1));
        assert(l.Ack("202",1));
        assert(l.CommitHome(202,a->Slot,chest,item,{},-2,3,1));
        assert(count(l,a->Slot)==2&&!l.NextRefund("202"));
        assert(Set(host,101,a->Slot,202,Right::None,2));
    }
    {
        TradeLedger l(host,71);assert(l.Stream()==71);
        assert(count(l,a->Slot)==2&&l.History("202").size()==2);
        assert(l.MatchesReceipt("202",71,1,2,"skyrim.esm",42,-2,3));
        assert(l.Ack("202",2)&&l.Ack("202",2));
        assert(count(l,a->Slot)==4&&!l.NextRefund("202"));
        assert(!l.CommitHome(202,a->Slot,chest,item,{},1,1,2));
        assert(l.CommitHome(101,a->Slot,chest,item,owned.Encode(),1,0,0));
        assert(l.Ack("101",1));
        assert(count(l,a->Slot,owned.Encode())==1);
        // Existing player trade and house movement share one cursor.
        assert(l.Commit({"101","202","Skyrim.esm",42,-1,1},1));
        assert(l.Ack("101",2)&&l.NextRefund("101"));
        assert(l.CommitHome(101,a->Slot,chest,item,{},1,0,2));
        assert(count(l,a->Slot)==3);
    }
    CampaignJournal::Journal restart;
    assert(restart.Open(std::filesystem::path(argv[1]),71,"profile",false));
    TradeLedger l(restart,71);assert(l.Stream()==71);
    assert(count(l,a->Slot)==3&&count(l,b->Slot)==5);
    assert(l.History("101").size()==3&&l.AcceptCursor("101",71,0));
    assert(l.Ack("101",3)&&count(l,a->Slot)==3);
    assert(HomeSnapshot::Read(restart,a->Slot).Bytes==s.Encode());
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp,"home.cpp");p.write_text(source)
    exe=Path(tmp,"home")
    subprocess.run(["g++","-std=c++20","-Wall","-Wextra","-Werror","-Wno-unused-parameter",
                    "-I",str(common),str(common/"CampaignJournal.cpp"),str(common/"QuestJournalDisk.cpp"),
                    str(p),"-o",str(exe)],check=True)
    subprocess.run([str(exe),str(Path(tmp,"journal"))],check=True)
print("PASS home stock reservation, confirmed deposit, shared wallet cursor, revocation, restart, exact variants and distinct instances")
