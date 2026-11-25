"""Production catalog parser and source/base/target binding."""
from pathlib import Path
import subprocess,sys,tempfile
root=Path(sys.argv[1])
source=r'''
#include "HomeCatalog.h"
#include <cassert>
int main(){
    using TradeProperties::FormKey;
    const FormKey root{"mod.esp",100},src{"mod.esp",101},base{"skyrim.esm",202},
        door{"mod.esp",102},outside{"skyrim.esm",301};
    const std::string header="STM-HOME-INSTANCES 2\nH "+root.Encode()+" 2048\n";
    const std::string row="R "+root.Encode()+" "+src.Encode()+" "+base.Encode()+" 2049 "+base.Encode()+"\n";
    const std::string exit="D "+root.Encode()+" "+door.Encode()+" "+outside.Encode()+" 2049 0 0 0 0 0 0 0 0 0 0 0 0\n";
    std::istringstream in(header+row+exit);
    auto catalog=HomeCatalog::Read(in);assert(catalog&&catalog->size()==1);
    HomeSnapshot::State s;HomeSnapshot::Ref r;r.Key=src;r.Base=base;r.Type=29;r.Scale=100;s.Refs.push_back(r);
    auto c=catalog->at(root.Encode());
    assert(HomeCatalog::Bind(c,s));
    s.Refs[0].Base.Local++;assert(!HomeCatalog::Bind(c,s));s.Refs[0].Base=base;
    c.Unsupported.insert("x");assert(!HomeCatalog::Bind(c,s));
    for(auto bad:{header+row+row+exit,header+exit,header+row+exit+exit,header+row+"junk\n"}){
        std::istringstream stream(bad);assert(!HomeCatalog::Read(stream));
    }
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp,"catalog.cpp");p.write_text(source)
    exe=Path(tmp,"catalog")
    subprocess.run(["g++","-std=c++20","-Wall","-Wextra","-Werror","-I",str((root/"Code/common").resolve()),str(p),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print("PASS catalog keys, unique targets, exit linkage and fail-closed binding")
