import argparse, struct, zlib, zipfile, itertools, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "required/tools"))
sys.path.insert(0, str(ROOT / "obsolete/tools"))
from build_trade_plugin import sub, record
parser=argparse.ArgumentParser(description="Create read-only co-op guest interiors from an MO2 plugin snapshot.")
parser.add_argument('archive',type=Path)
parser.add_argument('loadorder',type=Path)
parser.add_argument('plugins',type=Path)
parser.add_argument('output',type=Path)
mode=parser.add_mutually_exclusive_group()
mode.add_argument('--estate-catalog', action='store_true', help='Write Hearthfire evidence only, not runtime permissions')
mode.add_argument('--native-capabilities', action='store_true', help='Write original interaction metadata only')
mode.add_argument('--instances', action='store_true', help='Build generic isolated projection cells, not legacy decorative homes')
args=parser.parse_args()
P=args.output;P.mkdir(parents=True,exist_ok=True)
archive=zipfile.ZipFile(args.archive)
names={}
for v in archive.namelist():
 name=Path(v).name
 if Path(name).suffix.casefold() not in ('.esm','.esp','.esl'):continue
 if name.casefold() in names:raise ValueError(f'Duplicate plugin basename in snapshot: {name}')
 names[name.casefold()]=v
def lines(path):
 return [s.strip() for s in path.read_text(encoding="utf-8-sig").splitlines() if s.strip() and not s.startswith("#")]
inactive={n.casefold() for n in lines(args.plugins) if not n.startswith("*")}
order=[Path(names[n.casefold()]).name for n in lines(args.loadorder) if n.casefold() not in inactive]
if len({n.casefold() for n in order})!=len(order):raise ValueError('Duplicate plugin in load order')
u32=struct.Struct('<I')
rec_head=struct.Struct('<4sIII')
sub_head=struct.Struct('<4sH')
def records(data,start=0,end=None,context=()):
 end=len(data) if end is None else end
 i=start
 while i<end:
  tag,n,flags,form=rec_head.unpack_from(data,i)
  begin=i+24
  if tag==b'GRUP':
   assert n>=24 and i+n<=end
   yield from records(data,begin,i+n,context+(data[i+8:i+24],))
   i+=n
  else:
   assert begin+n<=end
   yield i,tag,flags,form,begin,begin+n,context
   i=begin+n
def payload(data,start,end,flags):
 raw=data[start:end]
 if flags&0x40000:
  expanded=zlib.decompress(raw[4:])
  assert len(expanded)==u32.unpack_from(raw)[0]
  return expanded
 return raw
def fields(data):
 i=0
 while i<len(data):
  start=i
  tag,n=sub_head.unpack_from(data,i); i+=6
  if tag==b'XXXX':
   assert n==4
   n=u32.unpack_from(data,i)[0]
   tag=sub_head.unpack_from(data,i+4)[0]
   i+=10
  assert i+n<=len(data)
  yield tag,start,i,i+n
  i+=n
def decode(v):
 if not v.endswith(b'\0') or b'\0' in v[:-1]:return None
 try:return v[:-1].decode('utf-8')
 except UnicodeDecodeError:return None
def metadata(name,data):
 header=next(records(data))
 _,tag,flags,_,begin,end,_=header
 assert tag==b'TES4'
 raw=payload(data,begin,end,flags)
 masters=[decode(raw[a:b]).casefold() for tag,_,a,b in fields(raw) if tag==b'MAST']
 return masters+[name.casefold()],bool(flags&0x80)
def unpack(d): return struct.unpack_from('<I',d)[0]
def reader(want):
 for name in order:
  data=archive.read(names[name.casefold()]); ms,_=metadata(name,data)
  key=lambda id,ms=ms: (ms[id>>24],id&0xffffff) if id and id>>24<len(ms) else None
  for _,k,f,id,a,b,ctx in records(data):
   if k not in want or not key(id):continue
   yield k,key(id),f,ctx,key,lambda data=data,a=a,b=b,f=f: {t:raw[x:y] for raw in [payload(data,a,b,f)] for t,_,x,y in fields(raw)}
def home_info():
 locs={};cells={}
 for k,id,f,ctx,key,read in reader({b'LCTN',b'CELL'}):
  d=read()
  if k==b'LCTN':
   keywords=[key(x[0]) for x in struct.iter_unpack('<I',d.get(b'KWDA',b''))]
   locs[id]=not(f&32) and ('skyrim.esm',0xfc1a3) in keywords
  elif d.get(b'DATA',b'\0')[0]&1:
   cells[id]=(key(unpack(d.get(b'XLCN',bytes(4)))),d,key,f)
 houses={id:(d,key) for id,(loc,d,key,f) in cells.items() if not f&32 and locs.get(loc,False)}
 return houses
def refs(houses):
 allrefs={};doorlinks={}
 for k,id,f,ctx,key,read in reader({b'REFR'}):
  cell=next((key(unpack(g)) for g in reversed(ctx) if unpack(g[4:]) in (6,8,9,10)),None)
  if cell in houses or id in allrefs:
   if f&32 or cell not in houses:allrefs.pop(id,None);continue
   allrefs[id]=(cell,read(),key,f)
  # Exterior links are selected in a later pass by the identified interior door's destination.
 required={key(unpack(d[b'XTEL'])) for _,d,key,_ in allrefs.values() if b'XTEL' in d}
 for _,id,f,ctx,key,read in reader({b'REFR'}):
  if id in required:
   d=read()
   if f&32:doorlinks.pop(id,None)
   elif b'XTEL' in d:doorlinks[id]=(d,key)
 return allrefs,doorlinks
def grp(typ,label,data):
 if isinstance(label,int):label=struct.pack('<I',label)
 return struct.pack('<4sI4sIII',b'GRUP',24+len(data),label,typ,0,0)+data
if args.estate_catalog:
 # Reject inconsistent MO2 exports rather than silently reading unlisted plugins.
 implicit={'skyrim.esm','update.esm','dawnguard.esm','hearthfires.esm','dragonborn.esm',
           'ccbgssse001-fish.esm','ccqdrsse001-survivalmode.esl','ccbgssse037-curios.esl',
           'ccbgssse025-advdsgs.esm','_resourcepack.esl'}
 listed={n.lstrip('*').casefold() for n in lines(args.plugins)}
 active={n[1:].casefold() for n in lines(args.plugins) if n.startswith('*')}
 selected={n.casefold() for n in order}
 if active-selected or selected-listed-implicit:
  raise ValueError('Inconsistent loadorder/plugins exports')
 from build_estate_catalog import build
 build(P,reader,unpack,order)
 sys.exit(0)
if args.native_capabilities:
 from build_native_home_capabilities import build
 build(P,reader,unpack)
 sys.exit(0)
if args.instances:
 from build_home_instances import build
 build(P,order,reader,grp,unpack)
 sys.exit(0)
houses={k:v for k,v in home_info().items() if k[0]=='skyrim.esm' and k[1] in (91560,93690,92024,92678,93149)}
placed,doors=refs(houses)
models={};lights={};bases={k(unpack(d[b'NAME'])) for _,d,k,_ in placed.values() if b'NAME'in d}
for typ,id,flags,_,key,read in reader({b'STAT',b'MSTT',b'CONT',b'FURN',b'DOOR',b'ACTI',b'MISC',b'BOOK',b'INGR',b'ARMO',b'WEAP',b'AMMO',b'LIGH',b'ALCH',b'SLGM',b'FLOR',b'APPA',b'TREE',b'KEYM',b'TACT'}):
 if id in bases:
  d=read()
  if flags&32:models.pop(id,None);lights.pop(id,None);continue
  if typ==b'LIGH' and len(d.get(b'DATA',b''))>=16:
   light=bytearray(d[b'DATA']);flag,=struct.unpack_from('<I',light,12);struct.pack_into('<I',light,12,flag&~2)
   lights[id]=sub(b'OBND',d.get(b'OBND',bytes(12)))+(sub(b'MODL',d[b'MODL']) if d.get(b'MODL') else b'')+sub(b'DATA',light)
   if b'FNAM' in d:lights[id]+=sub(b'FNAM',d[b'FNAM'])
  elif d.get(b'MODL'):models[id]=(typ,d[b'MODL'])
seq=itertools.count(0x800)
cellids={k:next(seq) for k in houses}
baseids={model:next(seq) for _,model in sorted(set(models.values()))}
refids={k:next(seq) for k in placed}
lightids={k:next(seq) for k in sorted(lights)}
masters=set(['skyrim.esm'])
for _,(c,d,k,f) in placed.items():
 if b'XTEL' in d and k(unpack(d[b'XTEL'])) in doors and k(unpack(d[b'NAME'])):masters.update([k(unpack(d[b'NAME']))[0],k(unpack(d[b'XTEL']))[0]])
for d,key in houses.values():
 for tag in [b'LTMP',b'XCIM',b'XCAS',b'XCWT']:
  if tag in d and key(unpack(d[tag])):masters.add(key(unpack(d[tag]))[0])
masterlist=[n for n in order if n.lower() in masters]
assert len(masterlist)==len(masters)
idx={n.lower():i for i,n in enumerate(masterlist)}; own=len(masterlist)<<24
def source(k):return idx[k[0]]<<24|k[1]
def fid(x):return struct.pack('<I',x)
def form(k):return fid(source(k))
stats=b''.join(record(b'STAT',0,own|id,sub(b'OBND',bytes(12))+sub(b'MODL',model)) for model,id in baseids.items())
cells={};mapping=[];entry_count=len(baseids);nvisible=0
for home,(d,kcell) in houses.items():
 cid=own|cellids[home]
 data=sub(b'EDID',b'STRGuest'+d[b'EDID'])+sub(b'FULL','Гостевой интерьер'.encode()+b'\0')+sub(b'DATA',b'\x01\x00')
 for tag in [b'XCLL',b'LTMP',b'LNAM',b'XCIM',b'XCAS',b'XCWT']:
  if tag in d:
   if tag in (b'XCLL',b'LNAM'):data+=sub(tag,d[tag])
   elif kcell(unpack(d[tag])):data+=sub(tag,form(kcell(unpack(d[tag]))))
 mapping.append(f'H {home[1]} {cellids[home]}')
 persistent=b'';temp=b''
 for orig,(cell,r,key,flags) in placed.items():
  if cell!=home or b'NAME' not in r or len(r.get(b'DATA',b''))!=24:continue
  base=key(unpack(r[b'NAME']))
  linked=key(unpack(r[b'XTEL'])) if b'XTEL'in r else None
  if linked and linked in doors:
   exterior,ke=doors[linked]
   if ke(unpack(exterior[b'XTEL']))!=orig:continue
   raw=sub(b'NAME',form(base))+sub(b'XTEL',form(linked)+r[b'XTEL'][4:])+sub(b'DATA',r[b'DATA'])
   persistent+=record(b'REFR',0x400,own|refids[orig],raw)
   xyzrot=struct.unpack_from('<6f',exterior[b'XTEL'],4)
   mapping.append(f'D "{linked[0]}" {linked[1]} {cellids[home]} '+' '.join(map(str,xyzrot)))
  elif base in models or base in lights:
   target=lightids[base] if base in lights else baseids[models[base][1]]
   # All visitor decorations are static forms. No container inventory, aliases or scripts.
   raw=sub(b'NAME',fid(own|target))+sub(b'DATA',r[b'DATA'])
   if base in lights and b'XLIG' in r:raw+=sub(b'XLIG',r[b'XLIG'])
   if b'XSCL' in r:raw+=sub(b'XSCL',r[b'XSCL'])
   temp+=record(b'REFR',flags&0x800,own|refids[orig],raw)
   mapping.append(f'R "{orig[0]}" {orig[1]} {refids[orig]} {cellids[home]}')
   nvisible+=1
  else:continue
  entry_count+=1
 children=(grp(8,cid,persistent) if persistent else b'') + (grp(9,cid,temp) if temp else b'')
 cells.setdefault((cellids[home]%10,(cellids[home]//10)%10),[]).append(record(b'CELL',0,cid,data)+grp(6,cid,children))
 entry_count+=1
blocks=b''
for block in sorted({b for b,s in cells}):
 blocks+=grp(2,block,b''.join(grp(3,s,b''.join(items)) for (b,s),items in cells.items() if b==block))
lightgroup=b''.join(record(b'LIGH',0,own|lightids[k],v) for k,v in lights.items())
data=grp(0,b'LIGH',lightgroup)+grp(0,b'STAT',stats)+grp(0,b'CELL',blocks)
# HEDR counts records and groups, excluding TES4 itself.
def count(data):
 off=n=0
 while off<len(data):
  tag,size=struct.unpack_from('<4sI',data,off); n+=1
  if tag==b'GRUP':n+=count(data[off+24:off+size]);off+=size
  else:off+=24+size
 return n
header=sub(b'HEDR',struct.pack('<fII',1.7,count(data),next(seq)))+sub(b'CNAM',b'Skyrim Together\0')
for n in masterlist:header+=sub(b'MAST',n.encode()+b'\0')+sub(b'DATA',bytes(8))
(P/'SkyrimCoopGuestHomes.esp').write_bytes(record(b'TES4',0,0,header)+data)
(P/'CoopHomes.txt').write_text('\n'.join(mapping)+'\n')
print(len(houses),nvisible,'decorative references',masterlist)
