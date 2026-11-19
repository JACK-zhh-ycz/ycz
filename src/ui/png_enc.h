#pragma once
// ============================================================
//  极简 PNG 编码器（RGBA8888，无外部依赖）
//  - 与项目自研 ZIP/DEFLATE 的路线一致：不依赖 GDI+ 的 Save
//    （本机 Gdiplus Save 对 png/bmp/jpg 一律返回 Win32Error）
//  - 压缩采用 zlib stored（不压缩）块，保证正确性优先
// ============================================================

#include <string>
#include <cstdint>

namespace ppt {

// rgba 为行主序 RGBA 字节，stride 为每行字节数（>= w*4）
// 成功写入 path 返回 true
bool write_png(const std::string& path, int w, int h,
               const uint8_t* rgba, int stride, std::string* err = nullptr);

} // namespace ppt
