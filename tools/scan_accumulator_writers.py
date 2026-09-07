# String-based scan for Accumulate callers / 0xF0 strides / +0x48 stores
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

EXE = r"D:/ENB-EJ/Skyrim Special Edition/SkyrimSE.exe"
ACC_ABS = 0x140000000 + 0x1511C80


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

    calls_acc, imuls, st48 = [], [], []
    for ins in md.disasm(text, vaddr):
        op = ins.op_str
        if ins.mnemonic == "call" and hex(ACC_ABS) in op:
            calls_acc.append(ins.address)
        if ins.mnemonic == "imul" and "0xf0" in op:
            imuls.append((ins.address, op))
        if ins.mnemonic == "mov" and op.endswith(", 0x48") is False and \
           op.count("0x48") >= 1 and "qword ptr [" in op and op.startswith("mov qword ptr [") and "], " in op:
            # store qword [base+0x48], src  (disp=0x48, two operands, mem dst)
            st48.append((ins.address, op))

    print(f"== direct callers of Accumulate: {len(calls_acc)} ==")
    for c in calls_acc:
        print(f"  call @ 0x{c:X}")
    print(f"\n== imul ...,0xF0: {len(imuls)} ==")
    for a, o in imuls[:60]:
        print(f"  0x{a:X}: {o}")
    print(f"\n== store [r+0x48] patterns (matching qword): {len(st48)} ==")
    # dedup rough filter: only those with exactly one 0x48 disp and two operands
    seen = set()
    for a, o in st48:
        if a in seen:
            continue
        seen.add(a)
        if len(seen) > 50:
            break
        print(f"  0x{a:X}: {o}")


if __name__ == "__main__":
    main()
