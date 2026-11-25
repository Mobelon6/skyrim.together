"""Exercise the actual durable campaign journal, not a substitute implementation."""
from pathlib import Path
import sys, tempfile, subprocess
common = Path(sys.argv[1], "Code/common").resolve()
test = r"""
#include "CampaignJournal.h"
#include "QuestJournalDisk.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
using namespace CampaignJournal;
namespace fs=std::filesystem;
int main(int argc,char** argv)
{
    const fs::path root=argv[1], host=root/"host", guest=root/"guest", other=root/"other";
    Journal h,g;
    assert(!h.Open(host,71,"profile",false));
    assert(h.Open(host,71,"profile",true));
    auto zero=h.Current();
    assert(zero.Revision==0 && zero.Tag);
    assert(!g.Open(host,71,"profile",true));
    assert(g.Open(guest,71,"profile",true)&&g.Current()==zero);
    assert(!g.Open(guest,71,"wrong",false));
    assert(g.Open(guest,71,"profile",false));
    Operation move{"op-1","house-transfer",std::string("a\0b\n",4)};
    assert(h.Append(zero,move)==Result::Committed);
    const auto one=h.Current();
    assert(one.Revision==1 && h.Append(zero,move)==Result::Duplicate && h.Current()==one);
    assert(h.Append(one,{"op-1","house-transfer","different"})==Result::Conflict);
    assert(h.Append(zero,{"op-2","trade","x"})==Result::Conflict);
    assert(h.Append(one,{"bad/id","trade","x"})==Result::Invalid);
    assert(h.Append(one,{"large","trade",std::string(1801,'a')})==Result::Invalid);
    assert(h.Append(one,{"op-2","quest","stage 20"})==Result::Committed);
    assert(g.Import(2,*h.Export(2))==Result::Conflict);
    auto corrupt=*h.Export(1);corrupt.back()=corrupt.back()=='1'?'2':'1';
    assert(g.Import(1,corrupt)==Result::Invalid);
    assert(g.Import(1,*h.Export(1))==Result::Committed);
    assert(g.Import(1,*h.Export(1))==Result::Duplicate);
    assert(g.Import(1,corrupt)==Result::Conflict);
    // Reopen a stale replica, then catch up without applying any game effects.
    Journal restart;
    assert(restart.Open(guest,71,"profile",false)&&restart.Current()==one);
    assert(restart.Import(2,*h.Export(2))==Result::Committed);
    assert(restart.Current()==h.Current()&&restart.Operations()==h.Operations());
    Journal serverRestart;
    assert(serverRestart.Open(host,71,"profile",false));
    assert(serverRestart.Current()==h.Current()&&serverRestart.Operations()==h.Operations());
    assert(serverRestart.Append(zero,move)==Result::Duplicate);
    // A stale writer must not overwrite an already published conflicting operation.
    assert(h.Append(h.Current(),{"op-3","trade","A"})==Result::Committed);
    assert(serverRestart.Append(serverRestart.Current(),{"op-3","trade","B"})==Result::StorageError);
    assert(serverRestart.Open(host,71,"profile",false)&&serverRestart.Current()==h.Current());
    // Different campaign or profile cannot consume the same history.
    Journal o;
    assert(o.Open(other,72,"profile",true));
    assert(o.Import(1,*h.Export(1))==Result::Invalid);
    assert(o.Open(other,73,"different",true));
    assert(o.Import(1,*h.Export(1))==Result::Invalid);
    std::ofstream(host/"71"/"5.tsv.aborted.tmp")<<"partial";
    assert(serverRestart.Open(host,71,"profile",false));
    fs::remove(host/"71"/"3.tsv");
    assert(!serverRestart.Open(host,71,"profile",false)&&!serverRestart.Ready());
    assert(serverRestart.Append({},move)==Result::Invalid);
    assert(QuestJournalDisk::Store(host,71,3,*h.Export(2)));
    assert(serverRestart.Open(host,71,"profile",false));
    std::ofstream(host/"71"/"4.tsv",std::ios::trunc)<<"4\tbad\n";
    assert(!serverRestart.Open(host,71,"profile",false));
    uint32_t count{};
    std::ofstream(guest/"71"/"4.tsv")<<"4\tincomplete";
    assert(!QuestJournalDisk::Inspect(guest,71,count));
    assert(QuestJournalDisk::Contiguous(guest,71)==3);
}
"""
with tempfile.TemporaryDirectory() as folder:
    src=Path(folder,"campaign.cpp")
    src.write_text(test)
    exe=Path(folder,"test")
    subprocess.run(["g++","-std=c++20","-Wall","-Wextra","-Werror","-Wno-unused-parameter","-I",str(common),str(common/"CampaignJournal.cpp"),str(common/"QuestJournalDisk.cpp"),str(src),"-o",str(exe)],check=True)
    subprocess.run([str(exe),folder],check=True)
print("PASS campaign disk history, restart, idempotence, stale writer, replica catch-up, identity, gaps and corruption")
