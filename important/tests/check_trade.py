"""Exercise the actual disk ledger across client and server restarts."""
import subprocess,sys,tempfile
from pathlib import Path
common=Path(sys.argv[1],'Code/common').resolve()
with tempfile.TemporaryDirectory() as work:
 p=Path(work)
 (p/'trade.cpp').write_text(r'''
#include "TradeLedger.h"
#include <cassert>
int main(){
 {
 // A single atomic expense, using the existing campaign and receipt wallet.
 // Neither receipt credits escrow; applying/acknowledging one cannot create
 // stock, and an older valid ESS can apply its own missing debit once.
 CampaignJournal::Journal host;assert(host.Open("quest-expense",191,"profile",true));
 TradeLedger ledger(host,191);assert(ledger.Stream()==191);
 using E=TradeLedger::QuestExpense;
 E e{{"skyrim.esm",100},{"skyrim.esm",200},{"skyrim.esm",300},101,202,3,0,0,2,1};
 assert(e.Valid()&&e.SpeakerDebit()==2&&e.PeerDebit()==1);
 assert(E::Decode(e.Encode())&&E::Decode(e.Encode())->Encode()==e.Encode());
 assert(!E::Decode(e.Encode()+" extra"));
 auto bad=e;bad.PeerBefore=0;assert(!bad.Valid()&&!ledger.CommitQuestExpense(bad));
 bad=e;bad.Peer=101;assert(!ledger.CommitQuestExpense(bad));
 bad=e;bad.Item.Local=15;assert(!ledger.CommitQuestExpense(bad));
 bad=e;bad.SpeakerCursor=1;assert(!ledger.CommitQuestExpense(bad));
 assert(host.Current().Revision==0);
 assert(ledger.CommitQuestExpense(e)&&host.Current().Revision==1);
 assert(ledger.CommitQuestExpense(e)&&host.Current().Revision==1);
 bad=e;bad.Required=2;assert(!ledger.CommitQuestExpense(bad));
 assert(!ledger.Commit({"101","quest/x","skyrim.esm",300,-1,2},1));
 assert(!ledger.Commit({"quest/x","202","skyrim.esm",300,-1,2},0));
 assert(ledger.History("101").size()==1&&ledger.History("202").size()==1);
 assert(ledger.MatchesReceipt("101",0,0,1,"skyrim.esm",300,-2,2));
 assert(ledger.MatchesReceipt("202",0,0,1,"skyrim.esm",300,-1,1));
 assert(!ledger.MatchesReceipt("101",0,0,1,"skyrim.esm",300,-3,2));
 assert(ledger.Stock("101","202").empty()&&!ledger.NextRefund("101")&&!ledger.NextRefund("202"));
 assert(ledger.Ack("101",1)&&ledger.Ack("101",1));
 // The other character's unfinished receipt prevents another expense.
 auto next=e;next.Info.Local=201;next.SpeakerCursor=1;next.PeerCursor=1;
 next.SpeakerBefore=0;next.PeerBefore=4;
 const auto prior=host.Current();assert(!ledger.CommitQuestExpense(next)&&host.Current()==prior);
 assert(ledger.Ack("202",1));
 assert(ledger.Stock("101","202").empty()&&!ledger.NextRefund("101")&&!ledger.NextRefund("202"));
 assert(ledger.CommitQuestExpense(next)); // all items come from the peer
 assert(ledger.History("101").size()==1&&ledger.History("202").size()==2);
 assert(ledger.Ack("202",2));
 auto reverse=e;reverse.Info.Local=202;reverse.Speaker=202;reverse.Peer=101;
 reverse.SpeakerCursor=2;reverse.PeerCursor=1;reverse.SpeakerBefore=5;reverse.PeerBefore=0;
 assert(ledger.CommitQuestExpense(reverse)); // either participant, local inventory first
 assert(ledger.History("101").size()==1&&ledger.History("202").size()==3);
 assert(ledger.Ack("202",3));
 CampaignJournal::Journal restored;assert(restored.Open("quest-expense",191,"profile",false));
 TradeLedger after(restored,191);assert(after.Stream()==191);
 assert(after.CommitQuestExpense(e)&&restored.Current()==host.Current());
 assert(after.MatchesReceipt("101",0,0,1,"skyrim.esm",300,-2,2));
 assert(after.MatchesReceipt("202",191,1,2,"skyrim.esm",300,-3,4));
 assert(after.MatchesReceipt("202",191,2,3,"skyrim.esm",300,-3,5));
 assert(after.Ack("101",1)&&after.Ack("202",3));
 assert(after.Stock("101","202").empty()&&!after.NextRefund("101")&&!after.NextRefund("202"));
 assert(!after.HasOutstanding("101")&&!after.HasOutstanding("202"));
 const auto& row=after.History("101")[0];assert(TradeLedger::CanApply(row,2));
 assert(!TradeLedger::CanApply(row,0)&&!TradeLedger::CanApply(row,3));
 TradeLedger legacy("no-quest-expense");assert(legacy.Stream()&&!legacy.CommitQuestExpense(e));
 }
 {
 using namespace TradeProperties;
 Values v{Values::HasHealth,std::bit_cast<uint32_t>(1.25f),0,0};
 auto key=v.Encode();assert(Decode(key)&&Decode(key)->Health==v.Health);
 Values charge{Values::HasCharge,0,0,0};assert(Decode(charge.Encode())&&charge.Encode()!=key);
 assert(!Decode("1:01:1067450368:0:0")&&!Decode("1:8:0:0:0"));
 assert(!Decode("1:1:2143289344:0:0")&&!Decode("1:4:0:0:6"));
 assert(Decode("")&&Decode("")->Mask==0);
 Values enchanted=v;
 enchanted.Mask|=Values::HasEnchant|Values::HasPoison;
 enchanted.Enchant={"skyrim.esm",0x12345};enchanted.EnchantCharge=250;
 enchanted.Poison={"patch.esl",0xABC};enchanted.PoisonCount=3;
 auto extended=enchanted.Encode();auto decoded=Decode(extended);
 assert(decoded&&decoded->Enchant.Plugin=="skyrim.esm"&&decoded->Enchant.Local==0x12345);
 assert(decoded->Poison.Plugin=="patch.esl"&&decoded->PoisonCount==3&&decoded->EnchantCharge==250);
 assert(Decode(key)&&Decode(key)->Encode()==key&&key.starts_with("1:"));
 auto invalid=enchanted;invalid.Enchant.Plugin="Skyrim.esm";assert(!Decode(invalid.Encode()));
 invalid=enchanted;invalid.EnchantCharge=65536;assert(!Decode(invalid.Encode()));
 invalid=enchanted;invalid.PoisonCount=0;assert(!Decode(invalid.Encode()));
 assert(!Decode(extended+":0"));
 Values named=enchanted;
 named.Mask|=Values::HasName;
 named.Name="Меч: \"Север\"";
 auto nameKey=named.Encode();
 assert(nameKey.starts_with("3:")&&Decode(nameKey)&&Decode(nameKey)->Name==named.Name);
 assert(Decode(extended)->Encode()==extended && Decode(key)->Encode()==key);
 auto otherName=named;otherName.Name="Other";
 assert(otherName.Encode()!=nameKey);
 auto badName=named;badName.Name=std::string("x\0y",3);assert(!Decode(badName.Encode()));
 badName.Name=std::string(256,'a');assert(!Decode(badName.Encode()));
 badName.Name="";assert(!Decode(badName.Encode()));
 assert(!Decode(nameKey+":0"));
 Values custom=named;
 custom.Mask|=Values::HasCreated;
 custom.Enchant={};
 custom.Created=CreatedEnchant{};
 custom.Created->Weapon=true;
 custom.Created->Metadata={0,0,1,0,1,6,0};
 custom.Created->Effects.push_back({{"skyrim.esm",123},std::bit_cast<uint32_t>(12.f),0,1,0});
 custom.Created->Effects.push_back({{"patch.esl",456},std::bit_cast<uint32_t>(3.f),0,2,0});
 auto customKey=custom.Encode();
 auto restored=Decode(customKey);
 assert(restored && restored->Created && restored->Created->Effects.size()==2);
 assert(restored->Encode()==customKey && restored->Name==named.Name);
 auto changed=custom;changed.Created->Effects[1].Duration=3;
 assert(changed.Encode()!=customKey && Decode(changed.Encode()));
 auto invalidCustom=custom;invalidCustom.Created->Effects.clear();
 assert(!Decode(invalidCustom.Encode()));
 invalidCustom=custom;invalidCustom.Created->Effects[0].Magnitude=0x7fc00000;
 assert(!Decode(invalidCustom.Encode()));
 invalidCustom=custom;invalidCustom.Enchant={"skyrim.esm",42};
 assert(!Decode(invalidCustom.Encode()));
 Values owned;
 owned.Mask=Values::HasOwnership;owned.Owner={"skyrim.esm",0x12345};
 auto ownedKey=owned.Encode();assert(ownedKey.starts_with("5:")&&Decode(ownedKey));
 assert(Decode(ownedKey)->Owner.Local==0x12345 && Decode(ownedKey)->Encode()==ownedKey);
 auto otherOwner=owned;otherOwner.Owner={"patch.esl",0x123};
 assert(Decode(otherOwner.Encode())&&otherOwner.Encode()!=ownedKey);
 auto ownedCustom=custom;ownedCustom.Mask|=Values::HasOwnership;ownedCustom.Owner=owned.Owner;
 auto ownedCustomKey=ownedCustom.Encode();auto oc=Decode(ownedCustomKey);
 assert(oc&&oc->Created&&oc->Name==custom.Name&&oc->Owner.Local==owned.Owner.Local);
 assert(Decode(customKey)->Encode()==customKey);
 auto invalidOwner=owned;invalidOwner.Owner={"skyrim.esm",7};assert(!Decode(invalidOwner.Encode()));
 invalidOwner.Owner={};assert(!Decode(invalidOwner.Encode()));
 assert(!Decode("5:"+owned.Owner.Encode()+":"+ownedKey));
 assert(!Decode("5:"+owned.Owner.Encode()+":"));
 assert(!Decode(ownedKey+":0"));
 assert(!Decode("1:128:0:0:0"));
 {
 TradeLedger own("ownership-ledger");assert(own.Stream());
 assert(own.Commit({"A","B","Skyrim.esm",17,-2,2,false,ownedKey},0));
 assert(own.Ack("A",1));
 assert(!own.Commit({"B","A","Skyrim.esm",17,1,0},0));
 assert(!own.Commit({"B","A","Skyrim.esm",17,1,0,false,otherOwner.Encode()},0));
 assert(own.Commit({"B","A","Skyrim.esm",17,1,0,false,ownedKey},0));
 assert(own.Ack("B",1));
 assert(own.NextRefund("A")->variant==ownedKey);
 assert(own.Commit({"A","B","Skyrim.esm",17,1,0,true,ownedKey},1));
 }
 {
 TradeLedger own("ownership-ledger");auto stream=own.Stream();assert(stream);
 assert(own.History("A").size()==2);
 assert(own.MatchesReceipt("A",stream,1,2,"Skyrim.esm",17,1,0,ownedKey));
 assert(!own.MatchesReceipt("A",stream,1,2,"Skyrim.esm",17,1,0,otherOwner.Encode()));
 assert(own.Ack("A",2)&&!own.NextRefund("A"));
 }
 auto unnamed=custom;unnamed.Mask&=~Values::HasName;unnamed.Name.clear();
 assert(Decode(unnamed.Encode()) && Decode(unnamed.Encode())->Name.empty());
 {
 TradeLedger crafted("crafted-ledger");assert(crafted.Stream());
 assert(crafted.Commit({"A","B","Skyrim.esm",17,-1,1,false,customKey},0));
 assert(crafted.Ack("A",1));
 assert(!crafted.Commit({"B","A","Skyrim.esm",17,1,0,false,changed.Encode()},0));
 assert(crafted.NextRefund("A")->variant==customKey);
 assert(crafted.Commit({"A","B","Skyrim.esm",17,1,0,true,customKey},1));
 }
 {
 TradeLedger crafted("crafted-ledger");assert(crafted.Stream());
 assert(crafted.History("A").size()==2 && crafted.History("A").back().variant==customKey);
 assert(crafted.Ack("A",2)&&!crafted.NextRefund("A"));
 }
 {
 TradeLedger names("named-ledger");assert(names.Stream());
 assert(names.Commit({"A","B","Skyrim.esm",17,-1,1,false,nameKey},0));
 assert(names.Ack("A",1));
 assert(!names.Commit({"B","A","Skyrim.esm",17,1,0,false,otherName.Encode()},0));
 assert(names.NextRefund("A")->variant==nameKey);
 assert(names.Commit({"A","B","Skyrim.esm",17,1,0,true,nameKey},1));
 }
 {
 TradeLedger names("named-ledger");assert(names.Stream());
 assert(names.History("A").size()==2&&names.History("A").back().variant==nameKey);
 assert(names.Ack("A",2)&&!names.NextRefund("A"));
 }
 {
 TradeLedger mixed("enchantment-ledger");assert(mixed.Stream());
 assert(mixed.Commit({"A","B","Skyrim.esm",17,-1,1,false,extended},0));
 assert(mixed.Ack("A",1));
 auto distinct=enchanted;distinct.PoisonCount=2;
 assert(!mixed.Commit({"B","A","Skyrim.esm",17,1,0,false,distinct.Encode()},0));
 auto refund=mixed.NextRefund("A");assert(refund&&refund->variant==extended);
 assert(mixed.Commit({"A","B",refund->plugin,refund->base,1,0,true,extended},1));
 }
 {
 TradeLedger mixed("enchantment-ledger");assert(mixed.Stream());
 assert(mixed.History("A").size()==2 && mixed.History("A")[1].variant==extended);
 assert(mixed.Stock("A","B").at({"Skyrim.esm",17,extended})==0);
 assert(mixed.Ack("A",2)&&!mixed.NextRefund("A"));
 }
 TradeLedger l("variant-ledger");assert(l.Stream());
 assert(l.Commit({"A","B","Skyrim.esm",17,-2,2,false,key},0));
 assert(l.Ack("A",1));
 assert(l.Commit({"A","B","Skyrim.esm",17,-3,3,false,charge.Encode()},1));
 assert(l.Ack("A",2));
 assert(!l.Commit({"B","A","Skyrim.esm",17,1,0},0));
 assert(l.Commit({"B","A","Skyrim.esm",17,1,0,false,key},0));
 assert(l.Ack("B",1));
 assert(l.Stock("A","B").at({"Skyrim.esm",17,key})==1);
 assert(l.Stock("A","B").at({"Skyrim.esm",17,charge.Encode()})==3);
 auto refund=l.NextRefund("A");assert(refund && !refund->variant.empty());
 assert(l.Commit({"A","B",refund->plugin,refund->base,int(refund->count),0,true,refund->variant},2));
 }
 {
 TradeLedger l("variant-ledger");assert(l.Stream());
 assert(l.History("A").size()==3&&!l.History("A").back().variant.empty());
 assert(l.Ack("A",3));
 auto refund=l.NextRefund("A");assert(refund&&!refund->variant.empty());
 assert(l.Commit({"A","B",refund->plugin,refund->base,int(refund->count),0,true,refund->variant},3));
 assert(l.Ack("A",4)&&!l.NextRefund("A"));
 }
 // Close/restart: unclaimed stock belongs to its depositor, not the next session.
 {
 TradeLedger l("refund-ledger");auto stream=l.Stream();assert(stream);
 assert(l.Commit({"A","B","Skyrim.esm",17,-4},0));
 assert(l.Ack("A",1));
 assert(l.Commit({"B","A","Skyrim.esm",17,-3},0));
 assert(l.Ack("B",1));
 assert(l.Commit({"B","A","Skyrim.esm",17,5},1));
 assert(l.Ack("B",2));
 assert(l.Stock("A","B").at({"Skyrim.esm",17})==2);
 assert(l.HasOutstanding("A")&&!l.HasOutstanding("B"));
 auto refund=l.NextRefund("A");assert(refund && refund->peer=="B" && refund->count==2);
 assert(!l.Commit({"B","A","Skyrim.esm",17,1,5,true},2));
 assert(!l.Commit({"A","B","Skyrim.esm",17,3,0,true},1));
 assert(!l.Commit({"A","B","Skyrim.esm",17,2,-1,true},1));
 assert(l.Commit({"A","B","Skyrim.esm",17,2,0,true},1));
 assert(!l.Commit({"A","B","Skyrim.esm",17,2,0,true},1));
 assert(!l.NextRefund("A")&&l.HasOutstanding("A"));
 }
 {
 TradeLedger l("refund-ledger");assert(l.Stream());
 assert(l.History("A").size()==2 && l.History("A")[1].refund);
 assert(l.Stock("A","B").at({"Skyrim.esm",17})==0);
 assert(l.HasOutstanding("A")&&!l.NextRefund("A"));
 assert(l.Ack("A",2)&&l.Ack("A",2));
 assert(!l.HasOutstanding("A")&&!l.HasOutstanding("B"));
 // Deposit acknowledged after a close must still become refundable.
 assert(l.Commit({"A","B","Skyrim.esm",17,-2},2));
 assert(!l.NextRefund("A")&&l.HasOutstanding("A"));
 assert(l.Ack("A",3));assert(l.NextRefund("A")->count==2);
 }
 {
 CampaignJournal::Journal host;
 assert(host.Open("refund-campaign",94,"profile",true));
 TradeLedger l(host,94);assert(l.Stream()==94);
 assert(l.Commit({"1","2","Skyrim.esm",19,-2,3},0));
 assert(l.Ack("1",1));
 assert(l.Commit({"1","2","Skyrim.esm",19,2,1,true},1));
 assert(l.Ack("1",2));
 CampaignJournal::Journal restored;
 assert(restored.Open("refund-campaign",94,"profile",false));
 TradeLedger after(restored,94);assert(after.Stream()==94);
 assert(after.AcceptCursor("1",0,0));
 assert(after.MatchesReceipt("1",94,1,2,"Skyrim.esm",19,2,1));
 assert(!after.NextRefund("1")&&!after.HasOutstanding("1"));
 assert(after.Ack("1",1)&&after.Ack("1",2));
 assert(after.Stock("1","2").at({"Skyrim.esm",19})==0);
 }
 // Recover both ends of a previously acknowledged exchange from older saves.
 // The receipt cursor is part of the loaded save, not the server's ACK cursor.
 {
 using CampaignJournal::Result;
 CampaignJournal::Journal host;
 assert(host.Open("recovery",81,"profile",true));
 TradeLedger ledger(host,81);assert(ledger.Stream()==81);
 TradeLedger::Row give{"1","2","Skyrim.esm",567,-3,5};
 TradeLedger::Row take{"2","1","Skyrim.esm",567,3,0};
 assert(ledger.AcceptCursor("1",0,0));
 assert(ledger.Commit(give,0));
 assert(ledger.Ack("1",1));
 assert(ledger.Commit(take,0));
 assert(ledger.Ack("2",1));
 assert(ledger.Stock("1","2").at({"Skyrim.esm",567})==0);
 auto revision=host.Current();
 CampaignJournal::Journal restored;
 assert(restored.Open("recovery",81,"profile",false));
 TradeLedger after(restored,81);
 assert(after.Stream()==81);
 assert(after.AcceptCursor("1",0,0)&&after.AcceptCursor("2",0,0));
 assert(!after.AcceptCursor("1",82,0)&&!after.AcceptCursor("1",0,1));
 assert(!after.AcceptCursor("1",81,2));
 assert(after.MatchesReceipt("1",0,0,1,"Skyrim.esm",567,-3,5));
 assert(!after.MatchesReceipt("1",0,0,1,"Skyrim.esm",567,-3,4));
 assert(!after.MatchesReceipt("2",0,0,1,"Skyrim.esm",567,-3,5));
 assert(!after.MatchesReceipt("1",0,0,1,"Other.esp",567,-3,5));
 assert(!after.MatchesReceipt("1",81,1,1,"Skyrim.esm",567,-3,5));
 int64_t a=5,b=0;
 auto r=after.History("1")[0];assert(TradeLedger::CanApply(r,a));
 a+=r.count;
 assert(a==2&&!TradeLedger::CanApply(r,a)&&!TradeLedger::CanApply(r,4));
 r=after.History("2")[0];assert(TradeLedger::CanApply(r,b));
 b+=r.count;assert(b==3&&!TradeLedger::CanApply(r,b));
 assert(after.Ack("1",1)&&after.Ack("2",1));
 assert(restored.Current()==revision);
 assert(after.Stock("1","2").at({"Skyrim.esm",567})==0);
 // A later transfer is recovered in its own sequence, not by resetting
 // the shared buffer or by materializing the prior item again.
 assert(!after.Commit({"2","1","Skyrim.esm",567,-2,3},0));
 assert(!after.Commit({"2","1","Skyrim.esm",567,-4,3},1));
 assert(after.Commit({"2","1","Skyrim.esm",567,-2,3},1));
 assert(after.Ack("2",2));
 assert(after.AcceptCursor("2",0,0));
 assert(after.MatchesReceipt("2",0,0,1,"Skyrim.esm",567,3,0));
 assert(after.MatchesReceipt("2",81,1,2,"Skyrim.esm",567,-2,3));
 assert(!after.MatchesReceipt("2",0,0,2,"Skyrim.esm",567,-2,3));
 assert(after.Ack("2",1)&&after.Ack("2",2));
 assert(after.Stock("1","2").at({"Skyrim.esm",567})==2);
 CampaignJournal::Journal replica;
 assert(replica.Open("recovery-replica",81,"profile",true));
 for(uint32_t seq=1;seq<restored.Current().Revision;++seq)
     assert(replica.Import(seq,*restored.Export(seq))==Result::Committed);
 TradeLedger view(replica,81);
 assert(view.Stream()==81&&view.MatchesReceipt("2",81,1,2,"Skyrim.esm",567,-2,3));
 // Pending and acknowledged receipts are recoverable from a valid prefix,
 // but an unknown historical precondition is not.
 assert(after.Commit({"3","4","Skyrim.esm",567,-1},0));
 assert(after.Ack("3",1));
 assert(!after.AcceptCursor("3",0,0));
 assert(after.AcceptCursor("3",81,1));
 }

 using CampaignJournal::Result;
 CampaignJournal::Journal authority;
 assert(authority.Open("campaign",71,"profile",true));
 {
 TradeLedger l(authority,71);assert(l.Stream()==71);
 assert(l.Commit({"1","2","Patch.esp",567,-3},0));
 assert(l.Stock("1","2").empty());
 assert(l.Ack("1",1));
 assert(authority.Append(authority.Current(),{"quest-1","quest","unrelated"})==Result::Committed);
 assert(l.Commit({"2","1","Patch.esp",567,2},0));
 assert(l.Ack("2",1));
 }
 {
 CampaignJournal::Journal recovered;
 assert(recovered.Open("campaign",71,"profile",false));
 TradeLedger l(recovered,71);assert(l.Stream()==71);
 assert(l.Stock("1","2").at({"Patch.esp",567})==1);
 assert(l.History("1").size()==1&&l.History("2").size()==1);
 assert(!l.Commit({"2","1","Patch.esp",567,2},0));
 assert(!l.Commit({"2","1","Patch.esp",567,2},1));
 assert(l.Ack("2",1));
 assert(l.Commit({"2","1","Patch.esp",567,1},1));
 assert(recovered.Current().Revision==6);
 }
 assert(!std::filesystem::exists("config/CoopTrade"));

 TradeLedger::Row deposit{"Хаак","Дио","Patch.esp",567,-3}, take{"Дио","Хаак","Patch.esp",567,2};
 auto item=std::make_pair(std::string("Patch.esp"),567u);
 uint64_t uuid{};
 {TradeLedger l("ledger"); uuid=l.Stream(); assert(uuid);
 assert(l.Commit(deposit,0)); assert(!l.Commit(deposit,0)); assert(!l.Commit(take,0));
 assert(l.Stock("Хаак","Дио").empty());}
 {TradeLedger l("ledger"); assert(l.Stream()==uuid); assert(l.History("Хаак")[0].count==-3);
 assert(l.Ack("Хаак",1));assert(l.Ack("Хаак",1)); assert(l.Stock("Дио","Хаак").at(item)==3);
 assert(l.Commit(take,0)); assert(!l.Commit(take,0));assert(l.Stock("Дио","Хаак").at(item)==1);
 assert(!l.Commit({"X","Дио","Patch.esp",567,1},0));assert(!l.Ack("Дио",3));}
 {TradeLedger l("ledger"); assert(l.Stream()==uuid);assert(l.History("Дио")[0].count==2);assert(l.Ack("Дио",1));
 assert(l.Stock("Хаак","Дио").at(item)==1); assert(!l.Commit(take,1));
 assert(l.Commit({"Хаак","Дио","Patch.esp",567,1},1));}
}
''')
 subprocess.run(['g++','-std=c++20','-Wall','-Werror','-I',str(common),str(p/'trade.cpp'),str(common/'QuestJournalDisk.cpp'),str(common/'CampaignJournal.cpp'),'-o',str(p/'trade')],check=True)
 subprocess.run([str(p/'trade')],cwd=p,check=True)
print('PASS escrow commit before visibility, two restarts, both save cursors, duplicate operations, insufficient stock, pair isolation')
