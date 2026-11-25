"""Production ledger: dynamic placement conservation, permissions, pose and replay."""
from pathlib import Path
import tempfile,sys,subprocess
root=Path(sys.argv[1]);common=(root/"Code/common").resolve()
source=r'''
#include "TradeLedger.h"
#include <cassert>
int main(int argc,char** argv)
{
    using namespace HomePermissions;
    using TradeProperties::FormKey;
    CampaignJournal::Journal j;
    const auto path=std::filesystem::path(argv[1]);
    assert(j.Open(path,71,"profile",true));
    FormKey root{"house.esp",100},chest{"house.esp",101},item{"skyrim.esm",42};
    auto a=HomeRegistry::Register(j,101,root),b=HomeRegistry::Register(j,202,root);
    assert(a&&b);
    HomeSnapshot::Ref c;c.Key=chest;c.Base={"skyrim.esm",999};c.Type=28;c.Inventory=1;c.Scale=100;
    HomeSnapshot::State s;s.Refs={c};assert(s.Valid());
    assert(HomeSnapshot::Publish(j,a->Slot,s.Encode()));
    assert(HomeSnapshot::Publish(j,b->Slot,s.Encode()));
    auto cHome=HomeRegistry::Register(j,101,FormKey{"house.esp",200});assert(cHome);
    {
        TradeLedger live(j,71);assert(live.Stream()==71);
        auto legacy=s;
        HomeSnapshot::LegacyStack old{0xFF000123u,{item,2,{}},{}};
        legacy.Legacy.push_back(old);
        assert(legacy.Valid() && HomeSnapshot::Decode(legacy.Encode()));
        auto duplicate=legacy;duplicate.Legacy.push_back(old);assert(!duplicate.Valid());
        assert(live.Placed(cHome->Slot).empty());
        assert(HomeSnapshot::Publish(j,cHome->Slot,legacy.Encode()));
        assert(live.Placed(cHome->Slot).size()==1);
        auto imported=live.Placed(cHome->Slot).front();
        assert(imported.Id.LegacySource==old.Source && imported.Id.Receipt==0);
        assert(imported.Item.Count==2);
        assert(!live.TakePlacement(202,imported.Id.Encode(),item,{},2,0,0));
        assert(live.TakePlacement(101,imported.Id.Encode(),item,{},2,0,0));
        assert(live.Ack("101",1));
        assert(live.Placed(cHome->Slot).empty());
        assert(HomeSnapshot::Publish(j,cHome->Slot,legacy.Encode()));
        assert(live.Placed(cHome->Slot).empty());
        TradeLedger replay(j,71);assert(replay.Stream()==71);
        assert(replay.Placed(cHome->Slot).empty());
    }
    TradeProperties::Values owned;owned.Mask=TradeProperties::Values::HasOwnership;owned.Owner={"skyrim.esm",123};
    const auto variant=owned.Encode();
    HomePlacement::Pose initial{};initial[0]=std::bit_cast<uint32_t>(25.f);
    HomePlacement::Key key{a->Slot,202,1,initial};
    const auto account=key.Encode();
    assert(HomePlacement::Decode(account));
    assert(!HomePlacement::Decode(account+"/0"));
    assert(Set(j,101,a->Slot,202,Right::Visit,0));
    {
        TradeLedger l(j,71);assert(l.Stream()==71);
        assert(!l.CommitPlacement(202,a->Slot,item,variant,3,3,0,initial));
        assert(Set(j,101,a->Slot,202,Right::Modify,1));
        auto invalid=initial;invalid[1]=0x7f800000;
        assert(!l.CommitPlacement(202,a->Slot,item,variant,3,3,0,invalid));
        assert(!l.CommitPlacement(202,a->Slot,item,variant,3,2,0,initial));
        assert(l.CommitPlacement(202,a->Slot,item,variant,3,3,0,initial));
        assert(l.Placed(a->Slot).empty());
        assert(!l.TakePlacement(101,account,item,variant,3,0,0));
        assert(!l.CommitPlacement(202,a->Slot,item,variant,3,3,0,initial));
        assert(l.MatchesReceipt("202",71,0,1,item.Plugin,item.Local,-3,3,variant));
    }
    {
        TradeLedger l(j,71);assert(l.Stream()==71);
        assert(l.Placed(a->Slot).empty());
        assert(l.Ack("202",1)&&l.Ack("202",1));
        assert(l.Placed(a->Slot).size()==1&&l.Placed(b->Slot).empty());
        assert(l.PlacedStack(account)->Item.Variant==variant);
        HomePlacement::Move move{account,1,initial};move.Position[0]=std::bit_cast<uint32_t>(100.f);
        assert(HomePlacement::DecodeMove(move.Encode()));
        assert(l.MovePlacement(202,move));
        assert(!l.MovePlacement(202,move));
        assert(l.PlacedStack(account)->Position==move.Position);
        assert(!l.TakePlacement(101,account,item,{},3,0,0));
        assert(!l.TakePlacement(101,account,item,variant,2,0,0));
        assert(Set(j,101,a->Slot,202,Right::Visit,2));
        assert(!l.TakePlacement(202,account,item,variant,3,0,1));
        assert(l.TakePlacement(101,account,item,variant,3,2,1));
        assert(l.Placed(a->Slot).empty());
        assert(!l.TakePlacement(101,account,item,variant,3,0,0));
        assert(!l.MovePlacement(101,move));
    }
    CampaignJournal::Journal restart,replica;
    assert(restart.Open(path,71,"profile",false));
    assert(replica.Open(path.parent_path()/"replica",71,"profile",true));
    for(uint32_t i=1;i<=j.Current().Revision;++i)
        assert(replica.Import(i,*j.Export(i))==CampaignJournal::Result::Committed);
    for(auto* journal:{&restart,&replica})
    {
        TradeLedger l(*journal,71);assert(l.Stream()==71);
        assert(l.Placed(a->Slot).empty()&&l.Placed(b->Slot).empty());
        assert(l.MatchesReceipt("101",71,1,2,item.Plugin,item.Local,3,2,variant));
        assert(l.Ack("101",2)&&l.Ack("101",2));
        assert(l.AcceptCursor("202",71,0));
        assert(l.History("202").size()==1&&!l.NextRefund("202"));
        assert(HomeSnapshot::Read(*journal,a->Slot).Bytes==s.Encode());
    }
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp,"placement.cpp");p.write_text(source);exe=Path(tmp,"placement")
    subprocess.run(["g++","-std=c++20","-Wall","-Wextra","-Werror","-Wno-unused-parameter",
        "-I",str(common),str(common/"CampaignJournal.cpp"),str(common/"QuestJournalDisk.cpp"),str(p),"-o",str(exe)],check=True)
    subprocess.run([str(exe),str(Path(tmp,"journal"))],check=True)
server=(root/"Code/server/Services/PartyTrade.cpp").read_text()
assert "ledger.CommitPlacement(character,e.Peer,item,variant,e.Count" in server
assert "ledger.TakePlacement(character,payload,item,variant,e.Count" in server
assert "HomeVisit::CanEnter(player,cell.Cell.ModId,cell.Cell.BaseId)" in server
assert "ledger.MovePlacement(character,*move)" in server
print("PASS dynamic property receipts, permissions, pose, replay and server routes")

client=(root/"Code/client/Services/Generic/PartyTrade.cpp").read_text()
actor=(root/"Code/client/Games/Skyrim/Actor.cpp").read_text()
reference=(root/"Code/client/Games/Skyrim/TESObjectREFR.cpp").read_text()
assert "TradeOp::HomePlace,slot,0,w.Cursor()" in client
assert "TradeOp::HomeTake,slot,0,w.Cursor()" in client
assert "TradeOp::HomeMove,slot,0,w.Cursor()" in client
assert "ref->SetSkipSaveFlag(true)" in client
assert "HomePlacementClient::InterceptDrop(apThis,apObject,apExtraData" in actor
assert "HomePlacementClient::InterceptDrop(apThis,apItem,apExtraList" in reference
assert "HomePlacementClient::Tracked(apThis)" in reference
assert "HomePlacementClient::Forget();" in client
assert "return extra.GetCount()==count;" not in client
print("PASS placement client hook wiring (static, not Skyrim runtime)")

manager=(root/"Code/client/Games/ModManager.cpp").read_text()
remote=(root/"Code/client/Services/Generic/InventoryService.cpp").read_text()
assert "reinterpret_cast<uintptr_t>(&linkedRoom)" in manager
assert "if (r!=expected) r=Cast<TESObjectREFR>(TESForm::GetById(d.Ref));" in client
assert "const bool observing=modify && (holding || now<d.ObserveUntil);" in client
assert "!HomePlacementClient::Scoped(pActor)" in remote

assert "HomePlacementClient::CaptureLegacy(*ref)" in (root/"Code/client/Services/Generic/PartyHome.cpp").read_text()
assert "legacyScanned=authority->Operations().size()" in (common/"TradeLedger.h").read_text()
assert "legacySources.insert(source.Source)" in client
