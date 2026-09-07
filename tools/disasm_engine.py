# Disassemble regions of SkyrimSE.exe (1.6.1170 AE) around engine shadow functions.
# RVA base convention: SkyrimSE.exe+1511C80 means RVA 0x1511C80 (image base 0x140000000).
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

EXE = r"D:/ENB-EJ/Skyrim Special Edition/SkyrimSE.exe"


def load_pe(path):
    data = open(path, "rb").read()
    if data[:2] != b"MZ":
        raise SystemExit("not a PE")
    e_lfanew = int.from_bytes(data[0x3C:0x40], "little")
    assert data[e_lfanew:e_lfanew + 4] == b"PE\x00\x00"
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


def rva_to_off(secs, rva):
    for name, vaddr, vsize, roff, rsize in secs:
        if vaddr <= rva < vaddr + min(vsize, rsize):
            return roff + (rva - vaddr), name
    return None, None


def disasm(data, secs, rva, length, label=""):
    off, sec = rva_to_off(secs, rva)
    if off is None:
        print(f"[{label}] RVA 0x{rva:X} not in any section")
        return
    code = data[off:off + length]
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = False
    print(f"=== {label} @ 0x{rva:X} (section {sec}, {len(code)} bytes) ===")
    for ins in md.disasm(code, 0x140000000 + rva):
        print(f"  0x{ins.address - 0x140000000:08X}  {ins.mnemonic:10s} {ins.op_str}")
    print()


def main():
    data, secs = load_pe(EXE)
    print(f"PE loaded: {len(data)} bytes, {len(secs)} sections")
    for s in secs:
        print("  sec", s)
    print()
    # Usage: disasm_engine.py rva len [label]  (rva hex w/o 0x or with 0x)
    args = sys.argv[1:]
    rva = int(args[0], 16) if len(args) > 0 else 0x1511C80
    length = int(args[1], 16) if len(args) > 1 else 0x300
    label = args[2] if len(args) > 2 else f"RVA {rva:X}"
    disasm(data, secs, rva, length, label)


if __name__ == "__main__":
    main()
