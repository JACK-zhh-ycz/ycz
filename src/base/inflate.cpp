#include "inflate.h"

#include <cstring>

namespace ppt {
namespace {

// ---------------- 位读取（DEFLATE：低位优先） ----------------
struct BitReader {
    const uint8_t* p;
    size_t n, pos;
    uint32_t buf;
    int cnt;
    bool bad;

    BitReader(const uint8_t* d, size_t len) : p(d), n(len), pos(0), buf(0), cnt(0), bad(false) {}

    uint32_t bits(int k) {
        while (cnt < k) {
            if (pos >= n) { bad = true; return 0; }
            buf |= (uint32_t)p[pos++] << cnt;
            cnt += 8;
        }
        uint32_t v = buf & ((1u << k) - 1u);
        buf >>= k;
        cnt -= k;
        return v;
    }
    void align_byte() { buf = 0; cnt = 0; }
};

// ---------------- 正则 Huffman ----------------
struct Huff {
    uint16_t counts[16];
    uint16_t symbols[512];

    Huff() { memset(counts, 0, sizeof(counts)); memset(symbols, 0, sizeof(symbols)); }

    void build(const uint8_t* lens, int n) {
        memset(counts, 0, sizeof(counts));
        for (int i = 0; i < n; ++i) counts[lens[i]]++;
        counts[0] = 0;
        uint16_t offs[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        for (int i = 2; i < 16; ++i) offs[i] = (uint16_t)(offs[i - 1] + counts[i - 1]);
        for (int i = 0; i < n; ++i)
            if (lens[i] && offs[lens[i]] < 512) symbols[offs[lens[i]]++] = (uint16_t)i;
    }

    int decode(BitReader& b) const {
        uint32_t code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= b.bits(1);
            uint32_t count = counts[len];
            if (code - first < count) return symbols[index + (code - first)];
            first += count;
            index += count;
            code <<= 1;
        }
        b.bad = true;
        return -1;
    }
};

// ---------------- 长度/距离表 ----------------
static const uint16_t LBASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                   35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t LBITS[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                  3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t DBASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                   257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                   8193, 12289, 16385, 24577};
static const uint8_t DBITS[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                  7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static const uint64_t kMaxOutput = 400ull * 1024ull * 1024ull;   // 防御性上限

bool inflate_core(BitReader& b, std::vector<uint8_t>& out) {
    Huff lit_fixed, dist_fixed;
    bool fixed_ready = false;

    for (;;) {
        uint32_t last = b.bits(1);
        uint32_t type = b.bits(2);
        if (b.bad) return false;

        if (type == 0) {                       // stored
            b.align_byte();
            if (b.pos + 4 > b.n) return false;
            uint32_t len = (uint32_t)b.p[b.pos] | ((uint32_t)b.p[b.pos + 1] << 8);
            uint32_t nlen = (uint32_t)b.p[b.pos + 2] | ((uint32_t)b.p[b.pos + 3] << 8);
            b.pos += 4;
            if (len != ((~nlen) & 0xFFFF)) return false;
            if (b.pos + len > b.n) return false;
            if (out.size() + len > kMaxOutput) return false;
            out.insert(out.end(), b.p + b.pos, b.p + b.pos + len);
            b.pos += len;
        } else if (type == 1 || type == 2) {
            const Huff* litH;
            const Huff* dstH;
            static uint8_t litLens[288], distLens[32];

            if (type == 1) {
                if (!fixed_ready) {
                    for (int i = 0; i < 144; ++i) litLens[i] = 8;
                    for (int i = 144; i < 256; ++i) litLens[i] = 9;
                    for (int i = 256; i < 280; ++i) litLens[i] = 7;
                    for (int i = 280; i < 288; ++i) litLens[i] = 8;
                    for (int i = 0; i < 32; ++i) distLens[i] = 5;
                    lit_fixed.build(litLens, 288);
                    dist_fixed.build(distLens, 32);
                    fixed_ready = true;
                }
                litH = &lit_fixed;
                dstH = &dist_fixed;
            } else {                           // dynamic
                uint32_t hlit = b.bits(5) + 257;
                uint32_t hdist = b.bits(5) + 1;
                uint32_t hclen = b.bits(4) + 4;
                if (b.bad || hlit > 288 || hdist > 32) return false;
                static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                                  11, 4, 12, 3, 13, 2, 14, 1, 15};
                uint8_t clLens[19] = {0};
                for (uint32_t i = 0; i < hclen; ++i) clLens[order[i]] = (uint8_t)b.bits(3);
                Huff cl;
                cl.build(clLens, 19);

                uint32_t total = hlit + hdist;
                if (total > 512) return false;
                uint32_t i = 0;
                while (i < total) {
                    int sym = cl.decode(b);
                    if (sym < 0) return false;
                    if (sym < 16) {
                        i++;
                    } else if (sym == 16) {
                        if (i == 0) return false;
                        uint8_t prev = (i < hlit) ? litLens[i - 1] : distLens[i - hlit - 1];
                        uint32_t rep = 3 + b.bits(2);
                        while (rep-- && i < total) {
                            if (i < hlit) litLens[i] = prev; else distLens[i - hlit] = prev;
                            i++;
                        }
                    } else if (sym == 17) {
                        uint32_t rep = 3 + b.bits(3);
                        while (rep-- && i < total) {
                            if (i < hlit) litLens[i] = 0; else distLens[i - hlit] = 0;
                            i++;
                        }
                    } else {                   // 18
                        uint32_t rep = 11 + b.bits(7);
                        while (rep-- && i < total) {
                            if (i < hlit) litLens[i] = 0; else distLens[i - hlit] = 0;
                            i++;
                        }
                    }
                }
                if (i != total) return false;
                static Huff lit_dyn, dist_dyn;
                lit_dyn.build(litLens, (int)hlit);
                dist_dyn.build(distLens, (int)hdist);
                litH = &lit_dyn;
                dstH = &dist_dyn;
            }

            for (;;) {
                int sym = litH->decode(b);
                if (sym < 0) return false;
                if (sym < 256) {
                    out.push_back((uint8_t)sym);
                    if (out.size() > kMaxOutput) return false;
                } else if (sym == 256) {
                    break;
                } else {
                    sym -= 257;
                    if (sym >= 29) return false;
                    uint32_t len = LBASE[sym] + b.bits(LBITS[sym]);
                    int dsym = dstH->decode(b);
                    if (dsym < 0 || dsym >= 30) return false;
                    uint32_t dist = DBASE[dsym] + b.bits(DBITS[dsym]);
                    if (dist == 0 || dist > out.size()) return false;
                    if (out.size() + len > kMaxOutput) return false;
                    size_t src = out.size() - dist;
                    for (uint32_t k = 0; k < len; ++k) out.push_back(out[src + k]);
                }
            }
        } else {
            return false;                      // type=3 保留
        }
        if (last) break;
    }
    return !b.bad;
}

} // namespace

bool inflate_raw(const uint8_t* src, size_t src_len,
                 std::vector<uint8_t>& out, uint64_t expect_size) {
    out.clear();
    if (src_len == 0) return true;
    out.reserve(expect_size && expect_size < (64ull << 20) ? (size_t)expect_size : 0);
    BitReader b(src, src_len);
    return inflate_core(b, out);
}

bool zlib_inflate(const uint8_t* src, size_t src_len, std::vector<uint8_t>& out) {
    if (src_len >= 2) {
        uint8_t cmf = src[0];
        if ((cmf & 0x0F) == 8) {              // 标准 zlib 头，跳过 2 字节（adler32 忽略）
            return inflate_raw(src + 2, src_len - 2, out, 0);
        }
    }
    return inflate_raw(src, src_len, out, 0);
}

} // namespace ppt
