#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""SLF dead-code removal: strip 判死路线 blocks (P1C3 / SLF_B2B_ENABLED /
SLF_PS_ENABLED / SLF_POSTLIGHT_ENABLED). Run from repo root.
git baseline already holds the original - no backup needed.
"""
import io

def read(fn):
    with io.open(fn, 'r', encoding='utf-8') as f:
        return f.readlines()

def write(fn, lines):
    with io.open(fn, 'w', encoding='utf-8', newline='\n') as f:
        f.writelines(lines)

def squash_blank_runs(lines, maxb=2):
    out, blanks = [], 0
    for l in lines:
        if l.strip() == '':
            blanks += 1
            if blanks > maxb:
                continue
        else:
            blanks = 0
        out.append(l)
    return out

def remove_ranges(fn, ranges, verify=None):
    lines = read(fn)
    if verify:
        for ln, expect in verify:
            assert expect in lines[ln - 1], f'{fn}:{ln} expected {expect!r} got {lines[ln-1]!r}'
    drop = set()
    for s, e in ranges:
        for i in range(s, e + 1):
            drop.add(i)
    final = [l for i, l in enumerate(lines, 1) if i not in drop]
    final = squash_blank_runs(final)
    write(fn, final)
    print(f'{fn}: {len(lines)} -> {len(final)} lines (-{len(lines) - len(final)})')
    return final

# ---------------- ShaderReplace.cpp ----------------
# B2B #if/#else/#endif: drop dead branch + guards, KEEP the #else live body.
#   Block1 367 #if /372 #else/(373-375 void-casts live)/376 #endif
#   Block2 405 #if /407 #else/(408 live log)/409 #endif
remove_ranges('src/ShaderReplace.cpp', [
    (367, 372),   # B2B #1 dead branch incl #else guard
    (376, 376),   # B2B #1 #endif
    (405, 407),   # B2B #2 dead branch incl #else guard
    (409, 409),   # B2B #2 #endif
    (1325, 1329), # P1C3_ENABLED small (ReplaceLightingShaders call)
    (1565, 1831), # SLF_PS_ENABLED big block
    (1833, 2532), # SLF_POSTLIGHT_ENABLED big block
    (2546, 2555), # SLF_PS_ENABLED (b13/t103 rebind)
    (2586, 2598), # SLF_PS_ENABLED (lazy CompileSLFPS)
    (2611, 2633), # SLF_POSTLIGHT_ENABLED (post trigger)
    (2672, 2674), # SLF_PS_ENABLED (arm clear)
    (2717, 2796), # P1C3_ENABLED (PS swap)
], verify=[
    (367, '#if SLF_B2B_ENABLED'),
    (372, '#else'),
    (376, '#endif'),
    (405, '#if SLF_B2B_ENABLED'),
    (407, '#else'),
    (409, '#endif'),
    (1565, '#if SLF_PS_ENABLED'),
    (1833, '#if SLF_POSTLIGHT_ENABLED'),
    (2717, '#if P1C3_ENABLED'),
])

# ---------------- Scheduler.cpp ----------------
remove_ranges('src/Scheduler.cpp', [
    (1869, 1940), # SLF_POSTLIGHT_ENABLED block
    (1990, 1993), # SLF_POSTLIGHT_ENABLED small
], verify=[
    (1869, '#if SLF_POSTLIGHT_ENABLED'),
    (1990, '#if SLF_POSTLIGHT_ENABLED'),
])
print('DONE')
