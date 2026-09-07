"""Byte-scan SkyrimSE.exe .text for accesses to object offset 0x528
(sceneAccumArray of BSShadowLight, SE flat layout). Prints every hit RVA
with raw bytes + which REX/reg. Quick and dependency-free (no capstone).
Usage: scan_528.py [lo_rva] [hi_rva]
"""
import sys

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
    lo = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x1000
    hi = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x174D400
    data, secs = load_pe(EXE)
    text = [s for s in secs if s[0] == ".text"][0]
    name, va, vs, ro, rs = text
    blob = data[ro:ro + min(vs, rs)]

    needle = (0x28).to_bytes(1, "little") + (0x05).to_bytes(1, "little") + \
        (0x00).to_bytes(1, "little") + (0x00).to_bytes(1, "little")  # 28 05 00 00 = +0x528 disp32

    pos = 0
    hits = []
    while True:
        i = blob.find(needle, pos)
        if i < 0:
            break
        rva = va + i  # section vaddr is already the RVA (0x1000-based), no IMG subtract
        if lo <= rva <= hi:
            # sanity: the byte before the disp32 tells the scale -- look back
            # for the modrm byte; keep raw window for manual review
            win = blob[max(0, i - 8):i + 12]
            hits.append((rva, win.hex(" ")))
        pos = i + 1

    print(f"hits for disp32 +0x528 in .text[{hex(lo)}..{hex(hi)}]: {len(hits)}")
    for rva, hexw in hits:
        print(f"0x{rva:X}  {hexw}")


if __name__ == "__main__":
    main()
