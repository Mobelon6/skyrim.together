"""Durable observations for placed home NPCs: authority, CAS, replica and restart."""
from pathlib import Path
import subprocess,sys,tempfile
root=Path(sys.argv[1]);common=(root/"Code/common").resolve()
source=r'''
#include "HomeResidents.h"
#include "HomeCatalog.h"
#include <cassert>
#include <filesystem>
int main(int argc,char** argv)
{
    using namespace HomeResidents;
    using CampaignJournal::Journal;
    using CampaignJournal::Result;
    Journal j,replica;
    const auto path=std::filesystem::path(argv[1]);
    assert(j.Open(path/"host",71,"profile",true));
    assert(replica.Open(path/"replica",71,"profile",true));
    TradeProperties::FormKey cell{"house.esp",100};
    auto a=HomeRegistry::Register(j,101,cell),b=HomeRegistry::Register(j,202,cell);
    assert(a&&b);
    State s;s.Reference={"house.esp",201};s.Base={"skyrim.esm",101};s.Scale=100;
    assert(Decode(s.Encode()));
    assert(RecoveryPlan(s,s)==Recovery::Matched);
    auto altered=s;altered.Transform[0]=std::bit_cast<uint32_t>(128.f);
    assert(RecoveryPlan(s,altered)==Recovery::Restore);
    altered=s;altered.Flags=1;
    assert(RecoveryPlan(s,altered)==Recovery::Restore);
    auto hidden=altered;altered.Transform[0]=std::bit_cast<uint32_t>(128.f);
    assert(RecoveryPlan(hidden,altered)==Recovery::Matched);
    altered=s;altered.Flags=2;
    assert(RecoveryPlan(s,altered)==Recovery::Conflict);
    altered=s;altered.Flags=4;
    assert(RecoveryPlan(s,altered)==Recovery::Conflict);
    assert(RecoveryPlan(altered,altered)==Recovery::Matched);
    altered=s;altered.Scale=101;
    assert(RecoveryPlan(s,altered)==Recovery::Conflict);
    altered=s;altered.Base.Local++;
    assert(RecoveryPlan(s,altered)==Recovery::Conflict);
    altered=s;altered.Reference.Local++;
    assert(RecoveryPlan(s,altered)==Recovery::Conflict);
    auto bad=s;bad.Transform[0]=0x7f800000;assert(!Decode(bad.Encode()));
    bad=s;bad.Flags=8;assert(!Decode(bad.Encode()));
    assert(!Decode(s.Encode()+" "));
    assert(!Set(j,202,a->Slot,0,0,s));
    assert(Set(j,101,a->Slot,0,0,s));
    auto initial=Read(j,a->Slot);assert(initial&&initial->size()==1);
    const auto key=s.Reference.Encode();
    assert(initial->at(key).Revision==1);
    auto head=j.Current();
    assert(Set(j,101,a->Slot,1,0,s)&&j.Current()==head);
    auto moved=s;moved.Transform[0]=std::bit_cast<uint32_t>(32.f);
    assert(Set(j,101,a->Slot,1,0,moved)&&j.Current()==head);
    moved.Transform[0]=std::bit_cast<uint32_t>(128.f);
    assert(Set(j,101,a->Slot,1,head.Revision,moved));
    auto dead=moved;dead.Flags=2;
    assert(!Set(j,101,a->Slot,1,head.Revision,dead));
    assert(!Set(j,101,a->Slot,2,0,dead));
    assert(Set(j,101,a->Slot,2,head.Revision,dead));
    assert(Set(j,202,b->Slot,0,0,s));
    auto latest=Read(j,a->Slot);assert(latest&&latest->at(key).Revision==3);
    assert(!CanResume(latest->at(key),j.Current().Revision,s));
    assert(!CanResume(latest->at(key),0,dead));
    assert(CanResume(latest->at(key),head.Revision,dead));
    bad=dead;bad.Base.Local++;
    assert(!Set(j,101,a->Slot,3,head.Revision,bad));
    assert(Read(j,b->Slot)->at(key).Value.Flags==0);
    for(uint32_t i=1;i<=j.Current().Revision;++i)
        assert(replica.Import(i,*j.Export(i))==Result::Committed);
    assert(Read(replica,a->Slot)->at(key).Value.Encode()==dead.Encode());
    Journal restart;assert(restart.Open(path/"host",71,"profile",false));
    assert(Read(restart,a->Slot)->at(key).Value.Encode()==dead.Encode());
    std::istringstream manifest("STM-HOME-INSTANCES 2\nH "+cell.Encode()+" 2048\nU "+cell.Encode()+" "+key+" ACHR\n");
    auto catalog=HomeCatalog::Read(manifest);assert(catalog);
    auto c=catalog->at(cell.Encode());assert(c.ResidentCandidates.count(key)&&c.Unsupported.count(key));
    HomeSnapshot::State staticState;assert(!HomeCatalog::Bind(c,staticState));
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp,"resident.cpp");p.write_text(source);exe=Path(tmp,"resident")
    subprocess.run(["g++","-std=c++20","-Wall","-Wextra","-Werror","-Wno-unused-parameter",
        "-I",str(common),str(common/"CampaignJournal.cpp"),str(common/"QuestJournalDisk.cpp"),str(p),"-o",str(exe)],check=True)
    subprocess.run([str(exe),tmp],check=True)
client=(root/"Code/client/Services/Generic/PartyHome.cpp").read_text()
server=(root/"Code/server/Services/PartyHome.cpp").read_text()
assert "SyncResidents(world,*cell,*identity,slot,encoded,slots)" in client
assert "e.AccessOp=5;e.AccessRevision=t.Revision" in client
assert "RestoreResident(cell,old->Value,*state,slots)" in client
assert "HomeResidents::RecoveryPlan(committed,observed)" in client
assert "ctx.SaveRevision<old->SaveRevision" in client
assert "ctx.Tracks.insert_or_assign(key,ResidentTrack{revision,{},{}})" in client
restore=client.split("HomeResidents::Recovery RestoreResident(",1)[1].split("void SyncResidents(",1)[0]
assert "actor->MoveTo(&cell" in restore and "actor->Enable()" in restore and "actor->Disable()" in restore
assert all(x not in restore for x in ("->Kill(", "->Respawn(", "->RestoreRemoteLife(", "->Reset(", "->Delete("))
assert "if (e.AccessOp==5)" in server
assert "HomeResidents::Set(link.Journal,character,e.House,e.AccessRevision,e.SaveRevision,*resident)" in server
print("PASS resident state, scoped authority, revisions, restart, replica, catalog candidates and fail-closed visits")
