"""Stable placed stacks use the same production ledger as containers and trade."""
from pathlib import Path
import sys,tempfile,subprocess
root=Path(sys.argv[1])
common=(root/"Code/common").resolve()
source=r'''
#include "TradeLedger.h"
#include "HomeProjectionPlan.h"
#include <cassert>
int main(int argc,char** argv)
{
    using namespace HomePermissions;
    using TradeProperties::FormKey;
    CampaignJournal::Journal j;
    const auto path=std::filesystem::path(argv[1]);
    assert(j.Open(path,71,"profile",true));
    FormKey root{"house.esp",100},chest{"house.esp",101},loose{"house.esp",102},item{"skyrim.esm",42};
    auto a=HomeRegistry::Register(j,101,root),b=HomeRegistry::Register(j,202,root);
    assert(a&&b);
    TradeProperties::Values owned;owned.Mask=TradeProperties::Values::HasOwnership;owned.Owner={"skyrim.esm",123};
    const auto variant=owned.Encode();
    HomeSnapshot::State s;
    HomeSnapshot::Ref c;c.Key=chest;c.Base={"skyrim.esm",999};c.Type=28;c.Inventory=1;c.Scale=100;
    c.Items={{item,5,{}}};
    HomeSnapshot::Ref r;r.Key=loose;r.Base=item;r.Type=32;r.Inventory=3;r.Scale=100;
    r.Items={{item,3,variant}};
    s.Refs={c,r};
    assert(s.Valid()&&s.Encode().starts_with("static-v3 "));
    assert(HomeSnapshot::Decode(s.Encode()));
    auto legacy=s;legacy.Refs[1].Inventory=0;legacy.Refs[1].Items.clear();
    assert(legacy.Valid()&&HomeSnapshot::Decode(legacy.Encode()));
    const std::string plugin="skyrimcoophomeinstances.esp";
    std::vector<HomeProjection::Binding> bindings{
        {chest,{plugin,0x800},{plugin,0x801},28},{loose,{plugin,0x802},item,32}};
    assert(!HomeProjection::Plan(legacy,bindings,plugin));
    assert(HomeProjection::Plan(s,bindings,plugin));
    auto bad=s;bad.Refs[1].Items[0].Count=10000;assert(!bad.Valid());
    bad=s;bad.Refs[1].Flags=1;assert(!bad.Valid());
    bad=s;bad.Refs[1].Items[0].Base.Local++;assert(!bad.Valid());
    bad=s;bad.Refs[1].Items.clear();assert(!bad.Valid());
    assert(HomeSnapshot::Publish(j,a->Slot,s.Encode()));
    assert(HomeSnapshot::Publish(j,b->Slot,s.Encode()));
    assert(Set(j,101,a->Slot,202,Right::Visit,0));
    auto state=[&](TradeLedger& l,uint32_t slot){auto v=l.HomeState(slot);assert(v);return *v;};
    {
        TradeLedger l(j,71);assert(l.Stream()==71);
        assert(!l.CommitHome(202,a->Slot,loose,item,variant,3,0,0));
        assert(Set(j,101,a->Slot,202,Right::Modify,1));
        assert(!l.CommitHome(202,a->Slot,loose,item,variant,2,0,0));
        assert(!l.CommitHome(202,a->Slot,loose,item,{},3,0,0));
        assert(!l.CommitHome(202,a->Slot,loose,item,variant,-1,3,0));
        assert(l.CommitHome(202,a->Slot,loose,item,variant,3,0,0));
        auto v=state(l,a->Slot);
        assert(v.Refs[1].Items.empty()&&(v.Refs[1].Flags&1));
        assert(HomeProjection::Plan(v,bindings,plugin));
        assert(state(l,b->Slot).Refs[1].Items.front().Count==3);
        assert(!l.CommitHome(101,a->Slot,loose,item,variant,3,0,0));
        assert(!l.CommitHome(202,a->Slot,loose,item,variant,3,0,0));
        assert(l.MatchesReceipt("202",71,0,1,item.Plugin,item.Local,3,0,variant));
        assert(!l.NextRefund("202"));
    }
    {
        TradeLedger l(j,71);assert(l.Stream()==71);
        assert(state(l,a->Slot).Refs[1].Items.empty());
        assert(l.MatchesReceipt("202",71,0,1,item.Plugin,item.Local,3,0,variant));
        assert(l.Ack("202",1)&&l.Ack("202",1));
        assert(l.CommitHome(202,a->Slot,chest,item,{},1,0,1));
        assert(l.Ack("202",2));
        assert(!l.CommitHome(202,a->Slot,loose,item,variant,-3,3,2));
        assert(l.AcceptCursor("202",71,0));
    }
    CampaignJournal::Journal restart;assert(restart.Open(path,71,"profile",false));
    TradeLedger l(restart,71);assert(l.Stream()==71);
    assert(state(l,a->Slot).Refs[0].Items[0].Count==4);
    assert(state(l,a->Slot).Refs[1].Items.empty());
    assert(state(l,b->Slot).Refs[1].Items[0].Count==3);
    assert(l.History("202").size()==2);
    assert(HomeSnapshot::Read(restart,a->Slot).Bytes==s.Encode());
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp,"loose.cpp");p.write_text(source);exe=Path(tmp,"loose")
    subprocess.run(["g++","-std=c++20","-Wall","-Wextra","-Werror","-Wno-unused-parameter",
                    "-I",str(common),str(common/"CampaignJournal.cpp"),str(common/"QuestJournalDisk.cpp"),
                    str(p),"-o",str(exe)],check=True)
    subprocess.run([str(exe),str(Path(tmp,"journal"))],check=True)
client=(root/"Code/client/Services/Generic/PartyTrade.cpp").read_text()
home=(root/"Code/client/Services/Generic/PartyHome.cpp").read_text()
server=(root/"Code/server/Services/PartyTrade.cpp").read_text()
player=(root/"Code/client/Games/Skyrim/PlayerCharacter.cpp").read_text()
assert "PartyTrade request{TradeOp::Take,home->Slot,0,w.Cursor(),item.Count,w.Stream()}" in client
assert "extra->HasQuestObjectAlias()" in client
assert "if (e.Op==TradeOp::Take && !e.Session)" in server
assert "row->Items.front().Count!=e.Count" in server
assert "if (!container || container->Inventory!=1)" in server
assert "ApplyLoose(*current,*state,copy)" in home
assert "ApplyLoose(*cell,*state,nullptr)" in home
assert "World::Get().GetOverlayService().TryTrade(apObject->formID);" in player
print("PASS placed stack receipt, exact variant, rights, concurrency, restart, legacy refusal and hook integration")
