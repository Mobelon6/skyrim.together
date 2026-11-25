"""Durable owner lock changes, isolated slots and compare-and-swap replay."""
from pathlib import Path
import tempfile,sys,subprocess
r=Path(sys.argv[1]);common=(r/"Code/common").resolve()
source=r'''
#include "TradeLedger.h"
#include <cassert>
int main(int argc,char** argv)
{
    CampaignJournal::Journal j;
    assert(j.Open(std::filesystem::path(argv[1]),71,"profile",true));
    TradeProperties::FormKey root{"house.esp",100};
    auto a=HomeRegistry::Register(j,101,root),b=HomeRegistry::Register(j,202,root);
    assert(a&&b);
    HomeSnapshot::State s;HomeSnapshot::Ref chest;
    chest.Key={"house.esp",101};chest.Base={"skyrim.esm",123};chest.Type=28;
    chest.Inventory=1;chest.Scale=100;chest.LockState=2;chest.LockLevel=40;
    s.Refs.push_back(chest);
    assert(HomeSnapshot::Publish(j,a->Slot,s.Encode()));
    assert(HomeSnapshot::Publish(j,b->Slot,s.Encode()));
    auto key=chest.Key.Encode();
    assert(!HomeLocks::Set(j,202,a->Slot,key,0,1,40));
    assert(HomeLocks::Set(j,101,a->Slot,key,0,1,40));
    auto locks=HomeLocks::Read(j,a->Slot);assert(locks&&locks->at(key).Revision==1);
    assert(!HomeLocks::Set(j,101,a->Slot,key,0,2,40));
    assert(HomeLocks::Set(j,101,a->Slot,key,1,2,40));
    assert(!HomeLocks::Set(j,101,a->Slot,key,0,1,40)); // ABA
    assert(HomeLocks::Set(j,101,a->Slot,key,2,1,25));
    assert(!HomeLocks::Set(j,101,a->Slot,key,3,0,0));
    auto revision=j.Current();assert(HomeLocks::Set(j,101,a->Slot,key,3,1,25)&&j.Current()==revision);
    CampaignJournal::Journal restart;assert(restart.Open(std::filesystem::path(argv[1]),71,"profile",false));
    TradeLedger l(restart,71);
    auto sa=l.HomeState(a->Slot),sb=l.HomeState(b->Slot);
    assert(sa&&sb&&sa->Refs[0].LockState==1&&sa->Refs[0].LockLevel==25);
    assert(sb->Refs[0].LockState==2&&sb->Refs[0].LockLevel==40);
    assert(HomeSnapshot::Read(restart,a->Slot).Bytes==s.Encode());
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp,"lock.cpp");p.write_text(source);exe=Path(tmp,"lock")
    subprocess.run(["g++","-std=c++20","-Wall","-Wextra","-Werror","-Wno-unused-parameter",
        "-I",str(common),str(common/"CampaignJournal.cpp"),str(common/"QuestJournalDisk.cpp"),str(p),"-o",str(exe)],check=True)
    subprocess.run([str(exe),str(Path(tmp,"journal"))],check=True)
print("PASS lock authority, stale revision, ABA, idempotence, restart and independent slots")
