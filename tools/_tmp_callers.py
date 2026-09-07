import sys
sys.argv=['x']
src=open('disasm_engine.py').read()
src=src.replace("EXE = r\"D:/ENB-EJ/Skyrim Special Edition/SkyrimSE.exe\"", "EXE = r\"E:/ENB-EJ/Skyrim Special Edition/SkyrimSE.exe\"")
exec(src.split('def main():')[0])
data, secs = load_pe(EXE)
TARGET=0x14FCF80
code=data  # whole file
# scan .text for E8 rel32 / FF 15 etc
text=None
for name,vaddr,vsize,roff,rsize in secs:
    if name==b'.text':
        text=(vaddr,roff,rsize); break
print("text", text)
vaddr,roff,rsize=text
import struct
res=[]
for i in range(roff, roff+rsize-5):
    if data[i]==0xE8:
        rel=struct.unpack_from('<i', data, i+1)[0]
        src_a=0x140000000+vaddr+(i-roff)
        dst=src_a+5+rel
        if dst==0x140000000+TARGET:
            res.append(src_a)
print("direct E8 callers of 107784:")
for a in res: print("  %08X"%(a-0x140000000))
