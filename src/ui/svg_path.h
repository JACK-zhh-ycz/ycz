#pragma once
// ============================================================
//  SVG path d="" -> 折线点集（纯几何模块，不依赖 GDI+ / 不依赖 ppt 模型）
//  用途：
//   1) 编辑器画布把 preset_geom 表与 custGeom 导出的路径展平后用 GDI+ 绘制
//   2) 命中测试（点选形状时判断是否在轮廓内）
//   3) 计算形状真实包围盒
//  说明：输入路径多为归一化(0..1)坐标，flatten 时按 sx/sy 缩放到像素。
//  解析后命令统一转为绝对坐标，type 为大写字母。
// ============================================================

#include <string>
#include <vector>

namespace svgpath {

struct Pt {
    double x = 0, y = 0;
};

struct PathCmd {
    char type = 'M';                 // M L H V C S Q T A Z（绝对）
    std::vector<double> pts;         // 已转为绝对坐标的参数序列
};

// 解析 path d="" 字符串；非法字符按 SVG 容错规则跳过
std::vector<PathCmd> parse_path(const std::string& d);

struct Polyline {
    std::vector<Pt> pts;
    bool closed = false;
};

// 展平为绝对坐标折线。samples 为每条曲线的细分段数（>=1）。
// sx/sy 分别是 x/y 方向的缩放系数（归一化路径传入宽高即可）。
std::vector<Polyline> flatten(const std::vector<PathCmd>& cmds,
                              double sx, double sy, int samples = 16);

struct Bounds {
    double xmin = 0, xmax = 0, ymin = 0, ymax = 0;
    bool valid = false;
};

Bounds polyline_bounds(const std::vector<Polyline>& polys);

// 奇偶填充规则命中测试（等价 SVG fill-rule 默认值）
bool point_in_polylines(const std::vector<Polyline>& polys, double x, double y);

// 点到轮廓的最短距离（用于边框拾取与句柄定位）
double distance_to_polylines(const std::vector<Polyline>& polys, double x, double y);

// 便捷：一步完成解析 + 展平
inline std::vector<Polyline> flatten_d(const std::string& d, double sx, double sy,
                                       int samples = 16) {
    return flatten(parse_path(d), sx, sy, samples);
}

} // namespace svgpath
