"""Run the real persistent home registry against the real campaign journal."""
from pathlib import Path
import sys,tempfile,subprocess
root=Path(sys.argv[1])
common=(root/"Code/common").resolve()
source=r'''
#include "HomeRegistry.h"
#include <cassert>
#include <filesystem>
int main(int argc,char** argv)
{
    using namespace HomeRegistry;
    using CampaignJournal::Journal;
    using CampaignJournal::Result;
    const auto path=std::filesystem::path(argv[1]);
    Journal host,replica;
    assert(host.Open(path/"host",71,"profile",true));
    assert(replica.Open(path/"replica",71,"profile",true));
    TradeProperties::FormKey breeze{"skyrim.esm",0x165A8},modded{"modhouse.esl",0x900};
    auto a=Register(host,101,breeze);auto b=Register(host,202,breeze);
    assert(a&&b&&a->Slot==1&&b->Slot==2);
    assert(OwnsCell(host,101,ScopePrefix|a->Slot,breeze.Local));
    assert(!OwnsCell(host,202,ScopePrefix|a->Slot,breeze.Local));
    assert(!OwnsCell(host,101,ScopePrefix|a->Slot,breeze.Local+1));
    assert(!OwnsCell(host,101,PendingPrefix|a->Slot,breeze.Local));
    const auto before=host.Current();
    assert(Register(host,101,breeze)->Slot==a->Slot&&host.Current()==before);
    assert(!Register(host,0,breeze));
    assert(!Register(host,101,{"Skyrim.esm",1}));
    assert(!Find(replica,101,breeze));
    assert(replica.Import(1,*host.Export(1))==Result::Committed);
    assert(Find(replica,101,breeze)->Slot==a->Slot&&!Find(replica,202,breeze));
    assert(replica.Import(2,*host.Export(2))==Result::Committed);
    assert(Find(replica,202,breeze)->Slot==b->Slot);
    assert(host.Append(host.Current(),{"unrelated-quest","quest","stage 20"})==Result::Committed);
    auto c=Register(host,101,modded);assert(c&&c->Slot==3);
    Journal reopened;assert(reopened.Open(path/"host",71,"profile",false));
    assert(Register(reopened,101,breeze)->Slot==1);
    assert(Register(reopened,202,breeze)->Slot==2);
    assert(Register(reopened,101,modded)->Slot==3);
    assert(reopened.Current()==host.Current());
    assert(!BySlot(reopened,0)&&!BySlot(reopened,4));
    Journal other;assert(other.Open(path/"other",72,"profile",true));
    assert(!Find(other,101,breeze)&&other.Import(1,*host.Export(1))==Result::Invalid);
    // A valid journal checksum does not make a malformed registry row valid.
    assert(reopened.Append(reopened.Current(),{Id(4),"home-register-v1",Payload(*a)})==Result::Committed);
    assert(!Read(reopened)&&!Register(reopened,303,modded));
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp,"home.cpp");src.write_text(source)
    exe=Path(tmp,"home")
    subprocess.run(["g++","-std=c++20","-Wall","-Wextra","-Werror","-Wno-unused-parameter",
                    "-I",str(common),str(common/"CampaignJournal.cpp"),str(common/"QuestJournalDisk.cpp"),
                    str(src),"-o",str(exe)],check=True)
    subprocess.run([str(exe),tmp],check=True)
client=(root/"Code/client/Services/Generic/PartyHome.cpp").read_text()
server=(root/"Code/server/Services/PartyHome.cpp").read_text()
discovery=(root/"Code/client/Services/Generic/DiscoveryService.cpp").read_text()
player=(root/"Code/server/Services/PlayerService.cpp").read_text()
assert "CampaignAccess::Admit(m_world,*owner,stamp" in server
assert "HomeRegistry::Register(link.Journal,CampaignAccess::Character(*owner),*root)" in server
assert "owner->Send(accepted)" in server and "accepted.Event.Credential" not in server
assert "acknowledgedHome.slot==slot && acknowledgedHome.root==encoded" in client
assert "CampaignAccess::HomeScope(identity->Save.Campaign,identity->Save.Character,encoded)" in client
assert "HomeRegistry::PendingPrefix|id" in client
assert "HomeVisit::CanEnter(*pPlayer,message.CellId.ModId,message.CellId.BaseId)" in player
assert discovery.index("HomeInstance::Prepare(m_world,cell)") < discovery.index("m_interiorNetworkCell!=cellChangeEvent.CellId")
assert "*pCellIdComponent=cell;" in player
print("PASS durable home identity, two owners, restart, replica lag, duplicate registration, malformed history and scoped cell routing")
