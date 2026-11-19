#pragma once
// ============================================================
//  HTML 渲染器：Presentation -> 独立 HTML 文件
//  产出单文件 HTML，内嵌 CSS/JS，支持：
//   - 全屏放映 + 键盘/点击翻页
//   - 翻页动画 + 元素进入动画
//   - 演讲者备注弹窗
//   - 形状/图片/表格渲染
// ============================================================

#include <string>
#include "../core/model.h"

namespace ppt {

struct HtmlOptions {
    int canvas_w = 1280;          // CSS px
    int canvas_h = 720;
    bool embed_media = true;      // 图片以 base64 内嵌
    bool include_notes = true;    // 演讲者备注
    bool include_anim = true;     // 动画
    bool include_geoms = true;    // 形状 SVG
    std::string title = "演示文稿";
};

// 核心：把 pres 渲染为单一 HTML 字符串
std::string render_html(const Presentation& pres, const HtmlOptions& opt = HtmlOptions());

// 快捷写入
bool render_to_file(const std::string& path, const Presentation& pres,
                    const HtmlOptions& opt = HtmlOptions(), std::string* err_out = nullptr);

} // namespace ppt