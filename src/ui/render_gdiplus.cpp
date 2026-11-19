#include "render_gdiplus.h"
#include "svg_path.h"
#include "png_enc.h"
#include "../core/preset_geom.h"
#include "../base/strutil.h"

#include <shlwapi.h>
#include <cmath>
#include <algorithm>
#include <functional>

// 注意：MinGW 的 gdiplus 头可能引入 min/max 宏，统一在使用后清除
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace ppt {
using namespace Gdiplus;

static const double PI = 3.141592653589793;

Gdiplus::Color argb_of(const ARGB& c) {
    return Color(c.a, c.r, c.g, c.b);
}

static ARGB scale_alpha(const ARGB& c, double pct) {
    ARGB o = c;
    o.a = (uint8_t)((double)o.a * pct / 100.0 + 0.5);
    return o;
}

// ======================= 图片缓存 =======================
ImageCache::~ImageCache() {
    for (auto& kv : map_) {
        if (kv.second.img) delete kv.second.img;
        if (kv.second.st) kv.second.st->Release();
    }
}

Gdiplus::Image* ImageCache::get(const std::string& part) {
    if (part.empty()) return nullptr;
    auto it = map_.find(part);
    if (it != map_.end()) return it->second.ok ? it->second.img : nullptr;

    Ent e{}; e.ok = false;
    auto mit = pres_.media.find(part);
    if (mit != pres_.media.end() && !mit->second.empty()) {
        e.st = SHCreateMemStream(&mit->second[0], (UINT)mit->second.size());
        if (e.st) {
            e.img = Image::FromStream(e.st);
            if (e.img && e.img->GetLastStatus() == Ok) e.ok = true;
            else { delete e.img; e.img = nullptr; }
        }
    }
    map_[part] = e;
    return e.ok ? e.img : nullptr;
}

// ======================= 几何：形状 -> 展平路径 =======================
// 返回画布像素坐标的折线；empty 表示该形状不需要轮廓绘制
static std::vector<svgpath::Polyline> shape_outline(const Shape& s, double w, double h) {
    std::string d;
    if (!s.svg_path.empty()) d = s.svg_path;
    else {
        const char* p = preset_path(s.prst_geom);
        if (!p) return {};
        d = p;
        double adj = 0.16667;
        auto it = s.prst_adj.find("adj");
        if (it != s.prst_adj.end() && it->second > 0) adj = std::min(it->second, 0.5);
        replace_all(d, "{1-adj}", fmt_num(1.0 - adj, 5));
        replace_all(d, "{adj}", fmt_num(adj, 5));
        if (d.find('{') != std::string::npos) return {};
    }
    return svgpath::flatten_d(d, w, h, 14);
}

static bool needs_outline(const Shape& s) {
    const std::string& g = s.prst_geom;
    if (!s.svg_path.empty()) return true;
    if (g == "line" || starts_with(g, "straightConnector") ||
        starts_with(g, "bentConnector") || starts_with(g, "curvedConnector")) return true;
    if (g == "rect" || g.empty() || g == "roundRect" || g == "ellipse" || g == "oval") return false;
    return preset_path(g) != nullptr;
}

static bool is_line_geom(const Shape& s) {
    return s.prst_geom == "line" || starts_with(s.prst_geom, "straightConnector") ||
           starts_with(s.prst_geom, "bentConnector") || starts_with(s.prst_geom, "curvedConnector");
}

// ======================= 笔 =======================
static DashType dash_of(const Line& l) { return l.dash; }

static std::unique_ptr<Pen> make_pen(const Line& l, const DrawCtx& ctx) {
    if (l.is_none() || l.w <= 0) return nullptr;
    float px = (float)std::max(ctx.pt2px(emu_to_pt((double)l.w)), 0.4);
    Pen* pen = new Pen(argb_of(l.color), px);
    switch (l.cap) {
        case LineCap::Round:  pen->SetStartCap(LineCapRound);  pen->SetEndCap(LineCapRound); break;
        case LineCap::Square: pen->SetStartCap(LineCapFlat);   pen->SetEndCap(LineCapFlat); break;
        default:              pen->SetStartCap(LineCapFlat);   pen->SetEndCap(LineCapFlat); break;
    }
    if (l.join == 0) pen->SetLineJoin(LineJoinRound);
    else if (l.join == 1) pen->SetLineJoin(LineJoinBevel);
    else pen->SetLineJoin(LineJoinMiter);

    // 虚线：以笔宽为单位给固定 pattern
    static const float solid[1] = { 0 };
    float pxw = std::max(px, 0.5f);
    switch (dash_of(l)) {
    case DashType::SquareDot: case DashType::RoundDot: {
        float p[2] = { pxw * 0.01f, pxw * 2.0f };
        pen->SetDashPattern(p, 2); pen->SetDashCap(DashCapRound); break; }
    case DashType::Dash: { float p[2] = { pxw * 3.0f, pxw * 2.0f }; pen->SetDashPattern(p, 2); break; }
    case DashType::DashDot: { float p[4] = { pxw*3, pxw*2, pxw*0.1f, pxw*2 }; pen->SetDashPattern(p, 4); break; }
    case DashType::LongDash: { float p[2] = { pxw * 5.0f, pxw * 2.5f }; pen->SetDashPattern(p, 2); break; }
    case DashType::LongDashDot: { float p[4] = { pxw*5, pxw*2.5f, pxw*0.1f, pxw*2.5f }; pen->SetDashPattern(p, 4); break; }
    case DashType::LongDashDotDot: { float p[6] = { pxw*5, pxw*2, pxw*0.1f, pxw*2, pxw*0.1f, pxw*2 }; pen->SetDashPattern(p, 6); break; }
    default: pen->SetDashStyle(DashStyleSolid); (void)solid; break;
    }
    if (l.has_tail && l.tail.prst != "none")
        pen->SetEndCap(LineCapArrowAnchor);
    if (l.has_head && l.head.prst != "none")
        pen->SetStartCap(LineCapArrowAnchor);
    return std::unique_ptr<Pen>(pen);
}

// ======================= 填充画刷 =======================
static std::unique_ptr<Brush> make_fill_brush(const Fill& f, double x, double y,
                                              double w, double h, const DrawCtx& ctx) {
    if (f.is_none() || f.kind == FillKind::Inherit || f.kind == FillKind::Group) return nullptr;
    if (f.kind == FillKind::Solid) {
        if (f.solid.a == 0) return nullptr;
        return std::unique_ptr<Brush>(new SolidBrush(argb_of(f.solid)));
    }
    if (f.kind == FillKind::Gradient && !f.stops.empty()) {
        RectF r((REAL)x, (REAL)y, (REAL)w, (REAL)h);
        if (f.stops.size() == 1)
            return std::unique_ptr<Brush>(new SolidBrush(argb_of(f.stops[0].color)));
        if (f.grad_kind == GradKind::Linear) {
            // GDI+ 角度：0=水平向右，顺时针为正；OOXML ang 同向，可直接用
            REAL ang = (REAL)f.ang;
            LinearGradientBrush* b = new LinearGradientBrush(
                r, argb_of(f.stops.front().color), argb_of(f.stops.back().color), ang);
                        // 多于两级时用插色数组精确还原停靠点
            if (f.stops.size() > 2) {
                std::vector<Color> cols; std::vector<REAL> poss;
                for (const GradientStop& st : f.stops) {
                    cols.push_back(argb_of(st.color));
                    poss.push_back((REAL)std::max(0.0, std::min(1.0, st.pos)));
                }
                b->SetInterpolationColors(&cols[0], &poss[0], (INT)cols.size());
            }
            return std::unique_ptr<Brush>(b);
        }
        // 径向：用路径渐变笔近似（中心 -> 四周）
        GraphicsPath* gp = new GraphicsPath();
        gp->AddEllipse((REAL)x, (REAL)y, (REAL)w, (REAL)h);
        PathGradientBrush* b = new PathGradientBrush(gp);
        delete gp;
        REAL alpha = 0;
        Color surround((BYTE)0, 0, 0, 0);
        for (const GradientStop& st : f.stops) {
            if (st.pos >= 0.5) { surround = argb_of(st.color); alpha = 1; break; }
        }
        (void)alpha;
        Color center = argb_of(f.stops.front().color);
        Color arr[1] = { surround };
        INT cnt = 1;
        b->SetCenterColor(center);
        b->SetSurroundColors(arr, &cnt);
        b->SetCenterPoint(PointF((REAL)(x + w / 2), (REAL)(y + h / 2)));
        return std::unique_ptr<Brush>(b);
    }
    if (f.kind == FillKind::Pattern) {
        return std::unique_ptr<Brush>(new SolidBrush(argb_of(f.pat_fg)));
    }
    return nullptr;
}

// ======================= 文本（近似：整段用首 run 样式 + GDI+ 自动换行） =======================
static std::wstring field_val(DrawCtx& ctx, const std::string& t) {
    if (t == "{slide_num}") return utf8_to_utf16(std::to_string(ctx.slide_idx + 1));
    if (t == "{date}") return utf8_to_utf16(ctx.date_str);
    return utf8_to_utf16(t);
}

static int count_wrapped_lines(Graphics& g, const std::wstring& text,
                               const std::wstring& fam, float size, int style, float maxw) {
    if (maxw < 1) return 1;
    FontFamily ff(fam.c_str());
    FontFamily alt(L"Microsoft YaHei");
    FontFamily* use = ff.IsAvailable() ? &ff : &alt;
    Font font(use, size, style, UnitPixel);
    int lines = 0;
    size_t start = 0;
    for (;;) {
        size_t nl = text.find(L'\n', start);
        std::wstring seg = text.substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start);
        if (seg.empty()) {
            lines += 1;
        } else {
            RectF big(0, 0, 100000.0f, 100000.0f), out;
            g.MeasureString(seg.c_str(), -1, &font, big, &out);
            int n = (int)std::ceil(out.Width / maxw);
            lines += std::max(1, n);
        }
        if (nl == std::wstring::npos) break;
        start = nl + 1;
    }
    return std::max(1, lines);
}

static void draw_text_body(Graphics& g, const TextBody& tb, const Shape& owner,
                           double x, double y, double w, double h,
                           DrawCtx& ctx, bool bold_override = false) {
    if (tb.empty() || w <= 1 || h <= 1) return;

    double pad_l = ctx.pt2px(emu_to_pt((double)tb.lIns));
    double pad_t = ctx.pt2px(emu_to_pt((double)tb.tIns));
    double pad_r = ctx.pt2px(emu_to_pt((double)tb.rIns));
    double pad_b = ctx.pt2px(emu_to_pt((double)tb.bIns));
    RectF box((REAL)(x + pad_l), (REAL)(y + pad_t),
              (REAL)std::max(1.0, w - pad_l - pad_r),
              (REAL)std::max(1.0, h - pad_t - pad_b));

    StringFormat sf;
    sf.SetAlignment(StringAlignmentNear);
    sf.SetLineAlignment(StringAlignmentNear);
    if (!tb.wrap) sf.SetFormatFlags(StringFormatFlagsNoWrap);

    // 先量总高度以支持垂直锚定
    struct ParaDraw { std::wstring text; float size; int style; Color color;
                      std::wstring fam; RectF rect; StringAlignment algn; };
    std::vector<ParaDraw> items;
    float total_h = 0;

    // 按缩放系数重新排版（用于 normAutofit / 文字溢出时自动缩小）
    auto build = [&](double sc) {
        items.clear();
        total_h = 0;
        for (const TextParagraph& para : tb.paras) {
            std::wstring text;
            float size = 12; int style = 0; Color col(255, 0, 0, 0); std::wstring fam = L"Calibri";
            StringAlignment al = StringAlignmentNear;
            bool first = true;
            for (const TextRun& r : para.runs) {
                if (r.text.empty()) continue;
                if (first) {
                    ResolvedRun rr = ctx.rt->resolve_run(owner, para, r);
                    size = (float)std::max(1.0, ctx.pt2px(rr.sz_pt)) * (float)sc;
                    style = (rr.b ? FontStyleBold : 0) | (rr.i ? FontStyleItalic : 0) |
                            (rr.u ? FontStyleUnderline : 0) | (rr.strike ? FontStyleStrikeout : 0);
                    if (bold_override) style |= FontStyleBold;
                    col = argb_of(rr.color);
                    std::wstring use = utf8_to_utf16(rr.font_ea.empty() ? rr.font : rr.font_ea);
                    if (use.empty()) use = L"Microsoft YaHei";
                    fam = use;
                    first = false;
                }
                text += field_val(ctx, r.text);
            }
            if (para.props.algn_set) {
                switch (para.props.algn) {
                    case AlignL::Center: al = StringAlignmentCenter; break;
                    case AlignL::Right:  al = StringAlignmentFar;    break;
                    case AlignL::Justify: case AlignL::Distributed:  al = StringAlignmentFar; break;
                    default: al = StringAlignmentNear;
                }
            }
            if (para.bullet.kind == BulletKind::Char && !para.bullet.char_.empty())
                text = utf8_to_utf16(para.bullet.char_ + " ") + text;
            if (text.empty()) text = L" ";

            double lh = para.props.lnspc_set ? para.props.lnspc_pct / 100.0 : 1.18;
            if (para.props.lnspc_pts) lh = para.props.lnspc_pts / 100.0;
            lh = std::max(lh, 0.7);

            RectF pr = box;
            if (para.props.marL_set) {
                long long ml = para.props.marL + para.props.indent;
                if (ml > 0) pr.Offset((REAL)ctx.pt2px(emu_to_pt((double)ml)), 0);
            }
            if (para.props.lvl_set && para.props.lvl > 0)
                pr.Offset((REAL)(ctx.pt2px(18) * para.props.lvl), 0);

            int nlines = 1;
            if (tb.wrap) {
                nlines = count_wrapped_lines(g, text, fam, size, style, pr.Width);
            }
            float ph = size * (float)lh * nlines;
            if (para.props.spcbef_set) ph += (float)ctx.pt2px(para.props.spcbef / 100.0);
            if (para.props.spcaft_set) ph += (float)ctx.pt2px(para.props.spcaft / 100.0);

            ParaDraw pd; pd.text = text; pd.size = size; pd.style = style; pd.color = col;
            pd.fam = fam; pd.algn = al; pd.rect = RectF(pr.X, total_h, pr.Width, ph);
            items.push_back(pd);
            total_h += ph;
        }
    };

    double sc = 1.0;
    if (tb.fontScale > 0 && tb.fontScale < 100) sc = tb.fontScale / 100.0;
    build(sc);
    // 溢出时自动缩小字号（最多缩到 55%），避免文字跑出形状
    for (int it = 0; it < 8 && total_h > box.Height; ++it) {
        double f = (double)box.Height / std::max(1.0f, total_h);
        double ns = sc * std::max(0.6, std::min(0.97, f));
        if (ns < sc * 0.55) break;
        if (ns < sc * 0.98) sc = ns;
        build(sc);
    }


    // 垂直锚定
    REAL dy = 0;
    if (tb.anchor == VAlign::Center)      dy = (REAL)((box.Height - total_h) / 2);
    else if (tb.anchor == VAlign::Bottom) dy = (REAL)(box.Height - total_h);
    if (tb.anchor != VAlign::Top && dy < 0) dy = 0;

    for (const ParaDraw& pd : items) {
        FontFamily ff(pd.fam.c_str());
        Font font(&ff, pd.size, pd.style, UnitPixel);
        RectF rr(pd.rect.X, box.Y + dy + pd.rect.Y, pd.rect.Width, pd.rect.Height);
        SolidBrush sb(pd.color);
        StringFormat s2;
        s2.SetAlignment(pd.algn);
        s2.SetLineAlignment(StringAlignmentNear);
        if (!tb.wrap) s2.SetFormatFlags(StringFormatFlagsNoWrap);
        g.DrawString(pd.text.c_str(), -1, &font, rr, &s2, &sb);
    }
}

// ======================= 单个形状 =======================
static void draw_shape(Graphics& g, const Shape& s_in, DrawCtx& ctx, ImageCache& imgs,
                       double ox, double oy, double kx, double ky);

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

static void draw_table(Graphics& g, const Shape& owner, double x, double y,
                       double w, double h, DrawCtx& ctx) {
    const Table& t = *owner.table;
    if (t.cells.empty() || t.cols.empty()) return;
    (void)w; (void)h;
    const Theme* th = ctx.rt->theme;
    ARGB accent = th ? th->colors.accent[0] : ARGB(255, 0x44, 0x72, 0xC4);

    // 用 EMU 累计列宽行高换算到画布
    std::vector<double> colpx(t.cols.size()), rowpx(t.rows.size());
    double cw_all = 0, ch_all = 0;
    for (size_t i = 0; i < t.cols.size(); ++i) { colpx[i] = ctx.pxW(t.cols[i]); cw_all += colpx[i]; }
    for (size_t i = 0; i < t.rows.size(); ++i) { rowpx[i] = ctx.pxH(t.rows[i]); ch_all += rowpx[i]; }
    if (cw_all <= 0) cw_all = 1;
    double sx = w / cw_all;                       // 实际宽度可能与声明不一致，做一次拉伸
    double sy = (ch_all > 0 && h > 0) ? h / ch_all : 1.0;

    std::vector<std::vector<char>> skip;
    skip.resize(t.cells.size());
    for (size_t r = 0; r < t.cells.size(); ++r) skip[r].resize(t.cells[r].size(), 0);

    Shape cell_owner = owner;
    cell_owner.ph_type = PhType::Other;

    for (size_t r = 0; r < t.cells.size(); ++r) {
        double ry = y;
        for (size_t k = 0; k < r && k < rowpx.size(); ++k) ry += rowpx[k] * sy;
        for (size_t col = 0; col < t.cells[r].size(); ++col) {
            if (skip[r][col]) continue;
            const TableCell& cell = t.cells[r][col];
            if (cell.merge_h || cell.merge_v) continue;
            int cs = (int)std::max<long long>(1, cell.span_h);
            int rs = (int)std::max<long long>(1, cell.span_v);
            double cx0 = x;
            for (size_t k = 0; k < col && k < colpx.size(); ++k) cx0 += colpx[k] * sx;
            double cw = 0, chh = 0;
            for (int k = 0; k < cs && col + k < colpx.size(); ++k) cw += colpx[col + k] * sx;
            for (int k = 0; k < rs && r + k < rowpx.size(); ++k) chh += rowpx[r + k] * sy;
            for (int dr = 0; dr < rs; ++dr) {
                if (r + dr >= skip.size()) break;
                for (int dc = 0; dc < cs; ++dc) {
                    if (col + dc >= skip[r + dr].size()) break;
                    if (dr || dc) skip[r + dr][col + dc] = 1;
                }
            }
            RectF rc((REAL)cx0, (REAL)ry, (REAL)cw, (REAL)chh);
            REAL rl = rc.X, rt = rc.Y, rr2 = rc.X + rc.Width, rb = rc.Y + rc.Height;

            // 底色：显式 > 首行 > 条纹
            if (!cell.fill.is_inherit() && !cell.fill.is_none()) {
                Fill f = cell.fill;
                if (!f.solid_scheme.empty() && th) {
                    ARGB c; if (th->colors.resolve(f.solid_scheme, c)) { c.a = f.solid.a; f.solid = c; }
                }
                if (auto br = make_fill_brush(f, rl, rt, rc.Width, rc.Height, ctx))
                    g.FillRectangle(br.get(), rc);
            } else if (t.first_row && r == 0) {
                SolidBrush b(argb_of(accent)); g.FillRectangle(&b, rc);
            } else if (t.band_row && r > 0 && (r % 2) == 0) {
                SolidBrush b(argb_of(scale_alpha(accent, 20))); g.FillRectangle(&b, rc);
            }

            // 四边边框
            for (int bi = 0; bi < 4; ++bi) {
                if (!cell.has_border[bi] || cell.border_w[bi] <= 0) continue;
                Line bl; bl.color = cell.border_color[bi]; bl.w = cell.border_w[bi];
                bl.dash = cell.border_dash[bi]; bl.inherit = false; bl.explicit_none = false;
                auto pen = make_pen(bl, ctx);
                if (!pen) continue;
                if (bi == 0)      g.DrawLine(pen.get(), rl, rt, rl, rb);
                else if (bi == 1) g.DrawLine(pen.get(), rr2, rt, rr2, rb);
                else if (bi == 2) g.DrawLine(pen.get(), rl, rt, rr2, rt);
                else              g.DrawLine(pen.get(), rl, rb, rr2, rb);
            }

            cell_owner.text = cell.text;
            double ip_l = cell.mar_set ? ctx.pt2px(emu_to_pt((double)cell.marL)) : 4;
            double ip_t = cell.mar_set ? ctx.pt2px(emu_to_pt((double)cell.marT)) : 2;
            double ip_r = cell.mar_set ? ctx.pt2px(emu_to_pt((double)cell.marR)) : 4;
            double ip_b = cell.mar_set ? ctx.pt2px(emu_to_pt((double)cell.marB)) : 2;
            // 表格内文字按单元格区域绘制（用默认内边距覆盖）
            TextBody tcopy = cell.text;
            if (!cell.mar_set) { tcopy.lIns = 38100; tcopy.rIns = 38100; tcopy.tIns = 22860; tcopy.bIns = 22860; }
            Shape tmp = cell_owner; tmp.text = tcopy;
            draw_text_body(g, tcopy, tmp, rl + ip_l, rt + ip_t,
                           std::max(1.0, (double)rc.Width - ip_l - ip_r),
                           std::max(1.0, (double)rc.Height - ip_t - ip_b), ctx,
                           t.first_row && r == 0 && cell.fill.is_inherit());
        }
    }
}

static void draw_shape(Graphics& g, const Shape& s_in, DrawCtx& ctx, ImageCache& imgs,
                       double ox, double oy, double kx, double ky) {
    if (s_in.hidden) return;
    Shape s = s_in;
    if (s.cx <= 0 || s.cy <= 0) {
        long long x, y, cx, cy;
        if (ctx.rt->placeholder_geometry(s, x, y, cx, cy)) { s.x = x; s.y = y; s.cx = cx; s.cy = cy; }
    }
    const bool is_group = s.is_group();
    if (!is_group && !is_line_geom(s) && (s.cx <= 0 || s.cy <= 0)) return;

    s.fill = ctx.rt->resolve_fill(s);
    s.line = ctx.rt->resolve_line(s);
    s.fx = ctx.rt->resolve_effects(s);

    // 画布像素矩形（EMU -> px，再叠加组偏移与缩放）
    double w = s.cx * ctx.kx() * kx;
    double h = s.cy * ctx.ky() * ky;
    double x = s.x  * ctx.kx() * kx + ox;
    double y = s.y  * ctx.ky() * ky + oy;
    if (is_line_geom(s)) { if (w < 1) w = 1; if (h < 1) h = 1; }
    if (w <= 0 || h <= 0) return;

    if (is_group) {
        for (const ShapePtr& ch : s.children)
            draw_shape(g, map_child(*ch, s), ctx, imgs, ox, oy, kx, ky);
        return;
    }

    int save = g.Save();
    // 旋转 + 翻转（绕中心）
    {
        double ccx = x + w / 2, ccy = y + h / 2;
        g.TranslateTransform((REAL)ccx, (REAL)ccy);
        if (s.rot != 0) g.RotateTransform((REAL)s.rot);
        if (s.flip_h || s.flip_v)
            g.ScaleTransform((REAL)(s.flip_h ? -1 : 1), (REAL)(s.flip_v ? -1 : 1));
        g.TranslateTransform((REAL)-ccx, (REAL)-ccy);
    }

    RectF rc((REAL)x, (REAL)y, (REAL)w, (REAL)h);

    // 阴影（近似：偏移实心形状）
    if (s.fx.shadow.on && s.fx.shadow.alpha > 0) {
        double d = s.fx.shadow.dist * ctx.kx();
        double rad = s.fx.shadow.dir * PI / 180.0;
        double dx = d * std::cos(rad), dy = d * std::sin(rad);
        Color sc = argb_of(scale_alpha(s.fx.shadow.color, s.fx.shadow.alpha));
        SolidBrush sb(sc);
        if (needs_outline(s)) {
            auto pl = shape_outline(s, w, h);
            GraphicsPath gp;
            for (const auto& line : pl)
                if (line.pts.size() > 1) {
                    std::vector<PointF> ps;
                    for (const auto& p : line.pts)
                        ps.push_back(PointF((REAL)(x + dx + p.x), (REAL)(y + dy + p.y)));
                    gp.AddLines(&ps[0], (INT)ps.size());
                    if (line.closed) gp.CloseFigure();
                }
            g.FillPath(&sb, &gp);
        } else if (s.prst_geom == "ellipse" || s.prst_geom == "oval") {
            g.FillEllipse(&sb, (REAL)(x + dx), (REAL)(y + dy), (REAL)w, (REAL)h);
        } else {
            g.FillRectangle(&sb, (REAL)(x + dx), (REAL)(y + dy), (REAL)w, (REAL)h);
        }
    }

    // 图片
    if (s.is_pic()) {
        Image* img = imgs.get(s.pic_part);
        if (img) {
            double iw = img->GetWidth(), ih = img->GetHeight();
            double l = s.src_rect_l, t = s.src_rect_t, r = s.src_rect_r, b = s.src_rect_b;
            if (l > 0 || t > 0 || r > 0 || b > 0) {
                REAL sl = (REAL)(l * iw), stf = (REAL)(t * ih);
                REAL sw = (REAL)std::max(1.0, (1.0 - l - r) * iw);
                REAL shh = (REAL)std::max(1.0, (1.0 - t - b) * ih);
                g.DrawImage(img, rc, sl, stf, sw, shh, UnitPixel);
            } else {
                g.DrawImage(img, rc, 0, 0, (REAL)iw, (REAL)ih, UnitPixel);
            }
        } else {
            SolidBrush ph(Color(255, 235, 238, 245));
            g.FillRectangle(&ph, rc);
        }
        if (s.line.w > 0 && !s.line.is_none()) {
            auto pen = make_pen(s.line, ctx);
            if (pen) g.DrawRectangle(pen.get(), rc);
        }
        if (!s.text.paras.empty()) draw_text_body(g, s.text, s, x, y, w, h, ctx);
        g.Restore(save);
        return;
    }

    // 表格
    if (s.is_table() && s.table) {
        draw_table(g, s, x, y, w, h, ctx);
        if (s.line.w > 0 && !s.line.is_none()) {
            auto pen = make_pen(s.line, ctx);
            if (pen) g.DrawRectangle(pen.get(), rc);
        }
        g.Restore(save);
        return;
    }

    // 图表 / 媒体占位
    if (s.is_chart() || s.is_media()) {
        SolidBrush ph(Color(255, 240, 242, 246));
        g.FillRectangle(&ph, rc);
        Pen bp(Color(255, 170, 178, 190), 1.0f);
        REAL dash[2] = { 4, 3 }; bp.SetDashPattern(dash, 2);
        g.DrawRectangle(&bp, rc);
        g.Restore(save);
        return;
    }

    // 轮廓路径形状
    if (needs_outline(s)) {
        auto pl = shape_outline(s, w, h);
        if (!pl.empty()) {
            GraphicsPath gp;
            for (const auto& line : pl) {
                if (line.pts.size() < 2) continue;
                std::vector<PointF> ps;
                ps.reserve(line.pts.size());
                for (const auto& p : line.pts)
                    ps.push_back(PointF((REAL)(x + p.x), (REAL)(y + p.y)));
                if (line.closed) gp.AddPolygon(&ps[0], (INT)ps.size());
                else gp.AddLines(&ps[0], (INT)ps.size());
            }
            if (auto br = make_fill_brush(s.fill, x, y, w, h, ctx)) g.FillPath(br.get(), &gp);
            if (s.fx.glow) {
                Color gc = argb_of(scale_alpha(s.fx.glow_color, s.fx.glow_alpha));
                float gw = (float)std::max(2.0, s.fx.glow_rad * ctx.kx() * 2);
                SolidBrush gb(gc);
                Pen gp2(gc, gw); gp2.SetAlignment(PenAlignmentInset);
                g.DrawPath(&gp2, &gp);
                (void)gb;
            }
            if (auto pen = make_pen(s.line, ctx)) g.DrawPath(pen.get(), &gp);
            if (!s.text.paras.empty()) draw_text_body(g, s.text, s, x, y, w, h, ctx);
            g.Restore(save);
            return;
        }
    }

    // 普通矩形 / 圆角 / 椭圆
    bool rounded = (s.prst_geom == "roundRect");
    float rad = 0;
    if (rounded) {
        double adj = 0.16667;
        auto it = s.prst_adj.find("adj");
        if (it != s.prst_adj.end() && it->second > 0) adj = std::min(it->second, 0.5);
        rad = (float)(adj * std::min(w, h));
    }
    GraphicsPath body;
    if (rounded && rad > 0.5f) {
        REAL d = rad * 2;
        body.AddArc(x, y, d, d, 180, 90);
        body.AddArc(x + w - d, y, d, d, 270, 90);
        body.AddArc(x + w - d, y + h - d, d, d, 0, 90);
        body.AddArc(x, y + h - d, d, d, 90, 90);
        body.CloseFigure();
    }

    if (auto br = make_fill_brush(s.fill, x, y, w, h, ctx)) {
        if (s.prst_geom == "ellipse" || s.prst_geom == "oval") g.FillEllipse(br.get(), rc);
        else if (rounded && rad > 0.5f) g.FillPath(br.get(), &body);
        else g.FillRectangle(br.get(), rc);
        // 图片型填充（形状填充图片）
        if (s.fill.kind == FillKind::Blip) {
            Image* img = imgs.get(s.fill.blip_part);
            if (img) {
                double iw = img->GetWidth(), ih = img->GetHeight();
                g.DrawImage(img, rc, 0, 0, (REAL)iw, (REAL)ih, UnitPixel);
            }
        }
    } else if (s.fill.kind == FillKind::Blip) {
        Image* img = imgs.get(s.fill.blip_part);
        if (img) {
            double iw = img->GetWidth(), ih = img->GetHeight();
            g.DrawImage(img, rc, 0, 0, (REAL)iw, (REAL)ih, UnitPixel);
        }
    }
    if (s.fx.glow) {
        Color gc = argb_of(scale_alpha(s.fx.glow_color, s.fx.glow_alpha));
        float gw = (float)std::max(2.0, s.fx.glow_rad * ctx.kx() * 2);
        Pen gp2(gc, gw); gp2.SetAlignment(PenAlignmentInset);
        if (s.prst_geom == "ellipse" || s.prst_geom == "oval") g.DrawEllipse(&gp2, rc);
        else g.DrawRectangle(&gp2, rc);
    }
    if (auto pen = make_pen(s.line, ctx)) {
        if (s.prst_geom == "ellipse" || s.prst_geom == "oval") g.DrawEllipse(pen.get(), rc);
        else if (rounded && rad > 0.5f) g.DrawPath(pen.get(), &body);
        else g.DrawRectangle(pen.get(), rc);
    }

    if (!s.text.paras.empty())
        draw_text_body(g, s.text, s, x, y, w, h, ctx);

    g.Restore(save);
}

// ======================= 整页 =======================
static bool ph_taken(const std::vector<ShapePtr>& shapes, PhType t_, const std::string& idx) {
    for (const ShapePtr& sp : shapes)
        if (sp->is_place_holder && sp->ph_type == t_ && sp->ph_idx == idx) return true;
    return false;
}

void draw_slide(Graphics& g, const Presentation& pres, const Slide& sl,
                DrawCtx& ctx, ImageCache& imgs, bool with_bg_layer) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    Fill bgf = ctx.rt->background_fill(sl);
    if (!bgf.solid_scheme.empty() && ctx.rt->theme) {
        ARGB c; if (ctx.rt->theme->colors.resolve(bgf.solid_scheme, c)) { c.a = bgf.solid.a; bgf.solid = c; }
    }
    Color bgcol = (bgf.kind == FillKind::Solid) ? argb_of(bgf.solid) : Color(255, 255, 255, 255);
    SolidBrush bg(bgcol);
    g.FillRectangle(&bg, (REAL)0, (REAL)0, (REAL)ctx.cw, (REAL)ctx.ch);

    if (with_bg_layer && sl.show_master_sp != "0") {
        const SlideMasterDef* md = ctx.rt->master;
        const SlideLayoutDef* ld = ctx.rt->layout;
        if (md) {
            for (const ShapePtr& sp : md->shapes) {
                if (sp->is_place_holder) {
                    if (ld && ph_taken(ld->shapes, sp->ph_type, sp->ph_idx)) continue;
                    if (ph_taken(sl.shapes, sp->ph_type, sp->ph_idx)) continue;
                }
                draw_shape(g, *sp, ctx, imgs, 0, 0, 1.0, 1.0);
            }
        }
        if (ld) {
            for (const ShapePtr& sp : ld->shapes) {
                if (sp->is_place_holder && ph_taken(sl.shapes, sp->ph_type, sp->ph_idx)) continue;
                draw_shape(g, *sp, ctx, imgs, 0, 0, 1.0, 1.0);
            }
        }
    }
    for (const ShapePtr& sp : sl.shapes)
        draw_shape(g, *sp, ctx, imgs, 0, 0, 1.0, 1.0);
}

// ======================= 命中测试（自顶向下） =======================
ShapePtr hit_test_slide(const Presentation& pres, const Slide& sl,
                        double px, double py, const RenderTheme& rt,
                        double cw, double ch) {
    (void)pres;
    DrawCtx ctx;
    ctx.sw = pres.slide_w; ctx.sh = pres.slide_h; ctx.cw = cw; ctx.ch = ch; ctx.rt = &rt;

    // 展平形状列表，同时保留原始指针用于返回
    struct Entry { ShapePtr orig; Shape geo; };
    std::vector<Entry> flat;

    std::function<void(const std::vector<ShapePtr>&, ShapePtr)> walk =
        [&](const std::vector<ShapePtr>& list, ShapePtr grp_orig) {
        for (const ShapePtr& sp : list) {
            Shape s = *sp;
            if (s.hidden) continue;
            if ((s.cx <= 0 || s.cy <= 0) && s.is_place_holder) {
                long long ex, ey, ecx, ecy;
                if (rt.placeholder_geometry(s, ex, ey, ecx, ecy)) { s.x = ex; s.y = ey; s.cx = ecx; s.cy = ecy; }
            }
            if (s.is_group()) {
                // 把组的子形状展平：子坐标映射到组的局部空间，再通过组 xfrm 映射到幻灯片
                for (const ShapePtr& ch : s.children) {
                    Shape cc = *ch;
                    if (s.ch_ext_cx > 0 && s.ch_ext_cy > 0) {
                        cc.x  = (long long)((double)(ch->x - s.ch_off_x) * s.cx / s.ch_ext_cx);
                        cc.y  = (long long)((double)(ch->y - s.ch_off_y) * s.cy / s.ch_ext_cy);
                        cc.cx = (long long)((double)ch->cx * s.cx / s.ch_ext_cx);
                        cc.cy = (long long)((double)ch->cy * s.cy / s.ch_ext_cy);
                    }
                    cc.x += s.x; cc.y += s.y;
                    cc.rot += s.rot;
                    if (cc.is_group()) {
                        std::vector<ShapePtr> one; one.push_back(std::make_shared<Shape>(cc));
                        walk(one, sp);
                    } else {
                        // orig 必须是模型里的真实子形状，编辑器才能改到它
                        flat.push_back({ch, cc});
                    }
                }
                continue;
            }
            if (s.cx <= 0 && s.cy <= 0 && !is_line_geom(s)) continue;
            flat.push_back({sp, s});
        }
    };
    walk(sl.shapes, nullptr);

    // 自顶向下（z 序）
    for (size_t i = flat.size(); i-- > 0;) {
        const Shape& s = flat[i].geo;
        double x = s.x * ctx.kx(), y = s.y * ctx.ky();
        double w = std::max(s.cx * ctx.kx(), 2.0), h = std::max(s.cy * ctx.ky(), 2.0);
        // 粗筛（含旋转容忍）
        if (s.rot != 0) {
            double dx = px - (x + w / 2), dy = py - (y + h / 2);
            double a = -s.rot * PI / 180.0;
            double rx = dx * std::cos(a) - dy * std::sin(a);
            double ry = dx * std::sin(a) + dy * std::cos(a);
            if (std::fabs(rx) > w / 2 + 2 || std::fabs(ry) > h / 2 + 2) continue;
        } else {
            if (px < x - 2 || px > x + w + 2 || py < y - 2 || py > y + h + 2) continue;
        }

        bool has_fill = !s.fill.is_none() && s.fill.kind != FillKind::Inherit;
        if (needs_outline(s) && has_fill) {
            auto pl = shape_outline(s, w, h);
            if (!pl.empty()) {
                double lx = px, ly = py;
                if (s.rot != 0) {
                    double a = -s.rot * PI / 180.0;
                    double ddx = lx - (x + w / 2), ddy = ly - (y + h / 2);
                    lx = x + w / 2 + ddx * std::cos(a) - ddy * std::sin(a);
                    ly = y + h / 2 + ddx * std::sin(a) + ddy * std::cos(a);
                }
                if (svgpath::point_in_polylines(pl, lx - x, ly - y)) return flat[i].orig;
            }
        }
        // 矩形内命中
        if (px >= x && px <= x + w && py >= y && py <= y + h) return flat[i].orig;
    }
    return nullptr;
}

// ======================= 导出 PNG =======================
bool render_slide_png(const Presentation& pres, size_t slide_idx, int w, int h,
                      const std::string& out_path, std::string& err) {
    if (slide_idx >= pres.slides.size()) { err = "页码越界"; return false; }
    if (w <= 0 || h <= 0) { err = "尺寸非法"; return false; }
    ULONG_PTR tok = 0;
    GdiplusStartupInput si;
    bool own = (GdiplusStartup(&tok, &si, nullptr) == Ok);
    bool ok = false;
    {
        ImageCache imgs(pres);
        RenderTheme rt = make_render_theme(pres, *pres.slides[slide_idx]);
        DrawCtx ctx;
        ctx.sw = pres.slide_w; ctx.sh = pres.slide_h; ctx.cw = w; ctx.ch = h;
        ctx.rt = &rt; ctx.slide_idx = (int)slide_idx; ctx.slide_total = (int)pres.slides.size();
        SYSTEMTIME st; GetLocalTime(&st);
        char db[32]; snprintf(db, sizeof db, "%04d/%02d/%02d", st.wYear, st.wMonth, st.wDay);
        ctx.date_str = db;

        Bitmap bmp(w, h, PixelFormat32bppARGB);
        if (bmp.GetLastStatus() == Ok) {
            {
                Graphics g(&bmp);
                draw_slide(g, pres, *pres.slides[slide_idx], ctx, imgs, true);
                if (g.GetLastStatus() != Ok) err = "绘制失败";
            }
            // GDI+ 32bppARGB 在小端内存中按字节是 BGRA，PNG 需要 RGBA
            Rect area(0, 0, w, h);
            BitmapData bd;
            memset(&bd, 0, sizeof bd);
            if (bmp.LockBits(&area, ImageLockModeRead, PixelFormat32bppARGB, &bd) == Ok) {
                std::vector<uint8_t> rgba((size_t)w * h * 4);
                const uint8_t* src = (const uint8_t*)bd.Scan0;
                for (int y = 0; y < h; ++y) {
                    const uint8_t* srow = src + (size_t)y * bd.Stride;
                    uint8_t* drow = &rgba[(size_t)y * w * 4];
                    for (int x = 0; x < w; ++x) {
                        drow[x*4 + 0] = srow[x*4 + 2];   // R
                        drow[x*4 + 1] = srow[x*4 + 1];   // G
                        drow[x*4 + 2] = srow[x*4 + 0];   // B
                        drow[x*4 + 3] = srow[x*4 + 3];   // A
                    }
                }
                bmp.UnlockBits(&bd);
                std::string e2;
                ok = write_png(out_path, w, h, &rgba[0], w * 4, &e2);
                if (!ok) err = e2;
            } else {
                err = "锁定像素失败";
            }
        } else {
            err = "位图创建失败";
        }
    }
    if (own) GdiplusShutdown(tok);
    return ok;
}

} // namespace ppt
