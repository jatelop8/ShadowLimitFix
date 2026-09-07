# 1) Reverse-lookup: which Address Library IDs map to RVA 0x1512230 (and the neighbour fn family).
# 2) RTTI: for .rdata pointer table at RVA 0x1AC54B0, read vtable[-1] COL -> TypeDescriptor name.
import struct

OFFLIB = r'E:/EJ/mod/mods/Addmodress Library All in One/SKSE/Plugins/versionlib-1-6-1170-0.bin'
EXE = r"D:/ENB-EJ/Skyrim Special Edition/SkyrimSE.exe"
IMG = 0x140000000


def load_offlib(path):
    d = open(path, 'rb').read()
    pos = 0
    def rd(fmt):
        nonlocal pos
        sz = struct.calcsize(fmt)
        v = struct.unpack_from(fmt, d, pos)
        pos += sz
        return v[0] if len(v) == 1 else v
    assert rd('<i') == 2
    ver = rd('<4i')
    nameLen = rd('<i')
    name = d[pos:pos + nameLen].decode('ascii', 'replace'); pos += nameLen
    pointerSize = rd('<i')
    addressCount = rd('<i')
    table = {}
    prevID = 0; prevOff = 0
    for k in range(addressCount):
        t = d[pos]; pos += 1
        lo = t & 0xF; hi = t >> 4
        if lo == 0:   id_ = rd('<Q')
        elif lo == 1: id_ = prevID + 1
        elif lo == 2: id_ = prevID + d[pos]; pos += 1
        elif lo == 3: id_ = prevID - d[pos]; pos += 1
        elif lo == 4: id_ = prevID + rd('<H')
        elif lo == 5: id_ = prevID - rd('<H')
        elif lo == 6: id_ = rd('<H')
        elif lo == 7: id_ = rd('<I')
        else: raise ValueError(f'bad lo {lo}')
        tmp = (prevOff // pointerSize) if (hi & 8) else prevOff
        hi7 = hi & 7
        if hi7 == 0:   off = rd('<Q')
        elif hi7 == 1: off = tmp + 1
        elif hi7 == 2: off = tmp + d[pos]; pos += 1
        elif hi7 == 3: off = tmp - d[pos]; pos += 1
        elif hi7 == 4: off = tmp + rd('<H')
        elif hi7 == 5: off = tmp - rd('<H')
        elif hi7 == 6: off = rd('<H')
        elif hi7 == 7: off = rd('<I')
        else: raise ValueError(f'bad hi7 {hi7}')
        if hi & 8:
            off *= pointerSize
        table[id_] = off
        prevID = id_; prevOff = off
    return table


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


def sec_rva_to_off(secs, rva):
    for name, vaddr, vsize, roff, rsize in secs:
        if vaddr <= rva < vaddr + min(vsize, rsize):
            return roff + (rva - vaddr), name
    return None, None


def main():
    table = load_offlib(OFFLIB)
    print(f"offlib entries: {len(table)}")

    targets = [0x1512230, 0x1511C00, 0x1511C80, 0x1511ED0, 0x15151C0, 0x153CF18, 0x151474B]
    for t in targets:
        ids = [i for i, o in table.items() if o == t]
        print(f"RVA 0x{t:X}  ->  IDs {ids}")

    data, secs = load_pe(EXE)

    def rdq(rva):
        off, sec = sec_rva_to_off(secs, rva)
        if off is None:
            return None
        return int.from_bytes(data[off:off + 8], "little")

    def cstr(rva, maxlen=256):
        off, sec = sec_rva_to_off(secs, rva)
        if off is None:
            return None
        end = data.find(b"\x00", off, off + maxlen)
        return data[off:end].decode("ascii", "replace") if end >= 0 else None

    vt = 0x1AC54B0
    prev = rdq(vt - 8)
    print(f"\nvtable candidate .rdata+0x{vt:X}; [vt-8] = 0x{prev:X}" if prev else f"\n[vt-8] read failed at 0x{vt-8:X}")
    # if prev is not a .rdata pointer (looks like RTTI COL -> points into .rdata), step further back
    for back in (8, 16, 24, 32, 40):
        q = rdq(vt - back)
        name, sec = sec_rva_to_off(secs, q - IMG) if q and IMG <= q < IMG + 0x30000000 else (None, None)
        print(f"  [vt-0x{back:X}] = 0x{q:016X}  -> {sec} RVA 0x{q-IMG:X}" if q else f"  [vt-0x{back:X}] = -")
    # try COL parse at [vt-8]
    col = rdq(vt - 8)
    if col and IMG <= col < IMG + 0x30000000:
        off, sec = sec_rva_to_off(secs, col - IMG)
        if sec == ".rdata" or sec == ".data":
            sig = int.from_bytes(data[off:off + 4], "little")
            # x64: fields after signature are image-relative 32-bit RVAs
            f = lambda o: int.from_bytes(data[o:o + 4], "little")
            ptd_rva = f(off + 12)
            # pTypeDescriptor field at offset 12 in COL (sig,offset,cdOffset,pTypeDescriptor,...)
            td = IMG + ptd_rva
            tdoff, tdsec = sec_rva_to_off(secs, ptd_rva)
            print(f"COL@0x{col-IMG:X}: sig=0x{sig:X} pTypeDescriptor=0x{ptd_rva:X} -> {tdsec}")
            if tdoff is not None and tdsec in (".rdata", ".data"):
                name_rva = ptd_rva + 0x10  # TypeDescriptor: pVFTable,spare,name[]
                nm = cstr(name_rva)
                print(f"  TypeDescriptor name @0x{name_rva:X}: {nm}")
    else:
        print("  [vt-8] not a pointer into image -> not a plain vtable slot run")


if __name__ == "__main__":
    main()
