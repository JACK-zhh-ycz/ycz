#include "png_enc.h"
#include "../base/strutil.h"

#include <vector>
#include <cstring>

namespace ppt {
namespace {

// PNG 使用大端 4 字节整数与 CRC32（多项式同 zip，但这里是标准 PNG CRC）
static uint32_t png_crc(const uint8_t* data, size_t len, uint32_t seed = 0xFFFFFFFFu) {
    static uint32_t tab[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            tab[i] = c;
        }
        ready = true;
    }
    uint32_t c = seed;
    for (size_t i = 0; i < len; ++i) c = tab[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c;
}

static void be32(std::string& out, uint32_t v) {
    char b[4] = { (char)((v >> 24) & 0xFF), (char)((v >> 16) & 0xFF),
                  (char)((v >> 8) & 0xFF), (char)(v & 0xFF) };
    out.append(b, 4);
}

static void chunk_put(std::string& out, const char* type, const uint8_t* p, size_t n) {
    be32(out, (uint32_t)n);
    std::string body;
    body.append(type, 4);
    body.append((const char*)p, n);
    out += body;
    uint32_t c = png_crc((const uint8_t*)body.data(), body.size()) ^ 0xFFFFFFFFu;
    be32(out, c);
}

static uint32_t adler32_of(const uint8_t* p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) { a = (a + p[i]) % 65521; b = (b + a) % 65521; }
    return (b << 16) | a;
}

// zlib stored（无压缩）流：header + 若干 stored block + adler32
static std::string zlib_stored(const std::string& raw) {
    std::string z;
    z.push_back((char)0x78);   // CMF: deflate, 32K window
    z.push_back((char)0x01);   // FLG: 无字典，级别 0（CMF+FLG 需为 31 的倍数）
    size_t pos = 0;
    const size_t MAXB = 65535;
    if (raw.empty()) {
        z.push_back((char)0x01);   // BFINAL=1, BTYPE=00
        be32(z, 0x0000FFFFu);      // LEN=0, NLEN=0xFFFF
    }
    while (pos < raw.size()) {
        size_t n = std::min(MAXB, raw.size() - pos);
        bool last = (pos + n >= raw.size());
        z.push_back(last ? (char)0x01 : (char)0x00);
        uint16_t len = (uint16_t)n, nlen = (uint16_t)~n;
        z.push_back((char)(len & 0xFF)); z.push_back((char)((len >> 8) & 0xFF));
        z.push_back((char)(nlen & 0xFF)); z.push_back((char)((nlen >> 8) & 0xFF));
        z.append(raw.data() + pos, n);
        pos += n;
    }
    uint32_t ad = adler32_of((const uint8_t*)raw.data(), raw.size());
    be32(z, ad);
    return z;
}

} // namespace

bool write_png(const std::string& path, int w, int h,
               const uint8_t* rgba, int stride, std::string* err) {
    if (w <= 0 || h <= 0 || !rgba) { if (err) *err = "参数非法"; return false; }
    if (stride < w * 4) { if (err) *err = "stride 过小"; return false; }

    // 1) 原始扫描线：每行前加 filter type 0
    std::string raw;
    raw.reserve((size_t)h * ((size_t)w * 4 + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back((char)0);
        raw.append((const char*)(rgba + (size_t)y * stride), (size_t)w * 4);
    }

    std::string png;
    const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    png.append((const char*)sig, 8);

    // 2) IHDR
    {
        std::string ihdr;
        be32(ihdr, (uint32_t)w);
        be32(ihdr, (uint32_t)h);
        ihdr.push_back((char)8);    // bit depth
        ihdr.push_back((char)6);    // color type RGBA
        ihdr.push_back((char)0);    // compression
        ihdr.push_back((char)0);    // filter
        ihdr.push_back((char)0);    // interlace
        chunk_put(png, "IHDR", (const uint8_t*)ihdr.data(), ihdr.size());
    }
    // 3) IDAT
    {
        std::string z = zlib_stored(raw);
        chunk_put(png, "IDAT", (const uint8_t*)z.data(), z.size());
    }
    // 4) IEND
    chunk_put(png, "IEND", (const uint8_t*)"", 0);

    if (!write_file(path, png)) { if (err) *err = "无法写入 " + path; return false; }
    return true;
}

} // namespace ppt
