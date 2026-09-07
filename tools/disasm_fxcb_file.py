#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Disassemble ONE dumped engine .fxcb (original, unpatchable reference).
Usage: disasm_fxcb_file.py <file.fxcb> [out.txt]
Reuses the ID3DBlob disasm trick from disasm_payload.py.
"""
import ctypes
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import disasm_payload as _dp  # reuses its verified D3DDisassemble wrapper

def disasm_blob(data):
    return _dp.disasm(bytes(data))

def main():
    path = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else None
    data = open(path, 'rb').read()
    text = disasm_blob(data)
    if out:
        open(out, 'w', encoding='utf-8').write(text)
        print('wrote', out, len(text), 'bytes')
    else:
        print(text)

if __name__ == '__main__':
    main()
