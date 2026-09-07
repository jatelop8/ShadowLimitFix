# Disassemble several engine function starts for semantic identification.
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

exe = "D:/ENB-EJ/Skyrim Special Edition/SkyrimSE.exe"
pe = pefile.PE(exe, fast_load=True)
image_base = pe.OPTIONAL_HEADER.ImageBase

def rva_to_off(rva):
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
            return s.PointerToRawData + (rva - s.VirtualAddress)
    return None

def disasm(rva, length):
    off = rva_to_off(rva)
    if off is None:
        print("0x%08X: not mapped" % rva)
        return
    with open(exe, "rb") as f:
        f.seek(off)
        code = f.read(length)
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    for insn in md.disasm(code, image_base + rva):
        print("0x%08X  %-8s %s" % (insn.address - image_base, insn.mnemonic, insn.op_str))

for start, ln in [(0x1414a4010, 0x90), (0x140cf67f0, 0x30), (0x140cf6810, 0x30), (0x1414b5230, 0x40)]:
    print("==== RVA 0x%08X ====" % start)
    disasm(start, ln)
    print()
