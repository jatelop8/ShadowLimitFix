# Disassemble an RVA range of SkyrimSE.exe (diagnostic only).
# Usage: python disasm_range.py <exe> <rva_start_hex> <rva_end_hex>
import sys
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

exe = sys.argv[1]
rva_start = int(sys.argv[2], 16)
rva_end = int(sys.argv[3], 16)

pe = pefile.PE(exe, fast_load=True)
image_base = pe.OPTIONAL_HEADER.ImageBase

def rva_to_off(rva):
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
            return s.PointerToRawData + (rva - s.VirtualAddress)
    return None

off = rva_to_off(rva_start)
if off is None:
    print("RVA not in any section")
    sys.exit(1)

size = rva_end - rva_start
with open(exe, "rb") as f:
    f.seek(off)
    code = f.read(size)

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = False
for insn in md.disasm(code, image_base + rva_start):
    mark = ""
    if insn.address == image_base + 0x14CC1A2:
        mark = "   <== CRASH RIP"
    print("0x%08X  %-8s %s%s" % (insn.address - image_base, insn.mnemonic, insn.op_str, mark))
