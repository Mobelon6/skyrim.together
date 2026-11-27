"""Exercise the actual checkpoint codec, journal and retry storage."""
from pathlib import Path
import sys, subprocess, tempfile
root=Path(sys.argv[1])
common=root/"Code/common"
test=r"""
#include "CampaignCheckpoint.h"
#include <cassert>
#include <limits>
using namespace CampaignJournal;
int main(int argc,char** argv)
{
    const std::filesystem::path root=argv[1];
    Journal server,replica,restart;
    assert(server.Open(root/"server",71,"profile",true));
    assert(replica.Open(root/"replica",71,"profile",true));
    const auto initial=server.Current();
    CampaignCheckpoint::Record a;
    a.Save={71,5,initial};a.Observed=initial;a.Digest=std::string(64,'a');
    a.Cell={"skyrim.esm",0x1234};a.Pose[0]=std::bit_cast<uint32_t>(-25.25f);
    assert(a.Valid());
    auto decoded=CampaignCheckpoint::Decode(a.Encode());assert(decoded&&decoded->Encode()==a.Encode());
    assert(!CampaignCheckpoint::Decode(a.Encode()+" "));
    auto bad=a;bad.Pose[0]=std::bit_cast<uint32_t>(std::numeric_limits<float>::quiet_NaN());assert(!bad.Valid());
    bad=a;bad.Digest="../bad";assert(!bad.Valid()&&!CampaignCheckpoint::StoreLocal(root,bad));
    assert(CampaignCheckpoint::StoreLocal(root,a));assert(CampaignCheckpoint::StoreLocal(root,a));
    assert(CampaignCheckpoint::ReadLocal(root,71,5,a.Digest)->Encode()==a.Encode());
    assert(!CampaignCheckpoint::ReadLocal(root,72,5,a.Digest));
    assert(!CampaignCheckpoint::ReadLocal(root,71,6,a.Digest));
    assert(!CampaignCheckpoint::ReadLocal(root,71,5,std::string(64,'b')));
    assert(CampaignCheckpoint::Publish(server,71,6,a)==Result::Invalid);
    assert(CampaignCheckpoint::Publish(server,72,5,a)==Result::Invalid);
    bad=a;bad.Observed.Tag++;assert(CampaignCheckpoint::Publish(server,71,5,bad)==Result::Invalid);
    assert(CampaignCheckpoint::Publish(server,71,5,a)==Result::Committed);
    const auto one=server.Current();
    assert(CampaignCheckpoint::Publish(server,71,5,a)==Result::Duplicate&&server.Current()==one);
    bad=a;bad.Pose[0]++;assert(CampaignCheckpoint::Publish(server,71,5,bad)==Result::Conflict);
    assert(!CampaignCheckpoint::StoreLocal(root,bad));
    assert(replica.Import(1,*server.Export(1))==Result::Committed);
    assert(CampaignCheckpoint::Find(replica,71,5,a.Digest)->Encode()==a.Encode());
    assert(!CampaignCheckpoint::Find(replica,71,6,a.Digest));
    assert(server.Append(one,{"other","trade-test","new state"})==Result::Committed);
    auto b=a;b.Digest=std::string(64,'b');b.Observed=server.Current();
    assert(CampaignCheckpoint::Publish(server,71,5,b)==Result::Committed);
    assert(CampaignCheckpoint::Find(server,71,5,a.Digest)->Save.Applied==initial);
    assert(CampaignCheckpoint::Find(server,71,5,b.Digest)->Observed==b.Observed);
    assert(restart.Open(root/"server",71,"profile",false));
    assert(CampaignCheckpoint::Find(restart,71,5,a.Digest)->Encode()==a.Encode());
    assert(CampaignCheckpoint::Find(restart,71,5,b.Digest)->Encode()==b.Encode());
    // Two additional saves, then a process restart while a different ESS is loaded.
    auto c=b;c.Digest=std::string(64,'8');auto d=c;d.Digest=std::string(64,'9');
    auto otherCharacter=d;otherCharacter.Save.Character=6;otherCharacter.Digest=std::string(64,'7');
    assert(CampaignCheckpoint::StoreLocal(root,c)&&CampaignCheckpoint::StoreLocal(root,d));
    assert(CampaignCheckpoint::StoreLocal(root,otherCharacter));
    std::map<std::string,CampaignCheckpoint::Record> pending;
    assert(CampaignCheckpoint::RestoreOutbox(root,restart,71,5,pending)&&pending.size()==2);
    for (const auto& [id,r]:pending)
        assert(CampaignCheckpoint::Publish(restart,71,5,r)==Result::Committed);
    pending.clear();
    assert(CampaignCheckpoint::RestoreOutbox(root,restart,71,5,pending)&&pending.empty());
    assert(CampaignCheckpoint::RestoreOutbox(root,restart,71,6,pending)&&pending.size()==1);
    assert(server.Open(root/"server",71,"profile",false));
    auto home=HomeRegistry::Register(server,5,a.Cell);assert(home);
    auto owned=a;owned.Digest=std::string(64,'c');owned.HomeSlot=home->Slot;owned.Observed=server.Current();
    assert(CampaignCheckpoint::Publish(server,71,5,owned)==Result::Committed);
    auto foreign=owned;foreign.Save.Character=6;foreign.Digest=std::string(64,'d');
    assert(CampaignCheckpoint::Publish(server,71,6,foreign)==Result::Invalid);
    // A registered slot is not enough: authority must have granted access
    // at the captured prefix. New permission cannot retroactively authorize.
    assert(HomePermissions::Set(server,5,home->Slot,6,HomePermissions::Right::Visit,0));
    assert(CampaignCheckpoint::Publish(server,71,6,foreign)==Result::Invalid);
    foreign.Observed=server.Current();
    foreign.Entrance={"skyrim.esm",0x5678};foreign.Shape=1234;
    auto guest=CampaignCheckpoint::Decode(foreign.Encode());
    assert(guest&&guest->Encode()==foreign.Encode());
    auto noShape=foreign;noShape.Shape=0;assert(!noShape.Valid());
    auto noSlot=foreign;noSlot.HomeSlot=0;assert(!noSlot.Valid());
    assert(CampaignCheckpoint::CanResume(server,71,foreign));
    assert(CampaignCheckpoint::Publish(server,71,6,foreign)==Result::Committed);
    const auto historical=foreign.Observed;
    assert(HomePermissions::Set(server,5,home->Slot,6,HomePermissions::Right::None,1));
    assert(!CampaignCheckpoint::CanResume(server,71,foreign));
    assert(CampaignCheckpoint::Find(server,71,6,foreign.Digest));
    assert(CampaignCheckpoint::Publish(server,71,6,foreign)==Result::Duplicate);
    auto after=foreign;after.Digest=std::string(64,'e');after.Observed=server.Current();
    assert(CampaignCheckpoint::Publish(server,71,6,after)==Result::Invalid);
    assert(HomePermissions::Set(server,5,home->Slot,6,HomePermissions::Right::Modify,2));
    assert(CampaignCheckpoint::CanResume(server,71,foreign));
    assert(CampaignCheckpoint::Find(server,71,6,foreign.Digest)->Observed==historical);
    auto wrong=foreign;wrong.Digest=std::string(64,'f');wrong.Cell.Local++;
    assert(CampaignCheckpoint::Publish(server,71,6,wrong)==Result::Invalid);
    auto room=HomeRegistry::Register(server,5,{"skyrim.esm",0x1235});assert(room);
    auto linked=foreign;linked.Digest=std::string(64,'1');linked.Cell=room->Root;linked.HomeSlot=room->Slot;
    linked.EntrySlot=home->Slot;linked.ArrivalPose[0]=std::bit_cast<uint32_t>(12.5f);
    linked.Observed=server.Current();
    assert(linked.Valid()&&!CampaignCheckpoint::HadHomeAccess(server,linked));
    assert(HomePermissions::SetGroup(server,5,{home->Slot,room->Slot},6,HomePermissions::Right::Visit,3));
    linked.Observed=server.Current();
    assert(CampaignCheckpoint::CanResume(server,71,linked));
    auto parsed=CampaignCheckpoint::Decode(linked.Encode());assert(parsed&&parsed->Encode()==linked.Encode());
    assert(CampaignCheckpoint::Publish(server,71,6,linked)==Result::Committed);
    assert(CampaignCheckpoint::Find(server,71,6,linked.Digest)->EntrySlot==home->Slot);
    assert(!CampaignCheckpoint::Decode(linked.Encode()+" 1"));
    auto malformed=linked;malformed.EntrySlot=room->Slot;assert(!malformed.Valid());
    malformed=linked;malformed.ArrivalPose[0]=std::bit_cast<uint32_t>(std::numeric_limits<float>::infinity());assert(!malformed.Valid());
    malformed=linked;malformed.EntrySlot=0;assert(!malformed.Valid());
    auto other=HomeRegistry::Register(server,9,{"skyrim.esm",0x1234});assert(other);
    assert(HomePermissions::Set(server,9,other->Slot,6,HomePermissions::Right::Visit,4));
    malformed=linked;malformed.EntrySlot=other->Slot;malformed.Observed=server.Current();
    assert(!CampaignCheckpoint::HadHomeAccess(server,malformed));
    assert(HomePermissions::Set(server,5,home->Slot,6,HomePermissions::Right::None,5));
    assert(CampaignCheckpoint::Find(server,71,6,linked.Digest));
    assert(!CampaignCheckpoint::CanResume(server,71,linked));
}
"""
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp,"checkpoint.cpp");src.write_text(test);exe=Path(tmp,"checkpoint")
    subprocess.run(["g++","-std=c++20","-Wall","-Wextra","-Werror","-Wno-unused-parameter","-I",str(common),str(common/"CampaignIdentity.cpp"),str(common/"CampaignJournal.cpp"),str(common/"QuestJournalDisk.cpp"),str(src),"-o",str(exe)],check=True)
    subprocess.run([str(exe),tmp],check=True)
client=(root/"Code/client/Services/Generic/QuestJournal.cpp").read_text()
server=(root/"Code/server/Services/QuestJournal.cpp").read_text()
native=(root/"Code/client/Games/SaveLoad.cpp").read_text()
assert "request.Checkpoint=NextCheckpoint(*stamp)" in client
assert server.index("CampaignAccess::Admit(m_world,*event.pPlayer,stamp")<server.index("CampaignCheckpoint::Publish(")
assert "write.Entered&&write.Returned&&!write.Invalid" in native
assert "digest&&*digest!=write.Before" in native
assert "CampaignCheckpointFile::Digest(path)==before" in native
assert "0x60FD40" in native and "0x605880" in native
assert "HomeInstance::ProjectionForm(cell->formID)" in client
print("PASS exact checkpoint identity, immutable retry, authority, replication, old save, restart, owned and historical guest scope, revocation and regrant")
