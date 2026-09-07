# Disassemble a DXBC .fxcb/.fxc bytecode blob with D3DDisassemble.
# Usage: python disasm_fxcb.py <file.fxcb> [out.asm]
import ctypes, sys, os

def disasm(data):
    d3d = ctypes.WinDLL('d3dcompiler_47.dll')
    D3DDisassemble = d3d.D3DDisassemble
    D3DDisassemble.restype = ctypes.HRESULT
    D3DDisassemble.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint,
                               ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
    p = ctypes.c_void_p()
    hr = D3DDisassemble(data, len(data), 0, None, ctypes.byref(p))
    if hr != 0 or not p.value:
        raise RuntimeError('D3DDisassemble failed hr=%#x' % hr)
    try:
        s = ctypes.string_at(p.value)
        return s.decode('ascii', 'replace')
    finally:
        ctypes.windll.kernel32.FreeLibrary(ctypes.c_void_p(d3d._handle))

def main():
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else src + '.asm'
    data = open(src, 'rb').read()
    text = disasm(data)
    with open(out, 'w', encoding='ascii', errors='replace') as f:
        f.write(text)
    print('wrote %s (%d bytes asm)' % (out, len(text)))

if __name__ == '__main__':
    main()
