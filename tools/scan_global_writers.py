"""Scan all .text writers of a given .data global RVA (byte/dword/qword mov [rip+X]).
Usage: scan_global_writers.py <global_rva> [lo_rva] [hi_rva]
"""
import sys
from capstone import *
import reverse_id_rtti as R

data, secs = R.load_pe(R.EXE)

# section tuple: (name, vaddr, vsize, roff, rsize)
texts = [s for s in secs if s[0] == '.text']
def off_of(rva):
    for name, va, vs, ro, rs in texts:
        if va <= rva < va + min(vs, rs):
            return ro + (rva - va)
    return None

target = int(sys.argv[1], 16)
lo = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x1000
hi = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x1600000
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.skipdata = True

# iterate all .text ranges
hits = []
for name, va, vs, ro, rs in texts:
    size = min(vs, rs)
    start = max(va, lo)
    end = min(va + size, hi)
    code = data[off_of(start):off_of(end)]
    for ins in md.disasm(code, start):
        # mov [rip+disp] style writes or cmp [rip+disp]
        if ins.mnemonic in ('mov', 'cmp', 'test', 'or', 'and', 'xor', 'add', 'sub', 'inc', 'dec', 'lea') and 'rip' in ins.op_str:
            # parse [rip + 0x...]
            import re
            m = re.search(r'rip \+ (0x[0-9a-fA-F]+)', ins.op_str)
            if not m:
                m = re.search(r'rip - (0x[0-9a-fA-F]+)', ins.op_str)
                if m:
                    dst = ins.address + ins.size - int(m.group(1), 16)
                else:
                    continue
            else:
                dst = ins.address + ins.size + int(m.group(1), 16)
            if dst == target:
                hits.append(ins)
# print hits with window context
def window(rva, n=5):
    # disasm around rva
    out = []
    for name, va, vs, ro, rs in texts:
        if va <= rva < va + min(vs, rs):
            off = ro + (rva - va)
            st = max(va, rva - n*8)
            code = data[off_of(st):off_of(st)+ (n*2)*8]
            for ins in md.disasm(code, st):
                if va <= ins.address <= rva + n*8:
                    out.append('    0x%08X  %s %s' % (ins.address, ins.mnemonic, ins.op_str))
            break
    return out

print('writers/cmp of 0x%X: %d hits' % (target, len(hits)))
for ins in hits[:40]:
    print('  0x%08X  %s %s' % (ins.address, ins.mnemonic, ins.op_str))
    for w in window(ins.address, 2):
        print(w)
    print('  ---')
