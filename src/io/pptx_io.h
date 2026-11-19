#pragma once
// ============================================================
//  PPTX 读取器：从 .pptx (ZIP) 解析 OOXML 构建 Presentation
//  PPTX 写出器：将 Presentation 序列化回 .pptx ZIP
// ============================================================

#include <string>
#include "../core/model.h"

namespace ppt {

// ---- 读 ----
bool pptx_read(const std::string& path, Presentation& pres, std::string& err);

// ---- 写（回存为 .pptx，media 与 raw_parts 保真） ----
bool pptx_write(const std::string& path, const Presentation& pres, std::string& err);

} // namespace ppt