"""Run the production campaign pull/receive logic and physical journal."""
from pathlib import Path
import sys, subprocess, tempfile
common=Path(sys.argv[1],"Code/common").resolve()
test=r"""
#include "CampaignReplication.h"
#include <cassert>
#include <filesystem>
#include <string>
using namespace CampaignReplication;
using CampaignJournal::Result;
namespace fs=std::filesystem;
int main(int argc,char** argv)
{
    CampaignJournal::Journal h,g,other;
    const fs::path root=argv[1];
    assert(h.Open(root/"authority",71,"profile",true));
    assert(g.Open(root/"replica",71,"profile",true));
    assert(h.Append(h.Current(),{"one","quest","stage"})==Result::Committed);
    assert(h.Append(h.Current(),{"two","trade","item"})==Result::Committed);
    Pull p{71,1,"profile",g.Current()};
    auto r=Serve(h,71,"profile",p);
    assert(r.State==Status::Record&&r.Total==2&&r.Target.Revision==1);
    auto bad=r;bad.Nonce++;
    assert(Receive(g,p,bad)==Result::Conflict&&g.Current()==p.Base);
    bad=r;bad.Target.Tag++;
    assert(Receive(g,p,bad)==Result::Conflict&&g.Current()==p.Base);
    bad=r;bad.Row.back()='x';assert(Receive(g,p,bad)==Result::Conflict);
    assert(Receive(g,p,r)==Result::Committed);
    assert(Receive(g,p,r)==Result::Conflict);
    p.Nonce++;p.Base=g.Current();
    auto r2=Serve(h,71,"profile",p);
    assert(g.Open(root/"replica",71,"profile",false));
    assert(Receive(g,p,r2)==Result::Committed&&g.Current()==h.Current());
    p.Nonce++;p.Base=g.Current();
    r=Serve(h,71,"profile",p);
    assert(r.State==Status::Current&&Receive(g,p,r)==Result::Duplicate);
    p.Base.Tag++;assert(Serve(h,71,"profile",p).State==Status::Conflict);
    p.Base=g.Current();p.Profile="wrong";assert(Serve(h,71,"profile",p).State==Status::Unavailable);
    p.Profile="profile";p.Campaign=72;assert(Serve(h,71,"profile",p).State==Status::Unavailable);
    p.Campaign=71;p.Nonce=0;assert(Serve(h,71,"profile",p).State==Status::Unavailable);
    assert(other.Open(root/"other",71,"profile",true));
    assert(other.Append(other.Current(),{"different","trade","branch"})==Result::Committed);
    p.Nonce=7;p.Base=other.Current();r=Serve(h,71,"profile",p);
    assert(r.State==Status::Conflict&&r.Row.empty());
}
"""
with tempfile.TemporaryDirectory() as folder:
    src=Path(folder,"replication.cpp");src.write_text(test)
    exe=Path(folder,"test")
    subprocess.run(["g++","-std=c++20","-Wall","-Wextra","-Werror","-Wno-unused-parameter","-I",str(common),str(common/"CampaignReplication.cpp"),str(common/"CampaignJournal.cpp"),str(common/"QuestJournalDisk.cpp"),str(src),"-o",str(exe)],check=True)
    subprocess.run([str(exe),folder],check=True)
print("PASS campaign replication, interruption, stale reply, corruption, wrong identity, divergence")
