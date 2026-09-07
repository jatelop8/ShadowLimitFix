import sys
sys.argv=['x']
src=open('disasm_engine.py').read()
src=src.replace("EXE = r\"D:/ENB-EJ/Skyrim Special Edition/SkyrimSE.exe\"", "EXE = r\"E:/ENB-EJ/Skyrim Special Edition/SkyrimSE.exe\"")
exec(src.split('def main():')[0])
def go(rva,length,label):
    data, secs = load_pe(EXE)
    off, sec = rva_to_off(secs, rva)
    if off is None: print(f"[{label}] RVA 0x{rva:X} not in section"); return
    code=data[off:off+length]
    md=Cs(CS_ARCH_X86, CS_MODE_64); md.detail=False
    print(f"=== {label} @ 0x{rva:X} ===")
    for ins in md.disasm(code, 0x140000000+rva):
        print(f"  {ins.address-0x140000000:08X}  {ins.mnemonic:8s} {ins.op_str}")
go(0x14FCF80, 0x140, "orig_CalcActiveNonShadowCasterLights(107784)")
go(0x14FCF80, 0x400, "orig_Calc_full(107784)")
go(0x14DD5B7-0x80, 0x110, "crash_site_107300+0x577")
go(0x14DD040, 0x60, "fn107300_head")
