#include "units.h"
#include "../base/strutil.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <map>
#include <algorithm>

namespace ppt {

// 内部：把 0xRRGGBB 转成 "RRGGBB"
static std::string fmt_hex6(uint32_t v) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%06X", v & 0xFFFFFFu);
    return std::string(buf);
}

ARGB ARGB::from_hex(const std::string& hex, uint8_t alpha) {
    std::string h = hex;
    if (!h.empty() && h[0] == '#') h.erase(0, 1);
    unsigned v = 0;
    if (h.size() == 3) {
        char buf[7];
        buf[0] = buf[1] = h[0]; buf[2] = buf[3] = h[1]; buf[4] = buf[5] = h[2]; buf[6] = 0;
        h = buf;
    }
    if (h.size() == 8) {                       // AARRGGBB
        v = (unsigned)strtoul(h.c_str() + 2, nullptr, 16);
        alpha = (uint8_t)strtoul(h.substr(0, 2).c_str(), nullptr, 16);
    } else {
        v = (unsigned)strtoul(h.c_str(), nullptr, 16);
    }
    ARGB c;
    c.a = alpha;
    c.r = (uint8_t)((v >> 16) & 0xFF);
    c.g = (uint8_t)((v >> 8) & 0xFF);
    c.b = (uint8_t)(v & 0xFF);
    return c;
}

std::string ARGB::to_hex() const {
    char buf[8];
    snprintf(buf, sizeof(buf), "%02X%02X%02X", r, g, b);
    return std::string(buf);
}

std::string ARGB::to_css(bool with_alpha) const {
    if (!with_alpha || a == 255) {
        char buf[8];
        snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
        return std::string(buf);
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "rgba(%d,%d,%d,%.3f)", r, g, b, (double)a / 255.0);
    std::string s = buf;
    // 去掉多余尾 0
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s.size() > 6 && ends_with(s, ",1)")) { s = s.substr(0, s.size() - 3) + ")"; }
    return s;
}

// ---------------- HSL 转换（OOXML lum 基于 HLS 模型） ----------------
namespace {
struct HSL { double h, s, l; };

HSL rgb2hsl(const ARGB& c) {
    double r = c.r / 255.0, g = c.g / 255.0, b = c.b / 255.0;
    double mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
    HSL o{};
    o.l = (mx + mn) / 2.0;
    if (mx == mn) { o.h = o.s = 0; return o; }
    double d = mx - mn;
    o.s = o.l > 0.5 ? d / (2.0 - mx - mn) : d / (mx + mn);
    if (mx == r) o.h = (g - b) / d + (g < b ? 6 : 0);
    else if (mx == g) o.h = (b - r) / d + 2;
    else o.h = (r - g) / d + 4;
    o.h /= 6.0;
    return o;
}

inline double hue2rgb(double p, double q, double t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
    if (t < 1.0 / 2.0) return q;
    if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
    return p;
}

ARGB hsl2rgb(double h, double s, double l, uint8_t a) {
    double r, g, b;
    if (s == 0) { r = g = b = l; }
    else {
        double q = l < 0.5 ? l * (1 + s) : l + s - l * s;
        double p = 2 * l - q;
        r = hue2rgb(p, q, h + 1.0 / 3.0);
        g = hue2rgb(p, q, h);
        b = hue2rgb(p, q, h - 1.0 / 3.0);
    }
    auto cl = [](double v) {
        if (v < 0) return 0;
        if (v > 1) return 255;
        return (int)(v * 255.0 + 0.5);
    };
    return ARGB(a, (uint8_t)cl(r), (uint8_t)cl(g), (uint8_t)cl(b));
}

double clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
} // namespace

ARGB col_lum_mod(const ARGB& c, double pct) {           // pct 为百分比（100 = 不变）
    HSL h = rgb2hsl(c);
    h.l = clamp01(h.l * (pct / 100.0));
    ARGB o = hsl2rgb(h.h, h.s, h.l, c.a);
    return o;
}

ARGB col_lum_off(const ARGB& c, double pct) {           // pct 可为负
    HSL h = rgb2hsl(c);
    double off = pct / 100.0;
    h.l = clamp01(h.l + off * (off < 0 ? h.l : (1.0 - h.l)));
    return hsl2rgb(h.h, h.s, h.l, c.a);
}

ARGB col_tint(const ARGB& c, double pct) {
    double t = clamp01(pct / 100.0);
    auto mix = [t](int v) { return (int)(v + (255 - v) * (1.0 - t) + 0.5); };
    return ARGB(c.a, (uint8_t)mix(c.r), (uint8_t)mix(c.g), (uint8_t)mix(c.b));
}

ARGB col_shade(const ARGB& c, double pct) {
    double t = clamp01(pct / 100.0);
    auto mix = [t](int v) { return (int)(v * t + 0.5); };
    return ARGB(c.a, (uint8_t)mix(c.r), (uint8_t)mix(c.g), (uint8_t)mix(c.b));
}

ARGB col_sat(const ARGB& c, double pct) {
    HSL h = rgb2hsl(c);
    h.s = clamp01(h.s * (pct / 100.0));
    return hsl2rgb(h.h, h.s, h.l, c.a);
}

ARGB col_gamma(const ARGB& c, double pct) {
    double g = pct / 100.0;
    if (g <= 0) g = 1;
    auto f = [g](int v) { return (int)(pow(v / 255.0, g) * 255.0 + 0.5); };
    return ARGB(c.a, (uint8_t)f(c.r), (uint8_t)f(c.g), (uint8_t)f(c.b));
}

ARGB col_inv(const ARGB& c) { return ARGB(c.a, (uint8_t)(255 - c.r), (uint8_t)(255 - c.g), (uint8_t)(255 - c.b)); }

ARGB col_blend_white(const ARGB& base, const ARGB& fill) {
    double a = fill.a / 255.0;
    auto m = [a](int b, int f) { return (int)(b * (1 - a) + f * a + 0.5); };
    return ARGB(base.a, (uint8_t)m(base.r, fill.r), (uint8_t)m(base.g, fill.g), (uint8_t)m(base.b, fill.b));
}

bool prst_color(const std::string& name, ARGB& out) {
    static const std::map<std::string, uint32_t> tbl = {
        {"black", 0x000000}, {"blue", 0x0000FF}, {"light1", 0xFFFFFF}, {"light2", 0xEFEFEF},
        {"dark1", 0x000000}, {"dark2", 0x44546A}, {"gray", 0x808080}, {"green", 0x00FF00},
        {"light4", 0xBFBFBF}, {"light3", 0xD7D7D7}, {"dark4", 0x333333}, {"dark3", 0x808080},
        {"red", 0xFF0000}, {"white", 0xFFFFFF}, {"yellow", 0xFFFF00}, {"mint", 0x00FFFF},
        {"turquoise", 0x00FFFF}, {"lime", 0x00FF00}, {"fuchsia", 0xFF00FF}, {"purple", 0xFF00FF},
        {"aqua", 0x00FFFF}, {"silver", 0xC0C0C0}, {"teal", 0x008080}, {"olive", 0x808000},
        {"maroon", 0x800000}, {"navy", 0x000080}, {"orange", 0xFFC000}, {"rose", 0xFF007F},
        {"brown", 0x800000}, {"gold", 0xFFD700}, {"plum", 0x800080}, {"salmon", 0xFF8080},
        {"coral", 0xFF7F50}, {"khaki", 0xF0E68C}, {"sienna", 0xA0522D}
    };
    auto it = tbl.find(name);
    if (it == tbl.end()) return false;
    out = ARGB::from_hex(fmt_hex6(it->second));
    return true;
}

} // namespace ppt
