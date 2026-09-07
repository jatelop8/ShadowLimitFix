"""Filter scan_528 hits to REAL [reg+0x528] memory accesses.
A true modrm disp32 has modrm byte (mod==10 => 0x80..0xBF) immediately
before the 28 05 00 00 disp. False positives: call rel32 imm, mov
[rsp+0x28],imm5, etc. Then capstone-disasm a window to confirm the
instruction really reads/writes obj+0x528.
Usage: disasm_528.py
"""
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

EXE = r"D:/ENB-EJ/Skyrim Special Edition/SkyrimSE.exe"


def load_pe(path):
    data = open(path, "rb").read()
    e = int.from_bytes(data[0x3C:0x40], "little")
    n = int.from_bytes(data[e + 6:e + 8], "little")
    o = int.from_bytes(data[e + 20:e + 22], "little")
    s0 = e + 24 + o
    secs = []
    for i in range(n):
        p = s0 + i * 40
        name = data[p:p + 8].rstrip(b"\x00").decode("ascii", "replace")
        vs = int.from_bytes(data[p + 8:p + 12], "little")
        va = int.from_bytes(data[p + 12:p + 16], "little")
        rs = int.from_bytes(data[p + 16:p + 20], "little")
        ro = int.from_bytes(data[p + 20:p + 24], "little")
        secs.append((name, va, vs, ro, rs))
    return data, secs


def main():
    data, secs = load_pe(EXE)
    t = [s for s in secs if s[0] == ".text"][0]
    _, va, vs, ro, rs = t
    blob = data[ro:ro + min(vs, rs)]
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.skipdata = False

    needle = b"\x28\x05\x00\x00"
    hits = []
    pos = 0
    while True:
        i = blob.find(needle, pos)
        if i < 0:
            break
        # modrm byte = blob[i-1]; must be a mod=10 register-disp32 form
        if i >= 2:
            mr = blob[i - 1]
            if 0x80 <= mr <= 0xBF:
                # skip if the instruction before (from i-2, allow REX at i-2/i-3)
                start = max(0, i - 14)
                for ins in md.disasm(blob[start:i + 4], va + start):
                    pass  # warm
                # re-disasm picking the ins that ENDS at i+4
                for ins in md.disasm(blob[start:i + 4], va + start):
                    if ins.address + ins.size == va + i + 4 and "+ 0x528" in ins.op_str:
                        hits.append((ins.address, f"{ins.mnemonic} {ins.op_str}"))
        pos = i + 1

    seen = set()
    print(f"real [reg+0x528] accesses: {len(hits)}")
    for addr, txt in sorted(hits):
        if (addr, txt) in seen:
            continue
        seen.add((addr, txt))
        print(f"0x{addr - 0x140000000:X}  {txt}")


if __name__ == "__main__":
    main()
