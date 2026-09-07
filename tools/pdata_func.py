# Resolve the exact function (begin/end RVA) containing a given code RVA via .pdata,
# then find all direct callers of that function start in .text.
# SkyrimSE.exe 1.6.1170 AE. RVAs are relative to image base 0x140000000.
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


def sec_for(secs, rva):
    for name, vaddr, vsize, roff, rsize in secs:
        if vaddr <= rva < vaddr + min(vsize, rsize):
            return name, vaddr, vsize, roff, rsize
    return None


def main():
    target = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x151474B
    data, secs = load_pe(EXE)

    # ---- .pdata: 3 dwords per entry (BeginRVA, EndRVA, UnwindInfoRVA) ----
    p = sec_for(secs, 0)  # not used
    pdata = None
    for s in secs:
        if s[0] == ".pdata":
            _, pv, pvs, pr, prs = s
            raw = data[pr:pr + prs]
            entries = []
            for i in range(0, len(raw) - 11, 12):
                b = int.from_bytes(raw[i:i + 4], "little")
                e = int.from_bytes(raw[i + 4:i + 8], "little")
                entries.append((b, e))
            pdata = entries
            break
    if pdata is None:
        print("no .pdata section")
        return
    print(f".pdata entries: {len(pdata)}")

    hit = None
    for b, e in pdata:
        if b <= target < e:
            hit = (b, e)
            break
    if hit is None:
        print(f"RVA 0x{target:X} not inside any .pdata function")
        # fallback: nearest begin <= target
        cand = [(b, e) for b, e in pdata if b <= target]
        if cand:
            b, e = max(cand, key=lambda x: x[0])
            print(f"nearest containing (begin<=target): 0x{b:X} .. 0x{e:X} (target beyond End?)")
            hit = (b, e)
    else:
        print(f"containing function: 0x{hit[0]:X} .. 0x{hit[1]:X}  (size 0x{hit[1]-hit[0]:X})")

    b, e = hit
    # walk a few entries around to see neighbors
    idx = [i for i, (bb, ee) in enumerate(pdata) if bb == b]
    if idx:
        i = idx[0]
        for j in range(max(0, i - 2), min(len(pdata), i + 4)):
            print(f"  pdata[{j}]: 0x{pdata[j][0]:X} .. 0x{pdata[j][1]:X}")

    # ---- disasm function head ----
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.skipdata = True
    t = sec_for(secs, b)
    if t:
        _, tv, _, tr, _ = t
        code = data[tr + (b - tv):tr + (b - tv) + min(0x140, e - b)]
        print(f"\n=== function head 0x{b:X} (first 0x{min(0x140, e-b):X}) ===")
        for ins in md.disasm(code, IMG + b):
            if ins.address - IMG >= e:
                break
            print(f"  0x{ins.address - IMG:08X}  {ins.mnemonic:10s} {ins.op_str}")

    # ---- find direct callers (call rel32 target == b) across .text ----
    txt = [s for s in secs if s[0] == ".text"][0]
    _, tv, _, tr, trs = txt
    raw = data[tr:tr + trs]
    callers = []
    md2 = Cs(CS_ARCH_X86, CS_MODE_64)
    md2.skipdata = True
    for ins in md2.disasm(raw, IMG + tv):
        if ins.mnemonic == "call":
            m = re.fullmatch(r"0x([0-9a-fA-F]+)", ins.op_str)
            if m and int(m.group(1), 16) == IMG + b:
                callers.append(ins.address - IMG)
    print(f"\n== direct callers of function 0x{b:X}: {len(callers)} ==")
    for c in sorted(callers):
        print(f"  call @ 0x{c:08X}")

    # disasm window around each caller
    for c in sorted(callers)[:8]:
        print(f"\n----- caller window @ 0x{c:08X} ±0x60 -----")
        lo, hi = c - 0x60, c + 0x60
        foff = tr + (lo - tv)
        chunk = raw[foff - tr:foff - tr + (hi - lo)]
        for ins in md2.disasm(chunk, IMG + lo):
            rva = ins.address - IMG
            if lo <= rva < hi:
                print(f"  0x{rva:08X}  {ins.mnemonic:10s} {ins.op_str}")


if __name__ == "__main__":
    main()
