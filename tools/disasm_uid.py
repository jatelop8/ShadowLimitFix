# Disassemble a SkyrimSE.exe function region given an address-library VA
# (stored offset) or explicit RVA. Resolves the PE section table, dumps
# capstone listing, annotates E8 call targets with absolute VAs.
# Usage: python disasm_uid.py <exe> <va_or_rva> <length> [--rva]
import struct, sys
from capstone import *

def load_pe(path):
    d = open(path, 'rb').read()
    assert d[:2] == b'MZ'
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    assert d[pe:pe+4] == b'PE\0\0'
    coff = pe + 4
    nsec = struct.unpack_from('<H', d, coff+2)[0]
    optsz = struct.unpack_from('<H', d, coff+16)[0]
    opt = coff + 20
    magic = struct.unpack_from('<H', d, opt)[0]
    assert magic == 0x20B, hex(magic)  # PE32+
    imgbase = struct.unpack_from('<Q', d, opt+24)[0]
    sizeimg = struct.unpack_from('<I', d, opt+56)[0]
    sec = opt + optsz
    sections = []
    for i in range(nsec):
        o = sec + i*40
        name = d[o:o+8].rstrip(b'\0').decode('ascii', 'replace')
        vsz, va, rsz, ro = struct.unpack_from('<IIII', d, o+8)
        sections.append((name, va, vsz, ro, rsz))
    return d, imgbase, sizeimg, sections

def va_to_off(d, imgbase, sections, va, sizeimg):
    rva = va - imgbase
    for name, sva, svsz, ro, rsz in sections:
        if sva <= rva < sva + max(svsz, rsz):
            return ro + (rva - sva)
    return None

def main():
    path = sys.argv[1]
    target = int(sys.argv[2], 0)
    ln = int(sys.argv[3], 0)
    is_rva = '--rva' in sys.argv
    d, imgbase, sizeimg, sections = load_pe(path)
    if is_rva:
        va = imgbase + target          # arg is an RVA (image-base relative)
    else:
        va = target
        if not (imgbase <= va < imgbase + sizeimg):
            va = imgbase + target      # stored VA outside image -> treat as RVA
    rva = va - imgbase
    print(f'imagebase=0x{imgbase:X} size=0x{sizeimg:X} sections={[s[0] for s in sections]}')
    off = va_to_off(d, imgbase, sections, va, sizeimg)
    print(f'target VA=0x{va:X} RVA=0x{rva:X} fileoff=0x{off:X} len=0x{ln:X}')
    if off is None:
        print('OUT OF ANY SECTION'); return
    blob = d[off:off+ln]
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    for ins in md.disasm(blob, va):
        line = f'0x{ins.address:X}: {ins.mnemonic:<9s} {ins.op_str}'
        extra = ''
        # annotate call/jmp targets and disp32=0x528 style accesses
        if ins.mnemonic in ('call', 'jmp') and ins.op_str.startswith('0x'):
            tgt = int(ins.op_str, 16)
            toff = va_to_off(d, imgbase, sections, tgt, sizeimg)
            extra = f'   ; ->VA 0x{tgt:X} (file 0x{toff:X})' if toff is not None else f'   ; ->VA 0x{tgt:X}'
        if '+ 0x528' in ins.op_str or '- 0x528' in ins.op_str:
            extra += '   ; *** 0x528 access ***'
        print(line + extra)

if __name__ == '__main__':
    main()
