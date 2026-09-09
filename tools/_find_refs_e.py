# Find every reference (code lea rip-rel, and data/vtable pointer) to a target function
# address across the whole image, then dump the surrounding vtable window for data hits.
# SkyrimSE.exe 1.6.1170 AE, image base 0x140000000.
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


def main():
    target_rva = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x1512230
    T = IMG + target_rva
    data, secs = load_pe(EXE)

    # ---- code refs: lea reg, [rip+disp] / call-ish indirect that embed target ----
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.skipdata = True
    txt = [s for s in secs if s[0] == ".text"][0]
    _, tv, _, tr, trs = txt
    raw = data[tr:tr + trs]
    code_refs = []
    for ins in md.disasm(raw, IMG + tv):
        if ins.mnemonic in ("lea", "mov") and "rip" in ins.op_str:
            m = re.search(r"\[rip \+ 0x([0-9a-fA-F]+)\]", ins.op_str)
            if m:
                disp = int(m.group(1), 16)
                dest = ins.address + ins.size + disp
                if dest == T:
                    code_refs.append((ins.address - IMG, ins.mnemonic, ins.op_str))
    print(f"== code refs (lea/mov rip-rel) to 0x{target_rva:X}: {len(code_refs)} ==")
    for rva, mn, op in code_refs:
        print(f"  0x{rva:08X}  {mn:6s} {op}")

    # ---- data refs: 8-byte VA == T in any non-code section ----
    print(f"\n== data pointers == T (0x{T:X}) ==")
    tb = T.to_bytes(8, "little")
    found = []
    for name, vaddr, vsize, roff, rsize in secs:
        if name in (".text", ".pdata"):
            continue
        blob = data[roff:roff + rsize]
        start = 0
        while True:
            i = blob.find(tb, start)
            if i < 0:
                break
            found.append((name, vaddr + i, i))
            start = i + 1
    print(f"total data hits: {len(found)}")
    # cluster hits: vtable = several consecutive 8-byte VAs in .rdata/.data pointing into .text
    # print every hit with neighbors to spot vtable layout
    for name, va, off in found:
        # back up to ~8 entries before and show 20 entries around
        blob_off = off
        print(f"\n--- hit in {name} @ RVA 0x{va:08X} (file off 0x{off:X}) ---")
        # find section start file offset to index blob
        for nm2, vv, vvs, rr, rrs in secs:
            if nm2 == name:
                blob = data[rr:rr + rrs]
                break
        base = off - 4 * 8  # 4 entries before
        if base < 0:
            base = 0
        for k in range(base, min(base + 20 * 8, len(blob) - 7), 8):
            v = int.from_bytes(blob[k:k + 8], "little")
            mark = " <<<" if v == T else ""
            rva_disp = ""
            if IMG <= v < IMG + 0x40000000:
                rva_disp = f" (-> .text+0x{v-IMG:X})"
            elif v != 0:
                rva_disp = f" (non-code VA 0x{v:X})"
            print(f"    +0x{base + (k-base):04X}: 0x{v:016X}{rva_disp}{mark}")


if __name__ == "__main__":
    main()
