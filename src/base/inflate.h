#pragma once
// ============================================================
//  DEFLATE / zlib 解压（纯手写，工程不依赖第三方 zlib）
//  inflate_raw   : 裸 DEFLATE 数据流（ZIP method=8）
//  zlib_inflate  : 带 2 字节 zlib 头的数据流
// ============================================================

#include <string>
#include <vector>
#include <cstdint>

namespace ppt {

bool inflate_raw(const uint8_t* src, size_t src_len,
                 std::vector<uint8_t>& out, uint64_t expect_size = 0);

bool zlib_inflate(const uint8_t* src, size_t src_len,
                  std::vector<uint8_t>& out);

inline bool zlib_inflate(const std::string& src, std::string& out) {
    std::vector<uint8_t> b;
    if (!zlib_inflate((const uint8_t*)src.data(), src.size(), b)) return false;
    out.assign(b.begin(), b.end());
    return true;
}

} // namespace ppt
