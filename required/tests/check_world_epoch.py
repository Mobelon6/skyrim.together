"""Compile the production generation gate; verify queued callbacks cannot cross a world boundary."""
import subprocess, tempfile, sys
from pathlib import Path
common=Path(sys.argv[1],"Code/common").resolve()
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp,"test.cpp")
    src.write_text(r"""
#include "WorldTaskEpoch.h"
#include <cassert>
#include <thread>
int main(){
 WorldTaskEpoch gate;
 auto old=gate.Ticket();
 assert(gate.Allows(old));
 assert(gate.Suspend());
 auto suspended=gate.Ticket();
 assert(!gate.Allows(old)&&!gate.Allows(suspended));
 assert(!gate.Suspend());
 gate.Invalidate();
 assert(!gate.Allows(gate.Ticket()));
 assert(gate.Resume());
 assert(!gate.Resume());
 auto fresh=gate.Ticket();
 assert(gate.Allows(fresh)&&!gate.Allows(old)&&!gate.Allows(suspended));
 std::thread worker([&]{gate.Invalidate();});worker.join();
 assert(!gate.Allows(fresh)&&gate.Allows(gate.Ticket()));
 for(int i=0;i<100;++i){
  auto previous=gate.Ticket();
  assert(gate.Suspend());assert(gate.Resume());
  assert(!gate.Allows(previous));
 }
}
""")
    exe=Path(tmp,"test")
    subprocess.run(["g++","-std=c++20","-pthread","-Wall","-Wextra","-Werror","-I",str(common),str(src),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print("PASS world task generation, suspension, resume, disconnect invalidation")
