#include "zip.h"
#include "inflate.h"
#include "strutil.h"

#include <cstring>

namespace ppt {
namespace {

inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline void wr16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF); }
inline void wr32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF);
    v.push_back((x >> 16) & 0xFF); v.push_back((x >> 24) & 0xFF);
}

// 本地文件头签名 / central 签名 / EOCD 签名
static const uint32_t SIG_LOCAL = 0x04034b50u;
static const uint32_t SIG_CENTRAL = 0x02014b50u;
static const uint32_t SIG_EOCD = 0x06054b50u;

struct CentralRec {
    std::string name;
    uint16_t method;
    uint32_t crc;
    uint32_t comp_size;
    uint32_t uncomp_size;
    uint32_t local_off;
};

} // namespace

bool ZipArchive::open(const std::vector<uint8_t>& blob, std::string& err) {
    entries_.clear();
    index_.clear();
    if (blob.size() < 22) { err = "文件太小，不是有效的 ZIP/PPTX"; return false; }

    // 反向查找 EOCD（注释最长 65535 字节）
    size_t eocd = 0;
    bool found = false;
    size_t lo = blob.size() > 22 + 65536 ? blob.size() - 22 - 65536 : 0;
    for (size_t i = blob.size() - 22; i >= lo; --i) {
        if (rd32(&blob[i]) == SIG_EOCD) { eocd = i; found = true; break; }
        if (i == 0) break;
    }
    if (!found) { err = "未找到 ZIP 结束记录（可能不是 pptx/zip 文件）"; return false; }

    uint16_t total = rd16(&blob[eocd + 10]);
    uint32_t cd_off = rd32(&blob[eocd + 16]);
    if ((size_t)cd_off >= blob.size()) { err = "ZIP 中央目录偏移非法"; return false; }

    std::vector<CentralRec> recs;
    size_t p = cd_off;
    for (uint16_t i = 0; i < total; ++i) {
        if (p + 46 > blob.size() || rd32(&blob[p]) != SIG_CENTRAL) { err = "中央目录记录损坏"; return false; }
        CentralRec r;
        r.method = rd16(&blob[p + 10]);
        r.crc = rd32(&blob[p + 16]);
        r.comp_size = rd32(&blob[p + 20]);
        r.uncomp_size = rd32(&blob[p + 24]);
        uint16_t nlen = rd16(&blob[p + 28]);
        uint16_t elen = rd16(&blob[p + 30]);
        uint16_t clen = rd16(&blob[p + 32]);
        r.local_off = rd32(&blob[p + 42]);
        if (p + 46 + nlen > blob.size()) { err = "文件名越界"; return false; }
        r.name.assign((const char*)&blob[p + 46], nlen);
        p += 46ull + nlen + elen + clen;
        recs.push_back(std::move(r));
    }

    for (const CentralRec& r : recs) {
        if (r.name.empty() || r.name[r.name.size() - 1] == '/') continue;   // 目录项跳过
        if (r.local_off + 30 > blob.size()) { err = "本地头偏移非法"; return false; }
        if (rd32(&blob[r.local_off]) != SIG_LOCAL) { err = "本地文件头损坏"; return false; }
        uint16_t lnlen = rd16(&blob[r.local_off + 26]);
        uint16_t lelen = rd16(&blob[r.local_off + 28]);
        size_t data_off = r.local_off + 30ull + lnlen + lelen;
        if (data_off + r.comp_size > blob.size()) { err = "数据区越界"; return false; }
        const uint8_t* raw = &blob[data_off];

        std::vector<uint8_t> plain;
        if (r.method == 0) {
            if (r.comp_size != r.uncomp_size) { err = "stored 尺寸不一致"; return false; }
            plain.assign(raw, raw + r.comp_size);
        } else if (r.method == 8) {
            if (!inflate_raw(raw, r.comp_size, plain, r.uncomp_size)) {
                err = "DEFLATE 解压失败: " + r.name;
                return false;
            }
            if (r.uncomp_size && plain.size() != r.uncomp_size) { err = "解压尺寸不符: " + r.name; return false; }
        } else {
            err = "不支持的压缩方式 " + std::to_string(r.method) + ": " + r.name;
            return false;
        }
        if (r.crc && plain.size() < (64ull << 20)) {              // 大文件跳过校验（耗时）
            if (crc32_of(plain.data(), plain.size()) != r.crc) { err = "CRC 校验失败: " + r.name; return false; }
        }
        Entry e;
        e.name = r.name;
        e.data = std::move(plain);
        index_[e.name] = entries_.size();
        entries_.push_back(std::move(e));
    }
    return true;
}

bool ZipArchive::open_file(const std::string& path, std::string& err) {
    std::vector<uint8_t> blob;
    if (!read_file(path, blob)) { err = "无法读取文件: " + path; return false; }
    return open(blob, err);
}

// 路径归一化：把 \ 换成 /（ZIP 里条目名统一用 / ）
static std::string zip_norm(const std::string& name) {
    std::string s = name;
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}

bool ZipArchive::has(const std::string& name) const { return index_.count(zip_norm(name)) != 0; }

bool ZipArchive::get(const std::string& name, std::vector<uint8_t>& out) const {
    auto it = index_.find(zip_norm(name));
    if (it == index_.end()) return false;
    out = entries_[it->second].data;
    return true;
}

bool ZipArchive::get(const std::string& name, std::string& out) const {
    auto it = index_.find(zip_norm(name));
    if (it == index_.end()) return false;
    const std::vector<uint8_t>& d = entries_[it->second].data;
    out.assign(d.begin(), d.end());
    return true;
}

std::vector<std::string> ZipArchive::names() const {
    std::vector<std::string> v;
    v.reserve(entries_.size());
    for (const Entry& e : entries_) v.push_back(e.name);
    return v;
}

void ZipArchive::add(const std::string& name, const std::vector<uint8_t>& data) {
    std::string nm = name;
    for (char& c : nm) if (c == '\\') c = '/';
    auto it = index_.find(nm);
    if (it != index_.end()) { entries_[it->second].data = data; return; }
    Entry e;
    e.name = nm;
    e.data = data;
    index_[nm] = entries_.size();
    entries_.push_back(std::move(e));
}

void ZipArchive::add(const std::string& name, const std::string& data) {
    add(name, std::vector<uint8_t>(data.begin(), data.end()));
}

bool ZipArchive::remove(const std::string& name) {
    auto it = index_.find(zip_norm(name));
    if (it == index_.end()) return false;
    entries_.erase(entries_.begin() + it->second);
    index_.clear();
    for (size_t i = 0; i < entries_.size(); ++i) index_[entries_[i].name] = i;
    return true;
}

std::vector<uint8_t> ZipArchive::build() const {
    std::vector<uint8_t> out;
    out.reserve(1 << 20);
    struct Loc { uint32_t off, crc, size; std::string name; };
    std::vector<Loc> locs;

    for (const Entry& e : entries_) {
        Loc l;
        l.off = (uint32_t)out.size();
        l.size = (uint32_t)e.data.size();
        l.crc = crc32_of(e.data.data(), e.data.size());
        l.name = e.name;
        locs.push_back(l);

        wr32(out, SIG_LOCAL);
        wr16(out, 20);              // version needed
        wr16(out, 0);               // flags
        wr16(out, 0);               // method = stored
        wr16(out, 0);               // mod time
        wr16(out, 0x2821);          // mod date (2001-01-01，合法值)
        wr32(out, l.crc);
        wr32(out, l.size);
        wr32(out, l.size);
        wr16(out, (uint16_t)e.name.size());
        wr16(out, 0);
        for (char c : e.name) out.push_back((uint8_t)c);
        out.insert(out.end(), e.data.begin(), e.data.end());
    }

    uint32_t cd_off = (uint32_t)out.size();
    for (const Loc& l : locs) {
        wr32(out, SIG_CENTRAL);
        wr16(out, 20);              // version made by
        wr16(out, 20);              // version needed
        wr16(out, 0);               // flags
        wr16(out, 0);               // method stored
        wr16(out, 0);               // time
        wr16(out, 0x2821);          // date
        wr32(out, l.crc);
        wr32(out, l.size);
        wr32(out, l.size);
        wr16(out, (uint16_t)l.name.size());
        wr16(out, 0);               // extra
        wr16(out, 0);               // comment
        wr16(out, 0);               // disk
        wr16(out, 0);               // internal attrs
        wr32(out, 0);               // external attrs
        wr32(out, l.off);
        for (char c : l.name) out.push_back((uint8_t)c);
    }
    uint32_t cd_size = (uint32_t)out.size() - cd_off;

    wr32(out, SIG_EOCD);
    wr16(out, 0);
    wr16(out, 0);
    wr16(out, (uint16_t)locs.size());
    wr16(out, (uint16_t)locs.size());
    wr32(out, cd_size);
    wr32(out, cd_off);
    wr16(out, 0);
    return out;
}

bool ZipArchive::save(const std::string& path, std::string& err) const {
    if (entries_.empty()) { err = "ZIP 内容为空，拒绝写出"; return false; }
    std::vector<uint8_t> blob = build();
    if (!write_file(path, blob)) { err = "写入失败: " + path; return false; }
    return true;
}

} // namespace ppt
