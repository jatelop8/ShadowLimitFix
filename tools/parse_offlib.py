# Parse Address Library format-2 bin and resolve given IDs to RVAs
import struct, sys

p = r'E:/EJ/mod/mods/Addmodress Library All in One/SKSE/Plugins/versionlib-1-6-1170-0.bin'
d = open(p, 'rb').read()
pos = 0

def rd(fmt):
    global pos
    sz = struct.calcsize(fmt)
    v = struct.unpack_from(fmt, d, pos)
    pos += sz
    return v[0] if len(v) == 1 else v

fmt = rd('<i')
assert fmt == 2, fmt
ver = rd('<4i')
nameLen = rd('<i')
name = d[pos:pos + nameLen].decode('ascii', 'replace')
pos += nameLen
pointerSize = rd('<i')
addressCount = rd('<i')
print(f'version={ver} name={name} ptrSize={pointerSize} count={addressCount}')

table = {}
prevID = 0
prevOff = 0
for k in range(addressCount):
    t = d[pos]; pos += 1
    lo = t & 0xF
    hi = t >> 4
    # id
    if lo == 0:   id_ = rd('<Q')
    elif lo == 1: id_ = prevID + 1
    elif lo == 2: id_ = prevID + d[pos]; pos += 1
    elif lo == 3: id_ = prevID - d[pos]; pos += 1
    elif lo == 4: id_ = prevID + rd('<H')
    elif lo == 5: id_ = prevID - rd('<H')
    elif lo == 6: id_ = rd('<H')
    elif lo == 7: id_ = rd('<I')
    else: raise ValueError(f'bad lo {lo} at entry {k}')
    # offset
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
    prevID = id_
    prevOff = off

print(f'parsed {len(table)} entries, consumed {pos}/{len(d)} bytes')
want = [107137, 107132, 100419, 107133, 108488, 108487,
        100819, 107603,   # BSShadowDirectionalLight::SetupFocusShadowAccumulators
        100817, 107601,   # SetupFocusShadowMaps
        100810, 107594,   # BSShadowLight::ctor
        99708, 106342,    # ShadowSceneNode::EnableLight
        99728, 106365,    # GameSetShadowCasterSlot
        99753, 106401,    # ShadowSceneNode::AccumulateLight
        101302, 108289,   # BSLight::SetLight
        99692, 106326,    # ShadowSceneNode::AddLight
        101296, 108283,   # BSLight::AttachGeometry
        101298, 108285,   # BSLight::ClearGeometryList
        101317, 108304]
for i in want:
    off = table.get(i)
    print(f'ID {i:8d} -> {"0x%X" % off if off is not None else "MISSING"}')
