"""Check the production journal publication and restart behavior on a real filesystem."""
import subprocess
import sys
import tempfile
from pathlib import Path

common = Path(sys.argv[1], "Code/common").resolve()
test = r"""
#include "QuestJournalDisk.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
using namespace QuestJournalDisk;
int main(int argc, char** argv){
    std::filesystem::path host=std::filesystem::path(argv[1])/"host", guest=std::filesystem::path(argv[1])/"guest";
    auto stream=CreateStream(host);assert(stream && CreateStream(host)!=stream);
    auto dir=host/std::to_string(stream);
    std::ofstream(dir/"1.tsv.aborted.tmp")<<"partial";
    assert(Contiguous(host,stream)==0);
    assert(Store(host,stream,2,"quest-v1\tSkyrim.esm\t01\tstage\t20"));
    assert(Contiguous(host,stream)==0);
    assert(Store(host,stream,1,"first"));
    assert(Store(host,stream,1,"first")&&!Store(host,stream,1,"conflicting"));
    assert(Contiguous(host,stream)==2);
    std::thread a([&]{assert(Store(host,stream,3,"same"));});
    std::thread b([&]{assert(Store(host,stream,3,"same"));});a.join();b.join();
    assert(!Store(host,stream,4,"bad\nrow") && !Store(host,stream,0,"zero"));
    assert(Store(guest,stream,1,"first"));
    assert(Contiguous(guest,stream)==1);
    assert(Store(guest,stream,2,"quest-v1\tSkyrim.esm\t01\tstage\t20") && Store(guest,stream,3,"same"));
    assert(Contiguous(guest,stream)==3 && Contiguous(host,stream)==3);
    std::ifstream file(guest/std::to_string(stream)/"1.tsv");std::string row;std::getline(file,row);assert(row=="1\tfirst");
}
"""
with tempfile.TemporaryDirectory() as folder:
    src = Path(folder, "journal_test.cpp")
    src.write_text(test)
    binary = Path(folder, "journal_test")
    subprocess.run(["g++", "-std=c++20", "-pthread", "-I", common, common/"QuestJournalDisk.cpp", src, "-o", binary], check=True)
    subprocess.run([binary, folder], check=True)
print("PASS durable records, retries, competing writers, partial file, gap, independent host and guest cursors")

