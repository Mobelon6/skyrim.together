"""Exercise atomic linked-room permission changes and historical checkpoint access."""
from pathlib import Path
import subprocess, sys, tempfile
up=Path(sys.argv[1]).resolve()
common=up/"Code/common"
source=r'''
#include "HomeLinkedAccess.h"
#include "CampaignCheckpoint.h"
#include "HomeEstateConstruction.h"
#include <cassert>
int main(int argc,char** argv)
{
    using namespace HomePermissions;
    using namespace HomeLinkedAccess;
    CampaignJournal::Journal j;
    assert(j.Open(argv[1],71,"profile",true));
    const auto initial=j.Current();
    TradeProperties::FormKey a{"skyrim.esm",1},b{"skyrim.esm",2},c{"skyrim.esm",3};
    TradeProperties::FormKey x{"skyrim.esm",10},y{"skyrim.esm",11};
    const auto first=HomeRegistry::Register(j,101,a),second=HomeRegistry::Register(j,101,b),
        foreign=HomeRegistry::Register(j,202,b),unrelated=HomeRegistry::Register(j,101,c);
    assert(first&&second&&foreign&&unrelated);
    std::set<HomeNativeCapabilities::Row> rows{{'L',a.Encode(),x.Encode(),y.Encode()},{'L',b.Encode(),y.Encode(),x.Encode()}};
    const auto graph=BuildGraph(rows);assert(graph);
    auto slots=RegisteredRooms(j,*first,*graph);
    assert((slots==std::vector<uint32_t>{first->Slot,second->Slot}));
    assert(RegisteredRooms(j,*foreign,*graph).size()==1);
    assert(RegisteredRooms(j,*unrelated,*graph).size()==1);
    auto bad=rows;bad.erase(bad.begin());assert(!BuildGraph(bad));
    const auto q=c.Encode(),main=a.Encode(),secondKey=b.Encode(),worldKey=x.Encode(),table=y.Encode();
    const auto marker=TradeProperties::FormKey{"skyrim.esm",12}.Encode();
    const auto index="STM-ESTATE-INDEX 1\nE "+q+" "+main+" "+worldKey+" "+table+" "+marker+
        "\nI "+q+" "+main+"\nI "+q+" "+secondKey+"\n";
    std::istringstream input(index);
    const auto catalog=HomeEstateCatalog::Read(input);assert(catalog);
    const auto* estate=catalog->ForRoom(secondKey);assert(estate&&estate->Main==main);
    assert(HomeEstateCatalog::RegisteredRooms(*estate,j,*second)==slots);
    assert(!estate->ContainsCandidate(worldKey,0,0)); // legacy catalog carries no spatial proposal
    const auto regions=index+"B "+q+" main 4 -100 -100 100 -100 100 100 -100 100\n"+
        "B "+q+" hatchery 4 300 300 400 300 400 400 300 400\n";
    std::istringstream spatial(regions);
    const auto withRegions=HomeEstateCatalog::Read(spatial);assert(withRegions);
    const auto* plot=withRegions->ForRoom(main);assert(plot);
    assert(plot->ContainsCandidate(worldKey,0,0)&&plot->ContainsCandidate(worldKey,100,100));
    assert(plot->ContainsCandidate(worldKey,350,350)&&!plot->ContainsCandidate(worldKey,200,200));
    assert(!plot->ContainsCandidate(table,0,0)&&!plot->ContainsCandidate(worldKey,NAN,0));
    std::istringstream badPolygon(index+"B "+q+" main 4 0 0 100 100 0 100 100 0\n");
    assert(!HomeEstateCatalog::Read(badPolygon));
    std::istringstream duplicateRegion(regions+"B "+q+" main 3 0 0 100 0 100 100\n");
    assert(!HomeEstateCatalog::Read(duplicateRegion));
    const auto mainId=HomeEstateCatalog::Identity(*catalog,j,71,*first);
    const auto roomId=HomeEstateCatalog::Identity(*catalog,j,71,*second);
    assert(mainId&&roomId&&*mainId==*roomId&&mainId->Owner==101&&mainId->Plot==q);
    const auto foreignId=HomeEstateCatalog::Identity(*catalog,j,71,*foreign);
    assert(foreignId&&foreignId->Owner==202&&*foreignId!=*mainId); // no main-room registration needed
    const auto beforeEmpty=j.Current();
    const auto empty=HomeEstateCatalog::Identity(*catalog,71,404,q);
    assert(empty&&empty->Owner==404&&empty->Plot==q&&*empty!=*mainId);
    assert(!HomeRegistry::Find(j,404,a)&&j.Current()==beforeEmpty);
    assert(!HomeEstateCatalog::Identity(*catalog,71,0,q));
    assert(!HomeEstateCatalog::Identity(*catalog,0,404,q));
    assert(!HomeEstateCatalog::Identity(*catalog,71,404,"missing"));
    auto forged=*second;forged.Owner=404;
    assert(!HomeEstateCatalog::Identity(*catalog,j,71,forged));
    assert(!HomeEstateCatalog::Identity(*catalog,j,71,*unrelated));
    std::istringstream duplicate(index+"I "+q+" "+main+"\n");
    assert(!HomeEstateCatalog::Read(duplicate));
    std::istringstream incomplete("STM-ESTATE-INDEX 1\nE "+q+" "+main+" "+worldKey+" "+table+" "+marker+"\n");
    assert(!HomeEstateCatalog::Read(incomplete));
    assert(Set(j,101,first->Slot,303,Right::Visit,0));
    assert(Effective(j,second->Slot,303)==Right::None);
    assert(!SetGroup(j,101,{first->Slot,foreign->Slot},303,Right::Visit,1));
    assert(!SetGroup(j,101,slots,303,Right::Modify,0));
    const auto before=j.Current();
    assert(SetGroup(j,101,slots,303,Right::Modify,1));
    assert(j.Current().Revision==before.Revision+1&&Read(j)->Revision==2);
    assert(Effective(j,first->Slot,303)==Right::Modify&&Effective(j,second->Slot,303)==Right::Modify);
    assert(Effective(j,foreign->Slot,303)==Right::None&&Effective(j,unrelated->Slot,303)==Right::None);
    CampaignCheckpoint::Record save;
    save.Save={71,303,initial};save.Observed=j.Current();save.Digest=std::string(64,'a');
    save.Cell=b;save.HomeSlot=second->Slot;
    assert(CampaignCheckpoint::HadHomeAccess(j,save));
    assert(CampaignCheckpoint::CanResume(j,71,save));
    assert(SetGroup(j,101,slots,303,Right::Modify,2)&&Read(j)->Revision==2);
    assert(SetGroup(j,101,slots,303,Right::None,2));
    assert(CampaignCheckpoint::HadHomeAccess(j,save));
    assert(!CampaignCheckpoint::CanResume(j,71,save));
    auto after=save;after.Observed=j.Current();assert(!CampaignCheckpoint::HadHomeAccess(j,after));
    CampaignJournal::Journal restart;
    assert(restart.Open(argv[1],71,"profile",false));
    assert(Read(restart)->Revision==3&&CampaignCheckpoint::HadHomeAccess(restart,save));
    assert(Effective(restart,second->Slot,303)==Right::None);
    // A durable explicit estate grant is different from old per-slot grants.
    const auto lateKey=TradeProperties::FormKey{"skyrim.esm",13};
    const auto allowed=std::set<std::string>{main,secondKey,lateKey.Encode()};
    assert(Set(restart,202,foreign->Slot,505,Right::Visit,3));
    const auto newMain=HomeRegistry::Register(restart,202,a);assert(newMain);
    assert(Effective(restart,newMain->Slot,505)==Right::None);
    assert(EstateRight(*Read(restart),202,q,505)==Right::None);
    assert(!SetEstate(restart,404,q,allowed,505,Right::Modify,4)); // no observed owner room
    assert(!SetEstate(restart,202,q,allowed,505,Right::Modify,3)); // stale CAS
    assert(SetEstate(restart,202,q,allowed,505,Right::Modify,4));
    assert(Effective(restart,newMain->Slot,505)==Right::Modify);
    assert(Effective(restart,first->Slot,505)==Right::None);
    assert(EstateRight(*Read(restart),202,q,505)==Right::Modify);
    const auto grantHead=restart.Current();
    assert(SetEstate(restart,202,q,allowed,505,Right::Modify,5)&&restart.Current()==grantHead);
    const auto late=HomeRegistry::Register(restart,202,lateKey);assert(late);
    assert(Effective(restart,late->Slot,505)==Right::Modify);
    assert(EffectiveAt(restart,late->Slot,505,grantHead.Revision)==Right::None);
    auto saved=save;saved.Save.Character=505;saved.Cell=lateKey;saved.HomeSlot=late->Slot;
    saved.Observed=grantHead;assert(!CampaignCheckpoint::HadHomeAccess(restart,saved));
    saved.Observed=restart.Current();assert(CampaignCheckpoint::HadHomeAccess(restart,saved));
    assert(CampaignCheckpoint::CanResume(restart,71,saved));
    assert(!SetEstate(restart,202,q,{main,secondKey},505,Right::None,5)); // changed catalog requires migration
    assert(SetEstate(restart,202,q,allowed,505,Right::None,5));
    assert(!CampaignCheckpoint::CanResume(restart,71,saved));
    assert(CampaignCheckpoint::HadHomeAccess(restart,saved));
    assert(Effective(restart,foreign->Slot,505)==Right::None&&Effective(restart,late->Slot,505)==Right::None);
    CampaignJournal::Journal finalRead;assert(finalRead.Open(argv[1],71,"profile",false));
    assert(Read(finalRead)->Revision==6&&CampaignCheckpoint::HadHomeAccess(finalRead,saved));
    assert(Effective(finalRead,late->Slot,505)==Right::None&&EstateRight(*Read(finalRead),202,q,505)==Right::None);
    assert(!DecodeEstate("202 505 1 "+q+" 2 "+main+" "+main));
    const auto buildRows=index+"C "+q+" "+table+" "+main+"\nC "+q+" "+marker+" "+main+"\n";
    std::istringstream buildIndex(buildRows);const auto buildCatalog=HomeEstateCatalog::Read(buildIndex);assert(buildCatalog);
    const auto* buildEstate=buildCatalog->ForRoom(main);assert(buildEstate&&buildEstate->Construction.size()==2);
    const auto binding=HomeEstateConstruction::Binding(*buildEstate);assert(binding);
    assert(HomeEstateConstruction::Read(finalRead,101,q)->Revision==0);
    assert(!HomeEstateConstruction::Publish(finalRead,*buildEstate,101,0,0,binding,"1"));
    assert(!HomeEstateConstruction::Publish(finalRead,*buildEstate,101,0,0,binding+1,"01"));
    assert(HomeEstateConstruction::Publish(finalRead,*buildEstate,101,0,0,binding,"01"));
    assert(HomeEstateConstruction::Read(finalRead,202,q)->Revision==0);
    const auto buildHead=finalRead.Current();
    assert(HomeEstateConstruction::Publish(finalRead,*buildEstate,101,1,0,binding,"01")&&finalRead.Current()==buildHead);
    assert(!HomeEstateConstruction::Publish(finalRead,*buildEstate,101,0,0,binding,"11"));
    assert(HomeEstateConstruction::Publish(finalRead,*buildEstate,101,1,buildHead.Revision,binding,"11"));
    assert(!HomeEstateConstruction::Publish(finalRead,*buildEstate,101,2,0,binding,"00"));
    CampaignJournal::Journal buildRestart;assert(buildRestart.Open(argv[1],71,"profile",false));
    const auto buildState=HomeEstateConstruction::Read(buildRestart,101,q);
    assert(buildState&&buildState->Revision==2&&buildState->Bits=="11");
    assert(!HomeEstateConstruction::Decode(buildState->Encode()+" "));
    std::istringstream duplicateBuild(buildRows+"C "+q+" "+table+" "+main+"\n");
    assert(!HomeEstateCatalog::Read(duplicateBuild));
    assert(!DecodeGroup("101 303 2 2 2 1"));
    assert(!DecodeGroup("101 303 2 2 1 1"));
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d)/"test.cpp";p.write_text(source);exe=Path(d)/"test"
    subprocess.run(["g++","-std=c++20","-Wall","-Wextra","-Werror","-Wno-unused-parameter","-I",str(common),
        str(common/"CampaignIdentity.cpp"),str(common/"CampaignJournal.cpp"),str(common/"QuestJournalDisk.cpp"),
        str(p),"-o",str(exe)],check=True)
    subprocess.run([str(exe),str(Path(d)/"journal")],check=True)
print("PASS linked-room graph, owner isolation, atomic ACL, mixed grants, historical checkpoint and restart")
