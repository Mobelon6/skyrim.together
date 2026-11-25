"""Exercise durable object changes alongside the existing production property ledger."""
from pathlib import Path
import tempfile,sys,subprocess
root=Path(sys.argv[1]);common=(root/"Code/common").resolve()
source=r'''
#include "TradeLedger.h"
#include <cassert>
int main(int argc,char** argv)
{
    CampaignJournal::Journal j;
    const auto path=std::filesystem::path(argv[1]);
    assert(j.Open(path,71,"profile",true));
    using TradeProperties::FormKey;
    FormKey root{"house.esp",100},chest{"house.esp",101},door{"house.esp",102},loose{"house.esp",103},item{"skyrim.esm",42};
    auto a=HomeRegistry::Register(j,101,root),b=HomeRegistry::Register(j,202,root);
    assert(a&&b);
    HomeSnapshot::State s;
    HomeSnapshot::Ref c;c.Key=chest;c.Base={"skyrim.esm",999};c.Type=28;c.Inventory=1;c.Scale=100;
    c.Items={{item,5,{}}};c.LockState=2;c.LockLevel=40;
    HomeSnapshot::Ref d;d.Key=door;d.Base={"skyrim.esm",998};d.Type=29;d.Scale=100;
    HomeSnapshot::Ref r;r.Key=loose;r.Base=item;r.Type=32;r.Inventory=3;r.Scale=100;
    r.Items={{item,3,{}}};s.Refs={c,d,r};assert(s.Valid());
    assert(HomeSnapshot::Publish(j,a->Slot,s.Encode())&&HomeSnapshot::Publish(j,b->Slot,s.Encode()));
    auto view=HomeObjects::Read(j,a->Slot);assert(view&&view->size()==3);
    auto old=view->at(door.Encode()).Value;
    auto save=j.Current().Revision;
    HomeObjects::Change change{a->Slot,1,save,door.Encode(),old};
    change.Value.Open=1;
    assert(HomeObjects::Decode(change.Encode()));
    assert(!HomeObjects::Decode(change.Encode()+" "));
    assert(!HomeObjects::Set(j,202,change,0));
    assert(HomeObjects::Set(j,101,change,0));
    auto h=j.Current();assert(!HomeObjects::Set(j,101,change,0)&&j.Current()==h);
    change.Revision=2;change.Value.Open=3;
    assert(HomeObjects::Set(j,101,change,1));
    change.Revision=3;change.SaveRevision=save-1;
    assert(!HomeObjects::Set(j,101,change,2));
    change.SaveRevision=save;h=j.Current();
    assert(HomeObjects::Set(j,101,change,2)&&j.Current()==h);
    change.Value.Open=2;assert(!HomeObjects::Set(j,101,change,2));
    change.Value.Open=0;assert(!HomeObjects::Set(j,101,change,2));
    auto moved=view->at(loose.Encode()).Value;
    moved.Transform[0]=std::bit_cast<uint32_t>(100.f);
    HomeObjects::Change pose{a->Slot,1,save,loose.Encode(),moved};
    auto invalid=pose;invalid.Value.Disabled=1;assert(!HomeObjects::Set(j,101,invalid,0));
    invalid=pose;invalid.Value.Open=1;assert(!HomeObjects::Set(j,101,invalid,0));
    invalid=pose;invalid.Value.Transform[0]=0x7f800000;assert(!HomeObjects::Set(j,101,invalid,0));
    assert(HomeObjects::Set(j,101,pose,0));
    assert(HomeLocks::Set(j,101,a->Slot,chest.Encode(),0,1,25));
    {
        TradeLedger l(j,71);assert(l.Stream()==71);
        auto state=l.HomeState(a->Slot);assert(state);
        assert(state->Refs[2].Transform==moved.Transform && state->Refs[2].Items[0].Count==3);
        assert(l.CommitHome(101,a->Slot,loose,item,{},3,0,0));
        assert(l.Ack("101",1));
        pose.Revision=2;pose.Value.Transform[0]=std::bit_cast<uint32_t>(200.f);
        assert(HomeObjects::Set(j,101,pose,1));
        state=l.HomeState(a->Slot);assert(state);
        assert(state->Refs[2].Items.empty()&&(state->Refs[2].Flags&1));
        assert(state->Refs[2].Transform==pose.Value.Transform);
        assert(!l.CommitHome(101,a->Slot,loose,item,{},3,3,1));
        assert(state->Refs[0].LockState==1&&state->Refs[0].LockLevel==25);
        assert(l.HomeState(b->Slot)->Refs[2].Items[0].Count==3);
    }
    CampaignJournal::Journal restart,replica;
    assert(restart.Open(path,71,"profile",false));
    assert(replica.Open(path.parent_path()/"replica",71,"profile",true));
    for (uint32_t i=1;i<=j.Current().Revision;++i)
        assert(replica.Import(i,*j.Export(i))==CampaignJournal::Result::Committed);
    for (auto* journal:{&restart,&replica})
    {
        TradeLedger l(*journal,71);assert(l.Stream()==71);
        const auto v=HomeObjects::Read(*journal,a->Slot);assert(v);
        assert(v->at(door.Encode()).Revision==2&&v->at(door.Encode()).Value.Open==3);
        assert(l.HomeState(a->Slot)->Refs[2].Items.empty());
        assert(l.HomeState(b->Slot)->Refs[2].Transform==r.Transform);
        assert(HomeSnapshot::Read(*journal,a->Slot).Bytes==s.Encode());
    }
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp,"objects.cpp");p.write_text(source);exe=Path(tmp,"objects")
    subprocess.run(["g++","-std=c++20","-Wall","-Wextra","-Werror","-Wno-unused-parameter",
        "-I",str(common),str(common/"CampaignJournal.cpp"),str(common/"QuestJournalDisk.cpp"),str(p),"-o",str(exe)],check=True)
    subprocess.run([str(exe),str(Path(tmp,"journal"))],check=True)
home=(root/"Code/client/Services/Generic/PartyHome.cpp").read_text()
server=(root/"Code/server/Services/PartyHome.cpp").read_text()
assert "SyncObjects(world,*cell,*identity,slot,encoded)" in home
assert "ApplyObjectChanges(m_world,*current,*copy,*state,visit.Campaign,visit.Slot)" in home
assert "TryTrade(form); // Always consume activation" in home
assert "if (IsExit(copy,*binding)) continue;" in home
assert "PapyrusFunction<TESObjectREFR::OpenState,TESObjectREFR>" in home
assert "HomeObjects::Set(link.Journal,character,*change,e.AccessRevision)" in server
print("PASS object state authority, CAS, restart, replica, lock and placed property conservation")

assert "RestoreObject(world,cell,*object,old.Type,old.Value.Disabled!=0,old.Value)" in home
assert "if (!hide && ref->IsDisabled()) ref->Enable();" in home
assert "if (ctx.SaveRevision<old.SaveRevision)" in home
assert "Never publish a value in the same tick that restores it" in home
