#include "pptx_io.h"
#include "../base/strutil.h"
#include "../base/zip.h"
#include "../oxml/xmldom.h"
#include "../oxml/reltype.h"
#include "../core/preset_geom.h"

#include <algorithm>
#include <set>
#include <functional>

namespace ppt {

namespace {

// 切换/动画辅助（定义在本文件后半部分，读取时先声明）
static int tr_dir_val(const std::string& s);
static std::string anim_kind_of_preset(int p);

// ==================== 解析辅助工具 ====================

static bool emu_of(const std::string& s, long long& v) {
    v = parse_ll(s, 0);
    if (v == 0 && s != "0" && s.find("0") != 0) return false;
    return true;
}

static AlignL parse_align(const std::string& s) {
    if (s == "ctr") return AlignL::Center;
    if (s == "r" || s == "rtl") return AlignL::Right;
    if (s == "just" || s == "justLow") return AlignL::Justify;
    if (s == "dist") return AlignL::Distributed;
    return AlignL::Left;
}

static VAlign parse_anchor(const std::string& s) {
    if (s == "ctr" || s == "mid") return VAlign::Center;
    if (s == "b") return VAlign::Bottom;
    return VAlign::Top;
}

static DashType parse_dash(const std::string& s) {
    if (s == "sqDot" || s == "sysDot") return DashType::SquareDot;
    if (s == "rndDot") return DashType::RoundDot;
    if (s == "dash" || s == "sysDash") return DashType::Dash;
    if (s == "dashDot") return DashType::DashDot;
    if (s == "lgDash") return DashType::LongDash;
    if (s == "lgDashDot") return DashType::LongDashDot;
    if (s == "lgDashDotDot") return DashType::LongDashDotDot;
    return DashType::Solid;
}

static LineCap parse_cap(const std::string& s) {
    if (s == "sq") return LineCap::Square;
    if (s == "rnd") return LineCap::Round;
    if (s == "flat") return LineCap::Flat;
    return LineCap::Flat;
}

static PhType parse_ph(const std::string& s) {
    if (s == "title" || s == "ctrTitle") return PhType::Title;
    if (s == "body") return PhType::Body;
    if (s == "subTitle" || s == "centerTitle") return PhType::SubTitle;
    if (s == "sldTitle") return PhType::SlideTitle;
    if (s == "obj") return PhType::Object;
    if (s == "chart") return PhType::Chart;
    if (s == "tbl") return PhType::Table;
    if (s == "pic") return PhType::Pic;
    if (s == "media") return PhType::Media;
    if (s == "clipArt") return PhType::ClipArt;
    if (s == "dt") return PhType::Date;
    if (s == "ftr") return PhType::Footer;
    if (s == "sldNum") return PhType::SlideNumber;
    return PhType::Other;
}

static ShapeKind shape_kind_from(const std::string& tag) {
    if (tag == "p:sp") return ShapeKind::Rect;
    if (tag == "p:pic") return ShapeKind::Picture;
    if (tag == "p:grpSp") return ShapeKind::Group;
    if (tag == "p:cxnSp") return ShapeKind::Line;
    if (tag == "p:graphicFrame") return ShapeKind::Unknown;  // table/chart 由内部判断
    return ShapeKind::Unknown;
}

// ==================== 颜色解析 ====================

static ARGB parse_srgb(const std::string& hex, uint8_t a = 255) {
    return ARGB::from_hex(hex, a);
}

static ARGB parse_prst_clr(const std::string& name, const std::string& scheme) {
    ARGB c;
    if (prst_color(name, c)) return c;
    return ARGB(255, 0, 0, 0);
}

// OOXML 颜色变换的 val 均为千分比（100000 = 100%）
static void apply_color_mod(ARGB& c, const XmlPtr& n) {
    for (const XmlPtr& ch : n->children) {
        if (ch->tag == "a:lumMod") { double v = ch->get_dbl("val", 100000); c = col_lum_mod(c, v / 1000.0); }
        else if (ch->tag == "a:lumOff") { double v = ch->get_dbl("val", 0); c = col_lum_off(c, v / 1000.0); }
        else if (ch->tag == "a:tint") { double v = ch->get_dbl("val", 0); c = col_tint(c, 100.0 - v / 1000.0); }
        else if (ch->tag == "a:shade") { double v = ch->get_dbl("val", 0); c = col_shade(c, v / 1000.0); }
        else if (ch->tag == "a:alpha") { double v = ch->get_dbl("val", 100000); c.a = (uint8_t)(v / 100000.0 * 255.0 + 0.5); }
        else if (ch->tag == "a:satMod") { double v = ch->get_dbl("val", 100000); c = col_sat(c, v / 1000.0); }
        else if (ch->tag == "a:inv") { c = col_inv(c); }
        else if (ch->tag == "a:gamma") { double v = ch->get_dbl("val", 100000); c = col_gamma(c, v / 1000.0); }
        // 嵌套的 hslClr/srgbClr/... 继续递归
    }
}

// schemeClr 的 mod 只更新 scheme_name，颜色由调用方带入
static void apply_color_mod_scheme(const XmlPtr& n, std::string& scheme_name) {
    // scheme_name 已在外部记录
}

// 解析一个颜色容器（solidFill 或裸颜色节点）：
// 返回找到与否；scheme 名与 alpha（百分比）通过引用带出。
// 注意：srgb/prst/sys 的 alpha 已由各颜色变换统一处理（apply_color_mod 内含 a:alpha），
// 这里只把 a:alpha 值作为 alpha_pct 报告给调用方，不再二次乘算 out.a。
static bool parse_color_ex(const XmlPtr& holder, ARGB& out, std::string& scheme, double& alpha_pct) {
    auto report_alpha = [&](const XmlPtr& n) {
        XmlPtr al = n->first("a:alpha");
        alpha_pct = al ? al->get_dbl("val", 100000) / 1000.0 : 100.0;
    };
    for (const XmlPtr& ch : holder->children) {
        const std::string& t = ch->tag;
        if (t == "a:srgbClr") {
            out = parse_srgb(ch->get("val"));
            apply_color_mod(out, ch);            // 含 a:alpha 对 out.a 的作用
            report_alpha(ch);
            return true;
        }
        if (t == "a:prstClr") {
            ARGB c; if (!prst_color(ch->get("val"), c)) c = ARGB(255, 0, 0, 0);
            apply_color_mod(c, ch);
            out = c; report_alpha(ch);
            return true;
        }
        if (t == "a:sysClr") {
            out = parse_srgb(ch->get("lastClr", "000000"));
            apply_color_mod(out, ch);
            report_alpha(ch);
            return true;
        }
        if (t == "a:schemeClr") {
            scheme = ch->get("val", scheme);
            report_alpha(ch);
            return true;
        }
    }
    return false;
}

// 旧接口：保持既有调用点可用（schemeClr 返回默认色但标记 found）
static ARGB parse_color_node(const XmlPtr& n_to, const ARGB& def, bool& found, std::string& scheme_name) {
    ARGB out = def; double alpha = 100;
    found = parse_color_ex(n_to, out, scheme_name, alpha);
    return out;
}

// ==================== 填充解析 ====================

static Fill parse_fill(const XmlPtr& shape_props, const std::string& scheme) {
    Fill f;
    // 查找第一个填充子元素
    for (const XmlPtr& ch : shape_props->children) {
        std::string t = ch->tag;
        if (t == "a:noFill") { f.kind = FillKind::NoFill; return f; }
        if (t == "a:solidFill") {
            f.kind = FillKind::Solid;
            ARGB c(255, 0, 0, 0); std::string sc = scheme; double a = 100;
            if (parse_color_ex(ch, c, sc, a)) { f.solid = c; f.solid_scheme = sc; }
            return f;
        }
        if (t == "a:gradFill") {
            f.kind = FillKind::Gradient;
            XmlPtr lin = ch->first("a:lin");
            XmlPtr path = ch->first("a:path");
            if (lin) { f.grad_kind = GradKind::Linear; f.ang = lin->get_dbl("ang", 0) / 60000.0; }
            else if (path) {
                std::string pv = path->get("path", "linear");
                f.grad_kind = (pv == "circle" || pv == "rect") ? GradKind::Radial : GradKind::Linear;
            }
            // 渐变站点：OOXML 为 <a:gsLst><a:gs/></a:gsLst>，兼容直接子节点写法
            std::vector<XmlPtr> gs_nodes;
            XmlPtr gsLst = ch->first("a:gsLst");
            if (gsLst) gs_nodes = gsLst->all("a:gs");
            if (gs_nodes.empty()) gs_nodes = ch->all("a:gs");
            for (const XmlPtr& gs_n : gs_nodes) {
                GradientStop s;
                s.pos = gs_n->get_dbl("pos", 0) / 100000.0;      // 千分比 -> 0..1
                ARGB c(255, 0, 0, 0); std::string sc = scheme; double a = 100;
                // gs 的子节点即颜色容器（srgbClr / schemeClr 等）
                for (const XmlPtr& sub : gs_n->children) {
                    const std::string& st = sub->tag;
                    if (st == "a:srgbClr" || st == "a:schemeClr" || st == "a:prstClr" || st == "a:sysClr") {
                        XmlPtr wrap = XmlNode::make("a:fill");
                        wrap->add_child(sub);
                        if (parse_color_ex(wrap, c, sc, a)) { s.color = c; s.scheme = sc; s.alpha = a; }
                        break;
                    }
                }
                f.stops.push_back(s);
            }
            return f;
        }
        if (t == "a:blipFill") {
            f.kind = FillKind::Blip;
            XmlPtr blip = ch->first("a:blip");
            if (blip) f.blip_part = blip->get("r:embed");
            if (ch->first("a:stretch")) f.fill_mode = FillMode::Stretch;
            else if (ch->first("a:tile")) f.fill_mode = FillMode::Tile;
            if (blip) { XmlPtr alpha = blip->first("a:alphaModFix"); if (alpha) f.blip_alpha = alpha->get_dbl("amt", 100000) / 1000.0; }
            // 图片填充的裁剪
            XmlPtr srcR = ch->first("a:srcRect");
            if (srcR) {
                // 形状填充用 srcRect 为百分比(千分比)，这里存到 fill 的 tile_ratio 无意义，
                // 交由形状 pic 处理；blipFill 作为形状填充时裁剪近似忽略
            }
            return f;
        }
        if (t == "a:pattFill") {
            f.kind = FillKind::Pattern;
            f.prst_pat = ch->get("prst", "lgUp");
            XmlPtr bg = ch->first("a:bgClr"); XmlPtr fg = ch->first("a:fgClr");
            ARGB c(255,255,255,255); std::string sc; double a = 100;
            if (bg && parse_color_ex(bg, c, sc, a)) f.pat_bg = c;
            c = ARGB(255,0,0,0); sc.clear();
            if (fg && parse_color_ex(fg, c, sc, a)) f.pat_fg = c;
            return f;
        }
        if (t == "a:grpFill") { f.kind = FillKind::Group; return f; }
    }
    f.kind = FillKind::Inherit;
    return f;
}

// ==================== 线条解析 ====================

// 解析一个 <a:ln> 节点（形状连线或单元格边框通用）
static Line parse_line_of(const XmlPtr& ln) {
    Line l;
    l.inherit = false;
    if (!ln) { l.inherit = true; return l; }
    // w 缺省 0.75pt（9525 EMU）；只有显式 noFill 才是无线
    l.w = ln->get_num("w", 9525);
    XmlPtr nf = ln->first("a:noFill");
    if (nf) { l.explicit_none = true; l.w = 0; return l; }

    bool cf = false; std::string sc; double alpha = 100;
    ARGB c(255, 0x60, 0x60, 0x60);
    XmlPtr sf = ln->first("a:solidFill");
    if (sf) cf = parse_color_ex(sf, c, sc, alpha);
    else    cf = parse_color_ex(ln, c, sc, alpha);
    if (cf) { l.color = c; l.color_scheme = sc; }
    if (l.w == 0) l.w = 9525;

    XmlPtr pd = ln->first("a:prstDash");
    if (pd) l.dash = parse_dash(pd->get("val", "solid"));
    XmlPtr cap = ln->first("a:cap");
    if (cap) l.cap = parse_cap(cap->get("val", "flat"));
    XmlPtr jd = ln->first("a:round");  if (jd) l.join = 0;
    XmlPtr bv = ln->first("a:bevel");  if (bv) l.join = 1;
    XmlPtr mt = ln->first("a:miter");  if (mt) l.join = 2;

    XmlPtr hd = ln->first("a:headEnd");
    if (hd) { l.has_head = true; l.head.prst = hd->get("type", "none"); l.head.w = hd->get_dbl("w", 3); l.head.l = hd->get_dbl("len", 3); }
    XmlPtr tl = ln->first("a:tailEnd");
    if (tl) { l.has_tail = true; l.tail.prst = tl->get("type", "none"); l.tail.w = tl->get_dbl("w", 3); l.tail.l = tl->get_dbl("len", 3); }
    return l;
}

static Line parse_line(const XmlPtr& spPr) {
    XmlPtr ln = spPr->first("a:ln");
    if (!ln) { Line l; l.inherit = true; l.explicit_none = false; return l; }
    return parse_line_of(ln);
}

// ==================== 效果解析（阴影/发光/柔化/棱边） ====================

static void parse_effects(const XmlPtr& spPr, Shape& s) {
    XmlPtr fx = spPr->first("a:effectLst");
    if (!fx) return;
    for (const XmlPtr& ch : fx->children) {
        const std::string& t = ch->tag;
        if (t == "a:outerShdw") {
            Shadow& sh = s.fx.shadow;
            sh.on = true;
            sh.blur_rad = ch->get_num("blurRad", 0);
            sh.dist = ch->get_num("dist", 0);
            sh.dir = ch->get_dbl("dir", 0) / 60000.0;
            sh.alpha = ch->get_dbl("alpha", 100000) / 1000.0;
            std::string rot = ch->get("rotWithShape", "1");
            (void)rot;
            XmlPtr sx = ch->first("a:srgbClr"); XmlPtr scm = ch->first("a:schemeClr");
            ARGB c(255, 0, 0, 0); std::string scheme; double a = 100;
            if (sx || scm) { if (parse_color_ex(ch, c, scheme, a)) { sh.color = c; sh.color_scheme = scheme; sh.alpha = a; } }
        } else if (t == "a:softEdge") {
            s.fx.soft_edge = true; s.fx.soft_rad = ch->get_num("rad", 38100);
        } else if (t == "a:glow") {
            s.fx.glow = true;
            s.fx.glow_rad = ch->get_num("rad", 6350);
            ARGB c(255, 0, 0, 0); std::string scheme; double a = 100;
            if (parse_color_ex(ch, c, scheme, a)) { s.fx.glow_color = c; s.fx.glow_scheme = scheme; s.fx.glow_alpha = a; }
        } else if (t == "a:bevelT") {
            s.fx.bevel = true;
            s.fx.bevel_w = ch->get_num("w", 63500);
            s.fx.bevel_h = ch->get_num("h", 63500);
            s.fx.bevel_prst = ch->get("prst", "circle");
        } else if (t == "a:innerShdw") {
            Shadow& sh = s.fx.shadow;   // 内阴影近似为外阴影渲染
            if (!sh.on) {
                sh.on = true;
                sh.blur_rad = sh.blur_rad ? sh.blur_rad : ch->get_num("blurRad", 0);
                sh.dist = ch->get_num("dist", 0);
                sh.dir = ch->get_dbl("dir", 0) / 60000.0;
                ARGB c(255, 0, 0, 0); std::string scheme; double a = 40;
                if (parse_color_ex(ch, c, scheme, a)) { sh.color = c; sh.color_scheme = scheme; sh.alpha = a; }
            }
        }
    }
}

// ==================== 几何解析 ====================

// custGeom -> 归一化(0..1) SVG path。a:path 的 w/h 为该路径坐标系尺寸
static std::string custgeom_to_svg(const XmlPtr& cust) {
    XmlPtr pl = cust->first("a:pathLst");
    if (!pl) return "";
    std::string out;
    bool first_path = true;
    for (const XmlPtr& p : pl->all("a:path")) {
        double pw = p->get_dbl("w", 0), ph = p->get_dbl("h", 0);
        if (pw <= 0) pw = 1; if (ph <= 0) ph = 1;
        bool started = !first_path;
        first_path = false;
        for (const XmlPtr& cmd : p->children) {
            const std::string& t = cmd->tag;
            auto pt_css = [&](const XmlPtr& n, const char* xa, const char* ya)->std::string {
                double x = n->get_dbl(xa, 0) / pw, y = n->get_dbl(ya, 0) / ph;
                return fmt_num(x, 5) + "," + fmt_num(y, 5);
            };
            if (t == "a:moveTo") { out += "M" + pt_css(cmd->first("a:pt"), "x", "y") + " "; }
            else if (t == "a:lnTo") { out += "L" + pt_css(cmd->first("a:pt"), "x", "y") + " "; }
            else if (t == "a:cubicBezTo") {
                auto pts = cmd->all("a:pt");
                if (pts.size() >= 3) out += "C" + pt_css(pts[0], "x", "y") + " " + pt_css(pts[1], "x", "y") + " " + pt_css(pts[2], "x", "y") + " ";
            }
            else if (t == "a:quadBezTo") {
                auto pts = cmd->all("a:pt");
                if (pts.size() >= 2) out += "Q" + pt_css(pts[0], "x", "y") + " " + pt_css(pts[1], "x", "y") + " ";
            }
            else if (t == "a:arcTo") {
                // 近似：把端点当直线（arc 参数复杂，保真优先画端点连线）
                XmlPtr ext = cmd->first("a:ext"); (void)ext;
                auto pts = cmd->all("a:pt");
                if (!pts.empty()) out += "L" + pt_css(pts.back(), "x", "y") + " ";
            }
            else if (t == "a:close") { out += "Z "; }
        }
    }
    return out;
}

static void parse_geometry(const XmlPtr& spPr, Shape& s) {
    XmlPtr xfrm = spPr->first("a:xfrm");
    if (xfrm) {
        XmlPtr off = xfrm->first("a:off"); if (off) { s.x = off->get_num("x", 0); s.y = off->get_num("y", 0); }
        XmlPtr ext = xfrm->first("a:ext"); if (ext) { s.cx = ext->get_num("cx", 0); s.cy = ext->get_num("cy", 0); }
        s.rot = xfrm->get_dbl("rot", 0) / 60000.0;
        s.flip_h = xfrm->get("flipH", "") == "1";
        s.flip_v = xfrm->get("flipV", "") == "1";
    }
    // 几何预设
    XmlPtr prst = spPr->first("a:prstGeom");
    if (prst) {
        s.prst_geom = prst->get("prst", "rect");
        XmlPtr av = prst->first("a:avLst");
        if (av) {
            for (const XmlPtr& gd : av->all("a:gd")) {
                std::string nm = gd->get("name", "adj");
                // fmla 形如 "val 16667" 或 "ss" 公式，取最后一个数字
                std::string fmla = gd->get("fmla", "");
                std::vector<std::string> parts; split(fmla, ' ', parts);
                double raw = 0;
                for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
                    long long t = parse_ll(*it, -1);
                    if (t >= 0) { raw = (double)t; break; }
                }
                s.prst_adj[nm] = raw / 100000.0;
            }
        }
    }
    XmlPtr cust = spPr->first("a:custGeom");
    if (cust) {
        s.kind = ShapeKind::Freeform;
        s.prst_geom = "custom";
        s.svg_path = custgeom_to_svg(cust);
        for (const XmlPtr& cmd : cust->all_deep("a:path")) { s.path_cmds.push_back(cmd->to_xml(false)); }
    }
}

// 组合形状的子坐标空间（xfrm 的 chOff/chExt）
static void parse_group_xfrm(const XmlPtr& grpSpPr, Shape& s) {
    XmlPtr xfrm = grpSpPr ? grpSpPr->first("a:xfrm") : nullptr;
    if (!xfrm) return;
    XmlPtr off = xfrm->first("a:off"); if (off) { s.x = off->get_num("x", 0); s.y = off->get_num("y", 0); }
    XmlPtr ext = xfrm->first("a:ext"); if (ext) { s.cx = ext->get_num("cx", 0); s.cy = ext->get_num("cy", 0); }
    XmlPtr chOff = xfrm->first("a:chOff"); if (chOff) { s.ch_off_x = chOff->get_num("x", 0); s.ch_off_y = chOff->get_num("y", 0); }
    XmlPtr chExt = xfrm->first("a:chExt"); if (chExt) { s.ch_ext_cx = chExt->get_num("cx", 0); s.ch_ext_cy = chExt->get_num("cy", 0); }
    s.rot = xfrm->get_dbl("rot", 0) / 60000.0;
}

// ==================== 形状样式引用 <p:style> ====================

static void parse_shape_style(const XmlPtr& sp, Shape& s) {
    XmlPtr st = sp->first("p:style");
    if (!st) return;
    XmlPtr fr = st->first("a:fillRef");
    if (fr) { s.fill_ref_idx = (int)fr->get_num("idx", 0); s.style_ref_fill = fr->child_attr("a:schemeClr", "val", ""); }
    XmlPtr lr = st->first("a:lnRef");
    if (lr) { s.ln_ref_idx = (int)lr->get_num("idx", 0); s.style_ref_ln = lr->child_attr("a:schemeClr", "val", ""); }
    XmlPtr er = st->first("a:effectRef");
    if (er) s.eff_ref_idx = (int)er->get_num("idx", 0);
    XmlPtr fr2 = st->first("a:fontRef");
    if (fr2) s.style_ref = fr2->child_attr("a:schemeClr", "val", "");
}

// ==================== 文本解析 ====================

static void parse_text_props(const XmlPtr& bodyPr, TextBody& tb) {
    tb.autofit = AutoFit::None;
    XmlPtr norm = bodyPr->first("a:normAutofit");
    if (norm) { tb.autofit = AutoFit::Normal;
        tb.fontScale = norm->get_dbl("fontScale", 100000) / 1000.0;
        tb.lnSpcReduction = norm->get_dbl("lnSpcReduction", 0) / 1000.0; }
    XmlPtr nof = bodyPr->first("a:noAutofit"); if (nof) { tb.autofit = AutoFit::NoShrink; }
    XmlPtr shape = bodyPr->first("a:spAutoFit"); if (shape) { tb.autofit = AutoFit::Shape; }
    tb.anchor = parse_anchor(bodyPr->get("anchor", "t"));
    tb.anchor_set = !bodyPr->get("anchor", "").empty();
    if (bodyPr->get("anchorCtr", "0") == "1") { tb.anchor = VAlign::Center; tb.anchor_set = true; }
    tb.lIns = bodyPr->get_num("lIns", 91440);
    tb.tIns = bodyPr->get_num("tIns", 45720);
    tb.rIns = bodyPr->get_num("rIns", 91440);
    tb.bIns = bodyPr->get_num("bIns", 45720);
    tb.wrap = bodyPr->get("wrap", "square") != "none";
    tb.rtl = bodyPr->get("rtlCol", "0") == "1";
    // 文字方向：eaVert 中日韩竖排；vert/vert270 旋转排
    tb.vert_dir = bodyPr->get("vert", "");
    tb.vert = (tb.vert_dir == "eaVert" || tb.vert_dir == "vert" || tb.vert_dir == "vert270" ||
               tb.vert_dir == "mongolianVert" || tb.vert_dir == "vert270R" || tb.vert_dir == "vertR");
}

static void parse_run_props(const XmlPtr& rPr, RunProps& rp) {
    rp.sz = rPr->get_num("sz", 0); if (rp.sz) rp.sz_set = true;
    rp.b_set = rPr->has_attr("b"); if (rp.b_set) rp.b = rPr->get("b", "0") == "1";
    rp.i_set = rPr->has_attr("i"); if (rp.i_set) rp.i = rPr->get("i", "0") == "1";
    rp.u_set = !rPr->get("u", "").empty(); if (rp.u_set) rp.u = rPr->get("u", "none");
    rp.strike = rPr->get("strike", "noStrike") == "sngStrike";
    if (rPr->has_attr("baseline")) { rp.baseline_set = true; rp.baseline = (int)rPr->get_num("baseline", 0); }
    if (rPr->has_attr("spc")) { rp.spc_set = true; rp.spc = rPr->get_dbl("spc", 0) / 100.0; }
    // 字体：typeface 是属性不是文本
    rp.latin = rPr->child_attr("a:latin", "typeface", "");
    rp.ea = rPr->child_attr("a:ea", "typeface", "");
    rp.cs = rPr->child_attr("a:cs", "typeface", "");

    // 颜色与链接
    XmlPtr hlink = rPr->first("a:hlinkClick");
    if (hlink) {
        rp.link_target = hlink->get("r:id");
        if (rp.link_target.empty()) rp.link_target = hlink->get("r:href", "");
        if (rp.link_target.size() > 2 && rp.link_target[0] == 'r') rp.hyperlink_part = rp.link_target;  // rId
        else rp.hyperlink_is_ext = true;
    }
    // 颜色：solidFill 里的 srgbClr / schemeClr
    XmlPtr sf = rPr->first("a:solidFill");
    if (sf) {
        ARGB c; std::string scheme; double alpha = 100;
        if (parse_color_ex(sf, c, scheme, alpha)) {
            rp.color = c; rp.color_set = true; rp.color_scheme = scheme;
        }
    }
}

static void parse_paragraph(const XmlPtr& p_node, TextBody& tb) {
    TextParagraph para;
    XmlPtr pPr = p_node->first("a:pPr");
    if (pPr) {
        para.props.marL = pPr->get_num("marL", 0); para.props.marL_set = pPr->has_attr("marL");
        para.props.marR = pPr->get_num("marR", 0);
        para.props.indent = pPr->get_num("indent", 0);
        if (pPr->has_attr("lvl")) { para.props.lvl_set = true; para.props.lvl = (int)pPr->get_num("lvl", 0); }
        if (pPr->has_attr("algn")) { para.props.algn_set = true; para.props.algn = parse_align(pPr->get("algn", "")); }
        if (pPr->has_attr("rtl")) para.props.rtl = pPr->get("rtl", "0") == "1";
        if (pPr->has_attr("defTabSz")) { para.props.defTabSz_set = true; para.props.defTabSz = pPr->get_num("defTabSz", 0); }
        // 行距：lnSpc 子节点 spcPct（千分比）或 spcPts（百分磅）
        XmlPtr lnspc = pPr->first("a:lnSpc");
        if (lnspc) {
            XmlPtr pct = lnspc->first("a:spcPct");
            XmlPtr pts = lnspc->first("a:spcPts");
            if (pct) { para.props.lnspc_pct = pct->get_dbl("val", 100000) / 1000.0; para.props.lnspc_set = true; }
            else if (pts) { para.props.lnspc_pts = pts->get_num("val", 0); para.props.lnspc_set = true; }
        }
        XmlPtr sb4 = pPr->first("a:spcBef");
        if (sb4) { XmlPtr p2 = sb4->first("a:spcPts"); para.props.spcbef = p2 ? p2->get_num("val", 0) : 0; para.props.spcbef_set = true; }
        XmlPtr sa4 = pPr->first("a:spcAft");
        if (sa4) { XmlPtr p2 = sa4->first("a:spcPts"); para.props.spcaft = p2 ? p2->get_num("val", 0) : 0; para.props.spcaft_set = true; }
        XmlPtr tabLst = pPr->first("a:tabLst");
        if (tabLst) for (const XmlPtr& t : tabLst->all("a:tab")) {
            TabStop ts; ts.pos = t->get_num("pos", 0);
            std::string al = t->get("algn", "l"); ts.algn = al == "ctr" ? 1 : (al == "r" ? 2 : 0);
            para.tabs.push_back(ts);
        }
        // 项目符号 / 自动编号
        XmlPtr buChar = pPr->first("a:buChar");
        if (buChar) { para.bullet.kind = BulletKind::Char;
            para.bullet.char_ = buChar->get("char", "\xE2\x80\xA2");
            para.bullet.font = buChar->get("font", ""); }
        XmlPtr buAuto = pPr->first("a:buAutoNum");
        if (buAuto) {
            para.bullet.kind = BulletKind::AutoNum;
            para.bullet.num_arabic = buAuto->get("type", "arabicPeriod");
            const std::string& tp = para.bullet.num_arabic;
            if (tp == "arabicPeriod") para.bullet.num_fmt = "1.";
            else if (tp == "arabicParenR") para.bullet.num_fmt = "1)";
            else if (tp == "alphaUPeriod") para.bullet.num_fmt = "A.";
            else if (tp == "alphaUParenR") para.bullet.num_fmt = "A)";
            else if (tp == "romanUPeriod") para.bullet.num_fmt = "I.";
            else if (tp == "none") para.bullet.kind = BulletKind::None;
            else para.bullet.num_fmt = "1.";
            XmlPtr bs = pPr->first("a:buSzPct"); if (bs) para.bullet.size_pct = bs->get_dbl("val", 100000) / 1000.0;
            XmlPtr bc = pPr->first("a:buClr");
            if (bc) { ARGB c; std::string scheme; double a = 100;
                if (parse_color_ex(bc, c, scheme, a)) { para.bullet.color = c; para.bullet.color_set = true; para.bullet.color_scheme = scheme; } }
        }
        XmlPtr buNone = pPr->first("a:buNone");
        if (buNone) para.bullet.kind = BulletKind::None;
        // 段落级默认字形（defRPr）：run 缺 sz/color/字体时继承它
        XmlPtr defRPr = pPr->first("a:defRPr");
        if (defRPr) { parse_run_props(defRPr, para.para_default); para.para_default_set = true; }
    }
    for (const XmlPtr& r_node : p_node->all("a:r")) {
        TextRun run;
        XmlPtr rPr = r_node->first("a:rPr");
        if (rPr) parse_run_props(rPr, run.props);
        run.text = r_node->child_text("a:t", "");
        para.runs.push_back(std::move(run));
    }
    // 段内换行 a:br
    for (size_t i = 0; i < p_node->all("a:br").size(); ++i) {
        TextRun run; run.text = "\n";
        if (para.para_default_set) run.props = para.para_default;
        para.runs.push_back(std::move(run));
    }
    // 字段（页码/日期）
    for (const XmlPtr& fld : p_node->all("a:fld")) {
        TextRun run;
        XmlPtr rPr = fld->first("a:rPr");
        if (rPr) parse_run_props(rPr, run.props);
        std::string tp = fld->get("type", "slidenum");
        if (tp == "slidenum") run.text = "{slide_num}";
        else if (tp == "datetime") run.text = "{date}";
        else run.text = fld->child_text("a:t", "");
        para.runs.push_back(std::move(run));
    }
    XmlPtr endRPr = p_node->first("a:endParaRPr");
    if (endRPr) { para.endParaRPr_set = true; parse_run_props(endRPr, para.end_props); }
    tb.paras.push_back(std::move(para));
}

static void parse_text(const XmlPtr& txBody, Shape& s) {
    if (!txBody) return;
    XmlPtr bodyPr = txBody->first("a:bodyPr");
    if (bodyPr) parse_text_props(bodyPr, s.text);
    for (const XmlPtr& p_node : txBody->all("a:p")) parse_paragraph(p_node, s.text);
    s.has_text = !s.text.paras.empty();
}

// ==================== 表格解析 ====================

static void parse_table(const XmlPtr& tbl_node, Shape& s) {
    if (!tbl_node) return;
    s.kind = ShapeKind::Table;
    s.table = std::make_shared<Table>();
    Table& tbl = *s.table;

    XmlPtr tblPr = tbl_node->first("a:tblPr");
    if (tblPr) {
        tbl.style_id = tblPr->get("id", "");
        tbl.first_row = tblPr->get("firstRow", "1") == "1";
        tbl.first_col = tblPr->get("firstCol", "0") == "1";
        tbl.band_row = tblPr->get("bandRow", "1") == "1";
        tbl.band_col = tblPr->get("bandCol", "0") == "1";
    }

    // 列宽
    XmlPtr tGrid = tbl_node->first("a:tblGrid");
    if (tGrid) for (const XmlPtr& gc : tGrid->all("a:gridCol")) tbl.cols.push_back(gc->get_num("w", 0));
    // 行
    auto trs = tbl_node->all("a:tr");
    tbl.rows.resize(trs.size(), 0);
    tbl.cells.resize(trs.size());
    for (size_t ri = 0; ri < trs.size(); ++ri) {
        long long rh = trs[ri]->get_num("h", 0);
        tbl.rows[ri] = rh;
        auto tcs = trs[ri]->all("a:tc");
        tbl.cells[ri].resize(tcs.size());
        for (size_t ci = 0; ci < tcs.size(); ++ci) {
            TableCell& cell = tbl.cells[ri][ci];
            XmlPtr tcPr = tcs[ci]->first("a:tcPr");
            if (tcPr) {
                cell.span_h = tcPr->get_num("gridSpan", 1);
                cell.span_v = tcPr->get_num("rowSpan", 1);
                std::string hm = tcPr->get("hMerge", "0");
                std::string vm = tcPr->get("vMerge", "0");
                cell.merge_h = hm == "1";
                cell.merge_v = vm == "1";
                cell.dir = tcPr->get("textDirection", "") == "vert" ? 1 : 0;
                cell.anchor = parse_anchor(tcPr->get("anchor", "t"));
                cell.anchor_set = !tcPr->get("anchor", "").empty();
                cell.marL = tcPr->get_num("marL", 91440);
                cell.marR = tcPr->get_num("marR", 91440);
                cell.marT = tcPr->get_num("marT", 45720);
                cell.marB = tcPr->get_num("marB", 45720);
                cell.mar_set = true;
                cell.fill = parse_fill(tcPr, "");
                // 边框 lnL lnR lnT lnB
                const char* bk[4] = { "a:lnL", "a:lnR", "a:lnT", "a:lnB" };
                for (int bi = 0; bi < 4; ++bi) {
                    XmlPtr ln = tcPr->first(bk[bi]);
                    if (!ln) continue;
                    cell.has_border[bi] = true;
                    Line bl = parse_line_of(ln);
                    cell.border_w[bi] = bl.w;
                    cell.border_color[bi] = bl.color;
                    cell.border_dash[bi] = bl.dash;
                    if (ln->first("a:noFill")) { cell.has_border[bi] = false; cell.border_w[bi] = 0; }
                }
                // 单元格级效果（少用）忽略
            }
            XmlPtr txBody = tcs[ci]->first("a:txBody");
            if (txBody) {
                XmlPtr bodyPr = txBody->first("a:bodyPr");
                if (bodyPr) parse_text_props(bodyPr, cell.text);
                for (const XmlPtr& p : txBody->all("a:p")) parse_paragraph(p, cell.text);
            }
        }
    }
}

// ==================== 主题解析 ====================

static Theme parse_theme_xml(const std::string& xml, const std::string& name) {
    Theme th;
    th.name = name;
    std::string err;
    XmlPtr root = xml_parse(xml, err);
    if (!root) return th;

    // colors
    XmlPtr clrScheme = root->first_deep("a:clrScheme");
    if (clrScheme) {
        auto get = [&](const std::string& name) {
            XmlPtr c = clrScheme->first(name);
            if (!c) return ARGB(255, 0, 0, 0);
            XmlPtr srgb = c->first("a:srgbClr");
            if (srgb) return parse_srgb(srgb->get("val"));
            XmlPtr sys = c->first("a:sysClr");
            if (sys) return parse_srgb(sys->get("lastClr", "000000"));
            return ARGB(255, 0, 0, 0);
        };
        th.colors.dk1 = get("a:dk1"); th.colors.lt1 = get("a:lt1");
        th.colors.dk2 = get("a:dk2"); th.colors.lt2 = get("a:lt2");
        th.colors.accent[0] = get("a:accent1"); th.colors.accent[1] = get("a:accent2");
        th.colors.accent[2] = get("a:accent3"); th.colors.accent[3] = get("a:accent4");
        th.colors.accent[4] = get("a:accent5"); th.colors.accent[5] = get("a:accent6");
        th.colors.hlink = get("a:hlink"); th.colors.folhlink = get("a:folHlink");
        th.colors.scheme_name = clrScheme->get("name", "office");
    }
    // fonts
    XmlPtr fontScheme = root->first_deep("a:fontScheme");
    if (fontScheme) {
        auto mj = fontScheme->first("a:majorFont");
        auto mn = fontScheme->first("a:minorFont");
        if (mj) { th.fonts.major_latin = mj->child_attr("a:latin", "typeface", "Calibri Light");
                  th.fonts.major_ea   = mj->child_attr("a:ea", "typeface", ""); }
        if (mn) { th.fonts.minor_latin = mn->child_attr("a:latin", "typeface", "Calibri");
                  th.fonts.minor_ea   = mn->child_attr("a:ea", "typeface", ""); }
    }
    return th;
}

// ==================== 形状递归解析 ====================

static ShapePtr parse_shape(const XmlPtr& sp, long long& max_id) {
    std::string tag = sp->tag;
    ShapePtr s = std::make_shared<Shape>();
    s->name = "形状";
    // 保存该形状的原始 XML：回写时未被编辑的形状逐字节原样输出
    s->raw_xml = sp->to_xml(false);
    s->raw_preserve = true;

    // 公共属性
    XmlPtr nv = sp->first("p:nvSpPr") ? sp->first("p:nvSpPr") :
                (tag == "p:pic" ? sp->first("p:nvPicPr") :
                 (tag == "p:grpSp" ? sp->first("p:nvGrpSpPr") :
                  (sp->first("p:nvGraphicFramePr"))));

    if (nv) {
        XmlPtr cnvPr = nv->first("p:cNvPr");
        if (cnvPr) {
            s->id = cnvPr->get_num("id", 0);
            s->name = cnvPr->get("name", "形状");
            if (s->id > max_id) max_id = s->id;
        }
        XmlPtr nvPr = nv->first("p:nvPr");
        if (nvPr) {
            XmlPtr ph = nvPr->first("p:ph");
            if (ph) {
                s->is_place_holder = true;
                s->ph_type = parse_ph(ph->get("type", "body"));
                s->ph_idx = ph->get("idx", "");
                s->ph_type_raw = ph->get("type", "");
            }
        }
    }

    // 形状属性
    XmlPtr spPr = sp->first("p:spPr");
    if (!spPr && tag == "p:grpSp") spPr = sp->first("p:grpSpPr");
    if (tag == "p:cxnSp") spPr = sp->first("p:spPr");
    if (spPr) {
        std::string scheme;
        s->fill = parse_fill(spPr, scheme);
        s->line = parse_line(spPr);
        parse_geometry(spPr, *s);
        parse_effects(spPr, *s);
    }
    if (XmlPtr fx2 = sp->first("p:style")) parse_shape_style(sp, *s);

    // 按标签分发
    if (tag == "p:sp") {
        if (s->prst_geom == "ellipse" || s->prst_geom == "oval") s->kind = ShapeKind::Ellipse;
        else if (s->prst_geom == "roundRect") s->kind = ShapeKind::RoundedRect;
        else if (s->prst_geom == "triangle") s->kind = ShapeKind::Triangle;
        else if (s->prst_geom == "diamond") s->kind = ShapeKind::Diamond;
        else s->kind = ShapeKind::Rect;
        XmlPtr txBody = sp->first("p:txBody");
        if (txBody) { parse_text(txBody, *s); }
    } else if (tag == "p:pic") {
        s->kind = ShapeKind::Picture;
        XmlPtr blipFill = sp->first("p:blipFill");
        if (blipFill) {
            XmlPtr blip = blipFill->first("a:blip");
            if (blip) { s->pic_part = blip->get("r:embed"); s->pic_ext = blip->get("ext", "png"); }
            if (blip) { XmlPtr amf = blip->first("a:alphaModFix"); if (amf) s->fill.blip_alpha = amf->get_dbl("amt", 100000) / 1000.0; }
            XmlPtr stretch = blipFill->first("a:stretch");
            if (stretch) s->pic_fill_rect = true;
            // crop：srcRect 的 l/t/r/b 为千分比（正数裁掉），picCropL/R/T/B 为 EMU
            XmlPtr srcRect = blipFill->first("a:srcRect");
            if (srcRect) {
                s->src_rect_l = srcRect->get_dbl("l", 0) / 100000.0;
                s->src_rect_t = srcRect->get_dbl("t", 0) / 100000.0;
                s->src_rect_r = srcRect->get_dbl("r", 0) / 100000.0;
                s->src_rect_b = srcRect->get_dbl("b", 0) / 100000.0;
            }
            XmlPtr pc = blipFill->first("a:picCropL");
            if (pc) {
                s->pic_crop_l = pc->get_num("picCropL", 0);
                XmlPtr pt = blipFill->first("a:picCropT"); if (pt) s->pic_crop_t = pt->get_num("picCropT", 0);
                XmlPtr pr = blipFill->first("a:picCropR"); if (pr) s->pic_crop_r = pr->get_num("picCropR", 0);
                XmlPtr pb = blipFill->first("a:picCropB"); if (pb) s->pic_crop_b = pb->get_num("picCropB", 0);
            }
        }
        // 图片自身的效果（图片工具里的阴影等）在 spPr 中，已解析
    } else if (tag == "p:grpSp") {
        s->kind = ShapeKind::Group;
        XmlPtr grpSpPr = sp->first("p:grpSpPr");
        if (grpSpPr) {
            s->fill = parse_fill(grpSpPr, "");
            parse_group_xfrm(grpSpPr, *s);
        }
        for (const XmlPtr& ch : sp->children) {
            if (ch->tag == "p:sp" || ch->tag == "p:pic" || ch->tag == "p:grpSp" || ch->tag == "p:graphicFrame" || ch->tag == "p:cxnSp")
                s->children.push_back(parse_shape(ch, max_id));
        }
    } else if (tag == "p:cxnSp") {
        s->kind = ShapeKind::Line;
        if (s->prst_geom.empty()) s->prst_geom = "line";
    } else if (tag == "p:graphicFrame") {
        XmlPtr gf = sp->first("a:graphic");
        if (gf) {
            XmlPtr gd = gf->first("a:graphicData");
            if (gd) {
                XmlPtr tbl = gd->first("a:tbl");
                if (tbl) {
                    parse_table(tbl, *s);
                    // graphicFrame 的 xfrm 在 p:xfrm（无 a: 前缀）
                    XmlPtr gfx = sp->first("p:xfrm");
                    if (gfx) {
                        XmlPtr off = gfx->first("a:off"); if (off) { s->x = off->get_num("x", 0); s->y = off->get_num("y", 0); }
                        XmlPtr ext = gfx->first("a:ext"); if (ext) { s->cx = ext->get_num("cx", 0); s->cy = ext->get_num("cy", 0); }
                    }
                }
                else { s->kind = ShapeKind::Chart; s->chart_part = gd->get("r:embed", ""); }
            }
        }
    }
    return s;
}

// 背景：OOXML 为 <p:bg><p:bgPr><a:solidFill/></p:bgPr></p:bg>，兼容直接子节点写法
static Fill parse_bg_fill(const XmlPtr& bg) {
    if (!bg) return Fill();
    XmlPtr pr = bg->first("p:bgPr");
    if (!pr) pr = bg->first("p:bgRef");
    return parse_fill(pr ? pr : bg, "");
}

// ==================== 关系解析 ====================

static void parse_rels(const std::string& part, const std::string& xml, Presentation& pres) {
    std::string err;
    XmlPtr root = xml_parse(xml, err);
    if (!root) return;
    std::vector<Presentation::Rel> rv;
    for (const XmlPtr& r : root->all("Relationship")) {
        Presentation::Rel rel;
        rel.id = r->get("Id", "");
        rel.type = r->get("Type", "");
        rel.target = r->get("Target", "");
        rel.mode = r->get("TargetMode", "");
        rv.push_back(std::move(rel));
    }
    // 关系部件名：例如 "ppt/slides/_rels/slide1.xml.rels"
    pres.rels[part] = std::move(rv);
}

// ==================== 主读取流程 ====================

} // namespace

bool pptx_read(const std::string& path, Presentation& pres, std::string& err) {
    ZipArchive zip;
    if (!zip.open_file(path, err)) return false;
    pres.src_path = path;

    // 0 ----- 全量原文快照（步骤C：未编辑部件写出时逐字节回写）-----
    // 文本类（xml/rels）走 string；媒体与其他二进制走 string 亦可，
    // 因 ZipArchive::get(string) 按原始字节读，不做任何转码。
    for (const std::string& nm : zip.names()) {
        std::string bytes;
        if (zip.get(nm, bytes)) pres.orig_parts[nm] = std::move(bytes);
    }
    // 记录原始 zip 条目顺序，写回时尽量保持
    for (const std::string& nm : zip.names()) pres.entry_order.push_back(nm);

    // 1 ----- 读取所有 XML 关系文件 -----
    for (const std::string& nm : zip.names()) {
        if (ends_with(nm, ".rels")) {
            std::string xml;
            if (zip.get(nm, xml)) parse_rels(nm, xml, pres);
        }
    }

    // 2 ----- [Content_Types].xml -----
    std::string ct_xml;
    if (zip.get("[Content_Types].xml", ct_xml)) {
        std::string ct_err;
        XmlPtr ct_root = xml_parse(ct_xml, ct_err);
        if (ct_root) {
            for (const XmlPtr& ov : ct_root->all("Override")) {
                std::string pn = ov->get("PartName", "");
                if (!pn.empty() && !starts_with(pn, "/")) pn = "/" + pn;
                pres.content_types_overrides[pn]["ContentType"] = ov->get("ContentType", "");
            }
            for (const XmlPtr& d : ct_root->all("Default")) {
                std::string ext = d->get("Extension", "");
                if (!ext.empty()) pres.content_types_defaults[ext]["ContentType"] = d->get("ContentType", "");
            }
        }
    }

    // 3 ----- 主线：包关系 -> presentation.xml -> 幻灯片 -> 版式 -> 母版 -> 主题 -----
    const auto& main_rels = pres.rels["_rels/.rels"];
    std::string pres_part = "ppt/presentation.xml";
    for (const auto& r : main_rels) {
        if (rel::is(r.type, rel::OFFICE_DOCUMENT)) {
            if (!starts_with(r.target, "ppt/")) pres_part = norm_part_path(join_path("ppt", r.target));
            else pres_part = norm_part_path(r.target);
            break;
        }
    }

    std::string pres_xml;
    if (!zip.get(pres_part, pres_xml)) { err = "未找到 presentation.xml"; return false; }
    std::string pres_err;
    XmlPtr pres_root = xml_parse(pres_xml, pres_err);
    if (!pres_root) { err = "presentation.xml 解析失败: " + pres_err; return false; }

    // 幻灯片尺寸
    XmlPtr sldSz = pres_root->first("p:sldSz");
    if (sldSz) {
        pres.slide_w = sldSz->get_num("cx", 12192000);
        pres.slide_h = sldSz->get_num("cy", 6858000);
    }

    // 幻灯片列表
    XmlPtr sldIdLst = pres_root->first("p:sldIdLst");
    if (sldIdLst) {
        std::string pres_rels_xml;
        std::string pres_rels_part = pres.rel_part_for(pres_part);
        // rels 已统一读取，直接从 pres.rels 查
        const auto& prel = pres.rels[pres_rels_part];

        for (const XmlPtr& sldId : sldIdLst->all("p:sldId")) {
            long long sid = sldId->get_num("id", 0);
            std::string rid = sldId->get("r:id", "");

            std::string target;
            for (const auto& r : prel) if (r.id == rid) { target = r.target; break; }
            if (target.empty()) continue;

            // ppt/slides/slide1.xml
            std::string spart = norm_part_path(join_path(dir_name(pres_part), target));

            SlidePtr slide = std::make_shared<Slide>();
            slide->id = sid;
            slide->part = spart;

            std::string sxml;
            if (!zip.get(spart, sxml)) continue;
            XmlPtr s_root = xml_parse(sxml, err = "");
            if (!s_root) continue;   // 焦油页跳过
            slide->raw_xml = sxml;   // 原始页 XML：回写时只做局部手术

            // cSld -> spTree
            XmlPtr cSld = s_root->first("p:cSld");
            if (!cSld) { pres.slides.push_back(slide); continue; }

            // bg
            XmlPtr bg = cSld->first("p:bg"); if (bg) { slide->bg.fill = parse_bg_fill(bg); slide->bg.from_master = false; }

            // 版式关系
            std::string srels_part = pres.rel_part_for(spart);
            const auto& srels = pres.rels[srels_part];
            for (const auto& r : srels)
                if (rel::is(r.type, rel::SLIDE_LAYOUT))
                    slide->layout_part = norm_part_path(join_path(dir_name(spart), r.target));

            XmlPtr spTree = cSld->first("p:spTree");
            if (!spTree) { pres.slides.push_back(slide); continue; }

            long long max_id = 0;
            for (const XmlPtr& sp_node : spTree->children) {
                std::string t = sp_node->tag;
                if (t == "p:sp" || t == "p:pic" || t == "p:grpSp" || t == "p:cxnSp" || t == "p:graphicFrame") {
                    slide->shapes.push_back(parse_shape(sp_node, max_id));
                }
            }

            // 备注
            std::string notes_part;
            for (const auto& r : srels) if (rel::is(r.type, rel::NOTES_SLIDE)) {
                notes_part = norm_part_path(join_path(dir_name(spart), r.target));
                break;
            }
            if (!notes_part.empty() && zip.has(notes_part)) {
                std::string nxml;
                if (zip.get(notes_part, nxml)) {
                    XmlPtr n_root = xml_parse(nxml, err = "");
                    if (n_root) {
                        XmlPtr notes = n_root->first("p:notes");
                        if (notes) {
                            XmlPtr c = notes->first("p:cNvGrpSpPr");
                            // 走 textWalk 收集所有文本
                            std::string notes_text;
                            std::function<void(const XmlPtr&)> walk = [&](const XmlPtr& n) {
                                for (const XmlPtr& ch : n->children) {
                                    if (ch->tag == "a:t") notes_text += ch->text;
                                    walk(ch);
                                }
                            };
                            walk(notes);
                            slide->notes = notes_text;
                        }
                    }
                }
            }

            // 切换动画
            if (XmlPtr tr = s_root->first("p:transition")) {
                slide->trans.adv_click = tr->get("advClick", "1");
                slide->trans.adv_time = tr->get("advTm", "0");
                std::string spd = tr->get("spd", "med");
                slide->trans.dur_ms = spd == "fast" ? 300 : (spd == "slow" ? 1000 : 600);
                for (const XmlPtr& ch : tr->children) {
                    if (ch->tag.rfind("p:", 0) != 0) continue;
                    std::string k = ch->tag.substr(2);
                    if (k == "sndAc") continue;
                    slide->trans.kind = k;
                    slide->trans.dir = tr_dir_val(ch->get("dir", ""));
                    break;
                }
            }

            // 入场动画（p:timing 里的 clickEffect）
            if (XmlPtr tm = s_root->first("p:timing")) {
                std::function<void(const XmlPtr&)> walkCt = [&](const XmlPtr& n) {
                    if (n->tag == "p:cTn" && n->get("presetClass", "") == "entr" &&
                        n->get("nodeType", "") == "clickEffect") {
                        int preset = (int)n->get_num("presetID", 10);
                        AnimNode a;
                        a.effect = AnimEffect::Entrance;
                        a.raw_prst = anim_kind_of_preset(preset);
                        a.kind = AnimKind::Fade;
                        if (a.raw_prst == "appear") a.kind = AnimKind::Appear;
                        a.dur_ms = n->get_num("dur", 500);
                        a.delay_ms = 0;
                        // 找目标形状
                        std::function<long long(const XmlPtr&)> findSpid =
                            [&](const XmlPtr& x) -> long long {
                            if (x->tag == "p:spTgt") return x->get_num("spid", 0);
                            for (const XmlPtr& c : x->children) {
                                long long v = findSpid(c);
                                if (v) return v;
                            }
                            return 0;
                        };
                        a.target_id = findSpid(n);
                        if (a.target_id > 0) slide->trans.seq.push_back(a);
                    }
                    for (const XmlPtr& c : n->children) walkCt(c);
                };
                walkCt(tm);
            }

            pres.slides.push_back(slide);
        }
    }

    // 4 ----- 版式 -----
    std::string pres_dir = dir_name(pres_part);
    // 扫描 zip 里的 slideLayout*.xml
    for (const std::string& nm : zip.names()) {
        if (!ends_with(nm, "slideLayout.xml") && !ends_with(nm, "slideLayout1.xml") && 
            !ends_with(nm, "slideLayout2.xml") && !ends_with(nm, "slideLayout3.xml") &&
            !ends_with(nm, "slideLayout4.xml") && !ends_with(nm, "slideLayout5.xml") &&
            !ends_with(nm, "slideLayout6.xml") && !ends_with(nm, "slideLayout7.xml") &&
            !ends_with(nm, "slideLayout8.xml") && !ends_with(nm, "slideLayout9.xml") &&
            !ends_with(nm, "slideLayout10.xml") && !ends_with(nm, "slideLayout11.xml")) continue;
        if (!starts_with(nm, "ppt/slideLayouts/")) continue;
        if (nm.find("/_rels/") != std::string::npos) continue;

        std::string lxml;
        if (!zip.get(nm, lxml)) continue;
        XmlPtr l_root = xml_parse(lxml, err = "");
        if (!l_root) continue;

        SlideLayoutDef ld;
        ld.part = nm;
        ld.name = l_root->child_text("p:cSld/p:name", "空白");
        ld.type = l_root->get("type", "obj");
        // 关系 -> 母版部件
        {
            std::string lrels = pres.rel_part_for(nm);
            auto it = pres.rels.find(lrels);
            if (it != pres.rels.end()) {
                for (const auto& r : it->second)
                    if (rel::is(r.type, rel::SLIDE_MASTER)) {
                        ld.master_part = norm_part_path(join_path(dir_name(nm), r.target));
                        break;
                    }
            }
        }

        XmlPtr l_cSld = l_root->first("p:cSld");
        if (l_cSld) {
            XmlPtr l_bg = l_cSld->first("p:bg"); if (l_bg) ld.bg_fill = parse_bg_fill(l_bg);
            XmlPtr l_spTree = l_cSld->first("p:spTree");
            if (l_spTree) {
                long long max_id = 0;
                for (const XmlPtr& sp : l_spTree->children) {
                    std::string t = sp->tag;
                    if (t == "p:sp" || t == "p:pic" || t == "p:grpSp" || t == "p:cxnSp" || t == "p:graphicFrame")
                        ld.shapes.push_back(parse_shape(sp, max_id));
                }
            }
        }
        pres.layouts.push_back(std::move(ld));
    }

    // 5 ----- 母版 -----
    for (const std::string& nm : zip.names()) {
        if (!ends_with(nm, "slideMaster.xml") && !ends_with(nm, "slideMaster1.xml")) continue;
        if (!starts_with(nm, "ppt/slideMasters/")) continue;
        if (nm.find("/_rels/") != std::string::npos) continue;

        std::string mxml;
        if (!zip.get(nm, mxml)) continue;
        XmlPtr m_root = xml_parse(mxml, err = "");
        if (!m_root) continue;

        SlideMasterDef md;
        md.part = nm;

        XmlPtr m_cSld = m_root->first("p:cSld");
        if (m_cSld) {
            XmlPtr m_bg = m_cSld->first("p:bg"); if (m_bg) { md.bg_fill = parse_bg_fill(m_bg); md.has_bg = true; }
            XmlPtr m_spTree = m_cSld->first("p:spTree");
            if (m_spTree) {
                long long max_id = 0;
                for (const XmlPtr& sp : m_spTree->children) {
                    std::string t = sp->tag;
                    if (t == "p:sp" || t == "p:pic" || t == "p:grpSp" || t == "p:cxnSp")
                        md.shapes.push_back(parse_shape(sp, max_id));
                }
            }
        }
        pres.masters.push_back(std::move(md));
    }

    // 6 ----- 主题 -----
    for (const std::string& nm : zip.names()) {
        if (ends_with(nm, "theme.xml") || ends_with(nm, "theme1.xml") || ends_with(nm, "theme2.xml")) {
            if (nm.find("/_rels/") == std::string::npos) {
                std::string th_xml;
                if (zip.get(nm, th_xml)) pres.themes.push_back(parse_theme_xml(th_xml, stem_name(dir_name(nm))));
            }
        }
    }

    // 7 ----- 媒体文件 -----
    for (const std::string& nm : zip.names()) {
        if (starts_with(nm, "ppt/media/") || starts_with(nm, "media/")) {
            std::vector<uint8_t> data;
            if (zip.get(nm, data)) pres.media[nm] = std::move(data);
        }
    }

    // 8 ----- 未识别部件原样保留 -----
    std::set<std::string> known;
    for (const SlidePtr& sl : pres.slides) { known.insert(sl->part); std::string rp = pres.rel_part_for(sl->part); known.insert(rp); }
    for (const SlideLayoutDef& ld : pres.layouts) known.insert(ld.part);
    for (const SlideMasterDef& md : pres.masters) known.insert(md.part);
    known.insert(pres_part); known.insert(pres.rel_part_for(pres_part));
    known.insert("_rels/.rels"); known.insert("[Content_Types].xml");
    for (const auto& kv : pres.rels) known.insert(kv.first);

    for (const std::string& nm : zip.names()) {
        if (!known.count(nm) && pres.media.count(nm) == 0) {
            std::string raw;
            if (zip.get(nm, raw)) pres.raw_parts[nm] = raw;
        }
    }

    // 9 ----- 解析图片 rId -> 实际部件名 -----
    for (SlidePtr& sl : pres.slides) {
        std::string srp = pres.rel_part_for(sl->part);
        const auto& rv = pres.rels[srp];
        std::function<void(ShapePtr&)> fix = [&](ShapePtr& s) {
            // 图片 blip 里存的是 rId，替换为真实路径
            if (!s->pic_part.empty() && s->pic_part[0] == 'r') {
                for (const auto& r : rv) if (r.id == s->pic_part) {
                    s->pic_part = norm_part_path(join_path(dir_name(sl->part), r.target));
                    break;
                }
            }
            // fill blip 同理
            if (s->fill.kind == FillKind::Blip && !s->fill.blip_part.empty() && s->fill.blip_part[0] == 'r') {
                for (const auto& r : rv) if (r.id == s->fill.blip_part) {
                    s->fill.blip_part = norm_part_path(join_path(dir_name(sl->part), r.target));
                    break;
                }
            }
            for (ShapePtr& ch : s->children) fix(ch);
        };
        for (ShapePtr& sp : sl->shapes) fix(sp);
    }

    return true;
}

// ==================== 手术式回写（步骤 C） ====================
// 思路：读取时保存了 zip 全部原文（pres.orig_parts）。写回时
//  - 未编辑的部件：逐字节照抄（图片/图表/主题/母版/动画全部保真）
//  - 编辑过的页：只替换 <p:spTree> 里的形状列表，其余（背景/切换/动画）原样
//  - 编辑过的形状：把原始形状 XML 重新解析后只改 xfrm / txBody / 填充
namespace {

static std::string rpr_xml(const RunProps& p) {
    std::string out = "<a:rPr";
    char b[64];
    if (p.sz_set) { snprintf(b, sizeof b, " sz=\"%d\"", (int)(p.sz + 0.5)); out += b; }
    if (p.b_set && p.b) out += " b=\"1\"";
    if (p.i_set && p.i) out += " i=\"1\"";
    if (p.u_set && !p.u.empty() && p.u != "none") out += " u=\"" + xml_escape(p.u) + "\"";
    if (p.strike) out += " strike=\"sngStrike\"";
    if (p.baseline_set && p.baseline) {
        snprintf(b, sizeof b, " baseline=\"%d\"", p.baseline); out += b;
    }
    bool need_children = !p.latin.empty() || !p.ea.empty() || p.color_set || !p.color_scheme.empty();
    if (!need_children) { out += "/>"; return out; }
    out += ">";
    if (!p.latin.empty()) out += "<a:latin typeface=\"" + xml_escape(p.latin) + "\"/>";
    if (!p.ea.empty()) out += "<a:ea typeface=\"" + xml_escape(p.ea) + "\"/>";
    if (!p.color_scheme.empty())
        out += "<a:solidFill><a:schemeClr val=\"" + xml_escape(p.color_scheme) + "\"/></a:solidFill>";
    else if (p.color_set)
        out += "<a:solidFill><a:srgbClr val=\"" + p.color.to_hex() + "\"/></a:solidFill>";
    out += "</a:rPr>";
    return out;
}

static std::string algn_str(AlignL a) {
    switch (a) {
        case AlignL::Center: return "ctr";
        case AlignL::Right: return "r";
        case AlignL::Justify: return "just";
        case AlignL::Distributed: return "dist";
        default: return "l";
    }
}

static std::string ppr_xml(const TextParagraph& p) {
    std::string out = "<a:pPr";
    char b[64];
    if (p.props.algn_set) out += " algn=\"" + algn_str(p.props.algn) + "\"";
    if (p.props.lvl_set && p.props.lvl) { snprintf(b, sizeof b, " lvl=\"%d\"", p.props.lvl); out += b; }
    if (p.props.marL_set) {
        snprintf(b, sizeof b, " marL=\"%lld\" marR=\"%lld\" indent=\"%lld\"",
                 p.props.marL, p.props.marR, p.props.indent);
        out += b;
    }
    if (p.props.lnspc_set) {
        if (p.props.lnspc_pts > 0) { snprintf(b, sizeof b, " lnSpc=\"%lld\"", p.props.lnspc_pts * 100LL); out += b; }
        else { snprintf(b, sizeof b, " lnSpc=\"%d\"", (int)(p.props.lnspc_pct * 1000)); out += b; }
    }
    if (p.props.spcbef_set) { snprintf(b, sizeof b, " spcBef=\"%lld\"", p.props.spcbef); out += b; }
    if (p.props.spcaft_set) { snprintf(b, sizeof b, " spcAft=\"%lld\"", p.props.spcaft); out += b; }
    out += "/>";
    return out;
}

// txBody 的内容部分（不含外层标签，表格单元格要包成 a:txBody）
static std::string txbody_inner_xml(const TextBody& tb) {
    std::string out = "<a:bodyPr";
    out += std::string(" wrap=\"") + (tb.wrap ? "square" : "none") + "\"";
    if (tb.vert) out += " vert=\"" + xml_escape(tb.vert_dir.empty() ? std::string("eaVert") : tb.vert_dir) + "\"";
    out += "/><a:lstStyle/>";
    for (const TextParagraph& p : tb.paras) {
        out += "<a:p>";
        out += ppr_xml(p);
        for (const TextRun& r : p.runs) {
            out += "<a:r>" + rpr_xml(r.props) + "<a:t>" + xml_escape_text(r.text) + "</a:t></a:r>";
        }
        out += "</a:p>";
    }
    return out;
}

// 一个完整的 p:txBody
static std::string txbody_xml(const TextBody& tb) {
    return "<p:txBody>" + txbody_inner_xml(tb) + "</p:txBody>";
}

// 新建形状（没有原始 XML）序列化为 p:sp
static std::string new_shape_xml(const ShapePtr& s) {
    std::string out = "<p:sp><p:nvSpPr><p:cNvPr id=\"" + std::to_string(s->id) +
                      "\" name=\"" + xml_escape(s->name) + "\"/><p:cNvSpPr txBox=\"1\"/><p:nvPr/>"
                      "</p:nvSpPr><p:spPr><a:xfrm";
    if (s->rot != 0) {
        char b[48]; snprintf(b, sizeof b, " rot=\"%lld\"", (long long)(s->rot * 60000 + 0.5));
        out += b;
    }
    out += "><a:off x=\"" + std::to_string(s->x) + "\" y=\"" + std::to_string(s->y) +
           "\"/><a:ext cx=\"" + std::to_string(s->cx) + "\" cy=\"" + std::to_string(s->cy) +
           "\"/></a:xfrm><a:prstGeom prst=\"" + xml_escape(s->prst_geom.empty() ? std::string("rect") : s->prst_geom) +
           "\"><a:avLst/></a:prstGeom>";
    if (s->fill.kind == FillKind::Solid)
        out += "<a:solidFill><a:srgbClr val=\"" + s->fill.solid.to_hex() + "\"/></a:solidFill>";
    else if (s->fill.kind == FillKind::None || s->fill.kind == FillKind::NoFill)
        out += "<a:noFill/>";
    if (s->line.explicit_none) out += "<a:ln><a:noFill/></a:ln>";
    else if (!s->line.inherit)
        out += "<a:ln w=\"" + std::to_string((long long)s->line.w) + "\"><a:solidFill><a:srgbClr val=\"" +
               s->line.color.to_hex() + "\"/></a:solidFill></a:ln>";
    out += "</p:spPr>";
    if (!s->text.paras.empty()) out += txbody_xml(s->text);
    out += "</p:sp>\n";
    return out;
}

// 表格（graphicFrame）序列化：从模型重建 a:tbl（单元格文字编辑后用）
static std::string new_table_xml(const ShapePtr& s) {
    const Table& t = *s->table;
    std::string out = "<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id=\"" +
                      std::to_string(s->id) + "\" name=\"" + xml_escape(s->name.empty() ? std::string("表格") : s->name) +
                      "\"/><p:cNvGraphicFramePr><a:graphicFrameLocks noGrp=\"1\"/></p:cNvGraphicFramePr><p:nvPr/>"
                      "</p:nvGraphicFramePr><p:xfrm><a:off x=\"" + std::to_string(s->x) +
                      "\" y=\"" + std::to_string(s->y) + "\"/><a:ext cx=\"" + std::to_string(s->cx) +
                      "\" cy=\"" + std::to_string(s->cy) + "\"/></p:xfrm><a:graphic>"
                      "<a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/table\">"
                      "<a:tbl><a:tblPr firstRow=\"1\"/><a:tblGrid>";
    for (long long w : t.cols) out += "<a:gridCol w=\"" + std::to_string(w) + "\"/>";
    out += "</a:tblGrid>";
    const std::string ln_names[4] = {"a:lnL", "a:lnR", "a:lnT", "a:lnB"};
    for (size_t r = 0; r < t.cells.size(); ++r) {
        long long rh = (r < t.rows.size() ? t.rows[r] : 300000);
        out += "<a:tr h=\"" + std::to_string(rh) + "\">";
        for (size_t c = 0; c < t.cells[r].size(); ++c) {
            const TableCell& cell = t.cells[r][c];
            out += "<a:tc><a:txBody>" + txbody_inner_xml(cell.text) + "</a:txBody><a:tcPr"
                   " marL=\"91440\" marR=\"91440\" marT=\"45720\" marB=\"45720\">";
            for (int b = 0; b < 4; ++b) {
                double w = cell.has_border[b] ? cell.border_w[b] : 6350.0;
                std::string col = cell.has_border[b] ? cell.border_color[b].to_hex() : std::string("BFBFBF");
                out += "<" + ln_names[b] + " w=\"" + std::to_string((long long)w) + "\">"
                       "<a:solidFill><a:srgbClr val=\"" + col + "\"/></a:solidFill></" + ln_names[b] + ">";
            }
            out += "</a:tcPr></a:tc>";
        }
        out += "</a:tr>";
    }
    out += "</a:tbl></a:graphicData></a:graphic></p:graphicFrame>\n";
    return out;
}

// 原始形状 XML + 编辑标记 -> 新 XML（只动改过的部分）
static std::string patch_shape_xml(const Shape& s) {
    if (s.raw_xml.empty()) return std::string();
    if (!s.ed_geom && !s.ed_text && !s.ed_fill && !s.ed_line) return s.raw_xml;
    std::string err;
    XmlPtr root = xml_parse(s.raw_xml, err);
    if (!root) return s.raw_xml;

    if (s.ed_geom) {
        XmlPtr spPr = root->first("p:spPr");
        if (!spPr) spPr = root->first("p:grpSpPr");
        XmlPtr xf = spPr ? spPr->first("a:xfrm") : nullptr;
        if (!xf) xf = root->first("p:xfrm");                    // graphicFrame（表格/图表）
        if (!xf && spPr) xf = spPr->add_child("a:xfrm");
        if (xf) {
            XmlPtr off = xf->first("a:off"); if (!off) off = xf->add_child("a:off");
            off->set_num("x", s.x); off->set_num("y", s.y);
            XmlPtr ext = xf->first("a:ext"); if (!ext) ext = xf->add_child("a:ext");
            ext->set_num("cx", s.cx); ext->set_num("cy", s.cy);
            if (s.rot != 0) xf->set_num("rot", (long long)(s.rot * 60000 + 0.5));
            else xf->remove_attr("rot");
        }
    }

    if (s.ed_text) {
        XmlPtr old = root->first("p:txBody");
        XmlPtr bodyPr;
        if (old) bodyPr = old->first("a:bodyPr");
        root->remove_child("p:txBody");
        std::string tb = txbody_xml(s.text);
        XmlPtr n = xml_parse(tb, err);
        if (n) {
            if (bodyPr) {
                for (size_t i = 0; i < n->children.size(); ++i)
                    if (n->children[i]->tag == "a:bodyPr") { n->children[i] = bodyPr; break; }
            }
            root->add_child(n);
        }
    }

    if (s.ed_fill || s.ed_line) {
        XmlPtr spPr = root->first("p:spPr");
        if (spPr) {
            if (s.ed_fill) {
                for (const char* t : {"a:noFill", "a:solidFill", "a:gradFill", "a:blipFill", "a:pattFill", "a:grpFill"})
                    spPr->remove_child(t);
                std::string fill;
                if (s.fill.kind == FillKind::Solid)
                    fill = "<a:solidFill><a:srgbClr val=\"" + s.fill.solid.to_hex() + "\"/></a:solidFill>";
                else
                    fill = "<a:noFill/>";
                XmlPtr f = xml_parse(fill, err);
                if (f) {
                    size_t pos = 0;
                    for (size_t i = 0; i < spPr->children.size(); ++i) {
                        const std::string& t = spPr->children[i]->tag;
                        if (t == "a:xfrm" || t == "a:prstGeom" || t == "a:custGeom" || t == "a:prstTxWarp")
                            pos = i + 1;
                    }
                    spPr->children.insert(spPr->children.begin() + pos, f);
                }
            }
            if (s.ed_line) {
                spPr->remove_child("a:ln");
                std::string ln;
                if (s.line.explicit_none || s.line.color.a == 0) {
                    ln = "<a:ln><a:noFill/></a:ln>";
                } else {
                    char wb[64];
                    snprintf(wb, sizeof wb, "<a:ln w=\"%lld\">", (long long)(s.line.w > 0 ? s.line.w : 12700));
                    ln = std::string(wb) + "<a:solidFill><a:srgbClr val=\"" + s.line.color.to_hex() +
                         "\"/></a:solidFill></a:ln>";
                }
                XmlPtr n = xml_parse(ln, err);
                if (n) {
                    size_t pos = spPr->children.size();
                    for (size_t i = 0; i < spPr->children.size(); ++i)
                        if (spPr->children[i]->tag == "a:effectLst" || spPr->children[i]->tag == "a:effectDag") pos = i;
                    spPr->children.insert(spPr->children.begin() + pos, n);
                }
            }
        }
    }
    return root->to_xml(false);
}

// ======================= 切换 / 动画 =======================
// 切换动画名称 <-> OOXML 元素/方向
static const char* TR_KINDS[] = {"fade", "push", "wipe", "split", "cover", "uncover",
                                 "cut", "dissolve", "blinds", "checker", "circle",
                                 "diamond", "wheel", "random", "zoom", "strips", "wedge"};

static std::string tr_dir_str(int dir) {
    switch (dir) { case 1: return "u"; case 2: return "d"; case 3: return "l"; case 4: return "r"; }
    return "";
}
static int tr_dir_val(const std::string& s) {
    if (s == "u") return 1; if (s == "d") return 2; if (s == "l") return 3; if (s == "r") return 4;
    return 0;
}

static std::string transition_xml(const Transition& t) {
    if (t.kind.empty() || t.kind == "none") return std::string();
    std::string spd = "med";
    if (t.dur_ms > 0 && t.dur_ms <= 350) spd = "fast";
    else if (t.dur_ms > 800) spd = "slow";
    std::string out = "<p:transition";
    if (!t.adv_click.empty()) out += " advClick=\"" + t.adv_click + "\"";
    if (!t.adv_time.empty() && t.adv_time != "0") out += " advTm=\"" + t.adv_time + "\"";
    out += " spd=\"" + spd + "\">";
    std::string dir = tr_dir_str(t.dir);
    if (t.kind == "split") {
        out += "<p:split orient=\"horz\"";
        if (!dir.empty()) out += " dir=\"" + dir + "\"";
        out += "/>";
    } else if (t.kind == "blinds" || t.kind == "checker" || t.kind == "random" ||
               t.kind == "strips" || t.kind == "wedge") {
        out += "<p:" + t.kind + "/>";
    } else {
        out += "<p:" + t.kind;
        if (!dir.empty() && t.kind != "fade" && t.kind != "cut" && t.kind != "dissolve")
            out += " dir=\"" + dir + "\"";
        out += "/>";
    }
    out += "</p:transition>";
    return out;
}

// 形状入场动画
static int anim_preset_of(const std::string& k) {
    if (k == "appear") return 1; if (k == "blinds") return 3; if (k == "checker") return 5;
    if (k == "circle") return 6; if (k == "diamond") return 8; if (k == "dissolve") return 9;
    if (k == "fade") return 10; if (k == "wheel") return 21; if (k == "wipe") return 22;
    if (k == "zoom") return 23;
    return 0;
}
static std::string anim_kind_of_preset(int p) {
    switch (p) {
        case 1: return "appear"; case 3: return "blinds"; case 5: return "checker";
        case 6: return "circle"; case 8: return "diamond"; case 9: return "dissolve";
        case 10: return "fade"; case 21: return "wheel"; case 22: return "wipe";
        case 23: return "zoom";
    }
    return "fade";
}
static std::string anim_filter_of(const std::string& k) {
    if (k == "wipe") return "wipe(up)";
    if (k == "checker") return "checkerboard(across)";
    if (k == "circle") return "circle";
    if (k == "diamond") return "diamond";
    if (k == "dissolve") return "dissolve";
    if (k == "blinds") return "blinds(horizontal)";
    if (k == "wheel") return "wheel(1)";
    return "fade";
}
static long long anim_dur_of(const AnimNode& a) { return a.dur_ms > 0 ? a.dur_ms : 500; }

static std::string timing_xml(const Slide& sl) {
    if (sl.trans.seq.empty()) return std::string();
    std::string ch;
    int base = 3;
    for (const AnimNode& a : sl.trans.seq) {
        if (a.effect != AnimEffect::Entrance || a.target_id <= 0) continue;
        std::string kind;
        // 用 raw_prst 记录实际效果名；没有则按枚举回退
        kind = a.raw_prst.empty() ? "fade" : a.raw_prst;
        int preset = anim_preset_of(kind);
        if (preset == 0) { kind = "fade"; preset = 10; }
        std::string d = std::to_string((long long)(anim_dur_of(a)));
        std::string sid = std::to_string(a.target_id);
        std::string ids[4] = {std::to_string(base), std::to_string(base + 1),
                              std::to_string(base + 2), std::to_string(base + 3)};
        ch += "<p:par><p:cTn id=\"" + ids[0] + "\" fill=\"hold\"><p:stCondLst><p:cond delay=\"indefinite\"/>"
              "</p:stCondLst><p:childTnLst><p:par><p:cTn id=\"" + ids[1] + "\" fill=\"hold\">"
              "<p:stCondLst><p:cond delay=\"0\"/></p:stCondLst><p:childTnLst>"
              "<p:par><p:cTn id=\"" + ids[2] + "\" presetID=\"" + std::to_string(preset) +
              "\" presetClass=\"entr\" presetSubtype=\"0\" fill=\"hold\" nodeType=\"clickEffect\">"
              "<p:stCondLst><p:cond delay=\"" + std::to_string(a.delay_ms > 0 ? a.delay_ms : 0) +
              "\"/></p:stCondLst><p:childTnLst>"
              "<p:set><p:cBhvr><p:cTn id=\"" + ids[3] + "\" dur=\"1\" fill=\"hold\">"
              "<p:stCondLst><p:cond delay=\"0\"/></p:stCondLst></p:cTn><p:tgtEl><p:spTgt spid=\"" +
              sid + "\"/></p:tgtEl><p:attrNameLst><p:attrName>style.visibility</p:attrName>"
              "</p:attrNameLst></p:cBhvr><p:to><p:strVal val=\"visible\"/></p:to></p:set>";
        if (kind != "appear")
            ch += "<p:animEffect transition=\"in\" filter=\"" + anim_filter_of(kind) +
                  "\"><p:cBhvr><p:cTn id=\"" + std::to_string(base + 4) + "\" dur=\"" + d + "\"/>"
                  "<p:tgtEl><p:spTgt spid=\"" + sid + "\"/></p:tgtEl></p:cBhvr></p:animEffect>";
        ch += "</p:childTnLst></p:cTn></p:par></p:childTnLst></p:cTn></p:par></p:childTnLst></p:cTn></p:par>";
        base += 5;
    }
    if (ch.empty()) return std::string();
    std::string out = "<p:timing><p:tnLst><p:par><p:cTn id=\"1\" dur=\"indefinite\" restart=\"never\" "
                      "nodeType=\"tmRoot\"><p:childTnLst><p:seq concurrent=\"1\" nextAc=\"seek\">"
                      "<p:cTn id=\"2\" dur=\"indefinite\" nodeType=\"mainSeq\"><p:childTnLst>" + ch +
                      "</p:childTnLst></p:cTn><p:prevCondLst><p:cond evt=\"onPrev\" delay=\"0\">"
                      "<p:tgtEl><p:sldTgt/></p:tgtEl></p:cond></p:prevCondLst><p:nextCondLst>"
                      "<p:cond evt=\"onNext\" delay=\"0\"><p:tgtEl><p:sldTgt/></p:tgtEl></p:cond>"
                      "</p:nextCondLst></p:seq></p:childTnLst></p:cTn></p:par></p:tnLst></p:timing>";
    return out;
}

// 从页 XML 中删掉某个顶层分段（自闭合或成对）
static void strip_xml_block(std::string& raw, const std::string& name) {
    size_t p = 0;
    while ((p = raw.find("<" + name, p)) != std::string::npos) {
        size_t gt = raw.find('>', p);
        if (gt == std::string::npos) return;
        size_t end = std::string::npos;
        if (gt > 0 && raw[gt - 1] == '/') end = gt + 1;
        else {
            size_t close = raw.find("</" + name + ">", gt);
            if (close == std::string::npos) return;
            end = close + name.size() + 3;
        }
        raw.erase(p, end - p);
    }
}

static std::string apply_playback_xml(std::string raw, const Slide& sl) {
    if (raw.empty()) return raw;
    strip_xml_block(raw, "p:transition");
    strip_xml_block(raw, "p:timing");
    std::string add = transition_xml(sl.trans) + timing_xml(sl);
    if (add.empty()) return raw;
    size_t close = raw.rfind("</p:sld>");
    if (close == std::string::npos) return raw;
    size_t ins = close;
    while (ins > 0 && (raw[ins - 1] == ' ' || raw[ins - 1] == '\n' || raw[ins - 1] == '\r' ||
                       raw[ins - 1] == '\t')) --ins;
    raw.insert(ins, add);
    return raw;
}

static std::string shape_xml_of(const ShapePtr& s) {
    if (s->table && s->ed_table) {
        std::string t = new_table_xml(s);
        if (!t.empty()) return t;
    }
    if (!s->raw_xml.empty()) {
        std::string p = patch_shape_xml(*s);
        if (!p.empty()) return p;
    }
    return new_shape_xml(s);
}

// 用模型里的形状列表替换原始页 XML 中的 spTree 内容
static std::string splice_slide_xml(const Slide& sl) {
    const std::string& raw = sl.raw_xml;
    if (raw.empty()) return std::string();
    size_t pos = raw.find("<p:spTree");
    if (pos == std::string::npos) return raw;
    size_t open_end = raw.find('>', pos);
    if (open_end == std::string::npos) return raw;
    size_t self_close = raw.find("/>", pos);
    if (self_close != std::string::npos && self_close < open_end) return raw;  // <p:spTree/> 不会出现
    open_end += 1;
    size_t close = raw.rfind("</p:spTree>");
    if (close == std::string::npos || close < open_end) return raw;

    std::string inner = raw.substr(open_end, close - open_end);
    // 头部 = nvGrpSpPr + grpSpPr（第一个形状标签之前的内容）
    size_t hdr_len = inner.size();
    for (const char* tag : {"<p:sp>", "<p:pic>", "<p:grpSp>", "<p:cxnSp>", "<p:graphicFrame>", "<p:sp ", "<p:pic "}) {
        size_t p = inner.find(tag);
        if (p != std::string::npos) hdr_len = std::min(hdr_len, p);
    }
    std::string header = inner.substr(0, hdr_len);
    std::string shapes;
    for (const ShapePtr& s : sl.shapes) {
        if (s->hidden) continue;
        shapes += shape_xml_of(s);
    }
    std::string out;
    out.reserve(raw.size() + shapes.size());
    out = raw.substr(0, open_end);
    out += header;
    out += shapes;
    out += raw.substr(close);
    return apply_playback_xml(out, sl);
}

static std::string presentation_part_of(const Presentation& pres) {
    return "ppt/presentation.xml";
}

static std::string patch_presentation_xml(const Presentation& pres,
                                         const std::vector<std::string>& parts,
                                         std::vector<std::pair<std::string, std::string>>& rid_targets) {
    const std::string pn = presentation_part_of(pres);
    auto it = pres.orig_parts.find(pn);
    if (it == pres.orig_parts.end()) return std::string();
    std::string err;
    XmlPtr root = xml_parse(it->second, err);
    if (!root) return it->second;

    std::set<int> used;
    for (const auto& kv : pres.rels)
        for (const auto& r : kv.second)
            if (starts_with(r.id, "rId")) used.insert(atoi(r.id.c_str() + 3));

    XmlPtr lst = root->first("p:sldIdLst");
    if (!lst) lst = root->add_child("p:sldIdLst");
    lst->children.clear();
    int rid = 1;
    for (size_t i = 0; i < parts.size(); ++i) {
        while (used.count(rid)) ++rid;
        used.insert(rid);
        XmlPtr n = lst->add_child("p:sldId");
        long long sid = pres.slides[i]->id;
        if (sid <= 0) sid = 256 + (long long)i;
        n->set_num("id", sid);
        std::string r = "rId" + std::to_string(rid);
        n->set("r:id", r);
        std::string target = norm_part_path(parts[i]);
        if (starts_with(target, "ppt/")) target = target.substr(4);
        rid_targets.emplace_back(r, target);
    }
    return root->to_xml(false);
}

static std::string patch_pres_rels(const Presentation& pres,
                                   const std::vector<std::pair<std::string, std::string>>& rid_targets) {
    std::string rp = pres.rel_part_for(presentation_part_of(pres));
    auto it = pres.orig_parts.find(rp);
    std::string err;
    XmlPtr root;
    if (it != pres.orig_parts.end()) root = xml_parse(it->second, err);
    if (!root) {
        root = XmlPtr();   // 没有就新建
        std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"/>";
        root = xml_parse(s, err);
        if (!root) return std::string();
    }
    // 删掉所有幻灯片关系
    std::vector<XmlPtr> keep;
    for (const XmlPtr& c : root->children) {
        if (c->tag == "Relationship" && rel::is(c->get("Type", ""), rel::SLIDE)) continue;
        keep.push_back(c);
    }
    root->children = keep;
    for (const auto& kv : rid_targets) {
        XmlPtr r = root->add_child("Relationship");
        r->set("Id", kv.first);
        r->set("Type", rel::t(rel::SLIDE));
        r->set("Target", kv.second);
    }
    return root->to_xml(true);
}

static std::string patch_content_types(const Presentation& pres,
                                       const std::vector<std::string>& parts,
                                       const std::set<std::string>& removed_parts) {
    auto it = pres.orig_parts.find("[Content_Types].xml");
    if (it == pres.orig_parts.end()) return std::string();
    std::string err;
    XmlPtr root = xml_parse(it->second, err);
    if (!root) return it->second;

    std::set<std::string> have;
    std::vector<XmlPtr> keep;
    for (const XmlPtr& c : root->children) {
        if (c->tag == "Override") {
            std::string pn = c->get("PartName", "");
            if (starts_with(pn, "/ppt/slides/")) {
                std::string bare = pn.substr(1);
                bool alive = std::find(parts.begin(), parts.end(), bare) != parts.end();
                if (!alive) continue;
                have.insert(bare);
            }
        }
        keep.push_back(c);
    }
    root->children = keep;
    for (const std::string& p : parts) {
        if (have.count(p)) continue;
        XmlPtr o = root->add_child("Override");
        o->set("PartName", "/" + p);
        o->set("ContentType", ctype::SLIDE);
    }
    // 媒体扩展名的 Default 项
    std::set<std::string> have_ext;
    for (const XmlPtr& c : root->children)
        if (c->tag == "Default") have_ext.insert(c->get("Extension", ""));
    for (const auto& kv : pres.media) {
        std::string e = kv.first;
        size_t dot = e.rfind('.');
        if (dot == std::string::npos) continue;
        e = e.substr(dot + 1);
        for (char& ch : e) ch = (char)tolower((unsigned char)ch);
        if (have_ext.count(e)) continue;
        std::string mime = "image/" + e;
        if (e == "jpg" || e == "jpeg") mime = "image/jpeg";
        else if (e == "mp4") mime = "video/mp4";
        else if (e == "mp3") mime = "audio/mpeg";
        XmlPtr d = root->add_child("Default");
        d->set("Extension", e);
        d->set("ContentType", mime);
        have_ext.insert(e);
    }
    (void)removed_parts;
    return root->to_xml(true);
}

static std::string gen_slide_rels(const Slide& sl) {
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n";
    if (!sl.layout_part.empty()) {
        std::string t = norm_part_path(sl.layout_part);
        if (starts_with(t, "ppt/")) t = t.substr(4);
        out += "<Relationship Id=\"rId1\" Type=\"" + std::string(rel::t(rel::SLIDE_LAYOUT)) +
               "\" Target=\"../" + t + "\"/>\n";
    }
    out += "</Relationships>";
    return out;
}

// 从模型里的 rels 表生成 .rels 文件（新增图片关系需要写进去）
static std::string gen_rels_from_model(const Presentation& pres, const std::string& part) {
    std::string rp = pres.rel_part_for(part);
    auto it = pres.rels.find(rp);
    if (it == pres.rels.end()) return std::string();
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n";
    for (const Presentation::Rel& r : it->second) {
        out += "<Relationship Id=\"" + xml_escape(r.id) + "\" Type=\"" + xml_escape(r.type) +
               "\" Target=\"" + xml_escape(r.target) + "\"";
        if (!r.mode.empty()) out += " TargetMode=\"" + xml_escape(r.mode) + "\"";
        out += "/>\n";
    }
    out += "</Relationships>";
    return out;
}

static std::string gen_simple_slide_xml(const Slide& sl) {
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<p:sld xmlns:p=\"" + std::string(xmlns::P) + "\" xmlns:a=\"" + std::string(xmlns::A) +
        "\" xmlns:r=\"" + std::string(xmlns::R) + "\">\n<p:cSld>\n<p:bg><p:bgPr><a:solidFill><a:srgbClr val=\"FFFFFF\"/></a:solidFill>"
        "<a:effectLst/></p:bgPr></p:bg>\n<p:spTree>\n"
        "<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
        "<p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/>"
        "<a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>\n";
    for (const ShapePtr& s : sl.shapes) out += new_shape_xml(s);
    out += "</p:spTree>\n</p:cSld>\n" + transition_xml(sl.trans) + timing_xml(sl) + "\n</p:sld>";
    return out;
}

static bool write_surgical(const std::string& path, const Presentation& pres, std::string& err) {
    ZipArchive zip;
    if (pres.orig_parts.empty()) { err = "没有原始包快照"; return false; }

    // 原始幻灯片部件集合
    std::set<std::string> orig_slide_parts;
    int max_no = 0;
    for (const auto& kv : pres.orig_parts) {
        const std::string& nm = kv.first;
        if (starts_with(nm, "ppt/slides/slide") && ends_with(nm, ".xml") &&
            nm.find("/_rels/") == std::string::npos) {
            orig_slide_parts.insert(nm);
            std::string num = nm.substr(std::string("ppt/slides/slide").size());
            num = num.substr(0, num.find('.'));
            max_no = std::max(max_no, atoi(num.c_str()));
        }
    }

    // 给新增页分配部件名
    std::vector<std::string> parts;
    std::set<std::string> used = orig_slide_parts;
    for (const SlidePtr& sl : pres.slides) {
        std::string p = sl->part;
        if (p.empty() || !pres.orig_parts.count(p)) {
            do { ++max_no; p = "ppt/slides/slide" + std::to_string(max_no) + ".xml"; } while (used.count(p));
            used.insert(p);
            sl->part = p;
        }
        parts.push_back(p);
    }

    // 结构是否变化（顺序/增删）
    std::vector<std::string> orig_order;
    for (const std::string& nm : pres.entry_order)
        if (orig_slide_parts.count(nm)) orig_order.push_back(nm);
    bool struct_changed = (orig_order != parts);

    std::set<std::string> removed;
    for (const std::string& p : orig_slide_parts)
        if (std::find(parts.begin(), parts.end(), p) == parts.end()) {
            removed.insert(p);
            removed.insert(pres.rel_part_for(p));
        }

    std::vector<std::pair<std::string, std::string>> rid_targets;
    std::string pres_xml_new, pres_rels_new, ct_new;
    if (struct_changed) {
        pres_xml_new = patch_presentation_xml(pres, parts, rid_targets);
        pres_rels_new = patch_pres_rels(pres, rid_targets);
    }
    ct_new = patch_content_types(pres, parts, removed);

    std::string pres_part = presentation_part_of(pres);
    std::string pres_rels_part = pres.rel_part_for(pres_part);

    for (const std::string& nm : pres.entry_order) {
        auto it = pres.orig_parts.find(nm);
        if (it == pres.orig_parts.end() || removed.count(nm)) continue;
        std::string data = it->second;

        if (struct_changed) {
            if (nm == pres_part && !pres_xml_new.empty()) data = pres_xml_new;
            else if (nm == pres_rels_part && !pres_rels_new.empty()) data = pres_rels_new;
        }
        if (nm == "[Content_Types].xml" && !ct_new.empty()) data = ct_new;

        auto pit = std::find(parts.begin(), parts.end(), nm);
        if (pit != parts.end()) {
            const Slide& sl = *pres.slides[pit - parts.begin()];
            if (sl.touched && !sl.raw_xml.empty()) data = splice_slide_xml(sl);
        }
        // 幻灯片关系文件：新增的图片等关系必须写回
        for (size_t k = 0; k < parts.size(); ++k) {
            if (nm == pres.rel_part_for(parts[k])) {
                std::string r = gen_rels_from_model(pres, parts[k]);
                if (!r.empty()) data = r;
                break;
            }
        }
        zip.add(nm, data);
    }

    // 补上 entry_order 里没记录的条目
    for (const auto& kv : pres.orig_parts) {
        if (removed.count(kv.first)) continue;
        if (std::find(pres.entry_order.begin(), pres.entry_order.end(), kv.first) != pres.entry_order.end()) continue;
        zip.add(kv.first, kv.second);
    }

    // 新页部件
    for (size_t i = 0; i < parts.size(); ++i) {
        if (pres.orig_parts.count(parts[i])) continue;
        const Slide& sl = *pres.slides[i];
        zip.add(parts[i], sl.raw_xml.empty() ? gen_simple_slide_xml(sl) : splice_slide_xml(sl));
        std::string rp = pres.rel_part_for(parts[i]);
        if (!zip.has(rp)) zip.add(rp, gen_slide_rels(sl));
    }

    // 新增媒体部件（插入图片）
    for (const auto& kv : pres.media) {
        if (pres.orig_parts.count(kv.first)) continue;
        zip.add(kv.first, kv.second);
    }

    // 结构变化时确保两张索引部件存在
    if (struct_changed) {
        if (!ct_new.empty() && !zip.has("[Content_Types].xml")) zip.add("[Content_Types].xml", ct_new);
        if (!pres_xml_new.empty() && !zip.has(pres_part)) zip.add(pres_part, pres_xml_new);
        if (!pres_rels_new.empty() && !zip.has(pres_rels_part)) zip.add(pres_rels_part, pres_rels_new);
    }
    return zip.save(path, err);
}

} // namespace

// ==================== PPTX 写出 ====================

bool pptx_write(const std::string& path, const Presentation& pres, std::string& err) {
    // 有原始包快照时走"手术式回写"，最大限度保住原文件内容
    if (!pres.orig_parts.empty()) return write_surgical(path, pres, err);

    ZipArchive zip;

    // ---- [Content_Types].xml ----
    std::string ct = "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\n";
    // Defaults
    ct += "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>\n";
    ct += "<Default Extension=\"xml\" ContentType=\"application/xml\"/>\n";
    // 先写入传统默认值
    for (const auto& kv : pres.content_types_defaults)
        ct += "<Default Extension=\"" + xml_escape(kv.first) + "\" ContentType=\"" + xml_escape(kv.second.begin()->second) + "\"/>\n";
    // 覆写
    for (const auto& kv : pres.content_types_overrides)
        ct += "<Override PartName=\"" + xml_escape(kv.first) + "\" ContentType=\"" + xml_escape(kv.second.begin()->second) + "\"/>\n";

    // 为缺失的部件补类型
    // 幻灯片
    ct += "<Override PartName=\"/ppt/presentation.xml\" ContentType=\"" + std::string(ctype::PRESENTATION) + "\"/>\n";
    for (size_t i = 0; i < pres.slides.size(); ++i) {
        std::string pn = pres.slides[i]->part.empty() ? ("/ppt/slides/slide" + std::to_string(i + 1) + ".xml") : ("/" + pres.slides[i]->part);
        ct += "<Override PartName=\"" + xml_escape(pn) + "\" ContentType=\"" + std::string(ctype::SLIDE) + "\"/>\n";
    }
    for (const SlideLayoutDef& ld : pres.layouts) {
        std::string pn = "/" + ld.part;
        ct += "<Override PartName=\"" + xml_escape(pn) + "\" ContentType=\"" + std::string(ctype::SLIDE_LAYOUT) + "\"/>\n";
    }
    for (const SlideMasterDef& md : pres.masters) {
        std::string pn = "/" + md.part;
        ct += "<Override PartName=\"" + xml_escape(pn) + "\" ContentType=\"" + std::string(ctype::SLIDE_MASTER) + "\"/>\n";
    }
    for (size_t i = 0; i < pres.themes.size(); ++i) {
        std::string pn = "/ppt/theme/theme" + std::to_string(i + 1) + ".xml";
        ct += "<Override PartName=\"" + xml_escape(pn) + "\" ContentType=\"" + std::string(ctype::THEME) + "\"/>\n";
    }
    for (const auto& kv : pres.media) {
        std::string ext = extension_of(kv.first);
        if (ext == ".png") ct += "<Default Extension=\"png\" ContentType=\"image/png\"/>\n";
        else if (ext == ".jpg" || ext == ".jpeg") ct += "<Default Extension=\"jpg\" ContentType=\"image/jpeg\"/>\n";
        else if (ext == ".gif") ct += "<Default Extension=\"gif\" ContentType=\"image/gif\"/>\n";
        else if (ext == ".svg") ct += "<Default Extension=\"svg\" ContentType=\"image/svg+xml\"/>\n";
        else if (ext == ".mp4") ct += "<Default Extension=\"mp4\" ContentType=\"video/mp4\"/>\n";
        else if (ext == ".wmv") ct += "<Default Extension=\"wmv\" ContentType=\"video/x-ms-wmv\"/>\n";
        else if (ext == ".mp3") ct += "<Default Extension=\"mp3\" ContentType=\"audio/mpeg\"/>\n";
    }
    ct += "</Types>";
    zip.add("[Content_Types].xml", ct);

    // ---- _rels/.rels ----
    std::string rootRels = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
        "<Relationship Id=\"rId1\" Type=\"" + rel::t(rel::OFFICE_DOCUMENT) + "\" Target=\"ppt/presentation.xml\"/>\n";
    rootRels += "<Relationship Id=\"rId2\" Type=\"" + rel::t(rel::PRST_COLORS) + "\" Target=\"docProps/app.xml\"/>\n";
    rootRels += "</Relationships>";
    zip.add("_rels/.rels", rootRels);

    // ====== 重建部件名（如果原版式引用存在就用原版式名，否则造） ======
    // 这里做简化：输出时按 Presentation::slides 顺序重新分配 slide 和 layout part
    // 同时更新 rels

    // 先确保 layout part 非空
    for (size_t i = 0; i < pres.layouts.size(); ++i) {
        if (pres.layouts[i].part.empty()) {
            const_cast<SlideLayoutDef&>(pres.layouts[i]).part = "ppt/slideLayouts/slideLayout" + std::to_string(i + 1) + ".xml";
        }
    }
    for (size_t i = 0; i < pres.masters.size(); ++i) {
        if (pres.masters[i].part.empty()) {
            const_cast<SlideMasterDef&>(pres.masters[i]).part = "ppt/slideMasters/slideMaster" + std::to_string(i + 1) + ".xml";
        }
    }

    // ---- ppt/presentation.xml ----
    std::string pres_xml = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<p:presentation xmlns:p=\"" + std::string(xmlns::P) + "\" xmlns:a=\"" + std::string(xmlns::A) +
        "\" xmlns:r=\"" + std::string(xmlns::R) + "\">\n";
    char buf[128];
    snprintf(buf, sizeof(buf), "<p:sldSz cx=\"%lld\" cy=\"%lld\"/>\n", pres.slide_w, pres.slide_h);
    pres_xml += buf;
    pres_xml += "<p:sldIdLst>\n";
    for (size_t i = 0; i < pres.slides.size(); ++i) {
        snprintf(buf, sizeof(buf), "<p:sldId id=\"%lld\" r:id=\"rId%zu\"/>\n", pres.slides[i]->id, i + 4);
        pres_xml += buf;
    }
    pres_xml += "</p:sldIdLst>\n";
    pres_xml += "</p:presentation>";
    zip.add("ppt/presentation.xml", pres_xml);

    // ---- ppt/_rels/presentation.xml.rels ----
    std::string pres_rels_xml = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n";
    // theme
    for (size_t i = 0; i < pres.themes.size(); ++i) {
        pres_rels_xml += "<Relationship Id=\"rId" + std::to_string(i + 1) + "\" Type=\"" + rel::t(rel::THEME) +
            "\" Target=\"theme/theme" + std::to_string(i + 1) + ".xml\"/>\n";
    }
    // slides：关系目标必须相对 ppt/ 目录，需保留 slides/ 这一级
    for (size_t i = 0; i < pres.slides.size(); ++i) {
        std::string sp;
        if (pres.slides[i]->part.empty()) {
            sp = "slides/slide" + std::to_string(i + 1) + ".xml";
        } else {
            sp = norm_part_path(pres.slides[i]->part);
            if (starts_with(sp, "ppt/")) sp = sp.substr(4);
        }
        pres_rels_xml += "<Relationship Id=\"rId" + std::to_string(i + 4) + "\" Type=\"" + rel::t(rel::SLIDE) +
            "\" Target=\"" + sp + "\"/>\n";
    }
    pres_rels_xml += "</Relationships>";
    zip.add("ppt/_rels/presentation.xml.rels", pres_rels_xml);

    // ---- 主题 ----
    std::string th_simple = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<a:theme xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" name=\"Office 主题\">\n"
        "<a:themeElements>\n"
        "<a:clrScheme name=\"Office\">\n"
        "<a:dk1><a:sysClr lastClr=\"000000\"/></a:dk1><a:lt1><a:sysClr lastClr=\"FFFFFF\"/></a:lt1>\n"
        "<a:dk2><a:srgbClr val=\"44546A\"/></a:dk2><a:lt2><a:srgbClr val=\"E7E6E6\"/></a:lt2>\n"
        "<a:accent1><a:srgbClr val=\"4472C4\"/></a:accent1><a:accent2><a:srgbClr val=\"ED7D31\"/></a:accent2>\n"
        "<a:accent3><a:srgbClr val=\"A5A5A5\"/></a:accent3><a:accent4><a:srgbClr val=\"FFC000\"/></a:accent4>\n"
        "<a:accent5><a:srgbClr val=\"5B9BD5\"/></a:accent5><a:accent6><a:srgbClr val=\"70AD47\"/></a:accent6>\n"
        "<a:hlink><a:srgbClr val=\"0563C1\"/></a:hlink><a:folHlink><a:srgbClr val=\"954F72\"/></a:folHlink>\n"
        "</a:clrScheme>\n"
        "<a:fontScheme name=\"Office\"><a:majorFont><a:latin typeface=\"Calibri Light\"/></a:majorFont>"
        "<a:minorFont><a:latin typeface=\"Calibri\"/></a:minorFont></a:fontScheme>\n"
        "<a:fmtScheme name=\"Office\"/>"
        "</a:themeElements>\n</a:theme>";
    for (size_t i = 0; i < pres.themes.size(); ++i) {
        // 如果有真正解析过的 theme 就输出简化版
        zip.add("ppt/theme/theme" + std::to_string(i + 1) + ".xml", th_simple);
    }
    if (pres.themes.empty()) zip.add("ppt/theme/theme1.xml", th_simple);

    // ---- 媒体 ----
    for (const auto& kv : pres.media) {
        std::string nm = kv.first;
        if (!starts_with(nm, "ppt/")) nm = "ppt/" + nm;
        zip.add(nm, kv.second);
    }

    // ---- 未识别部件 ----
    for (const auto& kv : pres.raw_parts) zip.add(kv.first, kv.second);

    // ---- 幻灯片 ----
    for (size_t si = 0; si < pres.slides.size(); ++si) {
        const Slide& sl = *pres.slides[si];
        std::string sp = sl.part.empty() ? ("ppt/slides/slide" + std::to_string(si + 1) + ".xml") : sl.part;

        std::string sxml = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<p:sld xmlns:p=\"" + std::string(xmlns::P) + "\" xmlns:a=\"" + std::string(xmlns::A) +
            "\" xmlns:r=\"" + std::string(xmlns::R) + "\">\n<p:cSld>\n<p:spTree>\n";

        // bg — 简化：如果不从母版继承就输出
        if (!sl.bg.from_master && sl.bg.fill.kind != FillKind::Inherit && sl.bg.fill.kind != FillKind::NoFill) {
            sxml += "<p:bg><p:bgPr><a:solidFill><a:srgbClr val=\"" + sl.bg.fill.solid.to_hex() + "\"/></a:solidFill></p:bgPr></p:bg>\n";
        }

        // 形状群组占位
        sxml += "<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>\n";

        std::function<void(const ShapePtr&, std::string&)> write_shape;
        write_shape = [&](const ShapePtr& s, std::string& out) {
            out += "<p:sp><p:nvSpPr><p:cNvPr id=\"" + std::to_string(s->id) + "\" name=\"" + xml_escape(s->name) + "\"/>"
                   "<p:cNvSpPr/><p:nvPr/></p:nvSpPr>\n";
            out += "<p:spPr><a:xfrm";
            if (s->rot != 0) { char db[32]; snprintf(db, sizeof(db), "%.0f", s->rot * 60000); out += " rot=\"" + std::string(db) + "\""; }
            out += "><a:off x=\"" + std::to_string(s->x) + "\" y=\"" + std::to_string(s->y) + "\"/>"
                   "<a:ext cx=\"" + std::to_string(s->cx) + "\" cy=\"" + std::to_string(s->cy) + "\"/></a:xfrm>\n";
            out += "<a:prstGeom prst=\"" + s->prst_geom + "\"/>\n";
            // fill
            if (s->fill.kind == FillKind::Solid) out += "<a:solidFill><a:srgbClr val=\"" + s->fill.solid.to_hex() + "\"/></a:solidFill>\n";
            else if (s->fill.kind == FillKind::NoFill) out += "<a:noFill/>\n";
            // line
            if (!s->line.inherit) {
                if (s->line.explicit_none) out += "<a:ln><a:noFill/></a:ln>\n";
                else {
                    out += "<a:ln w=\"" + std::to_string(s->line.w) + "\"><a:solidFill><a:srgbClr val=\"" +
                           s->line.color.to_hex() + "\"/></a:solidFill></a:ln>\n";
                }
            }
            out += "</p:spPr>\n";
            // text
            if (!s->text.paras.empty()) {
                out += "<p:txBody><a:bodyPr/><a:lstStyle/>\n";
                for (const TextParagraph& para : s->text.paras) {
                    out += "<a:p>";
                    for (const TextRun& run : para.runs) {
                        out += "<a:r><a:rPr sz=\"" + std::to_string((int)(run.props.sz_set ? run.props.sz : 1800)) +
                               "\"/><a:t>" + xml_escape_text(run.text) + "</a:t></a:r>";
                    }
                    out += "</a:p>\n";
                }
                out += "</p:txBody>\n";
            }
            out += "</p:sp>\n";
        };

        for (const ShapePtr& sp : sl.shapes) write_shape(sp, sxml);

        sxml += "</p:spTree>\n</p:cSld>\n</p:sld>";
        zip.add(sp, sxml);

        // slide rels
        std::string srel = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                           "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n";
        if (!sl.layout_part.empty())
            srel += "<Relationship Id=\"rId1\" Type=\"" + rel::t(rel::SLIDE_LAYOUT) +
                    "\" Target=\"../" + (starts_with(sl.layout_part, "ppt/") ? sl.layout_part.substr(4) : sl.layout_part) + "\"/>\n";
        srel += "</Relationships>";
        std::string srp = pres.rel_part_for(sp);
        zip.add(srp, srel);
    }

    // ---- 版式 ----
    for (const SlideLayoutDef& ld : pres.layouts) {
        std::string lxml = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<p:sldLayout xmlns:p=\"" + std::string(xmlns::P) + "\" xmlns:a=\"" + std::string(xmlns::A) +
            "\" xmlns:r=\"" + std::string(xmlns::R) + "\">\n<p:cSld><p:spTree>\n";
        lxml += "<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>\n";
        // shapes simplified — 省略单个形状写出
        lxml += "</p:spTree></p:cSld></p:sldLayout>";
        zip.add(ld.part, lxml);
    }

    // ---- 母版 ----
    for (const SlideMasterDef& md : pres.masters) {
        std::string mxml = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<p:sldMaster xmlns:p=\"" + std::string(xmlns::P) + "\" xmlns:a=\"" + std::string(xmlns::A) +
            "\" xmlns:r=\"" + std::string(xmlns::R) + "\">\n<p:cSld><p:spTree>\n";
        mxml += "<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>\n";
        mxml += "</p:spTree></p:cSld></p:sldMaster>";
        zip.add(md.part, mxml);
    }

    return zip.save(path, err);
}

} // namespace ppt