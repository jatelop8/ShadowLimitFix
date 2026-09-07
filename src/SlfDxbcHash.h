// SlfDxbcHash.h - DXBC container hash (d3dcompiler obfuscated MD5).
// Mirrors Python dxbc_hash.py (validated 2781/2781 engine dumps).
// The digest stored at container bytes [4,20) equals this modified MD5 over
// container bytes from 0x14 to end. Differences vs real MD5:
//   - the 32-bit bit-length is inserted at offset 56 of the final 512-bit
//     block (not appended as a 64-bit length at the end);
//   - a final 32-bit value (orig_len_bytes << 1) | 1 is appended.
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace slf_dxbc_hash {

inline uint32_t Rotl(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

inline void Md5Obf(const uint8_t* msg, size_t len, uint8_t out[16])
{
    static const int S[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
        0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
        0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
        0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
        0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

    size_t origLen = len;
    uint32_t origBits = static_cast<uint32_t>((origLen * 8) & 0xFFFFFFFFu);
    std::vector<uint8_t> m(msg, msg + len);
    m.push_back(0x80);
    size_t pad = 64 - (m.size() % 64);
    if (pad < 8)
        m.insert(m.end(), 64 + pad - 8, 0);
    else
        m.insert(m.end(), pad - 8, 0);
    // obfuscated tail: bit length INSERTED at -56 (message grows +4), then
    // a final dword (bytes<<1)|1 is appended.
    size_t base = m.size() - 56;
    std::vector<uint8_t> ins = {
        static_cast<uint8_t>(origBits), static_cast<uint8_t>(origBits >> 8),
        static_cast<uint8_t>(origBits >> 16),
        static_cast<uint8_t>(origBits >> 24)};
    m.insert(m.begin() + base, ins.begin(), ins.end());
    uint32_t tail = static_cast<uint32_t>((origLen << 1) | 1);
    m.push_back(static_cast<uint8_t>(tail));
    m.push_back(static_cast<uint8_t>(tail >> 8));
    m.push_back(static_cast<uint8_t>(tail >> 16));
    m.push_back(static_cast<uint8_t>(tail >> 24));

    uint32_t a0 = 0x67452301, b0 = 0xEFCDAB89, c0 = 0x98BADCFE, d0 = 0x10325476;
    for (size_t pos = 0; pos < m.size(); pos += 64) {
        uint32_t M[16];
        for (int i = 0; i < 16; i++) {
            size_t o = pos + i * 4;
            M[i] = m[o] | (m[o + 1] << 8) | (m[o + 2] << 16) |
                   (static_cast<uint32_t>(m[o + 3]) << 24);
        }
        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; i++) {
            uint32_t F;
            int g;
            if (i < 16) {
                F = (B & C) | (~B & D);
                g = i;
            } else if (i < 32) {
                F = (D & B) | (~D & C);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                F = B ^ C ^ D;
                g = (3 * i + 5) % 16;
            } else {
                F = C ^ (B | ~D);
                g = (7 * i) % 16;
            }
            uint32_t t = (A + F + K[i] + M[g]) & 0xFFFFFFFFu;
            A = D;
            D = C;
            C = B;
            B = (B + Rotl(t, S[i])) & 0xFFFFFFFFu;
        }
        a0 = (a0 + A) & 0xFFFFFFFFu;
        b0 = (b0 + B) & 0xFFFFFFFFu;
        c0 = (c0 + C) & 0xFFFFFFFFu;
        d0 = (d0 + D) & 0xFFFFFFFFu;
    }
    uint32_t dg[4] = {a0, b0, c0, d0};
    for (int i = 0; i < 4; i++) {
        out[i * 4 + 0] = static_cast<uint8_t>(dg[i]);
        out[i * 4 + 1] = static_cast<uint8_t>(dg[i] >> 8);
        out[i * 4 + 2] = static_cast<uint8_t>(dg[i] >> 16);
        out[i * 4 + 3] = static_cast<uint8_t>(dg[i] >> 24);
    }
}

// Recompute and store the hash at [4,20) of a DXBC container in place.
inline void SetDxbcHash(std::vector<uint8_t>& data)
{
    uint8_t dg[16];
    Md5Obf(data.data() + 0x14, data.size() - 0x14, dg);
    for (int i = 0; i < 16; i++)
        data[4 + i] = dg[i];
}

}  // namespace slf_dxbc_hash
