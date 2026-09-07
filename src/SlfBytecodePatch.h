// SlfBytecodePatch.h - SLF-B runtime shadow-unit patch engine (C++).
// Byte-level mirror of the validated Python pipeline (b2_finalize.py):
//   find channel/mask DP4 pair -> locate IF..ENDIF unit -> resolve
//   (counter.reg/comp, tail.reg) -> splice fixed payload (r18..r26) ->
//   bump dcl_temps -> rebuild DXBC container with recomputed hash.
//
// Engine facts (validated over all 1192 shadow-unit variants):
//   * world-position input is ALWAYS v2 (verified 1192/1192)
//   * icb rel reg of channel DP4 == mask DP4 dst == tail reg
//   * float loop counter readable from the FTOU src inside the unit
//   * engine dcl_temps ceiling = 17 -> payload band r18..r26 never collides
//
// Pure C++ (no CommonLibSSE / SKSE / D3D deps) so it can be exercised by an
// offline CLI harness before the game hook uses it.
#pragma once
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include "SlfPayloadLayout.h"
#include "SlfDxbcHash.h"

namespace slf_bc {

// ---- opcode constants (index into the D3D11 opcode-name table, as the
// Python tool enumerates them) ---------------------------------------------
enum {
    OP_ADD = 0,
    OP_DP4 = 17,
    OP_ELSE = 18,
    OP_ENDIF = 21,
    OP_ENDLOOP = 22,
    OP_FTOI = 27,
    OP_FTOU = 28,
    OP_GE = 29,
    OP_IF = 31,
    OP_LT = 49,
    OP_MAD = 50,
    OP_MOV = 54,
    OP_CUSTOMDATA = 53,
    OP_MUL = 56,
    OP_RET = 62,
    OP_RSQ = 68,
    OP_DCL_RESOURCE = 88,
    OP_DCL_CONSTANT_BUFFER = 89,
    OP_DCL_SAMPLER = 90,
    OP_DCL_INPUT = 95,
    OP_DCL_INPUT_PS = 98,
    OP_DCL_OUTPUT = 101,
    OP_DCL_TEMPS = 104
};

// operand types
enum {
    OT_TEMP = 0,
    OT_INPUT = 1,
    OT_CB = 8,
    OT_ICB = 9
};

// why a variant was left unpatched (falls back to vanilla bytecode)
enum RejectCode : uint32_t {
    REJ_NONE = 0,
    REJ_PARSE = 1,     // token stream did not parse
    REJ_NOUNIT = 2,    // no channel+mask DP4 pair found
    REJ_TEMPS = 3,     // declared temps > 17 (payload band r18..r26 would collide)
    REJ_SHAPE = 4      // unit does not match the validated 8-instr template shape
};

struct Instr {
    uint32_t op;
    uint32_t start;
    uint32_t len;
};

inline bool IsDcl(uint32_t op)
{
    // DCL_RESOURCE(88) .. DCL_TEMPS(104), DCL_INDEXABLE_TEMP(105),
    // DCL_GLOBAL_FLAGS(106) -- everything before real code
    return op >= OP_DCL_RESOURCE && op <= 106;
}

inline bool IsInputOutputTemps(uint32_t op)
{
    return op >= OP_DCL_INPUT && op <= OP_DCL_TEMPS;
}

// ---- instruction stream walk ----------------------------------------------
inline std::optional<std::vector<Instr>> ParseInstructions(
    const std::vector<uint32_t>& toks)
{
    std::vector<Instr> out;
    size_t pos = 0, n = toks.size();
    while (pos < n) {
        uint32_t t0 = toks[pos];
        uint32_t op = t0 & 0x7FFu;
        if (op == OP_CUSTOMDATA) {
            if (pos + 1 >= n)
                return std::nullopt;
            uint32_t l = toks[pos + 1];
            if (l < 2 || pos + l > n)
                return std::nullopt;
            out.push_back({op, (uint32_t)pos, l});
            pos += l;
        } else {
            uint32_t l = (t0 >> 24) & 0x7Fu;
            if (l < 1 || pos + l > n)
                return std::nullopt;
            out.push_back({op, (uint32_t)pos, l});
            pos += l;
        }
    }
    return out;
}

// ---- operand model ---------------------------------------------------------
struct Operand {
    uint32_t type = 0;
    std::vector<uint32_t> imms;  // IMMEDIATE32 index values in order
    int sel1Comp = -1;           // SELECT_1 component (0..3) if scalar sel
    uint32_t rawTok = 0;         // operand header token (write-mask etc.)
};

// Return index just past one operand starting at q (header + ext chain + all
// index tokens). q must point at the operand header.
inline size_t SkipOperand(const std::vector<uint32_t>& toks, size_t q)
{
    uint32_t t = toks[q];
    uint32_t dim = (t >> 20) & 3;
    q += 1;
    if ((t >> 31) & 1) {  // extended: chained modifier tokens
        while ((toks[q] >> 31) & 1)
            q += 1;
        q += 1;  // past the chain-tail ext token
    }
    for (uint32_t d = 0; d < dim; d++) {
        uint32_t rep = (t >> (22 + 3 * d)) & 7;
        if (rep == 0)
            q += 1;
        else if (rep == 1)
            q += 2;
        else if (rep == 2)
            q = SkipOperand(toks, q);  // relative: nested operand follows
        else if (rep == 3) {
            q += 1;
            q = SkipOperand(toks, q);
        } else if (rep == 4) {
            q += 2;
            q = SkipOperand(toks, q);
        } else {
            return q;  // malformed guard
        }
    }
    return q;
}

// Fully decode one operand (header + ext + indices) into a summary.
// Returns (Operand, next_q).
inline std::pair<Operand, size_t> DecodeOperand(
    const std::vector<uint32_t>& toks, size_t q)
{
    Operand op;
    uint32_t t = toks[q];
    op.rawTok = t;
    op.type = (t >> 12) & 0xFF;
    uint32_t nc = t & 3;
    uint32_t selmode = (t >> 2) & 3;
    uint32_t dim = (t >> 20) & 3;
    if (nc == 2 && selmode == 2)
        op.sel1Comp = (int)((t >> 4) & 3);
    q += 1;
    if ((t >> 31) & 1) {
        while ((toks[q] >> 31) & 1)
            q += 1;
        q += 1;
    }
    for (uint32_t d = 0; d < dim; d++) {
        uint32_t rep = (t >> (22 + 3 * d)) & 7;
        if (rep == 0) {
            op.imms.push_back(toks[q]);
            q += 1;
        } else if (rep == 1) {
            q += 2;
        } else if (rep == 2) {
            q = SkipOperand(toks, q);
        } else if (rep == 3) {
            q += 1;
            q = SkipOperand(toks, q);
        } else if (rep == 4) {
            q += 2;
            q = SkipOperand(toks, q);
        }
    }
    return {op, q};
}

inline std::vector<Operand> DecodeOperands(
    const std::vector<uint32_t>& toks, const Instr& it)
{
    std::vector<Operand> ops;
    size_t p = (size_t)it.start + 1;
    size_t end = (size_t)it.start + it.len;
    while (p < end) {
        auto [op, nq] = DecodeOperand(toks, p);
        ops.push_back(op);
        p = nq;
    }
    return ops;
}

// First imm of the given operand (register number for temp/input/cb reg 0).
inline std::optional<uint32_t> FirstImm(const Operand& o)
{
    if (o.imms.empty())
        return std::nullopt;
    return o.imms[0];
}

// ---- locator ---------------------------------------------------------------
struct UnitPair {
    size_t channelInstr;  // index into ins
    size_t maskInstr;
    uint32_t maskReg;   // icb rel reg == mask dp4 dst == tail reg
    uint32_t maskWm;    // 4_COMPONENT_MASK of the mask DP4 dst operand
                        // (bits 4-7: X=1,Y=2,Z=4,W=8). Audit over 1249 runtime
                        // variants: 58% W, but 42% write X/Y/Z -> the tail
                        // mov MUST replicate the variant's own lane or the
                        // engine light code multiplies garbage -> black on
                        // backlit surfaces (the 2026-09-04 black root cause).
};

struct UnitSpan {
    size_t startTok;
    size_t endTokExcl;
};

// Channel DP4: dst, cb2[2][2].xyzw, icb[rel+0].xyzw  (channel select)
// Mask DP4:    dst, rM.xyzw, icb[rel+0].xyzw         (t14 mask extract)
inline std::vector<UnitPair> FindUnits(
    const std::vector<uint32_t>& toks, const std::vector<Instr>& ins)
{
    // pass 1: classify every DP4
    std::vector<int> kind(ins.size(), 0);  // 1=channel 2=mask
    std::vector<uint32_t> dstReg(ins.size(), 0);
    for (size_t i = 0; i < ins.size(); i++) {
        if (ins[i].op != OP_DP4)
            continue;
        auto ops = DecodeOperands(toks, ins[i]);
        if (ops.size() < 3)
            continue;
        // operand[0] dst
        if (auto dr = FirstImm(ops[0]))
            dstReg[i] = *dr;
        const Operand& o1 = ops[1];
        const Operand& o2 = ops[2];
        // channel: cb2[2].xyzw (CB with imms {2,2}) + icb
        if (o1.type == OT_CB && o1.imms.size() == 2 && o1.imms[0] == 2 &&
            o1.imms[1] == 2 && o2.type == OT_ICB) {
            kind[i] = 1;
            continue;
        }
        // mask: temp + icb
        if (o1.type == OT_TEMP && o2.type == OT_ICB) {
            kind[i] = 2;  // dstReg already holds the DP4 dst (== tail reg)
        }
    }
    // pass 2: pair channel with the mask DP4 right after it (shared dst,
    // optional FTOU between)
    std::vector<UnitPair> out;
    for (size_t i = 0; i < ins.size(); i++) {
        if (kind[i] != 1)
            continue;
        for (size_t j = i + 1; j < ins.size() && j <= i + 3; j++) {
            if (ins[j].op == OP_FTOU)
                continue;
            if (kind[j] == 2 && dstReg[i] == dstReg[j]) {
                uint32_t wm = 0x8;  // default W
                auto ops = DecodeOperands(toks, ins[j]);
                if (!ops.empty())
                    wm = (ops[0].rawTok >> 4) & 0xF;
                out.push_back({i, j, dstReg[j], wm});
                break;
            }
            break;
        }
    }
    return out;
}

// Walk back from channel to the IF that opens the unit, forward from mask to
// the ENDIF that closes it (both within 8 instructions).
inline std::optional<UnitSpan> LocateUnit(
    const std::vector<Instr>& ins, const UnitPair& u)
{
    std::optional<size_t> ifIdx;
    size_t s = u.channelInstr;
    for (size_t i = (s > 8 ? s - 8 : 0); i < s; i++) {
        if (ins[i].op == OP_IF)
            ifIdx = i;
    }
    if (!ifIdx)
        return std::nullopt;
    std::optional<size_t> endIdx;
    size_t e = u.maskInstr;
    size_t lim = ins.size();
    for (size_t i = e + 1; i < lim && i <= e + 8; i++) {
        if (ins[i].op == OP_ENDIF) {
            endIdx = i;
            break;
        }
    }
    if (!endIdx)
        return std::nullopt;
    return UnitSpan{ins[*ifIdx].start, ins[*endIdx].start + ins[*endIdx].len};
}

// Counter (reg, comp): first FTOU/FTOI inside the unit, src operand.
// Engine: ftou rTail, rCounter.<comp>  -- counter feeds the icb index.
inline std::optional<std::pair<uint32_t, int>> FindCounter(
    const std::vector<uint32_t>& toks, const std::vector<Instr>& ins,
    size_t startTok, size_t endTokExcl)
{
    for (const auto& it : ins) {
        if (it.start < startTok || it.start >= endTokExcl)
            continue;
        if (it.op != OP_FTOU && it.op != OP_FTOI)
            continue;
        auto ops = DecodeOperands(toks, it);
        if (ops.size() < 2)
            continue;
        const Operand& src = ops[1];
        if (src.type != OT_TEMP)
            continue;
        auto reg = FirstImm(src);
        if (!reg)
            continue;
        int comp = src.sel1Comp >= 0 ? src.sel1Comp : 0;
        return std::pair<uint32_t, int>{*reg, comp};
    }
    return std::nullopt;
}

// ---- splice ----------------------------------------------------------------
// Verified shadow-consumption template shape (as validated over the offline
// corpus, see b2 templates): an LT immediately before an IF..ENDIF window
// containing exactly [IF, ftou, dp4(cb+icb), ftou, dp4(temp+icb), ELSE,
// mov, ENDIF], where the ELSE's mov writes the mask dp4 dst (the tail reg).
// Register numbers are free; only opcode order + operand roles are checked.
// Runtime variants are compiled per material-feature combo and were NOT all
// present in the offline corpus, so anything off-template is rejected and
// left vanilla rather than risk a broken splice.
inline std::optional<uint32_t> DeclaredTemps(const std::vector<uint32_t>& toks)
{
    for (size_t i = 0; i + 1 < toks.size(); i++) {
        uint32_t op = toks[i] & 0x7FFu;
        if (op == OP_CUSTOMDATA) {
            // CUSTOMDATA = header + length token + data; its data words look
            // like instructions, so skip the whole block
            uint32_t l = toks[i + 1];
            if (l < 2 || i + l > toks.size())
                return std::nullopt;
            i += l - 1;  // loop ++ lands just past the block
            continue;
        }
        if (op == OP_DCL_TEMPS)
            return toks[i + 1];
        if (!IsDcl(op))
            break;  // reached code
        // dcl instructions are multi-token (e.g. DCL_CB len=4): skip by the
        // header's length field or we mis-read value tokens as opcodes
        uint32_t l = (toks[i] >> 24) & 0x7Fu;
        if (l < 1 || i + l > toks.size())
            return std::nullopt;
        i += l - 1;  // loop ++ lands just past this dcl
    }
    return std::nullopt;
}

inline bool VerifyUnitShape(const std::vector<Instr>& ins,
    const std::vector<uint32_t>& toks, size_t ifInstr, size_t st, size_t et,
    uint32_t maskReg)
{
    // instruction right before the IF must be the LT light test
    if (ifInstr == 0 || ins[ifInstr - 1].op != OP_LT)
        return false;
    std::vector<uint32_t> ops;
    for (const auto& it : ins) {
        if (it.start < st || it.start >= et)
            continue;
        ops.push_back(it.op);
    }
    static const uint32_t kSeq[] = {OP_IF, OP_FTOU, OP_DP4, OP_FTOU,
        OP_DP4, OP_ELSE, OP_MOV, OP_ENDIF};
    if (ops.size() != sizeof(kSeq) / sizeof(kSeq[0]))
        return false;
    for (size_t i = 0; i < ops.size(); i++)
        if (ops[i] != kSeq[i])
            return false;
    // the ELSE's mov must write the mask dp4 dst register (the tail reg)
    for (const auto& it : ins) {
        if (it.start < st || it.start >= et)
            continue;
        if (it.op != OP_MOV)
            continue;
        auto os = DecodeOperands(toks, it);
        if (os.size() < 1)
            return false;
        auto dr = FirstImm(os[0]);
        if (!dr || *dr != maskReg)
            return false;
    }
    return true;
}

// Apply the fixed payload (from SlfPayloadLayout.h) into a copy of the code
// token stream. On success returns the full new token stream.
inline std::optional<std::vector<uint32_t>> PatchTokens(
    const std::vector<uint32_t>& toks, uint32_t* rejectCode = nullptr)
{
    if (rejectCode)
        *rejectCode = REJ_NONE;
    auto insOpt = ParseInstructions(toks);
    if (!insOpt) {
        if (rejectCode)
            *rejectCode = REJ_PARSE;
        return std::nullopt;
    }
    const auto& ins = *insOpt;

    auto units = FindUnits(toks, ins);
    if (units.empty()) {
        if (rejectCode)
            *rejectCode = REJ_NOUNIT;
        return std::nullopt;
    }
    const UnitPair& u = units[0];

    auto span = LocateUnit(ins, u);
    if (!span) {
        if (rejectCode)
            *rejectCode = REJ_SHAPE;
        return std::nullopt;
    }
    size_t st = span->startTok;
    size_t et = span->endTokExcl;

    // ---- shape guard: only the validated 8-instr template may be patched
    size_t ifInstr = 0;
    for (size_t i = 0; i < ins.size(); i++) {
        if (ins[i].start == st) {
            ifInstr = i;
            break;
        }
    }
    if (!VerifyUnitShape(ins, toks, ifInstr, st, et, u.maskReg)) {
        if (rejectCode)
            *rejectCode = REJ_SHAPE;
        return std::nullopt;
    }

    // ---- dcl_temps guard: payload band r18..r26 must not collide with the
    // shader's own temporaries (engine ceiling is 17 over the offline corpus;
    // runtime feature-combo variants can declare more)
    auto dt = DeclaredTemps(toks);
    if (!dt || *dt > 17) {
        if (rejectCode)
            *rejectCode = REJ_TEMPS;
        return std::nullopt;
    }

    auto counter = FindCounter(toks, ins, st, et);
    if (!counter) {
        if (rejectCode)
            *rejectCode = REJ_SHAPE;
        return std::nullopt;
    }
    uint32_t cntReg = counter->first;
    int cntComp = counter->second;

    // ---- fixed payload + the 3 runtime rewrites ----
    std::vector<uint32_t> pl(slf_b::kPayload, slf_b::kPayload + slf_b::kPayloadCount);

    // (1) ftoi src reg: sentinel -> engine counter reg
    pl[slf_b::kFtoiSrcImmTok] = cntReg;
    // (1b) ftoi src swizzle: sentinel zzzz -> counter comp replicated
    uint32_t swz = (uint32_t)(cntComp) | ((uint32_t)(cntComp) << 2) |
                   ((uint32_t)(cntComp) << 4) | ((uint32_t)(cntComp) << 6);
    pl[slf_b::kFtoiSrcHdrTok] =
        (pl[slf_b::kFtoiSrcHdrTok] & ~(0xFFu << 4)) | (swz << 4);
    // (2) tail mov dst reg: sentinel -> engine mask dst (tail) reg
    pl[slf_b::kMovDstImmTok] = u.maskReg;
    // (2b) tail mov dst WRITE MASK: sentinel .w -> the variant's own mask
    // DP4 dst lane. Hardcoding .w broke 42% of variants (mask DP4 writes
    // X/Y/Z there) -> engine light code read an untouched lane -> black.
    pl[slf_b::kMovDstHdrTok] = (pl[slf_b::kMovDstHdrTok] & ~0xF0u) |
                               ((u.maskWm & 0xFu) << 4);

    // ---- find dcl splice point ----
    // walk instr list; the splice goes after the last resource/sampler dcl
    // (and any CUSTOMDATA) but before the first input/output/temps dcl.
    size_t dclSplice = 0;
    bool haveBody = false;
    size_t bodyStart = toks.size();
    for (const auto& it : ins) {
        if (IsInputOutputTemps(it.op)) {
            dclSplice = it.start;
            haveBody = true;
            break;
        }
        if (it.op == OP_CUSTOMDATA)
            continue;
        dclSplice = it.start + it.len;  // advance past dcl
    }
    if (!haveBody)
        return std::nullopt;  // no input/temps dcl found
    for (const auto& it : ins) {
        if (!IsDcl(it.op) && it.op != OP_CUSTOMDATA) {
            bodyStart = it.start;
            break;
        }
    }

    std::vector<uint32_t> ntok;
    ntok.reserve(toks.size() + slf_b::kDclCount + pl.size());
    if (bodyStart < dclSplice || st < bodyStart || et > toks.size())
        return std::nullopt;
    ntok.insert(ntok.end(), toks.begin(), toks.begin() + dclSplice);
    ntok.insert(ntok.end(), slf_b::kDcl, slf_b::kDcl + slf_b::kDclCount);
    ntok.insert(ntok.end(), toks.begin() + dclSplice, toks.begin() + bodyStart);
    ntok.insert(ntok.end(), toks.begin() + bodyStart, toks.begin() + st);
    ntok.insert(ntok.end(), pl.begin(), pl.end());
    ntok.insert(ntok.end(), toks.begin() + et, toks.end());

    // bump dcl_temps to 27
    bool fixed = false;
    for (size_t i = 0; i < ntok.size(); i++) {
        if ((ntok[i] & 0x7FFu) == OP_DCL_TEMPS && ((ntok[i] >> 24) & 0x7F) == 2) {
            ntok[i + 1] = slf_b::kNewTemps;
            fixed = true;
            break;
        }
    }
    if (!fixed)
        return std::nullopt;
    return ntok;
}

// ---- DXBC container ---------------------------------------------------------
struct Chunk {
    size_t off;
    uint32_t tag;
    uint32_t size;  // payload length counted from chunk+8 (incl version+LenTok)
};

inline std::optional<Chunk> FindShaderChunk(const std::vector<uint8_t>& data)
{
    if (data.size() < 0x24)
        return std::nullopt;
    uint32_t n;
    std::memcpy(&n, data.data() + 0x1C, 4);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t off;
        std::memcpy(&off, data.data() + 0x20 + i * 4, 4);
        if (off + 8 > data.size())
            return std::nullopt;
        uint32_t tag;
        std::memcpy(&tag, data.data() + off, 4);
        uint32_t sz;
        std::memcpy(&sz, data.data() + off + 4, 4);
        if (tag == 0x52444853 /*SHDR*/ || tag == 0x58454853 /*SHEX*/)
            return Chunk{off, tag, sz};
    }
    return std::nullopt;
}

// Patch a full DXBC container. Returns the new container bytes or nullopt.
inline std::optional<std::vector<uint8_t>> PatchContainer(
    const std::vector<uint8_t>& data, uint32_t* rejectCode = nullptr)
{
    auto ch = FindShaderChunk(data);
    if (!ch) {
        if (rejectCode)
            *rejectCode = REJ_NOUNIT;
        return std::nullopt;
    }
    // token stream = [version(4) LenTok(4) code...]  (size counts from +8)
    size_t base = ch->off + 8;
    if (base + ch->size > data.size() || ch->size < 8)
        return std::nullopt;
    std::vector<uint32_t> toks((ch->size - 8) / 4);
    for (size_t i = 0; i < toks.size(); i++) {
        uint32_t v;
        std::memcpy(&v, data.data() + base + 8 + i * 4, 4);
        toks[i] = v;
    }
    // defensive: verify parse; if the first dwords are not code (rare
    // version-skew), skip ahead like the Python loader does
    auto check = ParseInstructions(toks);
    if (!check) {
        // try dropping up to 2 leading dwords (version/LenTok redundancy)
        for (size_t skip = 1; skip <= 2 && skip < toks.size(); skip++) {
            std::vector<uint32_t> sub(toks.begin() + skip, toks.end());
            if (ParseInstructions(sub))
                toks = std::move(sub);
        }
    }

    auto newToks = PatchTokens(toks, rejectCode);
    if (!newToks)
        return std::nullopt;
    const std::vector<uint32_t>& nt = *newToks;

    // ---- rebuild the container ----
    // chunk layout: off: tag(4)+size(4) ; off+8: version(4) ; off+12:
    // LenTok(4) ; off+16: code tokens.  chunk size field counts from off+8
    // (version..code end): newLen = 8 + 4*nt.size().
    size_t sz = ch->size;
    size_t codeStart = ch->off + 16;         // first code token position
    size_t trailSrc = ch->off + 8 + sz;      // first byte after orig chunk
    size_t newLen = 8 + nt.size() * 4;
    size_t finalSize = data.size() + (newLen - sz);

    std::vector<uint8_t> out;
    out.reserve(finalSize);
    // 1) everything up to the original code start (header+version+LenTok)
    out.insert(out.end(), data.begin(),
               data.begin() + std::min(codeStart, data.size()));
    // 2) the new code tokens
    for (uint32_t v : nt) {
        out.push_back(static_cast<uint8_t>(v));
        out.push_back(static_cast<uint8_t>(v >> 8));
        out.push_back(static_cast<uint8_t>(v >> 16));
        out.push_back(static_cast<uint8_t>(v >> 24));
    }
    // 3) everything after the original code chunk
    if (trailSrc < data.size())
        out.insert(out.end(), data.begin() + trailSrc, data.end());
    if (out.size() != finalSize)
        return std::nullopt;  // sanity

    // 4) chunk size field + LenTok
    uint32_t newLen32 = (uint32_t)newLen;
    uint32_t lenTok = (uint32_t)(nt.size() + 2);  // version+LenTok+code dwords
    std::memcpy(out.data() + ch->off + 4, &newLen32, 4);
    std::memcpy(out.data() + ch->off + 12, &lenTok, 4);
    // 5) fix other chunk offsets in the table
    uint32_t n;
    std::memcpy(&n, out.data() + 0x1C, 4);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t off;
        std::memcpy(&off, out.data() + 0x20 + i * 4, 4);
        if (off > ch->off) {
            uint32_t nv = (uint32_t)(off + (newLen - sz));
            std::memcpy(out.data() + 0x20 + i * 4, &nv, 4);
        }
    }
    // 6) container size @0x18 and recompute hash @[4,20)
    uint32_t csize = (uint32_t)out.size();
    std::memcpy(out.data() + 0x18, &csize, 4);
    slf_dxbc_hash::SetDxbcHash(out);
    return out;
}

}  // namespace slf_bc
