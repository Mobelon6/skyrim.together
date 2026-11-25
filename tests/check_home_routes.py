"""Exercise the production mapping of local and guest CELL ids for two owners."""
from pathlib import Path
import tempfile,subprocess,sys
s=Path(sys.argv[1],'Code/client/Services/Generic/PartyHome.cpp').read_text()
routes=s[s.index('struct Route'):s.index('std::optional<HomeCatalog::Catalog> projectionCatalog')]
impl=s[s.index('namespace HomeInstance\n'):s.index('void OverlayService::ClearHomes')]
source=r'''#include <cstdint>
#include <atomic>
#include <cassert>
#include "HomeRegistry.h"
'''+routes+impl+r'''
int main(){
 auto& a=routes[0]; auto& b=routes[1];
 a.source=50;a.copy=100;a.slot=800;b.source=51;b.copy=101;b.slot=801;routeCount=2;
 uint32_t m{},id{};
 assert(!HomeInstance::Encode(50,m,id));a.own=1;b.own=1;self=0;
 assert(HomeInstance::Encode(50,m,id) && id==800);auto first=m;
 assert(HomeInstance::Decode(first,id)==50 && !HomeInstance::Foreign(first));
 self=2;a.own=3;b.own=3;
 assert(HomeInstance::Encode(50,m,id));auto second=m;
 assert(first!=second && !HomeInstance::Decode(first,id));a.visit=1;
 assert(HomeInstance::Encode(100,m,id) && m==first && HomeInstance::Decode(first,id)==100);
 assert(HomeInstance::Decode(second,id)==50 && HomeInstance::Foreign(first));
 assert(HomeInstance::PrivateCell(50) && HomeInstance::PrivateCell(100));
 assert(!HomeInstance::Encode(777,m,id));assert(!HomeInstance::Decode(first,801));
 a.visit=0;assert(!HomeInstance::Encode(100,m,id) && !HomeInstance::Decode(first,800));
 const uint32_t local=0xFE123456;
 activeHomeRoute=(uint64_t(HomeRegistry::PendingPrefix|2)<<32)|local;
 assert(HomeInstance::Encode(local,m,id)&&m==(HomeRegistry::PendingPrefix|2)&&id==local);
 assert(HomeInstance::Decode(m,id)==local&&!HomeInstance::Foreign(m));
 assert(HomeInstance::Foreign(HomeRegistry::PendingPrefix|3));
 activeHomeRoute=(uint64_t(HomeRegistry::ScopePrefix|43)<<32)|local;
 assert(HomeInstance::Encode(local,m,id)&&m==(HomeRegistry::ScopePrefix|43)&&id==0x456);
 assert(HomeInstance::Decode(m,id)==local&&!HomeInstance::Foreign(m));
 assert(HomeInstance::Foreign(HomeRegistry::ScopePrefix|44));
 assert(!HomeInstance::Decode(m,0x457)&&HomeInstance::PrivateCell(local));
 const uint32_t copy=0x07000900;
 projectedRoot=(uint64_t(0x456)<<32)|copy;
 activeHomeRoute=(uint64_t(HomeRegistry::ScopePrefix|43)<<32)|copy;
 assert(HomeInstance::Encode(copy,m,id)&&m==(HomeRegistry::ScopePrefix|43)&&id==0x456);
 assert(HomeInstance::Decode(m,id)==copy&&!HomeInstance::Foreign(m));
 assert(!HomeInstance::Decode(m,0x900));
 projectedRoot=0;
 activeHomeRoute=0;
 assert(!HomeInstance::Encode(local,m,id)&&!HomeInstance::Decode(HomeRegistry::ScopePrefix|43,0x456));
}
'''
with tempfile.TemporaryDirectory() as tmp:
 exe=Path(tmp,'rooms')
 subprocess.run(['g++','-std=c++20','-Wall','-Werror','-I',str(Path(sys.argv[1],'Code/common').resolve()),'-x','c++','-','-o',str(exe)],input=source,text=True,check=True)
 subprocess.run([str(exe)],check=True)
print('PASS distinct home ids, private and visited routes, party id zero, unrelated interiors and withdrawn visit')
