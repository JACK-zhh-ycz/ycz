#pragma once
// ============================================================
//  OOXML 预设几何 -> SVG path d="" 转换
//  覆盖 prstGeom 常用形状，画布为 0~w 与 0~h（归一化单位正方形）
//  返回值中 {w},{h},{hw},{hh} 在调用方替换为实际宽/高
// ============================================================

#include <string>

namespace ppt {

// 返回归一化 SVG path data（在 (w,h) 框内，坐标值不带单位）
const char* preset_path(const std::string& prst);

// 部分形状需要用 adjust 值（尖角 / 圆角半径等），返回 adjust 列表
// 如 "tria" 的 adj = 50000（百分比），"roundRect" 的 adj = 16667
struct PresetMeta {
    const char* path;
    int n_adjust;
    double adjust[3];
};
PresetMeta preset_meta(const std::string& prst);

} // namespace ppt