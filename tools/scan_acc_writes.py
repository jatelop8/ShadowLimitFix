# Scan SkyrimSE.exe .text for writes to [reg+0x48] and [reg+0x40]
# (descriptor shaderAccumulator+0x48 / camera+0x40 candidates), filtered to shadow sys area
import re
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

data = open(r'D:/ENB-EJ/Skyrim Special Edition/SkyrimSE.exe', 'rb').read()
# .text sec1: file roff 0x400, VA 0x140001000, size 0x174D400
text = data[0x400:0x400 + 0x174D400]
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.skipdata = True

PAT_W48 = re.compile(r'\[([^\]]*)\+ *0x48\],')
PAT_W40 = re.compile(r'\[([^\]]*)\+ *0x40\],')

w48 = []
w40 = []
n = 0
for ins in md.disasm(text, 0x140001000):
    n += 1
    if ins.mnemonic == 'mov':
        o = ins.op_str
        m48 = PAT_W48.search(o)
        if m48:
            rva = ins.address - 0x140000000
            if 0x14A0000 <= rva <= 0x1540000:  # culling/scheduler/shadow region
                w48.append((rva, o[:90]))
            else:
                w48.append((rva, '*' + o[:80]))
        m40 = PAT_W40.search(o)
        if m40:
            rva = ins.address - 0x140000000
            if 0x14A0000 <= rva <= 0x1540000:
                w40.append((rva, o[:90]))

print('total insns:', n)
print('=== writes [reg+0x48] in shadow region (RVA 0x14A-0x154) ===')
for rva, o in sorted(w48):
    if rva < 0x14A0000 or rva > 0x1540000:
        continue
    print(f'  {rva:08X}: {o}')
print('=== writes [reg+0x40] in shadow region ===')
for rva, o in sorted(w40):
    print(f'  {rva:08X}: {o}')
