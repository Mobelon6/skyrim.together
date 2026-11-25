"""Production snapshot encoding, upload, journal recovery and immutable baseline."""
from pathlib import Path
import sys,tempfile,subprocess
root=Path(sys.argv[1])
common=(root/"Code/common").resolve()
source=r'''
#include "HomeSnapshot.h"
#include <cassert>
#include <filesystem>
int main(int argc,char** argv)
{
    using namespace HomeSnapshot;
    using CampaignJournal::Journal;
    using CampaignJournal::Result;
    const auto path=std::filesystem::path(argv[1]);
    Journal host,replica;
    assert(host.Open(path/"host",71,"profile",true));
    assert(replica.Open(path/"replica",71,"profile",true));
    TradeProperties::FormKey cell{"skyrim.esm",0x165A8};
    auto a=HomeRegistry::Register(host,101,cell),b=HomeRegistry::Register(host,202,cell);
    assert(a&&b);
    State s;s.Omitted=3;
    Ref chest;
    chest.Key={"house.esl",0x800};chest.Base={"skyrim.esm",0x500};
    chest.Type=28;chest.Inventory=1;chest.LockState=2;chest.LockLevel=40;chest.Scale=100;
    TradeProperties::Values own;own.Mask=TradeProperties::Values::HasOwnership;own.Owner={"skyrim.esm",0x123};
    chest.Items={{{"skyrim.esm",0x100},2,{}},{{"skyrim.esm",0x100},1,own.Encode()}};
    s.Refs.push_back(chest);
    // Enough distinct references to cross chunk boundaries.
    for (uint32_t i=801;i<850;++i) {
        auto r=chest;r.Key.Local=i;r.Items.clear();r.Inventory=2;r.Flags=i%2;s.Refs.push_back(r);
    }
    std::sort(s.Refs.begin(),s.Refs.end(),[](auto& x,auto& y){return x.Key.Encode()<y.Key.Encode();});
    assert(s.Valid());
    const auto bytes=s.Encode();assert(bytes.size()>PartBytes);
    auto decoded=Decode(bytes);assert(decoded&&decoded->Encode()==bytes&&decoded->Omitted==3);
    assert(!Decode(bytes+"junk"));
    auto bad=s;bad.Refs.front().Items.push_back(bad.Refs.front().Items.front());assert(!bad.Valid());
    bad=s;bad.Refs.front().Transform[0]=0x7f800000;assert(!bad.Valid());
    bad=s;bad.Refs.front().Inventory=0;assert(!bad.Valid());
    bad=s;bad.Refs.front().LockState=0;assert(!bad.Valid());
    Upload u;
    const auto n=static_cast<uint32_t>((bytes.size()+PartBytes-1)/PartBytes);
    const auto hash=Digest(bytes);
    assert(!u.Add(hash,n,1,bytes.substr(PartBytes,PartBytes)));
    assert(!u.Add(hash,n,0,"short"));
    for (uint32_t i=0;i<n;++i) assert(u.Add(hash,n,i,bytes.substr(i*PartBytes,PartBytes)));
    assert(u.Complete()&&u.Bytes==bytes);
    assert(host.Append(host.Current(),{Prefix(a->Slot,hash)+"0","home-static-part-v1",bytes.substr(0,PartBytes)})==Result::Committed);
    assert(Read(host,a->Slot).State==Status::Missing);
    assert(Publish(host,a->Slot,bytes));
    const auto revision=host.Current();
    assert(Publish(host,a->Slot,bytes)&&host.Current()==revision);
    assert(Read(host,a->Slot).Bytes==bytes&&Read(host,b->Slot).State==Status::Missing);
    auto changed=s;changed.Refs.front().Flags^=1;
    assert(!Publish(host,a->Slot,changed.Encode())&&host.Current()==revision);
    assert(Publish(host,b->Slot,changed.Encode()));
    assert(Read(host,b->Slot).Bytes==changed.Encode());
    for(uint32_t i=1;i<=revision.Revision;++i) {
        assert(replica.Import(i,*host.Export(i))==Result::Committed);
        if(i>=2 && i<revision.Revision) assert(Read(replica,a->Slot).State==Status::Missing);
    }
    assert(Read(replica,a->Slot).Bytes==bytes);
    Journal restart;assert(restart.Open(path/"host",71,"profile",false));
    assert(Read(restart,a->Slot).Bytes==bytes&&Read(restart,b->Slot).Bytes==changed.Encode());
    assert(!Publish(restart,999,bytes));
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
trade=(root/"Code/client/Services/Generic/PartyTrade.cpp").read_text()
assert "UploadBaseline(world,*cell,*identity,slot,encoded,slots)" in client
assert "HomeInventory::Capture(*ref)" in client
assert "HomeRegistry::OwnsCell(link.Journal,character" in server
assert "HomeSnapshot::Publish(link.Journal,e.House,p.Data.Bytes)" in server
assert "auto extra=ReadExtra(list)" in trade
print("PASS static home baseline, exact supported variant rows, bounded chunks, interrupted publication, replay and no replacement")
