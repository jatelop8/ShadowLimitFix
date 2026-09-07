# Find every caller of the BSShaderAccumulator ctor (0x1414b1cf0) in .text.
# Those callers are the engine's "create + mount shaderAccumulator" sites.
import re, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

EXE = r"D:/ENB-EJ/Skyrim Special Edition/SkyrimSE.exe"
CTOR = 0x1414b1cf0          # BSShaderAccumulator ctor, absolute image address (RVA 0x14b1cf0)


def load_pe(path):
    data = open(path, "rb").read()
    e_lfanew = int.from_bytes(data[0x3C:0x40], "little")
    nsec = int.from_bytes(data[e_lfanew + 6:e_lfanew + 8], "little")
    optsz = int.from_bytes(data[e_lfanew + 20:e_lfanew + 22], "little")
    sec0 = e_lfanew + 24 + optsz
    secs = []
    for i in range(nsec):
        o = sec0 + i * 40
        name = data[o:o + 8].rstrip(b"\x00").decode("ascii", "replace")
        vsize = int.from_bytes(data[o + 8:o + 12], "little")
        vaddr = int.from_bytes(data[o + 12:o + 16], "little")
        rsize = int.from_bytes(data[o + 16:o + 20], "little")
        roff = int.from_bytes(data[o + 20:o + 24], "little")
        secs.append((name, vaddr, vsize, roff, rsize))
    return data, secs


def main():
    data, secs = load_pe(EXE)
    name, vaddr, vsize, roff, rsize = [s for s in secs if s[0] == ".text"][0]
    text = data[roff:roff + rsize]
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.skipdata = True

    # .text section virtual address maps to image base 0x140000000 (vaddr 0x1000 -> 0x140001000)
    imgbase = 0x140000000
    txt_va = imgbase + vaddr
    sites = []
    for ins in md.disasm(text, txt_va):
        if ins.mnemonic == "call" and hex(CTOR) in ins.op_str:
            sites.append(ins.address - imgbase)
    print(f"== direct callers of BSShaderAccumulator ctor 0x{CTOR:X}: {len(sites)} ==")
    for s in sorted(sites):
        print(f"  call @ 0x{s:08X}")

    # disasm window around each site, filter to interesting ops
    rad = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x70
    md2 = Cs(CS_ARCH_X86, CS_MODE_64)
    md2.skipdata = True
    for s in sorted(sites):
        print(f"\n===== window around 0x{s:08X} ±0x{rad:X} =====")
        rva_lo = s - rad
        rva_hi = s + rad
        chunk_start = rva_lo
        foff = roff + (chunk_start - vaddr)
        chunk = text[foff - roff:foff - roff + (rva_hi - chunk_start)]
        for ins in md2.disasm(chunk, imgbase + chunk_start):
            rva = ins.address - 0x140000000
            if not (rva_lo <= rva <= rva_hi):
                continue
            o = ins.op_str
            hot = (ins.mnemonic == "call" and hex(CTOR) in o) or \
                  re.search(r"\[[^\]]+\+ *0x(48|1a8|150)\],", o) or \
                  "0x180" in o
            if hot:
                mk = "  <<<CTOR" if ins.mnemonic == "call" and hex(CTOR) in o else ""
                print(f"  0x{rva:08X}  {ins.mnemonic:8s} {o}{mk}")


if __name__ == "__main__":
    main()
