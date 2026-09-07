#!/usr/bin/env python3
"""fix17 log analysis: decide "rendered but cleared" vs "never rendered".

Usage: python analyze_fix17_log.py <ShadowLimitFix.log>

Outputs the decisive rows for the next step:
  1. RL readback lines  (right after our dispatch: does depth exist in t103 tex?)
  2. MAT-pass readback  (at the material t14-bind moment)
  3. engine accum roster dump  (engine accumulator contents at MAT time)
  4. v6.5 scheduler per-light diag (cam/acc/cull/sceneAccum/frustum -> which lights lit)
  5. dispatch summary / render loop counters / data channel health
"""
import re
import sys
from collections import Counter

path = sys.argv[1] if len(sys.argv) > 1 else "ShadowLimitFix.log"

rl = []        # (n_zeros_over32, raw)  raw is the "0=..% 1=..% ..." tail
mat = []
accum_roster = []
sched_diag = []
post_render = []
data_ch = []
disp_avg = []
render_loop = []
census = []    # fix17b per-light camera census rows

with open(path, "r", encoding="utf-8", errors="replace") as f:
    for ln in f:
        # readback lines: "RL shadow content (t103 tex): 0=..% ..." and
        # "MAT-pass shadow content (t103 tex): ..."
        m = re.search(r"\[SLF\] (RL|MAT-pass) shadow content \(t103 tex\): (.*)", ln)
        if m:
            kind, tail = m.group(1), m.group(2)
            pcts = re.findall(r"(\d+)=(\d+)%", tail)
            nz = sum(1 for _, p in pcts if int(p) > 0)
            if kind == "RL":
                rl.append((nz, tail))
            else:
                mat.append((nz, tail))
            continue
        m = re.search(r"\[SLF\] RL shadow content \(SRV tex\): (.*)", ln)
        if m:
            rl.append((-1, "SRVtex: " + m.group(1)))  # legacy line, keep visible
            continue
        m = re.search(r"\[SLF\]\[MP\] engine accum\[(\d+)\]: (.*)", ln)
        if m:
            accum_roster.append((int(m.group(1)), m.group(2)))
            continue
        m = re.search(r"\[SLF\]\s+light#(\d+) slot=(\d+) (\[(ENG|SLF)\])", ln)
        if m:
            sched_diag.append(ln.strip())
            continue
        m = re.search(r"\[SLF\]\s+post-render light#0", ln)
        if m:
            post_render.append(ln.strip())
            continue
        if "data channel:" in ln:
            data_ch.append(ln.strip())
            continue
        if "dispatch avg" in ln:
            disp_avg.append(ln.strip())
            continue
        if "render loop:" in ln:
            render_loop.append(ln.strip())
            continue
        if "[SLF][CN]" in ln:
            census.append(ln.strip())
            continue

def show(title, rows, limit=14):
    print(f"\n===== {title}  ({len(rows)} rows) =====")
    for r in rows[:limit]:
        print("  ", r)
    if len(rows) > limit:
        print(f"   ... +{len(rows) - limit} more")

show("RL readback (right after dispatch)", rl)
show("MAT-pass readback (t14-bind moment)", mat)
show("engine accum roster at MAT", accum_roster)
show("v6.5 per-light scheduler diag", sched_diag)
show("per-light camera census [CN]", census, limit=18)
show("post-render camera (light#0)", post_render)
show("SLF data channel", data_ch)
show("dispatch avg", disp_avg)
show("render loop counters", render_loop)

# ---- verdict helpers ----
def pct_of(rows):
    tot = nz = 0
    for z, tail in rows:
        for m in re.finditer(r"(\d+)=(\d+)%", tail):
            tot += 1
            if int(m.group(2)) > 0:
                nz += 1
    return (nz, tot)

if rl or mat:
    rl_nz, rl_tot = pct_of(rl) if rl else (0, 0)
    mat_nz, mat_tot = pct_of(mat) if mat else (0, 0)
    print("\n===== VERDICT =====")
    print(f"RL nonzero slices  : {rl_nz}/{rl_tot}  (depth present right after dispatch)")
    print(f"MAT nonzero slices : {mat_nz}/{mat_tot}  (depth present at material sample time)")
    if rl_tot and mat_tot:
        if rl_nz == 0 and mat_nz == 0:
            print(">>> NEVER RENDERED: dispatch writes 0 px (camera/frustum/culling) - "
                  "check v6.5 diag sceneAccum + post-render frustum; static lights need")
            print("    camera placement (engine gate B skips dynamic=0 lights).")
        elif rl_nz > 0 and mat_nz == 0:
            print(">>> RENDERED BUT CLEARED/OVERWRITTEN before the material pass - "
                  "shadow-pass clear ordering or wrong-array overwrite is the target.")
        elif rl_nz > 0 and mat_nz > 0:
            print(">>> DEPTH IS LIVE AT MATERIAL TIME - remaining issue is material-side")
            print("    consumption (SLF-C PS) / t14-mask 4-channel cap / channel mapping.")
        else:
            print(">>> odd mix; inspect raw rows.")
    if accum_roster:
        counts = Counter(r[1] for r in accum_roster)
        print("\nengine accum roster signature (top 5):")
        for sig, c in counts.most_common(5):
            print(f"   x{c}: {sig}")

if census:
    # Rows come in batches (one dump = consecutive [CN] rows). Summarize the
    # LAST batch = current scene's per-light camera state.
    batches = []
    cur = []
    for ln in census:
        # a new batch starts at a row containing "#0 slot=" or "#0 slot"
        if "[SLF][CN] #0 " in ln and cur:
            batches.append(cur)
            cur = []
        cur.append(ln)
    if cur:
        batches.append(cur)
    if batches:
        b = batches[-1]
        print(f"\n===== CAMERA CENSUS (last dump, {len(b)} lights) =====")
        for ln in b:
            print("  ", ln)
        n_eng = sum(1 for ln in b if "[ENG]" in ln)
        n_slf = sum(1 for ln in b if "[SLF]" in ln)
        n_def = sum(1 for ln in b if "DEF=1" in ln)
        n_nodef = sum(1 for ln in b if ("DEF=0" in ln or "NO-CAM" in ln))
        n_acc0 = sum(1 for ln in b if "acc=0 " in ln)
        print(f"  -> ENG-path: {n_eng} | SLF-path: {n_slf} | default-box cam: {n_def} | placed/no-cam: {n_nodef} | acc=0 (no casters): {n_acc0}")
        print("  -> default-box count = lights that CANNOT produce depth this frame.")
