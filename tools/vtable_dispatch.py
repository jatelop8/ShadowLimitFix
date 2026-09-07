# Resolve vtable run containing .rdata RVA slot_rva; report class name via RTTI COL,
# slot offset of slot_rva, then find .text indirect dispatch sites matching that slot offset.
import sys, re
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

EXE = r"D:/ENB-EJ/Skyrim Special Edition/SkyrimSE.exe"
IMG = 0x140000000


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


def rva_off(secs, rva):
    for name, vaddr, vsize, roff, rsize in secs:
        if vaddr <= rva < vaddr + min(vsize, rsize):
            return roff + (rva - vaddr), name, rsize
    return None, None, 0


def rd(data, secs, rva, n=8):
    off, sec, _ = rva_off(secs, rva)
    if off is None:
        return None
    return int.from_bytes(data[off:off + n], "little")


def main():
    slot_rva = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x1AC54B0
    data, secs = load_pe(EXE)

    # --- walk backward while qword points into .text; remember first non-text boundary ---
    q = slot_rva
    start = None
    while True:
        v = rd(data, secs, q - 8)
        if v is None:
            break
        tgt_rva = v - IMG
        _, sec, _ = rva_off(secs, tgt_rva) if (IMG <= v < IMG + 0x40000000) else (None, None, 0)
        if sec == ".text":
            start = q - 8
            q -= 8
        else:
            break
    print(f"vtable run start .rdata+0x{start:X} .. +0x{slot_rva:X}  (slots {(slot_rva - start)//8 + 1})")

    # vtable[-1] = RTTI COL at start-8
    col = rd(data, secs, start - 8)
    if col and IMG <= col < IMG + 0x40000000:
        col_rva = col - IMG
        coff, csec, _ = rva_off(secs, col_rva)
        if csec in (".rdata", ".data") and coff is not None:
            ptd = int.from_bytes(data[coff + 12:coff + 16], "little")  # pTypeDescriptor (image-relative)
            nm = None
            toff, tsec, _ = rva_off(secs, ptd)
            if tsec in (".rdata", ".data") and toff is not None:
                noff = toff + 0x10
                end = data.find(b"\x00", noff, noff + 256)
                nm = data[noff:end].decode("ascii", "replace") if end >= 0 else "?"
            print(f"vtable[-1] COL @ .rdata+0x{col_rva:X}  pTypeDescriptor=0x{ptd:X}  class: {nm}")
        else:
            print(f"vtable[-1] = 0x{col_rva:X} ({csec}) not .rdata")
    else:
        print(f"vtable[-1] not image ptr: 0x{col:X if col else -1}")

    # print a few slots around the end to confirm layout
    print("\nslots near end (last 12):")
    for i in range(12):
        rva = slot_rva - (11 - i) * 8
        v = rd(data, secs, rva)
        if v and IMG <= v < IMG + 0x40000000:
            print(f"  [{ (rva-start)//8 }] +0x{rva:06X}: .text+0x{v-IMG:X}")
        else:
            print(f"  [{ (rva-start)//8 }] +0x{rva:06X}: 0x{v:X if v else 0}")

    slot_idx = (slot_rva - start) // 8
    disp = slot_idx * 8
    print(f"\n== target slot idx {slot_idx} (disp 0x{disp:X}) ==")

    # --- scan .text: call qword ptr [reg+0xNN] with NN==disp ---
    txt = [s for s in secs if s[0] == ".text"][0]
    _, tv, _, tr, trs = txt
    raw = data[tr:tr + trs]
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.skipdata = True
    pat = re.compile(r"qword ptr \[(r[a-z0-9]+|rax|rcx|rdx|rbx|rsp|rbp|rsi|rdi|r8|r9|r10|r11|r12|r13|r14|r15) \+ 0x%X\]" % disp)
    sites = []
    for ins in md.disasm(raw, IMG + tv):
        if ins.mnemonic == "call" and ins.op_str.startswith("qword ptr ["):
            m = re.search(r"qword ptr \[([a-z0-9]+) \+ 0x([0-9a-fA-F]+)\]", ins.op_str)
            if m and int(m.group(2), 16) == disp:
                sites.append((ins.address - IMG, ins.op_str))
    print(f"indirect call sites with disp 0x{disp:X}: {len(sites)}")
    for rva, op in sites:
        print(f"  0x{rva:08X}  call {op}")

    # two-instruction pattern: mov reg,[reg]; call qword ptr [reg+disp] handled by window dump
    for rva, op in sites[:6]:
        print(f"\n----- dispatch window @ 0x{rva:08X} ±0x40 -----")
        lo, hi = rva - 0x40, rva + 0x40
        for ins in md.disasm(raw[ (lo - tv):(lo - tv) + (hi - lo) ], IMG + lo):
            rr = ins.address - IMG
            if lo <= rr < hi:
                print(f"  0x{rr:08X}  {ins.mnemonic:10s} {ins.op_str}")


if __name__ == "__main__":
    main()
