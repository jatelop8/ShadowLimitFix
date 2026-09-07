#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Scan dumped .fxcb files for the engine shadow unit (a DP4 whose 2nd
operand is cb2[2][2] + an icb 3rd operand == the channel-select DP4), then
disassemble the first few hits to a readable .asm. Pure-token scan, fast.
Usage: scan_dump_units.py [max_disasm]
"""
import os
import sys

DUMP = r"C:/Users/Administrator/Documents/My Games/Skyrim Special Edition/SKSE/shader_dump"
OP_DP4 = 17

def skip_operand(toks, q):
    t = toks[q]
    dim = (t >> 20) & 3
    q += 1
    if (t >> 31) & 1:
        while (toks[q] >> 31) & 1:
            q += 1
        q += 1
    for d in range(dim):
        rep = (t >> (22 + 3 * d)) & 7
        if rep == 0:
            q += 1
        elif rep == 1:
            q += 2
        elif rep == 2:
            q = skip_operand(toks, q)
        elif rep == 3:
            q += 1
            q = skip_operand(toks, q)
        elif rep == 4:
            q += 2
            q = skip_operand(toks, q)
        else:
            return q
    return q

def decode_operand(toks, q):
    """Return (type, imms, next_q)."""
    t = toks[q]
    typ = (t >> 12) & 0xFF
    dim = (t >> 20) & 3
    nc = t & 3
    imms = []
    q += 1
    if (t >> 31) & 1:
        while (toks[q] >> 31) & 1:
            q += 1
        q += 1
    for d in range(dim):
        rep = (t >> (22 + 3 * d)) & 7
        if rep == 0:
            imms.append(toks[q])
            q += 1
        elif rep == 1:
            q += 2
        elif rep == 2:
            q = skip_operand(toks, q)
        elif rep == 3:
            q += 1
            q = skip_operand(toks, q)
        elif rep == 4:
            q += 2
            q = skip_operand(toks, q)
    return typ, imms, nc, q

def has_shadow_unit(toks):
    """channel DP4: op==DP4 and operand[1] is CB with imms {2,2}."""
    i = 0
    n = len(toks)
    while i < n:
        t0 = toks[i]
        op = t0 & 0x7FF
        ln = (t0 >> 24) & 0x7F
        if op == 53:  # CUSTOMDATA: skip whole block
            if i + 1 >= n:
                return False
            l = toks[i + 1]
            i += max(l, 2)
            continue
        if ln < 1 or i + ln > n:
            return False
        if op == OP_DP4:
            try:
                q = i + 1
                o0, q = decode_operand(toks, q)[0], decode_operand(toks, q)[1]
                # decode operands properly
                types = []
                all_imms = []
                q = i + 1
                for _ in range(3):
                    typ, imms, nc, q = decode_operand(toks, q)
                    types.append(typ)
                    all_imms.append(imms)
                if len(types) >= 2 and types[1] == 8 and all_imms[1] == [2, 2]:
                    return True
            except Exception:
                pass
        i += ln
    return False

def parse_code(toks):
    """Extract code token vector from a raw SHEX chunk token list."""
    # toks here = full chunk incl version+LenTok already stripped by caller
    return toks

def main():
    max_disasm = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    hits = []
    import struct
    for name in sorted(os.listdir(DUMP)):
        if not name.lower().endswith('.fxcb'):
            continue
        path = os.path.join(DUMP, name)
        data = open(path, 'rb').read()
        if len(data) < 0x40:
            continue
        n = struct.unpack_from('<I', data, 0x1C)[0]
        chunk_off = None
        for i in range(n):
            off = struct.unpack_from('<I', data, 0x20 + i * 4)[0]
            if off + 8 > len(data):
                continue
            tag = data[off:off + 4]
            if tag in (b'SHEX', b'SHDR'):
                chunk_off = off
                break
        if chunk_off is None:
            continue
        sz = struct.unpack_from('<I', data, chunk_off + 4)[0]
        # chunk body: version(4) LenTok(4) code...
        base = chunk_off + 8
        if base + sz > len(data) or sz < 8:
            continue
        ntok = (sz - 8) // 4
        toks = struct.unpack_from('<%dI' % ntok, data, base + 8)
        toks = list(toks)
        # guard: skip possible leading version/LenTok redundancy
        if has_shadow_unit(toks):
            hits.append((name, path))
    print('shadow-unit hits: %d / %d' % (len(hits), len(os.listdir(DUMP))))
    for name, path in hits[:max_disasm]:
        print(name)
    # write a hits list for the caller
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '_unit_hits.txt'), 'w') as f:
        for name, _ in hits:
            f.write(name + '\n')
    print('hits list -> tools/_unit_hits.txt')

if __name__ == '__main__':
    main()
