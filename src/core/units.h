#pragma once
// ============================================================
//  单位换算 + 颜色工具
//  OOXML 长度单位：EMU（English Metric Unit）
//    914400 EMU = 1 inch, 12700 EMU = 1 pt, 9525 EMU = 1 px(96dpi)
//  HTML 画布基准：把幻灯片缩放到 1280 x 720 CSS px（16:9）
// ============================================================

#include <string>
#include <cstdint>

namespace ppt {

static const double EMU_PER_INCH = 914400.0;
static const double EMU_PER_PT = 12700.0;
static const double EMU_PER_PX = 9525.0;      // 96 dpi

inline double emu_to_pt(double e) { return e / EMU_PER_PT; }
inline double pt_to_emu(double p) { return p * EMU_PER_PT; }
inline double emu_to_px(double e, double scale) { return e * scale; }

// 把颜色 / 尺寸从字符串解析
struct ARGB {
    uint8_t a = 255, r = 0, g = 0, b = 0;
    ARGB() {}
    ARGB(uint8_t aa, uint8_t rr, uint8_t gg, uint8_t bb) : a(aa), r(rr), g(gg), b(bb) {}
    static ARGB from_hex(const std::string& hex, uint8_t alpha = 255);   // "RRGGBB"/"#RRGGBB"
    std::string to_hex() const;                                          // "RRGGBB"
    std::string to_css(bool with_alpha = true) const;                     // "#rrggbb" 或 "rgba()"
    bool visible() const { return a != 0; }
};

inline bool operator==(const ARGB& x, const ARGB& y) {
    return x.a == y.a && x.r == y.r && x.g == y.g && x.b == y.b;
}

// 亮度调整（对应 OOXML 的 lum / tint / shade / lumMod / lumOff）
ARGB col_lum_mod(const ARGB& c, double pct);
ARGB col_lum_off(const ARGB& c, double pct);
ARGB col_tint(const ARGB& c, double pct);
ARGB col_shade(const ARGB& c, double pct);
ARGB col_sat(const ARGB& c, double pct);
ARGB col_gamma(const ARGB& c, double pct);
ARGB col_inv(const ARGB& c);
ARGB col_blend_white(const ARGB& base, const ARGB& fill);   // 正片叠底前的 alpha 合成

// 预定义颜色名（OOXML prstClr）
bool prst_color(const std::string& name, ARGB& out);

} // namespace ppt
