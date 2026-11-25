"""Exercise the actual native door resolver with engine-layout stubs."""
from pathlib import Path
import sys, tempfile, subprocess
source=(Path(sys.argv[1])/"Code/client/Services/Generic/PartyHome.cpp").read_text()
def fn(signature):
    i=source.index(signature); start=source.index("{",i)
    d=1;j=start+1
    while d:
        d+=(source[j]=="{")-(source[j]=="}");j+=1
    return source[i:j]+"\n"
code=r"""
#include <cstdint>
#include <cstring>
#include <cassert>
#include <map>
enum class FormType {Door,Other};
enum class ExtraDataType {Teleport};
struct Base {FormType formType=FormType::Door;};
struct TESObjectCELL {
    uint32_t formID=100;unsigned cellFlags=1;void* worldspace=nullptr;
    bool temp=false;bool IsTemporary()const{return temp;}
};
namespace HomeInstance {bool ProjectionForm(uint32_t id){return id>=1000;}}
struct Teleport {unsigned char header[16]{};const void* data{};};
struct Extras {
    Teleport* teleport{};
    const void* GetByType(ExtraDataType)const{return teleport;}
};
struct TESObjectREFR {
    static inline std::map<uint32_t,TESObjectREFR*> refs;
    uint32_t formID{};
    Base* baseForm{};
    Extras extra;
    TESObjectCELL* cell{};
    bool temp{},deleted{};
    bool IsTemporary()const{return temp;}
    bool IsDeleted()const{return deleted;}
    Extras* GetExtraDataList(){return &extra;}
    TESObjectCELL* GetParentCellEx(){return cell;}
    static TESObjectREFR* GetByHandle(uint32_t handle){auto i=refs.find(handle);return i==refs.end()?nullptr:i->second;}
};
static_assert(offsetof(Teleport,data)==16);
"""
code+=fn("TESObjectREFR* NativeLinkedDoor(")
code+=fn("TESObjectCELL* NativeDoorDestination(")
code+=r"""
int main(){
    Base door;
    TESObjectCELL outside;outside.formID=10;outside.cellFlags=0;
    TESObjectCELL inside;inside.formID=20;
    uint32_t toInside=2,toOutside=1;
    Teleport a{},b{};a.data=&toInside;b.data=&toOutside;
    TESObjectREFR from{},to{};
    from.formID=11;from.baseForm=&door;from.extra.teleport=&a;from.cell=&outside;
    to.formID=21;to.baseForm=&door;to.extra.teleport=&b;to.cell=&inside;
    TESObjectREFR::refs={{1,&from},{2,&to}};
    assert(NativeDoorDestination(&from)==&inside);
    assert(!NativeDoorDestination(&to)); // exterior is not a root interior
    toOutside=2;assert(!NativeDoorDestination(&from));toOutside=1;
    to.deleted=true;assert(!NativeDoorDestination(&from));to.deleted=false;
    to.temp=true;assert(!NativeDoorDestination(&from));to.temp=false;
    to.formID=1001;assert(!NativeDoorDestination(&from));to.formID=21;
    inside.formID=1002;assert(!NativeDoorDestination(&from));inside.formID=20;
    to.cell=&outside;assert(!NativeDoorDestination(&from));to.cell=&inside;
    inside.worldspace=&inside;assert(!NativeDoorDestination(&from));inside.worldspace=nullptr;
    a.data=nullptr;assert(!NativeDoorDestination(&from));a.data=&toInside;
    toInside=999;assert(!NativeDoorDestination(&from));toInside=2;
    assert(NativeDoorDestination(&from)==&inside);
}
"""
code="#include <cstddef>\n"+code
with tempfile.TemporaryDirectory() as d:
    src=Path(d)/"door.cpp";exe=Path(d)/"door";src.write_text(code)
    subprocess.run(["g++","-std=c++20","-Wall","-Wextra","-Werror",str(src),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
owner=fn("void SyncObjects(")
assert "catalog?catalog->Refs.size():baseline.Refs.size()" in owner
assert "parentControlled=extra->Contains(static_cast<ExtraDataType>(0x36))" in owner
assert "extra->HasQuestObjectAlias()" in owner
assert "catalog?catalog->GuestDoors.count(key):nativeDoor" in owner
assert "NativeCapability('D',root,key,base.Encode())" in owner
assert "if (slot && Owned(m_world,original,p))" in source
assert "const auto destination=EntranceRoot(form,cell)" in source
assert "destination->Encode()!=menu.Root" in source
assert "SyncObjects(world,*cell,*identity,slot,encoded,*state)" in source
print("PASS original reciprocal door, invalid handle, exterior, projection, owner baseline fallback and ACL root")
