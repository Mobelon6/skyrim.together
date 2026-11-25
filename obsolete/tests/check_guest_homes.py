"""Validate the shipped CELL hierarchy, every dedicated reference and every real exit."""
from collections import Counter
from pathlib import Path
import shlex, struct, sys
root=Path(sys.argv[1])
records={}; counts=Counter()
def fields(data):
 pos=0
 while pos<len(data):
  tag,size=struct.unpack_from('<4sH',data,pos);value=data[pos+6:pos+6+size]
  assert len(value)==size
  yield tag,value;pos+=size+6
def walk(data,cell=None):
 pos=0
 while pos<len(data):
  tag,size,flags,form=struct.unpack_from('<4sIII',data,pos)
  counts[tag]+=1
  if tag==b'GRUP':
   assert 24<=size<=len(data)-pos
   walk(data[pos+24:pos+size],flags if form in (6,8,9,10) else cell);pos+=size;continue
  raw=data[pos+24:pos+24+size];assert len(raw)==size
  assert form not in records
  records[form]=(tag,list(fields(raw)),cell,flags)
  pos+=24+size
walk((root/'SkyrimCoopGuestHomes.esp').read_bytes())
header=dict(records[0][1]);masters=[v.rstrip(b'\0').decode().lower() for k,v in records[0][1] if k==b'MAST']
assert masters[0]=='skyrim.esm' and int.from_bytes(header[b'HEDR'][4:8],'little')==sum(counts.values())-1
prefix=len(masters)<<24
assert all((k>>24)==len(masters) for k in records if k)
assert set(counts)<={b'GRUP',b'TES4',b'CELL',b'STAT',b'LIGH',b'REFR'}
expected={};doors={};refs=set()
for line in (root/'CoopHomes.txt').read_text().splitlines():
 t=shlex.split(line)
 if t[0]=='H':
  real,slot=map(int,t[1:]); assert slot not in expected and records[prefix|slot][0]==b'CELL'
  expected[slot]=real
 elif t[0]=='R':
  copy,slot=map(int,t[3:]);rec,fs,c,_=records[prefix|copy];assert rec==b'REFR' and c==prefix|slot and copy not in refs
  fs=dict(fs);assert not ({b'VMAD',b'XOWN',b'XESP',b'XTEL'}&fs.keys())
  assert records[int.from_bytes(fs[b'NAME'],'little')][0] in (b'STAT',b'LIGH')
  refs.add(copy)
 else:
  assert t[0]=='D' and len(t)==10
  key=(int(t[3]),masters.index(t[1].lower())<<24|int(t[2]))
  assert key not in doors;doors[key]=False
for fid,(tag,fs,c,flags) in records.items():
 if tag!=b'REFR':continue
 fs=dict(fs)
 if b'XTEL' in fs:
  key=(c&0xffffff,struct.unpack_from('<I',fs[b'XTEL'])[0]);assert key in doors;doors[key]=True
assert all(doors.values()) and {s for s,_ in doors}==expected.keys()
assert counts[b'CELL']==len(expected)==5 and counts[b'REFR']==len(refs)+len(doors)
print('PASS',len(expected),'independent interiors,',len(refs),'inert decorations/lights,',len(doors),'linked exits, no overrides or container/script records')
