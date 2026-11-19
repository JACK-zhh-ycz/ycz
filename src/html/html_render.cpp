#include "html_render.h"
#include "../base/strutil.h"
#include "../core/preset_geom.h"
#include "../plugin/plugin_host.h"

#include <cstdio>
#include <ctime>
#include <algorithm>
#include <cmath>

namespace ppt {
namespace {

static const double PI = 3.141592653589793;

// ======================= 渲染上下文 =======================
// 坐标：EMU -> CSS px 按画布等比换算；字号由 pt 按高度比例换算
struct Ctx {
    long long sw = 12192000, sh = 6858000;   // 幻灯片尺寸 EMU
    double sx = 1280, sy = 720;              // 画布尺寸 px
    const RenderTheme* rt = nullptr;
    const Presentation* pres = nullptr;
    int slide_idx = 0, slide_total = 1;      // 页码字段用
    std::string date_str;                    // 日期字段用
    int uid = 0;                             // SVG 渐变 id 计数
};
static double pxW(Ctx& c, long long e) { return c.sw ? (double)e / c.sw * c.sx : 0; }
static double pxH(Ctx& c, long long e) { return c.sh ? (double)e / c.sh * c.sy : 0; }
static double ptToPx(Ctx& c, double pt) {
    return c.sh ? pt * 12700.0 / (double)c.sh * c.sy : pt * 1.333;
}

// ======================= 填充 =======================
static std::string stop_css(const GradientStop& st) {
    ARGB col = st.color;
    if (st.alpha < 100) col.a = (uint8_t)((double)col.a * st.alpha / 100.0 + 0.5);
    return col.to_css() + " " + fmt_num(st.pos * 100, 2) + "%";
}

static std::string grad_css(const Fill& f) {
    if (f.stops.empty()) return "#FFFFFF";
    std::string stops;
    for (size_t i = 0; i < f.stops.size(); ++i) {
        if (i) stops += ", ";
        stops += stop_css(f.stops[i]);
    }
    if (f.grad_kind == GradKind::Linear)
        // OOXML ang：0=向右、顺时针；CSS：0=向上、顺时针
        return "linear-gradient(" + fmt_num(f.ang + 90.0, 1) + "deg, " + stops + ")";
    if (f.grad_kind == GradKind::Radial)
        return "radial-gradient(ellipse at center, " + stops + ")";
    return "linear-gradient(" + stops + ")";
}

static std::string css_fill(const Fill& f) {
    if (f.is_none()) return "transparent";
    switch (f.kind) {
        case FillKind::Solid:    return f.solid.to_css();
        case FillKind::Gradient: return grad_css(f);
        case FillKind::Pattern: {
            std::string a = f.pat_fg.to_css(), b = f.pat_bg.to_css();
            return "repeating-linear-gradient(45deg, " + a + " 0 2px, " + b + " 2px 4px)";
        }
        default: return "transparent";
    }
}

// ======================= 线条 =======================
static const char* dash_word(DashType d) {
    switch (d) {
        case DashType::SquareDot:
        case DashType::RoundDot: return "dotted";
        case DashType::Dash: case DashType::DashDot: case DashType::LongDash:
        case DashType::LongDashDot: case DashType::LongDashDotDot: return "dashed";
        default: return "solid";
    }
}
static const char* svg_dash(DashType d) {
    switch (d) {
        case DashType::SquareDot: case DashType::RoundDot: return "2,3";
        case DashType::Dash: return "8,5";
        case DashType::DashDot: return "8,4,1,4";
        case DashType::LongDash: return "14,5";
        case DashType::LongDashDot: return "14,5,1,5";
        case DashType::LongDashDotDot: return "14,5,1,5,1,5";
        default: return "";
    }
}
static std::string css_line(Ctx& c, const Line& l) {
    if (l.is_none() || l.w <= 0) return "none";
    double px = ptToPx(c, emu_to_pt((double)l.w));
    if (px < 0.5) px = 0.5;
    return fmt_num(px, 2) + "px " + dash_word(l.dash) + " " + l.color.to_css();
}

// ======================= 效果（阴影/发光/柔化边） =======================
static std::string css_effects(Ctx& c, const Effect& fx) {
    std::string out;
    auto add = [&](const std::string& v) { if (!v.empty()) { if (!out.empty()) out += ", "; out += v; } };
    const Shadow& s = fx.shadow;
    if (s.on && s.alpha > 0) {
        ARGB col = s.color; col.a = (uint8_t)((double)col.a * s.alpha / 100.0 + 0.5);
        double d = pxW(c, (long long)s.dist);
        double rad = s.dir * PI / 180.0;              // OOXML: 0=右, 顺时针
        add(fmt_num(d * std::cos(rad), 1) + "px " + fmt_num(d * std::sin(rad), 1) + "px " +
            fmt_num(pxW(c, (long long)s.blur_rad), 1) + "px " + col.to_css());
    }
    if (fx.glow) {
        ARGB col = fx.glow_color; col.a = (uint8_t)((double)col.a * fx.glow_alpha / 100.0 + 0.5);
        add("0 0 " + fmt_num(pxW(c, (long long)fx.glow_rad), 1) + "px " + col.to_css());
    }
    if (fx.soft_edge)
        add("0 0 " + fmt_num(pxW(c, (long long)fx.soft_rad), 1) + "px rgba(0,0,0,.28)");
    return out;
}

// ======================= 圆角 =======================
static double round_emu(const Shape& s) {
    double adj = 0.16667;
    auto it = s.prst_adj.find("adj");
    if (it != s.prst_adj.end() && it->second > 0) adj = it->second;
    adj = std::min(adj, 0.5);
    return adj * std::min((double)s.cx, (double)s.cy);
}

// ======================= 文本 =======================
static std::string font_stack(const ResolvedRun& rr) {
    std::string out;
    if (!rr.font.empty())    out += "\"" + rr.font + "\", ";
    if (!rr.font_ea.empty()) out += "\"" + rr.font_ea + "\", ";
    out += "\"Microsoft YaHei\", \"PingFang SC\", \"Segoe UI\", sans-serif";
    return out;
}

struct NumState { int n[9] = {0,0,0,0,0,0,0,0,0}; };

static std::string autonum_text(const BulletProps& b, NumState& ns, int lvl) {
    if (lvl < 0 || lvl > 8) lvl = 0;
    ns.n[lvl]++;
    for (int i = lvl + 1; i <= 8; ++i) ns.n[i] = 0;
    int v = ns.n[lvl];
    const std::string& fmt = b.num_arabic;
    std::string num;
    if (fmt.find("roman") != std::string::npos || fmt.find("Roman") != std::string::npos) {
        static const char* R[13] = {"","I","II","III","IV","V","VI","VII","VIII","IX","X","XI","XII"};
        num = (v >= 1 && v <= 12) ? R[v] : std::to_string(v);
    } else if (fmt.find("alpha") != std::string::npos || fmt.find("Alpha") != std::string::npos) {
        num = (v >= 1 && v <= 26) ? std::string(1, (char)('A' + v - 1)) : std::to_string(v);
    } else num = std::to_string(v);
    if (fmt.find("ParenR") != std::string::npos) return num + ")";
    if (fmt.find("ParenC") != std::string::npos) return "(" + num + ")";
    if (fmt.find("NoEnd")  != std::string::npos) return num;
    return num + ".";
}

static std::string field_value(Ctx& c, const std::string& text) {
    if (text == "{slide_num}") return std::to_string(c.slide_idx + 1);
    if (text == "{date}")      return c.date_str;
    return text;
}

static std::string text_span(Ctx& c, const Shape& owner,
                             const TextParagraph& para, const TextRun& run) {
    std::string raw = field_value(c, run.text);
    if (raw.empty()) return "";
    ResolvedRun rr = c.rt->resolve_run(owner, para, run);

    std::string st = "font-size:" + fmt_num(ptToPx(c, rr.sz_pt), 1) + "px;";
    st += "font-family:" + font_stack(rr) + ";";
    if (rr.b) st += "font-weight:700;";
    if (rr.i) st += "font-style:italic;";
    std::string deco;
    if (rr.u)      deco += " underline";
    if (rr.strike) deco += " line-through";
    if (!deco.empty()) st += "text-decoration:" + deco.substr(1) + ";";
    st += "color:" + rr.color.to_css() + ";";
    if (run.props.spc_set && run.props.spc != 0)
        st += "letter-spacing:" + fmt_num(ptToPx(c, run.props.spc), 1) + "px;";
    if (run.props.baseline_set && run.props.baseline != 0) {
        st += run.props.baseline > 0 ? "vertical-align:super;" : "vertical-align:sub;";
        st += "font-size:" + fmt_num(ptToPx(c, rr.sz_pt * 0.58), 1) + "px;";
    }

    std::string esc = html_escape(raw);
    for (size_t p = 0; (p = esc.find('\n', p)) != std::string::npos; p += 4)
        esc.replace(p, 1, "<br>");

    std::string s = "<span style=\"" + st + "\">" + esc + "</span>";
    if (!rr.link.empty() && rr.link.find("://") != std::string::npos)
        s = "<a href=\"" + html_escape(rr.link) + "\" target=\"_blank\" rel=\"noopener\">" + s + "</a>";
    return s;
}

static std::string text_html(Ctx& c, const Shape& owner) {
    const TextBody& tb = owner.text;
    if (tb.empty()) return "";
    NumState ns;
    std::string inner;
    for (const TextParagraph& para : tb.paras) {
        std::string st = "margin:0;";
        if (para.props.algn_set) {
            const char* a = "left";
            switch (para.props.algn) {
                case AlignL::Center: a = "center"; break;
                case AlignL::Right:  a = "right";  break;
                case AlignL::Justify: case AlignL::Distributed: case AlignL::JustifyLow:
                    a = "justify"; break;
                default: break;
            }
            st += std::string("text-align:") + a + ";";
        }
        double left_pad = 0;
        if (para.props.marL_set) {
            long long marL = para.props.marL + para.props.indent;
            if (marL > 0) left_pad = ptToPx(c, emu_to_pt((double)marL));
        }
        if (para.props.lvl_set && para.props.lvl > 0)
            left_pad += ptToPx(c, 18.0) * para.props.lvl;
        if (left_pad > 0) st += "padding-left:" + fmt_num(left_pad, 1) + "px;";
        if (para.props.spcbef_set && para.props.spcbef > 0)
            st += "margin-top:" + fmt_num(ptToPx(c, para.props.spcbef / 100.0), 1) + "px;";
        if (para.props.spcaft_set && para.props.spcaft > 0)
            st += "margin-bottom:" + fmt_num(ptToPx(c, para.props.spcaft / 100.0), 1) + "px;";
        double lh = 1.22;
        if (para.props.lnspc_set) {
            lh = para.props.lnspc_pts ? para.props.lnspc_pts / 100.0
                                      : para.props.lnspc_pct / 100.0;
            if (tb.lnSpcReduction > 0) lh -= tb.lnSpcReduction / 100.0;
            if (lh < 0.7) lh = 0.7;
        }
        st += "line-height:" + fmt_num(lh, 2) + ";";
        if (para.props.rtl || tb.rtl) st += "direction:rtl;";

        bool has_text = false;
        for (const TextRun& r : para.runs) if (!r.text.empty()) { has_text = true; break; }

        std::string bullet;
        if (para.bullet.kind == BulletKind::Char) {
            ARGB bc = para.bullet.color_set ? para.bullet.color : ARGB(255, 0x66, 0x66, 0x66);
            bullet = "<span class=\"bullet\" style=\"color:" + bc.to_css() + ";margin-right:.45em\">" +
                     html_escape(para.bullet.char_) + "</span>";
        } else if (para.bullet.kind == BulletKind::AutoNum) {
            if (has_text) {
                std::string num = autonum_text(para.bullet, ns, para.props.lvl);
                bullet = "<span class=\"bullet\" style=\"margin-right:.45em\">" + html_escape(num) + "</span>";
            } else {
                for (int i = para.props.lvl + 1; i <= 8; ++i) ns.n[i] = 0;
            }
        }

        std::string runs;
        for (const TextRun& r : para.runs) runs += text_span(c, owner, para, r);
        if (runs.empty() && bullet.empty()) runs = "<br>";
        inner += "<p style=\"" + st + "\">" + bullet + runs + "</p>";
    }

    std::string wrap = "<div class=\"tx\" style=\"";
    wrap += "position:relative;width:100%;";
    wrap += "padding:" + fmt_num(ptToPx(c, emu_to_pt((double)tb.tIns)), 1) + "px " +
            fmt_num(ptToPx(c, emu_to_pt((double)tb.rIns)), 1) + "px " +
            fmt_num(ptToPx(c, emu_to_pt((double)tb.bIns)), 1) + "px " +
            fmt_num(ptToPx(c, emu_to_pt((double)tb.lIns)), 1) + "px;";
    if (!tb.wrap) wrap += "white-space:nowrap;";
    if (tb.vert) {
        if (tb.vert_dir == "vert270" || tb.vert_dir == "vert270R")
            wrap += "writing-mode:vertical-rl;transform:rotate(180deg);";
        else
            wrap += "writing-mode:vertical-rl;";
    }
    if (tb.anchor_set && tb.anchor != VAlign::Top) {
        wrap += "display:flex;flex-direction:column;justify-content:" +
                std::string(tb.anchor == VAlign::Center ? "center" : "flex-end") + ";";
    }
    wrap += "\">" + inner + "</div>";
    return wrap;
}

// ======================= 表格 =======================
static ARGB mix_alpha(const ARGB& col, double pct) {
    ARGB o = col; o.a = (uint8_t)((double)o.a * pct / 100.0 + 0.5); return o;
}

static std::string table_html(Ctx& c, const Shape& owner) {
    const Table& t = *owner.table;
    if (t.cells.empty()) return "";
    const Theme* th = c.rt->theme;
    ARGB accent = th ? th->colors.accent[0] : ARGB(255, 0x44, 0x72, 0xC4);
    ARGB tx1 = th ? th->colors.dk1 : ARGB(255, 0, 0, 0);

    std::vector<std::vector<char>> skip;
    skip.resize(t.cells.size());
    for (size_t r = 0; r < t.cells.size(); ++r) skip[r].resize(t.cells[r].size(), 0);

    std::string html = "<table class=\"pptx-table\" style=\"border-collapse:separate;border-spacing:0;"
                       "width:100%;height:100%;table-layout:fixed;font-size:" +
                       fmt_num(ptToPx(c, 14), 1) + "px;color:" + tx1.to_css() + ";\">";
    html += "<colgroup>";
    for (size_t col = 0; col < t.cols.size(); ++col)
        html += "<col style=\"width:" + fmt_num(pxW(c, t.cols[col]), 1) + "px;\">";
    html += "</colgroup>";

    Shape cell_owner = owner;               // 单元格文字不走占位符默认
    cell_owner.ph_type = PhType::Other;

    for (size_t r = 0; r < t.cells.size(); ++r) {
        std::string trst;
        if (r < t.rows.size() && t.rows[r] > 0)
            trst = " style=\"height:" + fmt_num(pxH(c, t.rows[r]), 1) + "px;\">";
        else trst = ">";
        html += "<tr" + trst;
        for (size_t col = 0; col < t.cells[r].size(); ++col) {
            if (skip[r][col]) continue;
            const TableCell& cell = t.cells[r][col];
            if (cell.merge_h || cell.merge_v) continue;      // 延续格
            int cs = (int)std::max<long long>(1, cell.span_h);
            int rs = (int)std::max<long long>(1, cell.span_v);
            for (int dr = 0; dr < rs; ++dr) {
                if (r + dr >= skip.size()) break;
                for (int dc = 0; dc < cs; ++dc) {
                    if (col + dc >= skip[r + dr].size()) break;
                    if (dr || dc) skip[r + dr][col + dc] = 1;
                }
            }

            std::string st;
            const char* side[4] = {"border-left:", "border-right:", "border-top:", "border-bottom:"};
            for (int bi = 0; bi < 4; ++bi) {
                if (cell.has_border[bi] && cell.border_w[bi] > 0) {
                    double px = ptToPx(c, emu_to_pt((double)cell.border_w[bi]));
                    if (px < 0.5) px = 0.5;
                    st += side[bi] + fmt_num(px, 1) + "px " + dash_word(cell.border_dash[bi]) + " " +
                          cell.border_color[bi].to_css() + ";";
                } else st += std::string(side[bi]) + "none;";
            }
            std::string bg;
            if (!cell.fill.is_inherit() && !cell.fill.is_none()) bg = css_fill(cell.fill);
            else if (t.first_row && r == 0) bg = accent.to_css();
            else if (t.band_row && r > 0 && (r % 2) == 0) bg = mix_alpha(accent, 20).to_css();
            if (!bg.empty()) st += "background:" + bg + ";";
            if (t.first_row && r == 0 && cell.fill.is_inherit())
                st += "font-weight:700;color:#fff;";
            st += std::string("vertical-align:") +
                  (cell.anchor == VAlign::Center ? "middle" :
                   (cell.anchor == VAlign::Bottom ? "bottom" : "top")) + ";";
            double ml = cell.mar_set ? ptToPx(c, emu_to_pt((double)cell.marL)) : 5;
            double mr = cell.mar_set ? ptToPx(c, emu_to_pt((double)cell.marR)) : 5;
            double mt = cell.mar_set ? ptToPx(c, emu_to_pt((double)cell.marT)) : 3;
            double mb = cell.mar_set ? ptToPx(c, emu_to_pt((double)cell.marB)) : 3;
            st += "padding:" + fmt_num(mt, 1) + "px " + fmt_num(mr, 1) + "px " +
                  fmt_num(mb, 1) + "px " + fmt_num(ml, 1) + "px;box-sizing:border-box;overflow:hidden;";

            std::string attrs;
            if (cs > 1) attrs += " colspan=\"" + std::to_string(cs) + "\"";
            if (rs > 1) attrs += " rowspan=\"" + std::to_string(rs) + "\"";
            cell_owner.text = cell.text;
            html += "<td" + attrs + " style=\"" + st + "\">" + text_html(c, cell_owner) + "</td>";
        }
        html += "</tr>";
    }
    html += "</table>";
    return html;
}

// ======================= 预设几何 -> SVG =======================
static std::string svg_grad_def(Ctx& c, const Fill& f, int uid) {
    std::string id = "g" + std::to_string(uid);
    std::string stops;
    for (const GradientStop& st : f.stops) {
        ARGB col = st.color;
        if (st.alpha < 100) col.a = (uint8_t)((double)col.a * st.alpha / 100.0 + 0.5);
        std::string op = col.a < 255 ? " stop-opacity=\"" + fmt_num(col.a / 255.0, 3) + "\"" : "";
        stops += "<stop offset=\"" + fmt_num(st.pos, 4) + "\" stop-color=\"#" + col.to_hex() + "\"" + op + "/>";
    }
    if (f.grad_kind == GradKind::Radial)
        return "<radialGradient id=\"" + id + "\">" + stops + "</radialGradient>";
    double rad = f.ang * PI / 180.0;
    double x1 = 0.5 - 0.5 * std::cos(rad), y1 = 0.5 - 0.5 * std::sin(rad);
    double x2 = 0.5 + 0.5 * std::cos(rad), y2 = 0.5 + 0.5 * std::sin(rad);
    char buf[160];
    snprintf(buf, sizeof(buf), " x1=\"%.4f\" y1=\"%.4f\" x2=\"%.4f\" y2=\"%.4f\"", x1, y1, x2, y2);
    return "<linearGradient id=\"" + id + "\"" + buf + ">" + stops + "</linearGradient>";
}

// 返回空串表示该形状没有可用的 SVG 轮廓（调用方回退到 div）
static std::string geom_svg(Ctx& c, const Shape& s) {
    std::string d;
    if (!s.svg_path.empty()) d = s.svg_path;
    else {
        const char* p = preset_path(s.prst_geom);
        if (!p) return "";
        d = p;
        double adj = 0.16667;
        auto it = s.prst_adj.find("adj");
        if (it != s.prst_adj.end() && it->second > 0) adj = std::min(it->second, 0.5);
        replace_all(d, "{1-adj}", fmt_num(1.0 - adj, 4));
        replace_all(d, "{adj}", fmt_num(adj, 4));
        if (d.find('{') != std::string::npos) return "";   // 仍有未替换占位
    }
    double w = pxW(c, s.cx), h = pxH(c, s.cy);
    bool degenerate = (w <= 0 || h <= 0);
    // 线条/连接符的包围盒可能有一个方向为 0：此时不能用 0..1 归一化 viewBox
    if (degenerate) {
        bool is_line = s.prst_geom == "line" || starts_with(s.prst_geom, "straightConnector") ||
                       starts_with(s.prst_geom, "bentConnector") || starts_with(s.prst_geom, "curvedConnector");
        if (!is_line) return "";
        if (w < 1) w = 1;
        if (h < 1) h = 1;
    }

    std::string fill_attr = "none", defs;
    if (s.fill.kind == FillKind::Solid) fill_attr = s.fill.solid.to_css();
    else if (s.fill.kind == FillKind::Gradient && !s.fill.stops.empty()) {
        int uid = ++c.uid;
        defs = "<defs>" + svg_grad_def(c, s.fill, uid) + "</defs>";
        fill_attr = "url(#g" + std::to_string(uid) + ")";
    } else if (s.fill.kind == FillKind::Pattern) fill_attr = css_fill(s.fill);

    std::string stroke = "none", sw = "0", sdash, cap = "butt";
    if (!s.line.is_none() && s.line.w > 0) {
        stroke = s.line.color.to_css();
        sw = fmt_num(std::max(ptToPx(c, emu_to_pt((double)s.line.w)), 0.5), 2);
        sdash = svg_dash(s.line.dash);
        if (s.line.cap == LineCap::Round) cap = "round";
        else if (s.line.cap == LineCap::Square) cap = "square";
    }
    std::string extra = " vector-effect=\"non-scaling-stroke\"";
    if (!sdash.empty()) extra += " stroke-dasharray=\"" + sdash + "\"";

    std::string svg;
    if (degenerate) {
        // 像素坐标直绘：flipH/flipV 决定端点方向
        double x1 = 0, y1 = 0, x2 = w, y2 = h;
        if (s.flip_h) { x1 = w; x2 = 0; }
        if (s.flip_v) { y1 = h; y2 = 0; }
        d = "M" + fmt_num(x1, 2) + "," + fmt_num(y1, 2) + " L" + fmt_num(x2, 2) + "," + fmt_num(y2, 2);
        svg = "<svg class=\"geom\" width=\"" + fmt_num(w, 2) + "\" height=\"" + fmt_num(h, 2) +
              "\" viewBox=\"0 0 " + fmt_num(w, 2) + " " + fmt_num(h, 2) + "\" "
              "style=\"position:absolute;left:0;top:0;overflow:visible\">" + defs +
              "<path d=\"" + d + "\" fill=\"none\" stroke=\"" + stroke +
              "\" stroke-width=\"" + sw + "\"" + extra +
              " stroke-linecap=\"" + cap + "\"/>";
        svg += "</svg>";
        return svg;
    }

    svg = "<svg class=\"geom\" width=\"" + fmt_num(w, 2) + "\" height=\"" + fmt_num(h, 2) +
          "\" viewBox=\"0 0 1 1\" preserveAspectRatio=\"none\" "
          "style=\"position:absolute;left:0;top:0;overflow:visible\">" + defs +
          "<path d=\"" + d + "\" fill=\"" + fill_attr + "\" stroke=\"" + stroke +
          "\" stroke-width=\"" + sw + "\"" + extra +
          " stroke-linecap=\"" + cap + "\" stroke-linejoin=\"" +
          (s.line.join == 0 ? "round" : (s.line.join == 1 ? "bevel" : "miter")) + "\"/>";
    svg += "</svg>";
    return svg;
}

// ======================= 形状 =======================
// 组合子坐标空间 -> 幻灯片 EMU 坐标
static Shape map_child(const Shape& child, const Shape& grp) {
    Shape m = child;
    if (grp.ch_ext_cx > 0 && grp.ch_ext_cy > 0) {
        m.x  = grp.x + (long long)((double)(child.x - grp.ch_off_x) * grp.cx / grp.ch_ext_cx);
        m.y  = grp.y + (long long)((double)(child.y - grp.ch_off_y) * grp.cy / grp.ch_ext_cy);
        m.cx = (long long)((double)child.cx * grp.cx / grp.ch_ext_cx);
        m.cy = (long long)((double)child.cy * grp.cy / grp.ch_ext_cy);
    }
    m.rot += grp.rot;
    m.flip_h = child.flip_h != grp.flip_h;
    m.flip_v = child.flip_v != grp.flip_v;
    return m;
}

static std::string bg_image_style(const Shape& s) {
    if (s.fill.kind == FillKind::Blip && !s.fill.blip_part.empty()) {
        std::string size = s.fill.fill_mode == FillMode::Tile ? "auto" : "100% 100%";
        std::string rep  = s.fill.fill_mode == FillMode::Tile ? "repeat" : "no-repeat";
        return "background-image:url('DATA:" + html_escape(s.fill.blip_part) + "');" +
               "background-size:" + size + ";background-repeat:" + rep + ";";
    }
    return "";
}

static bool is_line_geom(const Shape& s) {
    return s.prst_geom == "line" || starts_with(s.prst_geom, "straightConnector") ||
           starts_with(s.prst_geom, "bentConnector") || starts_with(s.prst_geom, "curvedConnector");
}

static void shape_blocks(Ctx& c, const Shape& s_in, std::string& out,
                         int anim_idx = -1, const std::string& anim = std::string(),
                         long long anim_dur = 500) {
    (void)anim_idx;
    if (s_in.hidden) return;
    Shape s = s_in;
    // 占位符几何继承（只作用于副本）
    if (s.cx <= 0 || s.cy <= 0) {
        long long x, y, cx, cy;
        if (c.rt->placeholder_geometry(s, x, y, cx, cy)) { s.x = x; s.y = y; s.cx = cx; s.cy = cy; }
    }
    const bool is_group = s.is_group();
    // 线条类允许一个方向尺寸为 0（水平/垂直线）
    if (!is_group && !is_line_geom(s) && (s.cx <= 0 || s.cy <= 0)) return;
    if (!is_group && is_line_geom(s) && s.cx <= 0 && s.cy <= 0) return;
    // 主题 / 版式 / 母版样式继承
    s.fill = c.rt->resolve_fill(s);
    s.line = c.rt->resolve_line(s);
    s.fx = c.rt->resolve_effects(s);

    const bool is_pic   = s.is_pic();
    const bool is_table = s.is_table() && s.table;

    bool use_svg = false;
    std::string svg;
    if (!is_group && !is_pic && !is_table && s.kind != ShapeKind::Chart && !s.is_media()) {
        const std::string& g = s.prst_geom;
        bool is_rectish = (g == "rect" || g.empty() || g == "roundRect");
        if (!s.svg_path.empty() || starts_with(g, "line") || starts_with(g, "straightConnector") ||
            starts_with(g, "bentConnector") || starts_with(g, "curvedConnector") ||
            (g != "ellipse" && g != "oval" && !is_rectish)) {
            svg = geom_svg(c, s);
            use_svg = !svg.empty();
        }
    }

    std::string sty = "position:absolute;";
    sty += "left:" + fmt_num(pxW(c, s.x), 2) + "px;top:" + fmt_num(pxH(c, s.y), 2) + "px;";
    sty += "width:" + fmt_num(pxW(c, s.cx), 2) + "px;height:" + fmt_num(pxH(c, s.cy), 2) + "px;";
    std::string tf;
    if (s.rot != 0) tf += " rotate(" + fmt_num(s.rot, 2) + "deg)";
    if (s.flip_h) tf += " scaleX(-1)";
    if (s.flip_v) tf += " scaleY(-1)";
    if (!tf.empty()) sty += "transform:" + tf.substr(1) + ";transform-origin:center;";

    if (is_group) {
        // 组合容器本身不绘外观
        for (const ShapePtr& ch : s.children)
            shape_blocks(c, map_child(*ch, s), out, anim_idx, anim, anim_dur);
        return;
    }

    if (use_svg) {
        std::string imgst = bg_image_style(s);
        if (!imgst.empty()) sty += imgst;
        sty += "background:transparent;border:none;";
    } else {
        sty += "background:" + css_fill(s.fill) + ";";
        std::string imgst = bg_image_style(s);
        if (!imgst.empty()) sty += imgst;
        std::string ln = css_line(c, s.line);
        sty += (ln == "none") ? "border:none;" : ("border:" + ln + ";");
        if (s.prst_geom == "roundRect")
            sty += "border-radius:" + fmt_num(pxW(c, (long long)round_emu(s)), 1) + "px;";
        else if (s.prst_geom == "ellipse" || s.prst_geom == "oval")
            sty += "border-radius:50%;";
        if (s.is_pic()) sty += "background-color:transparent;";
    }
    std::string fx = css_effects(c, s.fx);
    if (!fx.empty()) sty += "box-shadow:" + fx + ";";
    sty += use_svg ? "overflow:visible;" : "overflow:hidden;";

    std::string cls = "shape";
    if (!anim.empty()) cls += " anim-item";
    std::string anim_attr;
    if (!anim.empty())
        anim_attr = " data-a=\"" + anim + "\" data-ad=\"" + std::to_string(anim_dur > 0 ? anim_dur : 500) + "\"";

    std::string inner;
    if (is_pic) {
        std::string ist = "position:absolute;left:0;top:0;width:100%;height:100%;object-fit:fill;max-width:none;";
        double l = s.src_rect_l, t = s.src_rect_t, r = s.src_rect_r, b = s.src_rect_b;
        if (l > 0 || t > 0 || r > 0 || b > 0) {           // 裁剪：放大图形并按比例反向偏移
            double fw = std::max(1.0 - l - r, 0.01), fh = std::max(1.0 - t - b, 0.01);
            ist = std::string("position:absolute;object-fit:fill;max-width:none;") +
                  "width:" + fmt_num(100.0 / fw, 2) + "%;height:" + fmt_num(100.0 / fh, 2) + "%;" +
                  "left:" + fmt_num(-l / fw * 100.0, 2) + "%;top:" + fmt_num(-t / fh * 100.0, 2) + "%;";
        }
        if (!s.pic_part.empty())
            inner += "<img src=\"DATA:" + html_escape(s.pic_part) + "\" style=\"" + ist + "\">";
        else inner += "<div class=\"chart-ph\">[图片]</div>";
        if (!s.text.paras.empty()) inner += text_html(c, s);
    } else if (s.is_chart()) {
        inner += "<div class=\"chart-ph\">[图表]</div>";
    } else if (s.is_media()) {
        inner += "<div class=\"chart-ph\">[" +
                 std::string(s.media_kind == "audio" ? "音频" : "视频") + "]</div>";
    } else if (is_table) {
        inner += table_html(c, s);
    } else {
        if (use_svg) inner += svg;
        if (!s.text.paras.empty()) inner += text_html(c, s);
    }

    out += "<div class=\"" + cls + "\" data-sid=\"" + std::to_string(s.id) + "\"" +
           anim_attr + " style=\"" + sty + "\">" + inner + "</div>\n";
}

// ======================= 幻灯片 =======================
// 版式/母版中的占位符，若本页（或更上层版式）已有同类型同 idx 的占位符，则由后者接管
static bool ph_taken(const std::vector<ShapePtr>& shapes, PhType t_, const std::string& idx) {
    for (const ShapePtr& sp : shapes)
        if (sp->is_place_holder && sp->ph_type == t_ && sp->ph_idx == idx) return true;
    return false;
}

static std::string bg_layer_html(Ctx& c, const Slide& sl) {
    std::string out;
    const SlideMasterDef* md = c.rt->master;
    const SlideLayoutDef* ld = c.rt->layout;
    bool show_master = sl.show_master_sp != "0";
    int anim = -1;                              // 背景层不参与进入动画
    if (show_master && md) {
        for (const ShapePtr& sp : md->shapes) {
            if (sp->is_place_holder) {
                if (ld && ph_taken(ld->shapes, sp->ph_type, sp->ph_idx)) continue;
                if (ph_taken(sl.shapes, sp->ph_type, sp->ph_idx)) continue;
            }
            shape_blocks(c, *sp, out, anim);
        }
    }
    if (ld) {
        for (const ShapePtr& sp : ld->shapes) {
            if (sp->is_place_holder) {
                if (ph_taken(sl.shapes, sp->ph_type, sp->ph_idx)) continue;
            }
            shape_blocks(c, *sp, out, anim);
        }
    }
    return out;
}

static std::string slide_html(Ctx& c, const Presentation& pres, const Slide& sl,
                              size_t idx, size_t total, const HtmlOptions& opt) {
    c.slide_idx = (int)idx; c.slide_total = (int)total;
    std::string bg = css_fill(c.rt->background_fill(sl));
    if (bg == "transparent") bg = "#FFFFFF";

    std::string html;
    html += "<div class=\"slide\" id=\"slide-" + std::to_string(idx) +
            "\" data-index=\"" + std::to_string(idx) + "\" style=\"width:" +
            fmt_num(c.sx, 2) + "px;height:" + fmt_num(c.sy, 2) + "px;background:" + bg + ";\">\n";

    // 母版装饰层
    {
        std::string layer = bg_layer_html(c, sl);
        if (!layer.empty()) html += "<div class=\"bg-layer\">" + layer + "</div>\n";
    }

    std::string anim_attr, tr_attr;
    if (opt.include_anim && !sl.trans.kind.empty() && sl.trans.kind != "none") {
        tr_attr = " data-tr=\"" + sl.trans.kind + "\" data-td=\"" +
                  std::to_string(sl.trans.dur_ms > 0 ? sl.trans.dur_ms : 600) +
                  "\" data-tdir=\"" + std::to_string(sl.trans.dir) + "\"";
    }
    if (!tr_attr.empty()) {
        size_t pos = html.find("style=\"");
        if (pos != std::string::npos) html.insert(pos, tr_attr.substr(1) + " ");
    }

    for (const ShapePtr& sp : sl.shapes) {
        std::string a_kind;
        long long a_dur = 500;
        if (opt.include_anim) {
            for (const AnimNode& an : sl.trans.seq) {
                if (an.target_id == sp->id && an.effect == AnimEffect::Entrance) {
                    a_kind = an.raw_prst.empty() ? "fade" : an.raw_prst;
                    a_dur = an.dur_ms > 0 ? an.dur_ms : 500;
                    break;
                }
            }
        }
        shape_blocks(c, *sp, html, -1, a_kind, a_dur);
    }

    html += "<div class=\"slide-num\">" + std::to_string(idx + 1) + " / " +
            std::to_string(total) + "</div>\n";
    if (opt.include_notes && !sl.notes.empty())
        html += "<div class=\"notes-data\" style=\"display:none;\">" + html_escape(sl.notes) + "</div>\n";

    // 插件：每页注入片段
    {
        std::string meta = "idx=" + std::to_string(idx) + "&total=" + std::to_string(total) +
                           "&title=" + sl.title_text();
        std::string frag;
        PluginHost::instance().html_slide(meta, frag);
        if (!frag.empty()) html += frag;
    }
    html += "</div>\n";
    return html;
}

// ======================= 内嵌媒体 =======================
static std::string mime_for(const std::string& part) {
    std::string ext = to_lower(extension_of(part));
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif")  return "image/gif";
    if (ext == ".svg")  return "image/svg+xml";
    if (ext == ".bmp")  return "image/bmp";
    if (ext == ".tiff" || ext == ".tif") return "image/tiff";
    if (ext == ".wmf")  return "image/wmf";
    return "image/png";
}

static std::string base64_of(const std::vector<uint8_t>& v) {
    static const char* L = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64;
    b64.reserve(((v.size() + 2) / 3) * 4 + 8);
    for (size_t i = 0; i < v.size(); i += 3) {
        uint32_t trip = (uint32_t)v[i] << 16;
        if (i + 1 < v.size()) trip |= (uint32_t)v[i + 1] << 8;
        if (i + 2 < v.size()) trip |= (uint32_t)v[i + 2];
        b64 += L[(trip >> 18) & 0x3F];
        b64 += L[(trip >> 12) & 0x3F];
        b64 += (i + 1 < v.size()) ? L[(trip >> 6) & 0x3F] : '=';
        b64 += (i + 2 < v.size()) ? L[trip & 0x3F] : '=';
    }
    return b64;
}

// 把所有 DATA:<部件名> 占位替换为 base64 data URL
static void embed_media_data(const Presentation& pres, std::string& html, bool embed) {
    const std::string blank = "data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7";
    if (embed) {
        for (const auto& kv : pres.media) {
            std::string token = "DATA:" + kv.first;
            std::string repl = "data:" + mime_for(kv.first) + ";base64," + base64_of(kv.second);
            size_t pos = 0;
            while ((pos = html.find(token, pos)) != std::string::npos) {
                html.replace(pos, token.size(), repl);
                pos += repl.size();
            }
        }
    }
    // 剩余未命中的占位符（媒体缺失）替换为透明占位图
    size_t p = 0;
    while ((p = html.find("DATA:", p)) != std::string::npos) {
        size_t e = p + 5;
        while (e < html.size() && html[e] != '\'' && html[e] != '"' && html[e] != ';') ++e;
        html.replace(p, e - p, blank);
        p += blank.size();
    }
}

// ======================= CSS / JS =======================
static const char* BARE_CSS = R"CSS(
*{margin:0;padding:0;box-sizing:border-box}
html,body{height:100%}
body{font-family:"Microsoft YaHei","PingFang SC","Segoe UI",sans-serif;background:#1f2227;color:#333;overflow:hidden}
#app{width:100vw;height:100vh;display:flex;align-items:center;justify-content:center;
padding:24px 24px 92px}
#viewport{position:relative;overflow:hidden;flex:0 0 auto}
.slide{position:absolute;left:0;top:0;transform-origin:top left;transform:scale(var(--fit,1));
box-shadow:0 6px 40px rgba(0,0,0,.55);display:none;background:#fff;overflow:hidden}
.slide.active{display:block}
.slide.tr-anim{animation-duration:var(--td,600ms);animation-timing-function:ease;animation-fill-mode:both}
@keyframes trFade{from{opacity:0}to{opacity:1}}
@keyframes trPushL{from{transform:translateX(100%) scale(var(--fit,1));opacity:.85}to{transform:translateX(0) scale(var(--fit,1));opacity:1}}
@keyframes trPushR{from{transform:translateX(-100%) scale(var(--fit,1));opacity:.85}to{transform:translateX(0) scale(var(--fit,1));opacity:1}}
@keyframes trPushU{from{transform:translateY(100%) scale(var(--fit,1));opacity:.85}to{transform:translateY(0) scale(var(--fit,1));opacity:1}}
@keyframes trPushD{from{transform:translateY(-100%) scale(var(--fit,1));opacity:.85}to{transform:translateY(0) scale(var(--fit,1));opacity:1}}
@keyframes trWipeL{from{clip-path:inset(0 100% 0 0)}to{clip-path:inset(0 0 0 0)}}
@keyframes trWipeR{from{clip-path:inset(0 0 0 100%)}to{clip-path:inset(0 0 0 0)}}
@keyframes trWipeU{from{clip-path:inset(100% 0 0 0)}to{clip-path:inset(0 0 0 0)}}
@keyframes trWipeD{from{clip-path:inset(0 0 100% 0)}to{clip-path:inset(0 0 0 0)}}
@keyframes trSplit{from{clip-path:inset(0 50% 0 50%)}to{clip-path:inset(0 0 0 0)}}
@keyframes trZoom{from{transform:scale(calc(var(--fit,1) * .62));opacity:0}to{transform:scale(var(--fit,1));opacity:1}}
@keyframes trDissolve{from{opacity:0;filter:blur(8px)}to{opacity:1;filter:blur(0)}}
@keyframes trBlinds{from{clip-path:inset(0 0 100% 0)}to{clip-path:inset(0 0 0 0)}}
@keyframes trWheel{from{clip-path:circle(0 at 50% 50%)}to{clip-path:circle(75% at 50% 50%)}}
.tr-fade{animation-name:trFade}
.tr-push.tdir-l{animation-name:trPushL}.tr-push.tdir-r{animation-name:trPushR}
.tr-push.tdir-u{animation-name:trPushU}.tr-push.tdir-d{animation-name:trPushD}
.tr-push:not(.tdir-l):not(.tdir-r):not(.tdir-u):not(.tdir-d){animation-name:trPushL}
.tr-wipe.tdir-l{animation-name:trWipeL}.tr-wipe.tdir-r{animation-name:trWipeR}
.tr-wipe.tdir-u{animation-name:trWipeU}.tr-wipe.tdir-d{animation-name:trWipeD}
.tr-wipe:not(.tdir-l):not(.tdir-r):not(.tdir-u):not(.tdir-d){animation-name:trWipeL}
.tr-cover.tdir-l{animation-name:trWipeL}.tr-cover.tdir-r{animation-name:trWipeR}
.tr-cover.tdir-u{animation-name:trWipeU}.tr-cover.tdir-d{animation-name:trWipeD}
.tr-cover:not(.tdir-l):not(.tdir-r):not(.tdir-u):not(.tdir-d){animation-name:trWipeL}
.tr-split{animation-name:trSplit}
.tr-zoom{animation-name:trZoom}
.tr-dissolve{animation-name:trDissolve}
.tr-blinds{animation-name:trBlinds}
.tr-wipe,.tr-cover{animation-direction:normal}
.tr-checker{animation-name:trFade}
.tr-circle{animation-name:trWheel}
.tr-diamond{animation-name:trZoom}
.tr-wheel{animation-name:trWheel}
.tr-strips{animation-name:trBlinds}
.tr-random{animation-name:trFade}
.tr-uncover{animation-name:trFade}
.tr-wedge{animation-name:trWipeL}
.bg-layer{position:absolute;left:0;top:0;width:100%;height:100%;pointer-events:none}
.shape{position:absolute;user-select:none}
.tx{text-align:left}
.tx p{word-break:break-word}
.bullet{font-weight:400}
.chart-ph{width:100%;height:100%;display:flex;align-items:center;justify-content:center;
color:#8a9099;font-size:13px;background:rgba(128,128,128,.06);border:1px dashed #b9bec6}
.slide-num{position:absolute;bottom:.6% ;right:1.2%;font-size:12px;color:#9aa0a6;user-select:none;z-index:60}
.shape.anim-item{opacity:0}
.shape.anim-item.run{opacity:1}
@keyframes shFade{from{opacity:0}to{opacity:1}}
@keyframes shWipe{from{clip-path:inset(0 100% 0 0)}to{clip-path:inset(0 0 0 0)}}
@keyframes shZoom{from{opacity:0;transform:scale(.55)}to{opacity:1;transform:scale(1)}}
@keyframes shCircle{from{opacity:1;clip-path:circle(0 at 50% 50%)}to{clip-path:circle(75% at 50% 50%)}}
@keyframes shDiamond{from{opacity:1;clip-path:polygon(50% 50%,50% 50%,50% 50%,50% 50%)}to{clip-path:polygon(50% 0,100% 50%,50% 100%,0 50%)}}
@keyframes shBlinds{from{clip-path:inset(0 0 100% 0)}to{clip-path:inset(0 0 0 0)}}
.shape.run[data-a="appear"]{opacity:1}
.shape.run[data-a="fade"]{animation:shFade var(--ad,500ms) both}
.shape.run[data-a="wipe"]{animation:shWipe var(--ad,500ms) both}
.shape.run[data-a="checker"]{animation:shFade var(--ad,500ms) both}
.shape.run[data-a="circle"]{animation:shCircle var(--ad,500ms) both}
.shape.run[data-a="diamond"]{animation:shDiamond var(--ad,500ms) both}
.shape.run[data-a="dissolve"]{animation:shFade var(--ad,500ms) both}
.shape.run[data-a="blinds"]{animation:shBlinds var(--ad,500ms) both}
.shape.run[data-a="wheel"]{animation:shCircle var(--ad,500ms) both}
.shape.run[data-a="zoom"]{animation:shZoom var(--ad,500ms) both}
/* ---- 批注画笔 ---- */
#ink{position:absolute;left:0;top:0;width:100%;height:100%;z-index:900;pointer-events:none;touch-action:none}
#ink.on{pointer-events:auto;cursor:crosshair}
#inkbar{position:fixed;left:50%;bottom:72px;transform:translateX(-50%);display:flex;gap:6px;align-items:center;
background:rgba(24,26,31,.82);border:1px solid rgba(255,255,255,.16);padding:7px 10px;border-radius:12px;
z-index:1100;box-shadow:0 8px 26px rgba(0,0,0,.45);color:#e6e9ee;font-size:12px}
#inkbar button{background:rgba(255,255,255,.1);border:1px solid rgba(255,255,255,.18);color:#e6e9ee;
padding:5px 9px;border-radius:7px;cursor:pointer;font-size:12px}
#inkbar button:hover{background:rgba(255,255,255,.22)}
#inkbar button.on{background:#2f6df6;border-color:#2f6df6;color:#fff}
#inkbar .sw{width:18px;height:18px;border-radius:50%;padding:0;border:2px solid rgba(255,255,255,.35)}
#inkbar .sw.on{border-color:#fff;box-shadow:0 0 0 2px #2f6df6}
#inkbar .sep{width:1px;height:18px;background:rgba(255,255,255,.22);margin:0 3px}
#inkbar .hint{opacity:.65;margin-left:4px}
#controls{position:fixed;bottom:18px;left:50%;transform:translateX(-50%);display:flex;gap:10px;
align-items:center;z-index:1000;background:rgba(20,22,26,.55);padding:6px 12px;border-radius:10px}
#controls button{background:rgba(255,255,255,.14);color:#fff;border:1px solid rgba(255,255,255,.28);
padding:6px 14px;border-radius:6px;cursor:pointer;font-size:13px}
#controls button:hover{background:rgba(255,255,255,.3)}
#page-info{color:#dfe3e8;font-size:13px;min-width:72px;text-align:center}
#page-input{width:52px;text-align:center;background:#2c3138;color:#fff;border:1px solid #4a5058;border-radius:4px}
#notes-panel{position:fixed;right:0;top:0;width:360px;max-width:80vw;height:100vh;background:#191c21;color:#d7dbe0;
padding:22px;display:none;z-index:2000;overflow-y:auto;font-size:14px;line-height:1.7;box-shadow:-4px 0 22px rgba(0,0,0,.5)}
#notes-panel.show{display:block}
#notes-panel h3{color:#63a4d8;margin-bottom:14px;font-size:15px}
#btn-notes{position:fixed;top:14px;right:14px;z-index:3000;background:rgba(255,255,255,.12);color:#dfe3e8;
border:1px solid #515964;padding:5px 12px;border-radius:5px;cursor:pointer;font-size:12px}
.pptx-table{border-collapse:separate}
.pptx-table td p,.pptx-table th p{margin:0}
a{color:#0563c1;text-decoration:none}
a:hover{text-decoration:underline}
)CSS";

static const char* BARE_JS = R"JS(
var cur=0,total=0,notesOn=false,vw=0,vh=0,scale=1,cw=1280,ch=720;
function fit(){
  // 视口留出四周留白和底部控制条的高度
  var availW=window.innerWidth-48;
  var availH=window.innerHeight-24-92-16;
  var s=Math.min(availW/cw, availH/ch);
  if(!isFinite(s)||s<=0)s=1;
  scale=s;
  var vp=document.getElementById('viewport');
  vp.style.width=Math.round(cw*scale)+'px';
  vp.style.height=Math.round(ch*scale)+'px';
  var sl=document.querySelectorAll('.slide');
  for(var i=0;i<sl.length;i++){
    sl[i].style.transform='scale('+scale+')';
    sl[i].style.transformOrigin='top left';
  }
}
function show(i){
  if(i<0||i>=total)return;
  var ss=document.querySelectorAll('.slide');
  for(var k=0;k<ss.length;k++)ss[k].classList.remove('active');
  // 重新触发动画
  void ss[i].offsetWidth;
  ss[i].classList.add('active');
  cur=i;
  document.getElementById('page-info').textContent=(i+1)+' / '+total;
  document.getElementById('page-input').value=i+1;
  var nd=ss[i].querySelector('.notes-data');
  document.getElementById('notes-content').textContent=nd?nd.textContent:'(无备注)';
}
function next(){show(cur+1)}
function prev(){show(cur-1)}
function go(n){var v=parseInt(n,10);if(v>=1&&v<=total)show(v-1)}
function toggleNotes(f){
  var p=document.getElementById('notes-panel');
  notesOn=(typeof f==='boolean')?f:!notesOn;
  if(notesOn)p.classList.add('show');else p.classList.remove('show');
}
function toggleFullscreen(){
  if(document.fullscreenElement)document.exitFullscreen();
  else document.body.requestFullscreen();
}
document.addEventListener('DOMContentLoaded',function(){
  var ss=document.querySelectorAll('.slide'); total=ss.length;
  // 隐藏的 .slide offsetWidth 为 0，必须从内联样式读画布尺寸
  if(ss.length){
    cw=parseFloat(ss[0].style.width)||1280;
    ch=parseFloat(ss[0].style.height)||720;
  }
  fit(); show(0);
  window.addEventListener('resize',fit);
  document.addEventListener('keydown',function(e){
    if(e.key==='ArrowRight'||e.key==='ArrowDown'||e.key===' '||e.key==='PageDown'){next();e.preventDefault()}
    else if(e.key==='ArrowLeft'||e.key==='ArrowUp'||e.key==='PageUp'){prev();e.preventDefault()}
    else if(e.key==='Home'){show(0)}
    else if(e.key==='End'){show(total-1)}
    else if(e.key==='Escape'){toggleNotes(false)}
    else if(e.key==='n'||e.key==='N'){toggleNotes(null)}
    else if(e.key==='f'||e.key==='F'){toggleFullscreen()}
  });
  document.getElementById('viewport').addEventListener('click',function(e){
    if(e.target.tagName==='A'||e.target.closest('a'))return;
    var r=this.getBoundingClientRect();
    if((e.clientX-r.left)>r.width/2)next();else prev();
  });
  document.getElementById('btn-notes').addEventListener('click',function(){toggleNotes(null)});
});
)JS";

static const char* BARE_JS2 = R"JS(
var cur=0,total=0,notesOn=false,scale=1,cw=1280,ch=720;
var ss=[],pend=[];
function fit(){
  var availW=window.innerWidth-48, availH=window.innerHeight-24-92-16;
  var s=Math.min(availW/cw, availH/ch);
  if(!isFinite(s)||s<=0)s=1;
  scale=s;
  var vp=document.getElementById('viewport');
  vp.style.width=Math.round(cw*scale)+'px';
  vp.style.height=Math.round(ch*scale)+'px';
  for(var i=0;i<ss.length;i++) ss[i].style.setProperty('--fit',scale);
  inkResize();
}
function updateInfo(i){
  document.getElementById('page-info').textContent=(i+1)+' / '+total;
  document.getElementById('page-input').value=i+1;
}
var TR_CLASSES=['tr-anim','tr-fade','tr-push','tr-wipe','tr-split','tr-cover','tr-uncover',
 'tr-zoom','tr-dissolve','tr-blinds','tr-checker','tr-circle','tr-diamond','tr-wheel',
 'tr-strips','tr-random','tr-cut','tdir-l','tdir-r','tdir-u','tdir-d'];
function applyTr(s){
  for(var k=0;k<TR_CLASSES.length;k++) s.classList.remove(TR_CLASSES[k]);
  var kind=s.getAttribute('data-tr')||'none';
  if(!kind||kind==='none'||kind==='cut')return;
  var dur=parseInt(s.getAttribute('data-td')||'600',10);
  s.style.setProperty('--td',dur+'ms');
  s.classList.add('tr-'+kind,'tr-anim');
  var d=parseInt(s.getAttribute('data-tdir')||'0',10);
  var dirName={1:'u',2:'d',3:'l',4:'r'}[d]||'l';
  s.classList.add('tdir-'+dirName);
  void s.offsetWidth;
}
function show(i){
  if(i<0||i>=total)return;
  for(var k=0;k<ss.length;k++) ss[k].classList.remove('active');
  var s=ss[i];
  pend=[];
  var items=s.querySelectorAll('.shape.anim-item');
  for(var k=0;k<items.length;k++){ items[k].classList.remove('run'); pend.push(items[k]); }
  s.classList.add('active');
  applyTr(s);
  cur=i; updateInfo(i);
  inkRedraw();
  var nd=s.querySelector('.notes-data');
  document.getElementById('notes-content').textContent=nd?nd.textContent:'(无备注)';
}
function step(){
  if(pend.length){
    var el=pend.shift();
    el.style.setProperty('--ad',(parseInt(el.getAttribute('data-ad')||'500',10))+'ms');
    el.classList.add('run');
    return;
  }
  next();
}
function next(){show(cur+1)}
function prev(){show(cur-1)}
function go(n){var v=parseInt(n,10);if(v>=1&&v<=total)show(v-1)}
function toggleNotes(f){
  var p=document.getElementById('notes-panel');
  notesOn=(typeof f==='boolean')?f:!notesOn;
  if(notesOn)p.classList.add('show');else p.classList.remove('show');
}
function toggleFullscreen(){
  if(document.fullscreenElement)document.exitFullscreen();
  else document.body.requestFullscreen();
}

// ================= 批注画笔 =================
var inkCv=null,inkCx=null,inkTool='none',inkCol='#e03131',inkWidth=3;
var inkPages={},inkDrawing=false,inkLaser=null;
function inkList(){ if(!inkPages[cur]) inkPages[cur]=[]; return inkPages[cur]; }
function inkResize(){
  if(!inkCv)return;
  var vp=document.getElementById('viewport');
  var r=vp.getBoundingClientRect();
  var dpr=window.devicePixelRatio||1;
  inkCv.style.width=r.width+'px'; inkCv.style.height=r.height+'px';
  inkCv.width=Math.max(1,Math.round(r.width*dpr));
  inkCv.height=Math.max(1,Math.round(r.height*dpr));
  inkRedraw();
}
function inkRedraw(){
  if(!inkCx||!inkCv)return;
  var W=inkCv.width,H=inkCv.height;
  inkCx.setTransform(1,0,0,1,0,0);
  inkCx.clearRect(0,0,W,H);
  var strokes=inkPages[cur]||[];
  for(var i=0;i<strokes.length;i++){
    var st=strokes[i], pts=st.pts;
    if(pts.length<2)continue;
    inkCx.globalAlpha=st.a;
    inkCx.strokeStyle=st.c;
    inkCx.lineWidth=st.w*(W/1280);
    inkCx.lineCap='round';inkCx.lineJoin='round';
    inkCx.beginPath();
    inkCx.moveTo(pts[0][0]*W,pts[0][1]*H);
    for(var j=1;j<pts.length;j++) inkCx.lineTo(pts[j][0]*W,pts[j][1]*H);
    inkCx.stroke();
  }
  inkCx.globalAlpha=1;
  if(inkLaser){
    var x=inkLaser[0]*W,y=inkLaser[1]*H;
    var g=inkCx.createRadialGradient(x,y,0,x,y,14);
    g.addColorStop(0,'rgba(255,60,60,.95)');g.addColorStop(1,'rgba(255,60,60,0)');
    inkCx.fillStyle=g;inkCx.beginPath();inkCx.arc(x,y,14,0,Math.PI*2);inkCx.fill();
  }
}
function inkPos(e){
  var r=inkCv.getBoundingClientRect();
  return [(e.clientX-r.left)/r.width,(e.clientY-r.top)/r.height];
}
function inkEraseAt(p){
  var strokes=inkPages[cur]||[], W=inkCv.width, H=inkCv.height;
  var px=p[0]*W,py=p[1]*H,tol=18*W/1280;
  for(var i=strokes.length-1;i>=0;i--){
    var pts=strokes[i].pts,hit=false;
    for(var j=1;j<pts.length;j++){
      var ax=pts[j-1][0]*W,ay=pts[j-1][1]*H,bx=pts[j][0]*W,by=pts[j][1]*H;
      var dx=bx-ax,dy=by-ay;
      var t=((px-ax)*dx+(py-ay)*dy)/((dx*dx+dy*dy)||1);
      t=Math.max(0,Math.min(1,t));
      var qx=ax+dx*t,qy=ay+dy*t;
      if(Math.sqrt((px-qx)*(px-qx)+(py-qy)*(py-qy))<tol){hit=true;break;}
    }
    if(hit){strokes.splice(i,1);inkRedraw();}
  }
}
function inkStart(e){
  if(inkTool==='none')return;
  e.preventDefault();
  inkCv.setPointerCapture(e.pointerId);
  var p=inkPos(e);
  if(inkTool==='laser'){inkLaser=p;inkRedraw();return;}
  if(inkTool==='erase'){inkDrawing=true;inkEraseAt(p);return;}
  inkDrawing=true;
  inkList().push({c:inkCol,w:inkTool==='high'?12:inkWidth,a:inkTool==='high'?0.35:1,pts:[p]});
  inkRedraw();
}
function inkMove(e){
  if(inkTool==='none')return;
  var p=inkPos(e);
  if(inkTool==='laser'){inkLaser=p;inkRedraw();return;}
  if(!inkDrawing)return;
  e.preventDefault();
  if(inkTool==='erase'){inkEraseAt(p);return;}
  var strokes=inkList(), st=strokes[strokes.length-1];
  if(st){st.pts.push(p);inkRedraw();}
}
function inkEnd(e){inkDrawing=false;}
function inkUndo(){var s=inkList();if(s.length){s.pop();inkRedraw();}}
function inkClear(){inkPages[cur]=[];inkLaser=null;inkRedraw();}
function inkColor(c){
  inkCol=c;
  var sws=document.querySelectorAll('#inkbar .sw');
  for(var i=0;i<sws.length;i++)sws[i].classList.remove('on');
}
function inkMode(m){
  inkTool=m;
  var btns=['pen','high','erase','laser'];
  for(var i=0;i<btns.length;i++){
    var b=document.getElementById('ink-'+btns[i]);
    if(b)b.classList.remove('on');
  }
  if(m!=='none'){
    var b=document.getElementById('ink-'+m);
    if(b)b.classList.add('on');
    inkCv.classList.add('on');
    inkCv.style.cursor=(m==='erase'?'cell':(m==='laser'?'none':'crosshair'));
  } else {
    inkCv.classList.remove('on');
    inkLaser=null;inkRedraw();
  }
}
function inkInit(){
  inkCv=document.getElementById('ink');
  if(!inkCv)return;
  inkCx=inkCv.getContext('2d');
  inkCv.addEventListener('pointerdown',inkStart);
  inkCv.addEventListener('pointermove',inkMove);
  inkCv.addEventListener('pointerup',inkEnd);
  inkCv.addEventListener('pointercancel',inkEnd);
  inkCv.addEventListener('pointerleave',function(){inkLaser=null;inkRedraw();});
  inkResize();
}

document.addEventListener('DOMContentLoaded',function(){
  ss=document.querySelectorAll('.slide'); total=ss.length;
  if(ss.length){
    cw=parseFloat(ss[0].style.width)||1280;
    ch=parseFloat(ss[0].style.height)||720;
  }
  inkInit();
  fit(); show(0);
  window.addEventListener('resize',fit);
  document.addEventListener('keydown',function(e){
    if(e.key==='ArrowRight'||e.key==='ArrowDown'||e.key===' '||e.key==='PageDown'){step();e.preventDefault()}
    else if(e.key==='ArrowLeft'||e.key==='ArrowUp'||e.key==='PageUp'){prev();e.preventDefault()}
    else if(e.key==='Home'){show(0)}
    else if(e.key==='End'){show(total-1)}
    else if(e.key==='Escape'){toggleNotes(false);inkMode('none')}
    else if(e.key==='n'||e.key==='N'){toggleNotes(null)}
    else if(e.key==='f'||e.key==='F'){toggleFullscreen()}
    else if(e.key==='p'||e.key==='P'){inkMode(inkTool==='pen'?'none':'pen')}
    else if(e.key==='e'||e.key==='E'){inkMode('erase')}
  });
  document.getElementById('viewport').addEventListener('click',function(e){
    if(e.target.tagName==='A'||e.target.closest('a'))return;
    var r=this.getBoundingClientRect();
    if((e.clientX-r.left)>r.width/2)step();else prev();
  });
  document.getElementById('btn-notes').addEventListener('click',function(){toggleNotes(null)});
});
)JS";

} // namespace

// ======================= 对外接口 =======================
std::string render_html(const Presentation& pres, const HtmlOptions& opt) {
    Ctx c;
    c.sw = pres.slide_w; c.sh = pres.slide_h;
    c.sx = opt.canvas_w > 0 ? opt.canvas_w : 1280;
    c.sy = opt.canvas_h > 0 ? opt.canvas_h : 720;
    c.pres = &pres;
    c.slide_total = (int)pres.slides.size();
    {
        time_t tt = time(nullptr);
        struct tm local;
#ifdef _WIN32
        localtime_s(&local, &tt);
#else
        local = *localtime(&tt);
#endif
        char dbuf[32];
        snprintf(dbuf, sizeof(dbuf), "%04d/%02d/%02d",
                 local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
        c.date_str = dbuf;
    }

    std::string html;
    html += "<!DOCTYPE html>\n<html lang=\"zh\">\n<head>\n";
    html += "<meta charset=\"UTF-8\">\n<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n";
    html += "<title>" + html_escape(opt.title) + "</title>\n";
    html += "<style>\n" + std::string(BARE_CSS) + "\n</style>\n";
    // 插件：head 注入（水印 CSS、进度条脚本等）
    {
        std::string head_frag;
        PluginHost::instance().html_head("title=" + opt.title, head_frag);
        if (!head_frag.empty()) html += head_frag;
    }
    html += "</head>\n<body>\n";
    html += "<button id=\"btn-notes\">备注 (N)</button>\n";
    html += "<div id=\"notes-panel\"><h3>演讲者备注</h3><div id=\"notes-content\">(无备注)</div></div>\n";
    html += "<div id=\"app\">\n<div id=\"viewport\">\n";

    size_t total = pres.slides.size();
    for (size_t i = 0; i < total; ++i) {
        RenderTheme rt = make_render_theme(pres, *pres.slides[i]);
        c.rt = &rt;
        c.uid = 0;
        html += slide_html(c, pres, *pres.slides[i], i, total, opt);
    }

    html += "<canvas id=\"ink\"></canvas>\n";
    html += "</div>\n</div>\n";   // /#viewport /#app
    html += "<div id=\"inkbar\">\n";
    html += "<button id=\"ink-pen\" onclick=\"inkMode('pen')\">画笔</button>\n";
    html += "<button id=\"ink-high\" onclick=\"inkMode('high')\">荧光笔</button>\n";
    html += "<button id=\"ink-erase\" onclick=\"inkMode('erase')\">橡皮</button>\n";
    html += "<button id=\"ink-laser\" onclick=\"inkMode('laser')\">激光笔</button>\n";
    html += "<span class=\"sep\"></span>\n";
    html += "<button class=\"sw\" style=\"background:#e03131\" onclick=\"inkColor('#e03131')\"></button>\n";
    html += "<button class=\"sw\" style=\"background:#f5c400\" onclick=\"inkColor('#f5c400')\"></button>\n";
    html += "<button class=\"sw\" style=\"background:#2f9e44\" onclick=\"inkColor('#2f9e44')\"></button>\n";
    html += "<button class=\"sw\" style=\"background:#1c7ed6\" onclick=\"inkColor('#1c7ed6')\"></button>\n";
    html += "<button class=\"sw\" style=\"background:#ffffff\" onclick=\"inkColor('#ffffff')\"></button>\n";
    html += "<span class=\"sep\"></span>\n";
    html += "<button onclick=\"inkUndo()\">撤销</button>\n";
    html += "<button onclick=\"inkClear()\">清空</button>\n";
    html += "<button id=\"ink-off\" onclick=\"inkMode('none')\">收起</button>\n";
    html += "<span class=\"hint\">点“画笔”开始批注</span>\n";
    html += "</div>\n";
    html += "<div id=\"controls\">\n";
    html += "<button onclick=\"show(0)\">&#9194;</button>\n";
    html += "<button onclick=\"prev()\">&#9664;</button>\n";
    html += "<span id=\"page-info\">1 / " + std::to_string(total) + "</span>\n";
    html += "<input id=\"page-input\" type=\"number\" min=\"1\" max=\"" + std::to_string(total) +
            "\" value=\"1\" onchange=\"go(this.value)\">\n";
    html += "<button onclick=\"next()\">&#9654;</button>\n";
    html += "<button onclick=\"show(" + std::to_string(total > 0 ? total - 1 : 0) + ")\">&#9195;</button>\n";
    html += "<button onclick=\"toggleFullscreen()\">全屏</button>\n";
    html += "</div>\n";
    html += "<script>\n" + std::string(BARE_JS2) + "\n</script>\n</body>\n</html>";

    embed_media_data(pres, html, opt.embed_media);
    return html;
}

bool render_to_file(const std::string& path, const Presentation& pres,
                    const HtmlOptions& opt, std::string* err_out) {
    std::string html = render_html(pres, opt);
    if (!write_file(path, html)) {
        if (err_out) *err_out = "无法写入: " + path;
        return false;
    }
    return true;
}

} // namespace ppt
