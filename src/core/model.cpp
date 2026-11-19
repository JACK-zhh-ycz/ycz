#include "model.h"
#include "../base/strutil.h"
#include "../oxml/reltype.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <functional>

namespace ppt {

// ============== TextParagraph ==============

std::string TextParagraph::plain_text() const {
    std::string s;
    for (const TextRun& r : runs) s += r.text;
    return s;
}

void TextParagraph::set_plain_text(const std::string& t) {
    if (runs.empty()) runs.emplace_back();
    runs.resize(1);
    runs[0].text = t;
}

const RunProps* TextParagraph::first_props() const {
    return runs.empty() ? nullptr : &runs[0].props;
}

// ============== TextBody ==============

std::string TextBody::all_text() const {
    std::string s;
    for (size_t i = 0; i < paras.size(); ++i) {
        if (i) s += '\n';
        s += paras[i].plain_text();
    }
    return s;
}

void TextBody::set_all_text(const std::string& t) {
    clear();
    std::vector<std::string> lines;
    for (size_t p = 0; p <= t.size(); ++p) {
        if (p == t.size() || t[p] == '\n') {
            TextParagraph para;
            TextRun run;
            run.text = t;  // 临时全量——在外面 split
            para.runs.push_back(std::move(run));
            paras.push_back(std::move(para));
        }
    }
    // 正确的行为：按 \n 拆行
    paras.clear();
    size_t start = 0;
    for (size_t i = 0; i <= t.size(); ++i) {
        if (i == t.size() || t[i] == '\n') {
            std::string line = t.substr(start, i - start);
            if (i > start && t[i - 1] == '\r') line.pop_back();
            TextParagraph para;
            TextRun run;
            run.text = line;
            para.runs.push_back(std::move(run));
            paras.push_back(std::move(para));
            start = i + 1;
        }
    }
}

void TextBody::ensure_first() { if (paras.empty()) paras.emplace_back(); }

TextParagraph& TextBody::add_para() {
    paras.emplace_back();
    return paras.back();
}

TextParagraph& TextBody::insert_para(size_t idx) {
    if (idx > paras.size()) idx = paras.size();
    paras.insert(paras.begin() + idx, TextParagraph());
    return paras[idx];
}

void TextBody::remove_para(size_t idx) {
    if (idx < paras.size()) paras.erase(paras.begin() + idx);
}

void TextBody::move_para(size_t from, size_t to) {
    if (from >= paras.size() || to >= paras.size() || from == to) return;
    TextParagraph p = std::move(paras[from]);
    paras.erase(paras.begin() + from);
    if (to > from) --to;
    paras.insert(paras.begin() + to, std::move(p));
}

TextRun& TextBody::add_run(TextParagraph& p) {
    p.runs.emplace_back();
    return p.runs.back();
}

void TextBody::normalize_defaults(double default_sz_pt, const ARGB& default_color,
                                  const std::string& default_font) {
    for (TextParagraph& para : paras) {
        for (TextRun& r : para.runs) {
            if (!r.props.sz_set) { r.props.sz = default_sz_pt * 100; r.props.sz_set = true; }
            if (!r.props.color_set) { r.props.color = default_color; r.props.color_set = true; }
            if (r.props.latin.empty()) r.props.latin = default_font;
            if (r.props.ea.empty()) r.props.ea = default_font;
        }
    }
}

// ============== Table ==============

void Table::resize(size_t nr, size_t nc) {
    rows.resize(nr, 0);
    cols.resize(nc, 0);
    cells.resize(nr);
    for (size_t r = 0; r < nr; ++r) {
        cells[r].resize(nc);
    }
}

long long Table::total_w() const {
    long long w = 0;
    for (long long c : cols) w += c;
    return w;
}

long long Table::total_h() const {
    long long h = 0;
    for (long long r : rows) h += r;
    return h;
}

// ============== Shape ==============

std::string Shape::kind_label() const {
    switch (kind) {
        case ShapeKind::Group:       return "组合";
        case ShapeKind::Rect:        return "矩形";
        case ShapeKind::RoundedRect: return "圆角矩形";
        case ShapeKind::Ellipse:     return "椭圆";
        case ShapeKind::Triangle:    return "三角形";
        case ShapeKind::Diamond:     return "菱形";
        case ShapeKind::Arrow:       return "箭头";
        case ShapeKind::Picture:     return "图片";
        case ShapeKind::Table:       return "表格";
        case ShapeKind::Chart:       return "图表";
        case ShapeKind::Line:        return "线条";
        case ShapeKind::Freeform:    return "自由形";
        default: return "形状";
    }
}

ShapePtr clone_shape(const ShapePtr& src, long long new_id) {
    if (!src) return nullptr;
    // 浅拷贝可在多数情况下用 make_shared + 逐字段赋值，但 table/children 需要深拷贝
    ShapePtr c = std::make_shared<Shape>(*src);
    if (new_id) c->id = new_id;
    if (src->table) c->table = std::make_shared<Table>(*src->table);
    c->children.clear();
    for (const ShapePtr& ch : src->children) c->children.push_back(clone_shape(ch));
    return c;
}

// ============== Slide ==============

ShapePtr Slide::find_by_id(long long sid) const {
    for (const ShapePtr& sp : shapes) {
        if (sp->id == sid) return sp;
        if (sp->is_group()) {
            std::function<ShapePtr(const std::vector<ShapePtr>&)> rec =
                [&](const std::vector<ShapePtr>& ch)->ShapePtr {
                for (const ShapePtr& c : ch) {
                    if (c->id == sid) return c;
                    if (c->is_group()) { ShapePtr r = rec(c->children); if (r) return r; }
                }
                return nullptr;
            };
            ShapePtr r = rec(sp->children);
            if (r) return r;
        }
    }
    return nullptr;
}

ShapePtr Slide::find_deep(long long sid) const { return find_by_id(sid); }

ShapePtr Slide::find_by_name(const std::string& nm) const {
    for (const ShapePtr& sp : shapes) if (sp->name == nm) return sp;
    return nullptr;
}

bool Slide::remove_shape(long long sid) {
    for (size_t i = 0; i < shapes.size(); ++i) {
        if (shapes[i]->id == sid) { shapes.erase(shapes.begin() + i); return true; }
    }
    return false;
}

void Slide::normalize_ids(long long start) {
    long long cur = start;
    std::function<void(ShapePtr&)> fix = [&](ShapePtr& s) { s->id = cur++; };
    for (ShapePtr& sp : shapes) fix(sp);
}

long long Slide::max_id() const {
    long long mx = 0;
    std::function<void(const ShapePtr&)> walk = [&](const ShapePtr& s) {
        if (s->id > mx) mx = s->id;
        for (const ShapePtr& c : s->children) walk(c);
    };
    for (const ShapePtr& sp : shapes) walk(sp);
    return mx;
}

void Slide::recompute_max_id() { /* no-op: idempotent until next add */ }

size_t Slide::next_shape_id() const { return (size_t)(max_id() + 1); }

ShapePtr Slide::title_shape() const {
    for (const ShapePtr& sp : shapes)
        if (sp->ph_type == PhType::Title || sp->ph_type == PhType::CenterTitle) return sp;
    return nullptr;
}

std::string Slide::title_text() const {
    ShapePtr t = title_shape();
    return t ? t->text.all_text() : std::string();
}

// ============== Theme / Colors ==============

bool ThemeColors::resolve(const std::string& name, ARGB& out) const {
    if (name == "tx1" || name == dk1_name) { out = dk1; return true; }
    if (name == "bg1" || name == lt1_name) { out = lt1; return true; }
    if (name == "tx2" || name == dk2_name) { out = dk2; return true; }
    if (name == "bg2" || name == lt2_name) { out = lt2; return true; }
    if (name == "hlink") { out = hlink; return true; }
    if (name == "folHlink") { out = folhlink; return true; }
    if (name.find("accent") == 0 && name.size() == 7) {
        int idx = name[6] - '1';
        if (idx >= 0 && idx < 6) { out = accent[idx]; return true; }
    }
    if (name == "phClr") { out = ARGB(255, 0, 0, 0); return true; }
    return false;
}

// ============== Presentation ==============

Presentation::Presentation() {}

Theme* Presentation::theme(size_t idx) {
    if (idx < themes.size()) return &themes[idx];
    return nullptr;
}

const Theme* Presentation::theme(size_t idx) const {
    if (idx < themes.size()) return &themes[idx];
    return nullptr;
}

SlideLayoutDef* Presentation::layout_by_part(const std::string& part) {
    for (SlideLayoutDef& l : layouts) if (l.part == part) return &l;
    return nullptr;
}

const SlideLayoutDef* Presentation::layout_by_part(const std::string& part) const {
    for (const SlideLayoutDef& l : layouts) if (l.part == part) return &l;
    return nullptr;
}

SlideLayoutDef* Presentation::layout_by_id(long long id) {
    for (SlideLayoutDef& l : layouts) if (l.id == id) return &l;
    return nullptr;
}

SlideMasterDef* Presentation::master_by_part(const std::string& part) {
    for (SlideMasterDef& m : masters) if (m.part == part) return &m;
    return nullptr;
}

const SlideMasterDef* Presentation::master_by_part(const std::string& part) const {
    for (const SlideMasterDef& m : masters) if (m.part == part) return &m;
    return nullptr;
}

long long Presentation::add_layout_def(SlideLayoutDef d) {
    long long nid = 1;
    if (!layouts.empty()) nid = layouts.back().id + 1;
    d.id = nid;
    layouts.push_back(std::move(d));
    return nid;
}

long long Presentation::add_master_def(SlideMasterDef d) {
    long long nid = 1;
    if (!masters.empty()) nid = masters.back().id + 1;
    d.id = nid;
    masters.push_back(std::move(d));
    return nid;
}

SlidePtr Presentation::add_slide(const SlidePtr& after) {
    SlidePtr s = std::make_shared<Slide>();
    long long nid = 1;
    for (const SlidePtr& sl : slides) if (sl->id >= nid) nid = sl->id + 1;
    s->id = nid;
    s->name = "幻灯片" + std::to_string(slides.size() + 1);
    if (after) {
        for (size_t i = 0; i < slides.size(); ++i) {
            if (slides[i] == after) { slides.insert(slides.begin() + i + 1, s); return s; }
        }
    }
    slides.push_back(s);
    return s;
}

SlidePtr Presentation::duplicate_slide(size_t idx) {
    if (idx >= slides.size()) return nullptr;
    SlidePtr old = slides[idx];
    SlidePtr s = std::make_shared<Slide>();
    s->id = (long long)(slides.size() + 200);
    s->name = old->name + " 副本";
    s->layout_part = old->layout_part;
    s->layout_id = old->layout_id;
    s->trans = old->trans;
    s->bg = old->bg;
    s->notes = old->notes;
    for (const ShapePtr& sp : old->shapes) s->shapes.push_back(clone_shape(sp));
    slides.insert(slides.begin() + idx + 1, s);
    return s;
}

bool Presentation::remove_slide(size_t idx) {
    if (idx >= slides.size()) return false;
    slides.erase(slides.begin() + idx);
    return true;
}

bool Presentation::move_slide(size_t from, size_t to) {
    if (from >= slides.size() || to >= slides.size() || from == to) return false;
    SlidePtr s = slides[from];
    if (to > from) to--;
    slides.erase(slides.begin() + from);
    slides.insert(slides.begin() + to, s);
    return true;
}

void Presentation::swap_slides(size_t a, size_t b) {
    if (a >= slides.size() || b >= slides.size() || a == b) return;
    std::swap(slides[a], slides[b]);
}

SlidePtr Presentation::slide(size_t i) {
    return i < slides.size() ? slides[i] : nullptr;
}

size_t Presentation::index_of(const std::string& part) const {
    for (size_t i = 0; i < slides.size(); ++i) if (slides[i]->part == part) return i;
    return (size_t)-1;
}

size_t Presentation::index_of(const SlidePtr& s) const {
    for (size_t i = 0; i < slides.size(); ++i) if (slides[i] == s) return i;
    return (size_t)-1;
}

std::string Presentation::rel_part_for(const std::string& part) const {
    // 归一化分隔符为 '/'，与 ZIP 条目名保持一致
    std::string p = part;
    for (char& c : p) if (c == '\\') c = '/';
    std::string d = dir_name(p);
    std::string b = base_name(p);
    return (d.empty() ? std::string("_rels/") : (d + "/")) + "_rels/" + b + ".rels";
}

std::string Presentation::find_rel(const std::string& part, const std::string& rid,
                                   std::string& mode) const {
    std::string rp = rel_part_for(part);
    auto it = rels.find(rp);
    if (it != rels.end()) {
        for (const Rel& r : it->second) {
            if (r.id == rid) { mode = r.mode; return r.target; }
        }
    }
    return std::string();
}

std::string Presentation::add_rel(const std::string& part, const std::string& type_tail,
                                  const std::string& target, bool external,
                                  const std::string& rid_hint) {
    std::string rp = rel_part_for(part);
    auto& rv = rels[rp];
    long long maxN = 0;
    for (const Rel& r : rv) {
        const char* s = r.id.c_str();
        if (s[0] == 'r' || s[0] == 'R') {
            long long n = parse_ll(s + 1, 0);
            if (n > maxN) maxN = n;
        }
    }
    Rel rel;
    rel.id = rid_hint.empty() ? ("rId" + std::to_string(std::max(maxN + 1, 1LL))) : rid_hint;
    rel.type = rel::t(type_tail);
    rel.target = target;
    rel.mode = external ? "External" : "";
    rv.push_back(std::move(rel));
    return rv.back().id;
}

void Presentation::drop_rel(const std::string& part, const std::string& rid) {
    std::string rp = rel_part_for(part);
    auto it = rels.find(rp);
    if (it == rels.end()) return;
    auto& v = it->second;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Rel& r) { return r.id == rid; }), v.end());
}

std::string Presentation::add_media(const std::string& ext,
                                    const std::vector<uint8_t>& bytes,
                                    std::string& part_out) {
    std::string e = ext.empty() ? std::string("png") : ext;
    for (char& ch : e) ch = (char)tolower((unsigned char)ch);
    std::set<std::string> used;
    for (const auto& kv : media) used.insert(kv.first);
    for (const auto& kv : orig_parts) used.insert(kv.first);

    int n = 1;
    std::string part;
    do { part = "ppt/media/image" + std::to_string(n++) + "." + e; } while (used.count(part));
    media[part] = bytes;
    part_out = part;

    // 内容类型默认项（png/jpg/gif/bmp）
    if (!content_types_defaults.count(e)) {
        std::string ct = "image/" + (e == "jpg" ? std::string("jpeg") : e);
        content_types_defaults[e]["ContentType"] = ct;
    }
    return part;
}

long long Presentation::max_shape_id() const {
    long long mx = 1;
    for (const SlidePtr& sl : slides) { long long m = sl->max_id(); if (m > mx) mx = m; }
    return mx;
}

void Presentation::ensure_ids_unique() {
    std::set<long long> used;
    for (SlidePtr& sl : slides) {
        for (ShapePtr& sp : sl->shapes) {
            // 简单策略：遍历后递增
            std::function<void(ShapePtr&)> fix = [&](ShapePtr& s) {
                while (used.count(s->id)) s->id++;
                used.insert(s->id);
                for (ShapePtr& c : s->children) fix(c);
            };
            fix(sp);
        }
    }
}

void Presentation::set_size_by_aspect(const std::string& ratio, long long w_emu_default) {
    if (ratio == "16:9") { slide_w = 12192000; slide_h = 6858000; aspect = "16:9"; }
    else if (ratio == "4:3") { slide_w = 9144000; slide_h = 6858000; aspect = "4:3"; }
    else if (ratio == "16:10") { slide_w = 12192000; slide_h = 7620000; aspect = "16:10"; }
    else { slide_w = w_emu_default; slide_h = (long long)(w_emu_default / 1.7777 + 0.5); aspect = "16:9"; }
}

std::string Presentation::size_label() const {
    double wi = slide_w / 914400.0;
    double hi = slide_h / 914400.0;
    char buf[64];
    snprintf(buf, sizeof(buf), "%.2f x %.2f 英寸 (%s)", wi, hi, aspect.c_str());
    return std::string(buf);
}

size_t Presentation::shape_count() const {
    size_t n = 0;
    for (const SlidePtr& sl : slides) {
        std::function<void(const ShapePtr&)> cnt = [&](const ShapePtr& s) {
            n++;
            for (const ShapePtr& c : s->children) cnt(c);
        };
        for (const ShapePtr& sp : sl->shapes) cnt(sp);
    }
    return n;
}

size_t Presentation::word_count() const {
    size_t n = 0;
    for (const SlidePtr& sl : slides) {
        std::function<void(const ShapePtr&)> cnt = [&](const ShapePtr& s) {
            auto wc = [](const std::string& t) {
                size_t w = 0; bool in = false;
                for (char c : t) { if ((unsigned char)c > ' ') { if (!in) { w++; in = true; } } else in = false; }
                return w;
            };
            n += wc(s->text.all_text());
            if (s->table) for (auto& row : s->table->cells) for (auto& cell : row) n += wc(cell.text.all_text());
            for (const ShapePtr& c : s->children) cnt(c);
        };
        for (const ShapePtr& sp : sl->shapes) cnt(sp);
    }
    return n;
}

// ============== 渲染辅助 ==============

// scheme 引用颜色落地：scheme 非空时按主题重算 rgb，保留 alpha
static ARGB finalize_argb_with(const ARGB& c, const std::string& scheme, const Theme* th) {
    if (scheme.empty() || !th) return c;
    ARGB base;
    if (!th->colors.resolve(scheme, base)) return c;
    return ARGB(c.a, base.r, base.g, base.b);
}

ARGB RenderTheme::scheme_color(const std::string& name, const ARGB& def) const {
    if (theme) { ARGB c; if (theme->colors.resolve(name, c)) return c; }
    return def;
}

std::string RenderTheme::font_latin(bool major) const {
    if (theme) return major ? theme->fonts.major_latin : theme->fonts.minor_latin;
    return major ? "Calibri Light" : "Calibri";
}

std::string RenderTheme::font_ea(bool major) const {
    if (theme) return major ? theme->fonts.major_ea : theme->fonts.minor_ea;
    return "";
}

static Fill finalize_fill(const Fill& in, const Theme* th) {
    Fill f = in;
    if (!f.solid_scheme.empty()) f.solid = finalize_argb_with(f.solid, f.solid_scheme, th);
    for (GradientStop& st : f.stops)
        if (!st.scheme.empty()) st.color = finalize_argb_with(st.color, st.scheme, th);
    return f;
}

Fill RenderTheme::resolve_fill(const Shape& s) const {
    if (!s.fill.is_inherit()) return finalize_fill(s.fill, theme);
    // p:style fillRef：普通形状的主题样式引用（优先于占位符链）
    if (!s.is_place_holder && s.fill_ref_idx > 0 && !s.style_ref_fill.empty()) {
        Fill f; f.kind = FillKind::Solid;
        f.solid = scheme_color(s.style_ref_fill, ARGB(255, 0x44, 0x72, 0xC4));
        return f;
    }
    // 占位符继承（版式 -> 母版）：仅占位符之间才允许匹配
    if (s.is_place_holder) {
        if (layout) {
            for (const ShapePtr& lp : layout->shapes) {
                if (!lp->is_place_holder) continue;
                if (lp->ph_idx == s.ph_idx && lp->ph_type == s.ph_type && !lp->fill.is_inherit())
                    return finalize_fill(lp->fill, theme);
            }
        }
        if (master && layout && layout->show_master_bg) {
            for (const ShapePtr& mp : master->shapes) {
                if (!mp->is_place_holder) continue;
                if (mp->ph_idx == s.ph_idx && mp->ph_type == s.ph_type && !mp->fill.is_inherit())
                    return finalize_fill(mp->fill, theme);
            }
        }
    }
    if (s.fill.is_none()) return s.fill;
    Fill f; f.kind = FillKind::NoFill; return f;
}

Line RenderTheme::resolve_line(const Shape& s) const {
    if (!s.line.inherit) {
        Line l = s.line;
        if (!l.color_scheme.empty()) l.color = finalize_argb_with(l.color, l.color_scheme, theme);
        return l;
    }
    if (!s.is_place_holder && s.ln_ref_idx > 0 && !s.style_ref_ln.empty()) {
        static const double wid[4] = { 0, 6350, 12700, 28575 };
        Line l; l.inherit = false; l.explicit_none = false;
        l.w = wid[s.ln_ref_idx <= 3 ? s.ln_ref_idx : 3];
        l.color = scheme_color(s.style_ref_ln, ARGB(255, 0x44, 0x72, 0xC4));
        return l;
    }
    // 占位符继承：仅占位符之间才允许匹配
    if (s.is_place_holder) {
        if (layout) {
            for (const ShapePtr& lp : layout->shapes) {
                if (!lp->is_place_holder) continue;
                if (lp->ph_idx == s.ph_idx && lp->ph_type == s.ph_type && !lp->line.inherit) {
                    Line l = lp->line;
                    if (!l.color_scheme.empty()) l.color = finalize_argb_with(l.color, l.color_scheme, theme);
                    return l;
                }
            }
        }
        if (master && layout && layout->show_master_bg) {
            for (const ShapePtr& mp : master->shapes) {
                if (!mp->is_place_holder) continue;
                if (mp->ph_idx == s.ph_idx && mp->ph_type == s.ph_type && !mp->line.inherit) {
                    Line l = mp->line;
                    if (!l.color_scheme.empty()) l.color = finalize_argb_with(l.color, l.color_scheme, theme);
                    return l;
                }
            }
        }
    }
    if (s.ln_ref_idx > 0 && !s.style_ref_ln.empty()) {
        static const double wid[4] = { 0, 6350, 12700, 28575 };
        Line l; l.inherit = false; l.explicit_none = false;
        l.w = wid[s.ln_ref_idx <= 3 ? s.ln_ref_idx : 3];
        l.color = scheme_color(s.style_ref_ln, ARGB(255, 0x44, 0x72, 0xC4));
        return l;
    }
    Line l; l.explicit_none = true; l.inherit = false; l.w = 0;
    return l;
}

// 效果：把阴影/发光的主题色引用落地成具体颜色
Effect RenderTheme::resolve_effects(const Shape& s) const {
    Effect fx = s.fx;
    if (!fx.shadow.color_scheme.empty())
        fx.shadow.color = finalize_argb_with(fx.shadow.color, fx.shadow.color_scheme, theme);
    if (!fx.glow_scheme.empty())
        fx.glow_color = finalize_argb_with(fx.glow_color, fx.glow_scheme, theme);
    return fx;
}

// 占位符几何继承（只读）：返回版式/母版中同 idx 或同类型的几何
bool RenderTheme::placeholder_geometry(const Shape& s, long long& x, long long& y,
                                       long long& cx, long long& cy) const {
    if (s.cx > 0 && s.cy > 0) return false;
    if (!s.is_place_holder) return false;
    const Shape* src = nullptr;
    if (layout) {
        for (const ShapePtr& lp : layout->shapes)
            if (lp->ph_idx == s.ph_idx && lp->ph_type == s.ph_type && lp->cx > 0) { src = lp.get(); break; }
        if (!src)
            for (const ShapePtr& lp : layout->shapes)
                if (lp->ph_type == s.ph_type && lp->cx > 0) { src = lp.get(); break; }
    }
    if (!src && master) {
        for (const ShapePtr& mp : master->shapes)
            if (mp->ph_idx == s.ph_idx && mp->ph_type == s.ph_type && mp->cx > 0) { src = mp.get(); break; }
        if (!src)
            for (const ShapePtr& mp : master->shapes)
                if (mp->ph_type == s.ph_type && mp->cx > 0) { src = mp.get(); break; }
    }
    if (src) { x = src->x; y = src->y; cx = src->cx; cy = src->cy; return true; }

    // 兜底默认几何（常见版式位置）
    long long W = pres ? pres->slide_w : 12192000;
    long long H = pres ? pres->slide_h : 6858000;
    long long m = W / 20;                       // 5% 边距
    switch (s.ph_type) {
        case PhType::Title: case PhType::CenterTitle: case PhType::SlideTitle:
            x = m; y = H / 8;  cx = W - 2 * m; cy = H / 4; break;
        case PhType::SubTitle:
            x = m; y = H * 3 / 8; cx = W - 2 * m; cy = H / 6; break;
        case PhType::Body: case PhType::Object:
            x = m; y = H / 4;  cx = W - 2 * m; cy = H / 2; break;
        default:
            x = 0; y = 0; cx = W; cy = H;
    }
    return true;
}

// 占位符几何继承：形状自身没有 xfrm 时，取版式/母版同 idx 的几何
void RenderTheme::resolve_geometry(Shape& s) const {
    if (!s.is_place_holder) return;
    long long x, y, cx, cy;
    if (placeholder_geometry(s, x, y, cx, cy)) { s.x = x; s.y = y; s.cx = cx; s.cy = cy; }
}

// 段落默认字号（未显式设置时按占位符类型兜底）
static double ph_default_sz(PhType t) {
    switch (t) {
        case PhType::Title: case PhType::CenterTitle: case PhType::SlideTitle: return 44;
        case PhType::SubTitle: return 24;
        case PhType::Body: case PhType::Object: return 18;
        default: return 18;
    }
}

ResolvedRun RenderTheme::resolve_run(const Shape& s, const TextParagraph& p,
                                     const TextRun& r) const {
    // 继承顺序：run -> 段落 defRPr -> 占位符默认
    ResolvedRun rr;
    if (r.props.sz_set) rr.sz_pt = r.props.sz / 100.0;
    else if (p.para_default_set && p.para_default.sz_set) rr.sz_pt = p.para_default.sz / 100.0;
    else rr.sz_pt = ph_default_sz(s.ph_type);

    rr.b = (r.props.b_set && r.props.b) || (p.para_default_set && p.para_default.b_set && p.para_default.b);
    rr.i = (r.props.i_set && r.props.i) || (p.para_default_set && p.para_default.i_set && p.para_default.i);
    rr.u = (r.props.u_set && r.props.u != "none") ||
           (p.para_default_set && p.para_default.u_set && p.para_default.u != "none");
    rr.strike = r.props.strike || (p.para_default_set && p.para_default.strike);

    // 颜色
    ARGB c(255, 0, 0, 0); std::string sch;
    if (r.props.color_set) { c = r.props.color; sch = r.props.color_scheme; }
    else if (p.para_default_set && p.para_default.color_set) { c = p.para_default.color; sch = p.para_default.color_scheme; }
    if (!sch.empty()) c = finalize_argb_with(c, sch, theme);
    else if (!r.props.color_set && !(p.para_default_set && p.para_default.color_set)) {
        // 文字颜色默认继承主题 tx1
        c = scheme_color("tx1", ARGB(255, 0, 0, 0));
    }
    rr.color = c;

    // 字体：优先 run/段落默认；CJK 内容用 ea 字体
    auto pick = [&](const std::string& a, const std::string& b)->std::string {
        return !a.empty() ? a : b;
    };
    std::string lat = pick(r.props.latin, p.para_default_set ? p.para_default.latin : "");
    std::string ea  = pick(r.props.ea, p.para_default_set ? p.para_default.ea : "");
    bool major = (s.ph_type == PhType::Title || s.ph_type == PhType::CenterTitle);
    if (lat.empty()) lat = font_latin(major);
    if (ea.empty()) ea = font_ea(major);
    rr.font = lat;
    rr.font_ea = ea;

    if (r.props.has_link()) {
        rr.link = r.props.link_target;
        // 内部链接（幻灯片部件名）由渲染层换成页内锚点
        if (!r.props.hyperlink_is_ext && !r.props.action_part.empty()) rr.link = "";
    }
    if (rr.sz_pt > 0 && s.text.fontScale > 0 && s.text.fontScale != 100)
        rr.sz_pt = rr.sz_pt * s.text.fontScale / 100.0;
    return rr;
}

ARGB RenderTheme::background_of(const Slide& sl, bool with_master) const {
    Fill bf = background_fill(sl);
    if (bf.kind == FillKind::Solid || bf.kind == FillKind::NoFill) return bf.solid;
    return ARGB(255, 255, 255, 255);
}

Fill RenderTheme::background_fill(const Slide& sl) const {
    if (!sl.bg.fill.is_inherit() && !sl.bg.fill.is_none()) return sl.bg.fill;
    if (sl.layout_part.empty() || !layout || !sl.bg.from_master) return sl.bg.fill;
    if (master && layout->show_master_bg && master->has_bg && !master->bg_fill.is_inherit())
        return master->bg_fill;
    return layout->bg_fill;
}

RenderTheme make_render_theme(const Presentation& p, const Slide& s) {
    RenderTheme rt;
    rt.pres = &p;
    rt.slide = &s;
    rt.layout = p.layout_by_part(s.layout_part);
    if (rt.layout && !rt.layout->master_part.empty())
        rt.master = p.master_by_part(rt.layout->master_part);
    if (!rt.master && rt.layout)
        rt.master = p.master_by_part("ppt/slideMasters/slideMaster1.xml");
    if (!rt.master && !p.masters.empty()) rt.master = &p.masters[0];
    rt.theme = p.theme(0);
    return rt;
}

} // namespace ppt