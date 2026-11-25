"""Create nonconstant save globals; preserve original escrow FormIDs 800–803."""
import struct
import sys
from pathlib import Path
def sub(tag, value):
    return tag + struct.pack('<H', len(value)) + value
def record(tag, flags, form, data):
    return struct.pack('<4sIIIIHH',tag,len(data),flags,form,0,44,0)+data
def group(kind,label,data):
    if isinstance(label,int):label=struct.pack('<I',label)
    return struct.pack('<4sI4sIII',b'GRUP',24+len(data),label,kind,0,0)+data
def make():
    header=sub(b'HEDR',struct.pack('<fII',1.7,26,0x813))+sub(b'CNAM',b'Skyrim Together\0')+sub(b'MAST',b'Skyrim.esm\0')+sub(b'DATA',bytes(8))
    globals_=b''
    names=('STRTradeStreamLow','STRTradeStreamMiddle','STRTradeStreamHigh','STRTradeCursor',
           'STRCampaignCampaignLow','STRCampaignCampaignMiddle','STRCampaignCampaignHigh','STRCampaignCharacterLow','STRCampaignCharacterMiddle','STRCampaignCharacterHigh','STRCampaignRevisionLow','STRCampaignRevisionHigh','STRCampaignTagLow','STRCampaignTagMiddle','STRCampaignTagHigh','STRCampaignSchema')
    for i,name in enumerate(names):
        globals_+=record(b'GLOB',0,0x01000800+i,sub(b'EDID',name.encode()+b'\0')+sub(b'FNAM',b'f')+sub(b'FLTV',bytes(4)))
    # Empty local-only display chest, never placed in an accessible game cell.
    base,cell,ref=0x01000810,0x01000811,0x01000812
    container=record(b'CONT',0,base,sub(b'EDID',b'STRTradeDisplayChest\0')+sub(b'OBND',bytes(12))+sub(b'FULL',b'Trade'+b'\0')+sub(b'COCT',bytes(4))+sub(b'DATA',struct.pack('<Bf',0,0.0)))
    room=record(b'CELL',0,cell,sub(b'EDID',b'STRTradeDisplayCell\0')+sub(b'DATA',b'\x01\x00'))
    chest=record(b'REFR',0x400,ref,sub(b'NAME',struct.pack('<I',base))+sub(b'DATA',bytes(24)))
    children=group(6,cell,group(8,cell,chest))
    cells=group(0,b'CELL',group(2,(cell&0xffffff)%10,group(3,((cell&0xffffff)//10)%10,room+children)))
    return record(b'TES4',0x200,0,header)+group(0,b'GLOB',globals_)+group(0,b'CONT',container)+cells
if __name__=='__main__':
    Path(sys.argv[1]).write_bytes(make())
