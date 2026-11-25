"""Exercise real evidence reducer and corrected base-container count, without Skyrim."""
from pathlib import Path
import subprocess
import tempfile
import sys

root=Path(sys.argv[1])/"Code"
base=(root/"client/Games/Skyrim/Components/TESContainer.cpp").read_text()
start=base.index("int64_t TESContainer::GetItemCount(")
body=base[start:]
service=(root/"client/Services/Generic/InventoryService.cpp").read_text()
assert "containerChangedEvent.RegisterSink(this)" in service
assert "m_world.GetRunner().Queue([this,copy]" in service
assert "m_evidenceBaseline.reset()" in service
assert "STM_INVENTORY_EVIDENCE" in service
assert "CampaignJournal::" not in service
assert "m_evidenceEndpoints.clear()" in service
assert "object->parentCell!=player.parentCell" in service
assert "world.view<FormIdComponent,ObjectComponent>()" in service
code=r'''
#include "InventoryEvidence.h"
#include <cassert>
#include <cstdint>
struct TESForm{};
struct TESContainer {
    struct Entry { uint32_t count; TESForm* form; };
    Entry** entries; uint32_t count;
    int64_t GetItemCount(TESForm*) const noexcept;
};
'''+body+r'''
int main(){
    using namespace InventoryEvidence;
    TESForm a,b;
    TESContainer::Entry x{2,&a},y{3,&a},z{99,&b};
    TESContainer::Entry* entries[]={&x,nullptr,&y,&z};
    TESContainer container{entries,4};
    assert(container.GetItemCount(&a)==5 && container.GetItemCount(&b)==99);
    assert(container.GetItemCount(nullptr)==0);
    Counts before{{101,5},{102,2}},after{{101,4},{102,2},{103,1}};
    std::vector<Transfer> transfers{{Player,200,101,3},{200,Player,101,2}};
    auto result=Compare(before,after,transfers);
    assert(result.Valid && result.Rows.size()==2);
    assert(result.Rows[0].Item==101 && result.Rows[0].Expected==-1 && result.Rows[0].State==Status::MatchedNet);
    assert(result.Rows[1].Item==103 && result.Rows[1].State==Status::Unexplained);
    transfers.push_back({0,Player,103,2});
    result=Compare(before,after,transfers);
    assert(result.Rows[1].State==Status::Mismatch);
    result=Compare({{101,5}},{{101,5}},{{Player,200,101,1},{200,Player,101,1}});
    assert(result.Valid && result.Rows.size()==1 && result.Rows[0].State==Status::MatchedNet);
    result=Compare(before,after,{{Player,200,101,-1}});
    assert(!result.Valid && result.Rows[0].State==Status::Invalid);
    result=Compare(before,after,{{0,0,101,1}});
    assert(!result.Valid);
    result=Compare({{101,-1}},after,{});
    assert(!result.Valid);
    // Standard and ESL slot remaps, case normalization and ambiguous mappings.
    auto k=Resolve(0x04001234,{{4,false,"Patch.ESP"},{9,true,"Light.esl"}});
    assert(k && k->Plugin=="patch.esp" && k->Local==0x1234);
    auto remap=Resolve(0x08001234,{{8,false,"patch.esp"}});
    assert(remap && remap->Plugin==k->Plugin && remap->Local==k->Local);
    k=Resolve(0xFE009ABC,{{9,true,"Light.esl"}});
    assert(k && k->Plugin=="light.esl" && k->Local==0xABC);
    assert(!Resolve(0xFF000001,{{255,false,"Dynamic.esp"}}));
    assert(!Resolve(0x04001234,{{4,false,"A.esp"},{4,false,"B.esp"}}));
    assert(!Resolve(0x04001234,{{4,false,"A.esp"},{5,false,"a.ESP"}}));
    assert(!Resolve(0x04001234,{{4,true,"A.esl"}}));
    // The counterpart must have snapshots on both sides of the interval.
    transfers={{Player,200,101,3},{200,Player,101,2}};
    result=Compare(before,after,transfers);
    Endpoints start{{200,{9,12,{{101,7}}}}},end{{200,{9,12,{{101,8}}}}};
    auto paired=CompareEndpoints(start,end,transfers,result);
    assert(paired.size()==1 && paired[0].PlayerNet==-1 && paired[0].OtherNet==1 &&
        paired[0].State==PairStatus::MatchedBothNets);
    assert(CompareEndpoints({},end,transfers,result)[0].State==PairStatus::MissingSnapshot);
    end[200].Entity=13;
    assert(CompareEndpoints(start,end,transfers,result)[0].State==PairStatus::ChangedEndpoint);
    end[200].Entity=12;end[200].Items[101]=9;
    assert(CompareEndpoints(start,end,transfers,result)[0].State==PairStatus::OtherMismatch);
    auto wrong=Compare(before,before,transfers);
    assert(CompareEndpoints(start,end,transfers,wrong)[0].State==PairStatus::PlayerMismatch);
    // Distinct counterparties must not be treated as one transaction.
    transfers={{Player,200,101,2},{300,Player,101,1}};
    paired=CompareEndpoints(start,end,transfers,Compare(before,after,transfers));
    assert(paired.size()==2 && paired[1].State==PairStatus::MissingSnapshot);
    Counts n{{101,INT64_MAX}};
    assert(!Accumulate(n,101,1) && !Accumulate(n,0,1));
}
'''
with tempfile.TemporaryDirectory() as work:
    p=Path(work)
    (p/"test.cpp").write_text(code)
    subprocess.run(["g++","-std=c++20","-Wall","-Wextra","-Werror","-I",str(root/"common"),str(p/"test.cpp"),"-o",str(p/"test")],check=True)
    subprocess.run([str(p/"test")],check=True)
print("PASS inventory evidence net reconciliation and actual base-container count")
