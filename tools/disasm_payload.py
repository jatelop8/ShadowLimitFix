#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Disassemble the SLF-B payload tokens by splicing them into a real dumped
PS .fxcb (SHEX is the last chunk here -> no cross-chunk offset fixups).
Layout follows PatchContainer's model:
  chunk: tag(4) size(4) | version(4) LenTok(4) | code tokens ...
  size field counts from the version dword; LenTok = code_dwords + 2.
"""
import ctypes
import re
import sys

def parse_header_tokens(path):
    text = open(path, 'r', encoding='utf-8', errors='replace').read()
    blocks = {}
    for name in ('kDcl', 'kPayload'):
        m = re.search(r'constexpr std::uint32_t %s\[\] = \{(.*?)\};' % name, text, re.S)
        if not m:
            raise RuntimeError('block %s not found' % name)
        toks = [int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]+', m.group(1))]
        blocks[name] = toks
    return blocks['kDcl'], blocks['kPayload']

def splice_shex(blob, new_tokens):
    """Replace SHEX code tokens (must be the trailing chunk)."""
    n = int.from_bytes(blob[0x1C:0x20], 'little')
    offsets = [int.from_bytes(blob[0x20 + i * 4:0x24 + i * 4], 'little') for i in range(n)]
    shex_off = None
    for i, off in enumerate(offsets):
        tag = blob[off:off + 4]
        if tag in (b'SHEX', b'SHDR'):
            shex_off = off
    if shex_off is None:
        raise RuntimeError('no SHEX/SHDR chunk')
    version = blob[shex_off + 8:shex_off + 12]      # first body dword
    for off in offsets:
        if off > shex_off:
            raise RuntimeError('SHEX not trailing - cannot splice in place')
    code = b''.join(t.to_bytes(4, 'little') for t in new_tokens)
    len_tok = (len(new_tokens) + 2).to_bytes(4, 'little')
    size_field = (8 + len(new_tokens) * 4).to_bytes(4, 'little')
    out = bytearray(blob[:shex_off])
    out += blob[shex_off:shex_off + 4]               # tag
    out += size_field
    out += version
    out += len_tok
    out += code
    out[0x18:0x1C] = len(out).to_bytes(4, 'little')  # total container size
    h = md5_obf(bytes(out[0x14:]))
    out[4:20] = h
    return bytes(out)

_S = [7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
      5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
      4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
      6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21]
_K = [0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
      0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
      0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
      0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
      0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
      0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
      0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
      0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
      0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
      0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
      0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391]

def _rotl(x, c):
    return ((x << c) | (x >> (32 - c))) & 0xFFFFFFFF

def md5_obf(msg):
    orig_len = len(msg)
    orig_bits = (orig_len * 8) & 0xFFFFFFFF
    m = bytearray(msg)
    m.append(0x80)
    pad = 64 - (len(m) % 64)
    if pad < 8:
        m.extend([0] * (64 + pad - 8))
    else:
        m.extend([0] * (pad - 8))
    base = len(m) - 56
    m[base:base] = orig_bits.to_bytes(4, 'little')
    tail = ((orig_len << 1) | 1) & 0xFFFFFFFF
    m += tail.to_bytes(4, 'little')
    a0, b0, c0, d0 = 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476
    for pos in range(0, len(m), 64):
        M = [int.from_bytes(m[pos + i * 4:pos + i * 4 + 4], 'little') for i in range(16)]
        A, B, C, D = a0, b0, c0, d0
        for i in range(64):
            if i < 16:
                F = (B & C) | (~B & D)
                g = i
            elif i < 32:
                F = (D & B) | (~D & C)
                g = (5 * i + 1) % 16
            elif i < 48:
                F = B ^ C ^ D
                g = (3 * i + 5) % 16
            else:
                F = C ^ (B | ~D)
                g = (7 * i) % 16
            t = (A + F + _K[i] + M[g]) & 0xFFFFFFFF
            A = D
            D = C
            C = B
            B = (B + _rotl(t, _S[i])) & 0xFFFFFFFF
        a0 = (a0 + A) & 0xFFFFFFFF
        b0 = (b0 + B) & 0xFFFFFFFF
        c0 = (c0 + C) & 0xFFFFFFFF
        d0 = (d0 + D) & 0xFFFFFFFF
    return b''.join(x.to_bytes(4, 'little') for x in (a0, b0, c0, d0))

def disasm(blob):
    d3d = ctypes.WinDLL('d3dcompiler_47.dll')
    f = d3d.D3DDisassemble
    f.restype = ctypes.HRESULT
    f.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint,
                  ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
    p = ctypes.c_void_p()
    hr = f(blob, len(blob), 0, None, ctypes.byref(p))
    if hr != 0:
        return 'hr=%#x' % (hr & 0xFFFFFFFF)
    vtbl_addr = ctypes.cast(p.value, ctypes.POINTER(ctypes.c_size_t)).contents.value
    vtbl = ctypes.cast(vtbl_addr, ctypes.POINTER(ctypes.c_void_p))
    GP = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vtbl[3])
    GS = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vtbl[4])
    buf = GP(p.value)
    sz = GS(p.value)
    return ctypes.string_at(buf, sz).decode('ascii', 'replace')

def main():
    header = r'D:/Modding/ShadowLimitFix/src/SlfPayloadLayout.h'
    shell = r'C:/Users/Administrator/Documents/My Games/Skyrim Special Edition/SKSE/shader_dump/PS000000029AE92AB0.fxcb'
    dcl, payload = parse_header_tokens(header)
    blob = open(shell, 'rb').read()
    patched = splice_shex(blob, dcl + payload)
    text = disasm(patched)
    if text.startswith('hr='):
        print('Disassemble failed:', text)
        sys.exit(1)
    print(text)

if __name__ == '__main__':
    main()
