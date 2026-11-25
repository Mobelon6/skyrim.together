"""Check the generated plugin IDs and the actual native trade wiring without loading Skyrim."""
from pathlib import Path
import struct,sys
root=Path(sys.argv[1])
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from build_trade_plugin import make
def walk(data,start=0,end=None):
    end=len(data) if end is None else end
    while start<end:
        tag,size,flags,form=struct.unpack_from("<4sIII",data,start)
        if tag==b"GRUP":
            assert size>=24 and start+size<=end
            yield tag,0,0,b""
            yield from walk(data,start+24,start+size)
            start+=size
        else:
            assert start+24+size<=end
            yield tag,flags,form,data[start+24:start+24+size]
            start+=24+size
    assert start==end
data=make()
records=list(walk(data))
assert records[0][0]==b"TES4"
assert struct.unpack_from("<I",records[0][3],10)[0]==len(records)-1
forms={f:(t,flags,body) for t,flags,f,body in records if f}
assert set(forms)==set(range(0x01000800,0x01000813))
assert all(forms[i][0]==b"GLOB" for i in range(0x01000800,0x01000810))
assert forms[0x01000810][0]==b"CONT"
assert forms[0x01000811][0]==b"CELL"
assert forms[0x01000812][0:2]==(b"REFR",0x400)
assert b"NAME\x04\x00"+struct.pack("<I",0x01000810) in forms[0x01000812][2]
client=(root/"Code/client/Services/Generic/PartyTrade.cpp").read_text()
ref=(root/"Code/client/Games/Skyrim/TESObjectREFR.cpp").read_text()
obj=(root/"Code/client/Services/Generic/ObjectService.cpp").read_text()
server=(root/"Code/server/Services/PartyTrade.cpp").read_text()
assert "NativeTrade::BlockRemove(apThis,apMoveToRef" in ref
assert "NativeTrade::BlockAdd(apThis,apOldOwner)" in ref
assert "NativeTrade::IsProxy(apObject)" in obj
assert "GuardPlayerRemove" in client and "GuardPlayerAdd" in client
assert "stockRemaining=e.Count" in client and "++reset.Count" in server
assert "nativeToken.store(0)" in client
assert "p->AddOrRemoveItem(item,true);" in client
assert "ScopedInventoryOverride guard;p->AddOrRemoveItem" not in client
assert "shadowCleanupPending=!ClearShadow()" in client
assert "object->RemoveItem(form,count,ITEM_REMOVE_REASON::kRemove,nullptr,nullptr)" in client
assert client.index("MaintainShadow();")<client.index("if (receiptFailure || !m_transport.IsOnline()",client.index("void OverlayService::UpdateTrade("))
assert "if (!e.BeforePlusOne || e.BeforePlusOne>0x80000000u)" in server
assert "STM_NATIVE_TRADE" in client and "open(object,0)" in client
assert 'std::strcmp(name,"TakeAllItems")==0' in client
assert 'Address<void*>(215061)' in client and 'table[1]' in client
assert 'if (!token && !NativeMenuOpen())' in client
assert 'previousTakeAll(args)' in client
assert 'nativeIntents.push_back({item,static_cast<uint32_t>(count),true})' in client
assert 'nativeIntents.push_back({{id,selected->Variant},static_cast<uint32_t>(count),source})' in client
assert 'object.RemoveItem(form,chunk,ITEM_REMOVE_REASON::kRemove,current->First,nullptr)' in client
assert 'ChangeVariant(*p,key,count)' in client
assert 'PropertyForm<EnchantmentItem>(value->Enchant)' in client
assert 'list->SetEnchantmentData(enchant' in client and 'list->SetPoison(poison,value->PoisonCount)' in client
assert 'object.GetItemCountInInventory(form)==totalBefore+delta' in client
assert 'name->iOwnerInstance!=-1 || name->usCustomNameLength' in client
assert 'name->iOwnerInstance==-2' in client
assert 'base+0x01771390' in client and 'base+0x00184230' in client
assert 'api->Set(data,name.c_str())' in client
assert 'SetCustomName(*list,value->Name)' in client
assert 'effect->Condition.pHead' in client
assert 'EnchantmentItem::Create(data)' in client
assert 'restored->Encode()==value.Encode()' in client
assert '!RecreateEnchant(*properties->Created)' in client
assert 'name.usCustomNameLength>TradeProperties::MaxNameBytes' in client
assert 'case OwnershipType:' in client
assert 'owner->formID!=7' in client and 'static_cast<FormType>(0x0B)' in client
assert '0x01770830' in client and 'SetTradeOwner(*list,value->Owner)' in client
assert 'PropertyOwner(value.Owner) && ResolveOwnershipTable()' in client
assert 'UnambiguousBase(player,key.first)' in client and 'entry->data' in client
assert 'UnownedProxy(*object)' in client and 'Address<GetOwner>(20194)' in client
assert 'ReceiptCount(m_world,*p,key,true)' in client
assert 'return total-special;' in client
assert 'r.variant==variant' in (root/'Code/common/TradeLedger.h').read_text()
assert '!nativeAwaiting' in client
assert 'const auto chunk=std::min(intent.Count,9999u)' in client
assert 'nativeAwaiting=false;nativeReceiptDeadline={};' in client
assert 'ResetNativeIntents();takeAllRegistered=false;' in client
assert 'std::min<int64_t>(count,INT32_MAX)' in server
print("PASS stable display ESP IDs and native trade integration assertions (not runtime ABI)")
