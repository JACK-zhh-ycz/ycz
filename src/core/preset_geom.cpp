#include "preset_geom.h"
#include "../base/strutil.h"

#include <map>

namespace ppt {

// 以下路径均在 (0,0)-(1,1) 归一化框内；调用方乘以 w,h 即可
static std::map<std::string, std::string> g_paths;
static bool g_init = false;

static void reg(const char* name, const char* d) { g_paths[to_lower(name)] = d; }

static void init() {
    if (g_init) return;
    g_init = true;

    // 矩形 (rect)
    reg("rect",        "M 0,0 L 1,0 L 1,1 L 0,1 Z");
    // 圆角矩形 (adj = 圆角半径百分比 0..50000)
    reg("roundRect",   "M {adj},0 L {1-adj},0 Q 1,0 1,{adj} L 1,{1-adj} Q 1,1 {1-adj},1 L {adj},1 Q 0,1 0,{1-adj} L 0,{adj} Q 0,0 {adj},0 Z");
    // 椭圆
    reg("ellipse",     "M 0.5,0 C 0.776,0 1,0.224 1,0.5 C 1,0.776 0.776,1 0.5,1 C 0.224,1 0,0.776 0,0.5 C 0,0.224 0.224,0 0.5,0 Z");
    // 三角形 (等腰)
    reg("triangle",    "M 0.5,0 L 1,1 L 0,1 Z");
    // 直角三角形
    reg("rtTriangle",  "M 0,0 L 0,1 L 1,1 Z");
    // 菱形
    reg("diamond",     "M 0.5,0 L 1,0.5 L 0.5,1 L 0,0.5 Z");
    // 梯形
    reg("trapezoid",   "M 0.2,0 L 0.8,0 L 1,1 L 0,1 Z");
    // 平行四边形
    reg("parallelogram","M 0.2,0 L 1,0 L 0.8,1 L 0,1 Z");
    // 五边形
    reg("homePlate",   "M 0.5,0 L 1,0.38 L 1,1 L 0,1 L 0,0.38 Z");
    // 六边形
    reg("hexagon",     "M 0.5,0 L 1,0.25 L 1,0.75 L 0.5,1 L 0,0.75 L 0,0.25 Z");
    // 箭头：右箭头
    reg("rightArrow",  "M 0,0.28 L 0.6,0.28 L 0.6,0 L 1,0.5 L 0.6,1 L 0.6,0.72 L 0,0.72 Z");
    // 左箭头
    reg("leftArrow",   "M 1,0.28 L 0.4,0.28 L 0.4,0 L 0,0.5 L 0.4,1 L 0.4,0.72 L 1,0.72 Z");
    // 上箭头
    reg("upArrow",     "M 0.28,1 L 0.28,0.4 L 0,0.4 L 0.5,0 L 1,0.4 L 0.72,0.4 L 0.72,1 Z");
    // 下箭头
    reg("downArrow",   "M 0.28,0 L 0.28,0.6 L 0,0.6 L 0.5,1 L 1,0.6 L 0.72,0.6 L 0.72,0 Z");
    // 左右箭头
    reg("leftRightArrow","M 0,0.38 L 0.3,0.38 L 0.3,0 L 0.5,0.25 L 0.5,0.25 L 0.3,0.5 L 0.3,0.12 L 0,0.12 Z"); // 简化为单箭头示意
    // 星形 (5-point)
    reg("star5",       "M 0.5,0 L 0.618,0.382 L 1,0.382 L 0.691,0.618 L 0.809,1 L 0.5,0.764 L 0.191,1 L 0.309,0.618 L 0,0.382 L 0.382,0.382 Z");
    // 弯箭头
    reg("bentArrow",   "M 0.1,0.2 L 0.7,0.2 L 0.7,0.35 L 1,0.15 L 0.7,-0.05 L 0.7,0.1 L 0,0.1 L 0,0.7 L 0.1,0.7 Z");
    // 弧形
    reg("arc",         "M 0.2,0.85 A 0.6,0.6 0 1,1 0.87,0.25 L 0.82,0.2 A 0.5,0.5 0 1,0 0.15,0.73 Z");
    // 带状/卷边
    reg("ribbon",      "M 0,0 L 1,0 L 0.85,0.5 L 1,1 L 0,1 Z");
    reg("ribbon2",     "M 0,1 L 1,1 L 0.85,0.5 L 1,0 L 0,0 Z");
    // 立方体
    reg("cube",        "M 0.2,0 L 1,0 L 0.8,0.2 L 0,0.2 Z M 0,0.2 L 0.8,0.2 L 0.8,1 L 0,1 Z M 1,0 L 1,0.2 L 0.8,0.2");
    // 加号
    reg("plus",        "M 0.35,0 L 0.65,0 L 0.65,0.35 L 1,0.35 L 1,0.65 L 0.65,0.65 L 0.65,1 L 0.35,1 L 0.35,0.65 L 0,0.65 L 0,0.35 L 0.35,0.35 Z");
    // 等号
    reg("equal",       "M 0,0.25 L 1,0.25 L 1,0.45 L 0,0.45 Z M 0,0.55 L 1,0.55 L 1,0.75 L 0,0.75 Z");
    // 减号
    reg("minus",       "M 0.1,0.4 L 0.9,0.4 L 0.9,0.6 L 0.1,0.6 Z");
    // 乘号
    reg("mult",        "M 0.2,0 L 0.5,0.35 L 0.8,0 L 1,0.2 L 0.65,0.5 L 1,0.8 L 0.8,1 L 0.5,0.65 L 0.2,1 L 0,0.8 L 0.35,0.5 L 0,0.2 Z");
    // 除号
    reg("divide",      "M 0.2,0.4 L 0.8,0.4 L 0.8,0.6 L 0.2,0.6 Z M 0.45,0.1 A 0.05,0.05 0 1,1 0.55,0.1 A 0.05,0.05 0 1,1 0.45,0.1 Z M 0.45,0.9 A 0.05,0.05 0 1,1 0.55,0.9 A 0.05,0.05 0 1,1 0.45,0.9 Z");
    // 圆
    reg("circle",      "M 0.5,0 C 0.776,0 1,0.224 1,0.5 C 1,0.776 0.776,1 0.5,1 C 0.224,1 0,0.776 0,0.5 C 0,0.224 0.224,0 0.5,0 Z");
    // 云形
    reg("cloud",       "M 0.6,0.2 C 0.7,0.1 0.9,0.15 0.9,0.35 C 0.95,0.55 0.85,0.7 0.65,0.7 L 0.35,0.7 C 0.15,0.7 0,0.55 0,0.4 C 0,0.2 0.2,0.1 0.35,0.15 C 0.4,0.05 0.55,0 0.6,0.2 Z");
    // 流程图: process=rect, decision=菱形, terminator=圆角矩形, data=平行四边形
    reg("flowChartProcess","M 0,0 L 1,0 L 1,1 L 0,1 Z");
    reg("flowChartDecision","M 0.5,0 L 1,0.5 L 0.5,1 L 0,0.5 Z");
    reg("flowChartTerminator","M 0.1,0 L 0.9,0 Q 1,0 1,0.1 L 1,0.9 Q 1,1 0.9,1 L 0.1,1 Q 0,1 0,0.9 L 0,0.1 Q 0,0 0.1,0 Z");
    reg("flowChartDocument","M 0,0 L 0.8,0 L 1,0.2 L 1,1 L 0,1 Z");
    reg("flowChartData","M 0.2,0 L 1,0 L 0.8,1 L 0,1 Z");
    // 笑脸
    reg("smileyFace",  "M 0.5,0 C 0.224,0 0,0.224 0,0.5 C 0,0.776 0.224,1 0.5,1 C 0.776,1 1,0.776 1,0.5 C 1,0.224 0.776,0 0.5,0 Z M 0.3,0.4 A 0.07,0.07 0 1,0 0.44,0.4 A 0.07,0.07 0 1,0 0.3,0.4 Z M 0.56,0.4 A 0.07,0.07 0 1,0 0.7,0.4 A 0.07,0.07 0 1,0 0.56,0.4 Z M 0.25,0.7 Q 0.5,0.85 0.75,0.7");
    // 太阳
    reg("sun",         "M 0.5,0 L 0.5,0.15 M 0.5,0.85 L 0.5,1 M 0,0.5 L 0.15,0.5 M 0.85,0.5 L 1,0.5 M 0.15,0.15 L 0.25,0.25 M 0.75,0.75 L 0.85,0.85 M 0.15,0.85 L 0.25,0.75 M 0.75,0.25 L 0.85,0.15 M 0.5,0.25 C 0.638,0.25 0.75,0.362 0.75,0.5 C 0.75,0.638 0.638,0.75 0.5,0.75 C 0.362,0.75 0.25,0.638 0.25,0.5 C 0.25,0.362 0.362,0.25 0.5,0.25 Z");
    // 月亮 (crescent)
    reg("moon",        "M 0.6,0 C 0.8,0.2 0.8,0.8 0.6,1 C 0.85,0.85 1,0.65 1,0.5 C 1,0.35 0.85,0.15 0.6,0 Z");
    // 闪电 (lightning)
    reg("lightningBolt","M 0.6,0 L 0.2,0.55 L 0.45,0.55 L 0.3,1 L 0.8,0.42 L 0.55,0.42 Z");
    // 心形
    reg("heart",       "M 0.5,1 L 0,0.6 C -0.1,0.4 0,0.15 0.25,0.1 C 0.45,0.05 0.5,0.2 0.5,0.4 C 0.5,0.2 0.55,0.05 0.75,0.1 C 1,0.15 1.1,0.4 1,0.6 Z");
    // 圆柱体
    reg("can",         "M 0,0.15 Q 0.5,0.35 1,0.15 L 1,0.85 Q 0.5,1.05 0,0.85 Z M 0,0.15 Q 0.5,-0.05 1,0.15");
    // 立体：四棱锥
    reg("pyramid",     "M 0.5,0 L 1,0.35 L 0.5,0.7 L 0,0.35 Z M 0.5,0.7 L 1,0.35 L 1,1 L 0.5,1 Z M 0.5,0.7 L 0,0.35 L 0,1 L 0.5,1 Z");
    // 立体：球（用渐变填充即可显出体积）
    reg("sphere",      "M 0.5,0 C 0.776,0 1,0.224 1,0.5 C 1,0.776 0.776,1 0.5,1 C 0.224,1 0,0.776 0,0.5 C 0,0.224 0.224,0 0.5,0 Z");
    // 立体：三棱柱
    reg("prism3",      "M 0.5,0 L 1,0.3 L 1,0.85 L 0.5,0.55 L 0,0.85 L 0,0.3 Z M 0.5,0.55 L 1,0.85 L 0.5,1.15 L 0,0.85");
    // 对号
    reg("checkmark",   "M 0.1,0.5 L 0.4,0.8 L 0.9,0.2");
    // 叉号
    reg("xmark",       "M 0.2,0.2 L 0.8,0.8 M 0.8,0.2 L 0.2,0.8");
    // 禁止符
    reg("noSmoking",   "M 0.5,0 C 0.224,0 0,0.224 0,0.5 C 0,0.776 0.224,1 0.5,1 C 0.776,1 1,0.776 1,0.5 C 1,0.224 0.776,0 0.5,0 Z M 0.85,0.15 L 0.15,0.85");
    // 图文框 / 标注 (callout)
    reg("cloudCallout","M 0.6,0.2 H 0.9 Q 1,0.2 1,0.3 V 0.7 Q 1,0.8 0.9,0.8 H 0.5 L 0.35,1 L 0.4,0.8 H 0.1 Q 0,0.8 0,0.7 V 0.3 Q 0,0.2 0.1,0.2 H 0.3 Q 0.35,0 0.4,0.05 Q 0.5,0 0.6,0.2 Z");
    // 边框
    reg("border",      "M 0,0 L 1,0 L 1,1 L 0,1 Z");
    // 直线 / 连接符：OOXML line 为 (0,0)->(w,h) 对角线，翻转由渲染层处理
    reg("line",              "M 0,0 L 1,1");
    reg("straightConnector1","M 0,0 L 1,1");
    reg("bentConnector2",    "M 0,0 L 1,0 L 1,1");
    reg("bentConnector3",    "M 0,0.5 L 0.5,0.5 L 0.5,1 L 1,1");
    reg("curvedConnector3",  "M 0,0.5 C 0.35,0.5 0.35,1 0.65,1 C 0.85,1 0.85,0.75 1,0.75");
}

const char* preset_path(const std::string& prst) {
    init();
    auto it = g_paths.find(to_lower(prst));
    return it != g_paths.end() ? it->second.c_str() : nullptr;
}

PresetMeta preset_meta(const std::string& prst) {
    init();
    PresetMeta m = { nullptr, 0, {0, 0, 0} };
    auto it = g_paths.find(to_lower(prst));
    if (it == g_paths.end()) return m;
    m.path = it->second.c_str();
    if (prst == "roundRect") { m.n_adjust = 1; m.adjust[0] = 16667; }         // 约 1/6 圆角
    else if (prst == "trapezoid") { m.n_adjust = 1; m.adjust[0] = 0.2; }
    else if (prst == "homePlate") { m.n_adjust = 1; m.adjust[0] = 0.38; }
    return m;
}

} // namespace ppt