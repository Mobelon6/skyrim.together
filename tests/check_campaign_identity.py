"""Test the real save stamp codec and server admission with the generated ESP."""
import struct, subprocess, sys, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/"tools"))
from build_trade_plugin import make
data=make()
kind,length,flags,form,*_=struct.unpack_from("<4sIIIIHH",data)
assert kind==b"TES4" and flags==0x200 and form==0
end=24+length
assert struct.unpack_from("<I",data,38)[0]>=0x810
assert data[end:end+4]==b"GRUP" and data[end+8:end+12]==b"GLOB"
globals_end=end+struct.unpack_from("<I",data,end+4)[0]
assert end+24<=globals_end<=len(data)
i=end+24
names=[]
while i<globals_end:
    kind,length,flags,form,*_=struct.unpack_from("<4sIIIIHH",data,i)
    assert kind==b"GLOB" and flags==0 and form==0x01000800+len(names)
    start=i+24;stop=start+length
    fields={}
    while start<stop:
        tag,size=struct.unpack_from("<4sH",data,start);start+=6
        fields[tag]=data[start:start+size];start+=size
    assert start==stop and fields[b"FNAM"]==b"f" and fields[b"FLTV"]==bytes(4)
    names.append(fields[b"EDID"].rstrip(b"\0").decode())
    i=stop
assert i==globals_end
assert names==["STRTradeStreamLow","STRTradeStreamMiddle","STRTradeStreamHigh","STRTradeCursor","STRCampaignCampaignLow","STRCampaignCampaignMiddle","STRCampaignCampaignHigh","STRCampaignCharacterLow","STRCampaignCharacterMiddle","STRCampaignCharacterHigh","STRCampaignRevisionLow","STRCampaignRevisionHigh","STRCampaignTagLow","STRCampaignTagMiddle","STRCampaignTagHigh","STRCampaignSchema"]
common=Path(sys.argv[1],"Code/common").resolve()
test=r"""
#include "CampaignIdentity.h"
#include <cassert>
#include <fstream>
#include <limits>
using namespace CampaignIdentity;
using namespace CampaignJournal;
namespace fs=std::filesystem;
int main(int argc,char** argv)
{
    const fs::path root=argv[1];
    Journal journal;
    assert(journal.Open(root/"authority",71,"profile",true));
    const auto zero=journal.Current();
    Stamp a{71,UINT64_MAX,zero},out;
    Fields blank{};
    assert(Decode(blank,out)==SaveState::Empty);
    auto fields=Encode(a);
    assert(fields && Decode(*fields,out)==SaveState::Valid && out==a);
    assert(Known(journal,71,out));
    assert(!Known(journal,72,out));
    auto extreme=Stamp{UINT64_MAX,UINT64_MAX,{UINT32_MAX-1,UINT64_MAX}};
    assert(Decode(*Encode(extreme),out)==SaveState::Valid && out==extreme);
    assert(!Encode({71,0,zero}));
    auto bad=*fields;bad[11]=0;assert(Decode(bad,out)==SaveState::Invalid);
    bad=*fields;bad[2]=1048576;assert(Decode(bad,out)==SaveState::Invalid);
    bad=*fields;bad[6]=0.25f;assert(Decode(bad,out)==SaveState::Invalid);
    bad=*fields;bad[8]=std::numeric_limits<float>::quiet_NaN();assert(Decode(bad,out)==SaveState::Invalid);
    assert(journal.Append(zero,{"one","test","value"})==Result::Committed);
    // A save on a valid older revision is recognizable; it is not automatically advanced.
    assert(Known(journal,71,a));
    a.Applied.Tag++;assert(!Known(journal,71,a));
    a.Applied={7,zero.Tag};assert(!Known(journal,71,a));
    const std::string key1(64,'a'),key2(64,'b');
    {std::ofstream f(root/"Participants.txt");f<<"1 "<<key1<<"\n2 "<<key2<<"\n";}
    {std::ofstream f(root/"CharacterLink.txt");f<<"1 "<<key1<<"\n";}
    auto credential=ReadCredential(root);assert(credential && credential->Character==1);
    Admission admission;assert(admission.Load(root));
    assert(!admission.Claim(1,{1,key2}));
    assert(admission.Claim(1,*credential));
    assert(admission.Claim(1,*credential));
    assert(!admission.Claim(2,*credential));
    assert(!admission.Claim(1,{2,key2}));
    assert(admission.Claim(2,{2,key2}));
    admission.Release(1);assert(admission.Claim(3,*credential));
    {std::ofstream f(root/"Participants.txt");f<<"1 "<<key1<<"\n2 "<<key1<<"\n";}
    assert(!admission.Load(root)&&!admission.Claim(1,*credential));
    {std::ofstream f(root/"CharacterLink.txt");f<<"1 "<<key1<<" unexpected\n";}
    assert(!ReadCredential(root));
}
"""
with tempfile.TemporaryDirectory() as folder:
    src=Path(folder,"identity.cpp");src.write_text(test)
    exe=Path(folder,"test")
    subprocess.run(["g++","-std=c++20","-Wall","-Wextra","-Werror","-Wno-unused-parameter","-I",str(common),str(common/"CampaignIdentity.cpp"),str(common/"CampaignJournal.cpp"),str(common/"QuestJournalDisk.cpp"),str(src),"-o",str(exe)],check=True)
    subprocess.run([str(exe),folder],check=True)
print("PASS stable ESP globals, save identity and revision codec, invalid stamps, participant admission and reconnect")
