#include "pptx_api.h"
#include "../core/model.h"
#include "../io/pptx_io.h"
#include "../html/html_render.h"
#include "../ui/render_gdiplus.h"
#include "../base/strutil.h"
#include "../oxml/reltype.h"
#include "../plugin/plugin_host.h"

#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cstdio>
#include <algorithm>
#include <functional>
#include <windows.h>

using namespace ppt;

// ---------------- 内部状态 ----------------
struct UndoPair {
    std::string key;
    std::function<void()> undo;
    std::function<void()> redo;
};

struct DocHandle {
    Presentation pres;
    std::vector<UndoPair> undo_stack, redo_stack;
    std::vector<UndoPair> txn;
    bool txn_open = false;
};

// 记录一步可撤销操作；事务期间同 key 的多次修改合并（只保留第一次的 undo 与最后一次的 redo）
static void record_step(DocHandle* h, const std::string& key,
                        std::function<void()> undo, std::function<void()> redo,
                        bool coalesce = true) {
    if (!h) return;
    if (h->txn_open && coalesce) {
        for (UndoPair& p : h->txn)
            if (p.key == key) { p.redo = std::move(redo); return; }
        h->txn.push_back(UndoPair{key, std::move(undo), std::move(redo)});
        return;
    }
    h->undo_stack.push_back(UndoPair{key, std::move(undo), std::move(redo)});
    if (h->undo_stack.size() > 80) h->undo_stack.erase(h->undo_stack.begin());
    h->redo_stack.clear();
}
static DocHandle* H(PptDoc d) { return (DocHandle*)d; }

static std::string g_err = "未发生错误";
static ULONG_PTR g_tok = 0;
static bool g_gdi_ready = false;

static void set_err(const std::string& s) { g_err = s; }

static bool ensure_gdi() {
    if (g_gdi_ready) return true;
    Gdiplus::GdiplusStartupInput si;
    if (Gdiplus::GdiplusStartup(&g_tok, &si, nullptr) != Gdiplus::Ok) {
        set_err("GDI+ 初始化失败");
        return false;
    }
    g_gdi_ready = true;
    return true;
}

// UTF-8 安全截断复制（不切断多字节字符）
static void copy_utf8(char* dst, size_t cap, const std::string& s) {
    if (!dst || cap == 0) return;
    size_t i = 0, o = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        size_t len = 1;
        if (c >= 0xF0) len = 4; else if (c >= 0xE0) len = 3; else if (c >= 0xC0) len = 2;
        if (i + len > s.size() || o + len >= cap) break;
        for (size_t k = 0; k < len; ++k) dst[o++] = s[i + k];
        i += len;
    }
    dst[o] = 0;
}

static Slide* get_slide(PptDoc doc, int idx, bool write = false) {
    if (!doc) { set_err("文档句柄为空"); return nullptr; }
    Presentation& p = ((DocHandle*)doc)->pres;
    if (idx < 0 || idx >= (int)p.slides.size()) { set_err("页码越界"); return nullptr; }
    if (write) p.dirty = true;
    return p.slides[idx].get();
}

// 目标形状（可能嵌套于组）所属的顶层索引
static int top_index_of(Slide& sl, const ShapePtr& target) {
    std::function<bool(const ShapePtr&, const ShapePtr&)> in =
        [&](const ShapePtr& root, const ShapePtr& t) -> bool {
        if (root == t) return true;
        for (const ShapePtr& ch : root->children)
            if (in(ch, t)) return true;
        return false;
    };
    for (size_t i = 0; i < sl.shapes.size(); ++i)
        if (in(sl.shapes[i], target)) return (int)i;
    return -1;
}

// 编辑文本时保留原样式
static void edit_text_keep_style(TextBody& tb, const std::string& text) {
    RunProps style;
    bool have = false;
    for (const TextParagraph& p : tb.paras)
        if (!p.runs.empty()) { style = p.runs[0].props; have = true; break; }
    tb.set_all_text(text);
    if (have)
        for (TextParagraph& p : tb.paras)
            for (TextRun& r : p.runs) r.props = style;
}

// ---------------- 文档生命周期 ----------------
extern "C" {

static bool g_plugins_loaded = false;

static std::string default_plugin_dir() {
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(const void*)&pptx_open, &mod);
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(mod, buf, MAX_PATH);
    std::string dir = dir_name(utf16_to_utf8(std::wstring(buf)));
    std::string sub = join_path(dir, "plugins");
    DWORD attr = GetFileAttributesW(utf8_to_utf16(sub).c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return sub;
    return std::string();
}

PPT_API PptDoc pptx_open(const char* utf8_path) {
    if (!utf8_path || !*utf8_path) { set_err("路径为空"); return nullptr; }
    DocHandle* h = new (std::nothrow) DocHandle();
    if (!h) { set_err("内存不足"); return nullptr; }
    std::string err;
    if (!pptx_read(utf8_path, h->pres, err)) {
        set_err("打开失败: " + err);
        delete h;
        return nullptr;
    }
    if (h->pres.slides.empty()) { set_err("文件中没有幻灯片"); delete h; return nullptr; }
    plugin_host_bind(&h->pres);
    if (!g_plugins_loaded) {
        std::string pdir = default_plugin_dir();
        if (!pdir.empty()) {
            int n = PluginHost::instance().load_dir(pdir);
            g_plugins_loaded = true;
            (void)n;
        }
    }
    return (PptDoc)h;
}

PPT_API void pptx_close(PptDoc doc) {
    delete (DocHandle*)doc;
}

PPT_API int pptx_slide_count(PptDoc doc) {
    if (!doc) return 0;
    return (int)((DocHandle*)doc)->pres.slides.size();
}

PPT_API void pptx_slide_size(PptDoc doc, long long* w_emu, long long* h_emu) {
    if (!doc) return;
    const Presentation& p = ((DocHandle*)doc)->pres;
    if (w_emu) *w_emu = p.slide_w;
    if (h_emu) *h_emu = p.slide_h;
}

// ---------------- 渲染 ----------------
PPT_API int pptx_render_rgba(PptDoc doc, int slide_idx, int px_w, int px_h,
                             unsigned char** out_buf, int* out_w, int* out_h) {
    if (out_buf) *out_buf = nullptr;
    if (!doc || !out_buf) { set_err("参数非法"); return -1; }
    Presentation& p = ((DocHandle*)doc)->pres;
    if (slide_idx < 0 || slide_idx >= (int)p.slides.size()) { set_err("页码越界"); return -2; }
    if (px_w <= 0 || px_h <= 0 || px_w > 8192 || px_h > 8192) { set_err("尺寸非法"); return -3; }
    if (!ensure_gdi()) return -4;

    ImageCache imgs(p);
    RenderTheme rt = make_render_theme(p, *p.slides[slide_idx]);
    DrawCtx ctx;
    ctx.sw = p.slide_w; ctx.sh = p.slide_h;
    ctx.cw = px_w; ctx.ch = px_h;
    ctx.rt = &rt;
    ctx.slide_idx = slide_idx; ctx.slide_total = (int)p.slides.size();
    SYSTEMTIME st; GetLocalTime(&st);
    char db[32]; snprintf(db, sizeof db, "%04d/%02d/%02d", st.wYear, st.wMonth, st.wDay);
    ctx.date_str = db;

    Gdiplus::Bitmap bmp(px_w, px_h, PixelFormat32bppARGB);
    if (bmp.GetLastStatus() != Gdiplus::Ok) { set_err("位图创建失败"); return -5; }
    {
        Gdiplus::Graphics g(&bmp);
        draw_slide(g, p, *p.slides[slide_idx], ctx, imgs, true);
        if (g.GetLastStatus() != Gdiplus::Ok) { set_err("绘制失败"); return -6; }
    }

    Gdiplus::Rect area(0, 0, px_w, px_h);
    Gdiplus::BitmapData bd;
    memset(&bd, 0, sizeof bd);
    if (bmp.LockBits(&area, Gdiplus::ImageLockModeRead,
                     PixelFormat32bppARGB, &bd) != Gdiplus::Ok) {
        set_err("锁定像素失败"); return -7;
    }
    size_t n = (size_t)px_w * px_h * 4;
    unsigned char* buf = (unsigned char*)malloc(n);
    if (!buf) { bmp.UnlockBits(&bd); set_err("内存不足"); return -8; }

    // 内存小端 0xAARRGGBB -> 字节 B,G,R,A；转为 Qt 需要的 R,G,B,A
    const unsigned char* src = (const unsigned char*)bd.Scan0;
    for (int y = 0; y < px_h; ++y) {
        const unsigned char* s = src + (size_t)y * bd.Stride;
        unsigned char* d = buf + (size_t)y * px_w * 4;
        for (int x = 0; x < px_w; ++x) {
            d[x*4 + 0] = s[x*4 + 2];
            d[x*4 + 1] = s[x*4 + 1];
            d[x*4 + 2] = s[x*4 + 0];
            d[x*4 + 3] = s[x*4 + 3];
        }
    }
    bmp.UnlockBits(&bd);

    *out_buf = buf;
    if (out_w) *out_w = px_w;
    if (out_h) *out_h = px_h;
    return 0;
}

PPT_API void pptx_free_buf(unsigned char* buf) {
    // 与 pptx_render_rgba 的 malloc 配对，同模块内分配释放
    free(buf);
}

// ---------------- 形状级 ----------------
static int kind_code(const Shape& s) {
    if (s.is_group()) return 5;
    if (s.is_table() && s.table) return 2;
    if (s.is_pic()) return 1;
    if (s.is_chart()) return 3;
    if (s.is_media()) return 4;
    return 0;
}

PPT_API int pptx_shape_count(PptDoc doc, int slide_idx) {
    Slide* sl = get_slide(doc, slide_idx);
    return sl ? (int)sl->shapes.size() : -1;
}

PPT_API int pptx_shape_get(PptDoc doc, int slide_idx, int shape_idx, PptShapeInfo* out) {
    if (!out) { set_err("输出缓冲为空"); return -1; }
    Slide* sl = get_slide(doc, slide_idx);
    if (!sl) return -2;
    if (shape_idx < 0 || shape_idx >= (int)sl->shapes.size()) { set_err("形状索引越界"); return -3; }
    memset(out, 0, sizeof *out);

    Presentation& p = ((DocHandle*)doc)->pres;
    RenderTheme rt = make_render_theme(p, *sl);
    Shape s = *sl->shapes[shape_idx];           // 副本：解析几何与样式
    if (s.cx <= 0 || s.cy <= 0) {
        long long x, y, cx, cy;
        if (rt.placeholder_geometry(s, x, y, cx, cy)) { s.x = x; s.y = y; s.cx = cx; s.cy = cy; }
    }
    Fill f = rt.resolve_fill(s);

    out->id = s.id;
    out->kind = kind_code(s);
    out->x = s.x; out->y = s.y; out->cx = s.cx; out->cy = s.cy;
    out->rot = s.rot;
    copy_utf8(out->name, sizeof out->name, s.name);

    std::string txt = s.text.all_text();
    if (txt.empty() && s.is_table() && s.table) {
        char b[48]; snprintf(b, sizeof b, "[表格 %zu行x%zu列]",
                             s.table->nrows(), s.table->ncols());
        txt = b;
    }
    copy_utf8(out->text, sizeof out->text, txt);

    out->fontsize_pt = -1;
    out->italic = 0;
    out->underline = 0;
    out->line_rgb = 0;
    for (const TextParagraph& para : s.text.paras)
        if (!para.runs.empty()) {
            ResolvedRun rr = rt.resolve_run(s, para, para.runs[0]);
            out->fontsize_pt = rr.sz_pt;
            out->italic = rr.i ? 1 : 0;
            out->underline = rr.u ? 1 : 0;
            std::string fn = rr.font_ea.empty() ? rr.font : rr.font_ea;
            copy_utf8(out->font_name, sizeof out->font_name, fn);
            break;
        }
    out->fill_rgb = (f.kind == FillKind::Solid && f.solid.a > 0)
                  ? ((((unsigned)f.solid.r) << 16) | (((unsigned)f.solid.g) << 8) | f.solid.b) : 0;
    if (!s.line.inherit && !s.line.explicit_none && s.line.color.a > 0)
        out->line_rgb = (((unsigned)s.line.color.r) << 16) | (((unsigned)s.line.color.g) << 8) | s.line.color.b;
    return 0;
}

PPT_API int pptx_hit(PptDoc doc, int slide_idx, int px, int py, int canvas_w, int canvas_h) {
    Slide* sl = get_slide(doc, slide_idx);
    if (!sl) return -1;
    Presentation& p = ((DocHandle*)doc)->pres;
    RenderTheme rt = make_render_theme(p, *sl);
    ShapePtr t = hit_test_slide(p, *sl, px, py, rt, canvas_w, canvas_h);
    if (!t) return -2;
    return top_index_of(*sl, t);
}

static ShapePtr top_shape(PptDoc doc, int slide_idx, int shape_idx) {
    Slide* sl = get_slide(doc, slide_idx, true);
    if (!sl) return nullptr;
    if (shape_idx < 0 || shape_idx >= (int)sl->shapes.size()) { set_err("形状索引越界"); return nullptr; }
    ShapePtr s = sl->shapes[shape_idx];
    // 修改前把占位符继承几何实体化，否则显式改动会被渲染期的继承覆盖
    if (s->is_place_holder && (s->cx <= 0 || s->cy <= 0)) {
        Presentation& p = ((DocHandle*)doc)->pres;
        RenderTheme rt = make_render_theme(p, *sl);
        rt.resolve_geometry(*s);
    }
    return s;
}

static SlidePtr slide_ptr(PptDoc doc, int si) {
    DocHandle* h = H(doc);
    if (!h || si < 0 || si >= (int)h->pres.slides.size()) return nullptr;
    return h->pres.slides[si];
}

// 生成"把几何/文字/填充恢复成给定值"的撤销动作
static std::function<void()> restore_geom(SlidePtr sl, int i, long long x, long long y,
                                          long long cx, long long cy, double rot) {
    return [sl, i, x, y, cx, cy, rot]() {
        if (i < 0 || i >= (int)sl->shapes.size()) return;
        ShapePtr t = sl->shapes[i];
        t->x = x; t->y = y; t->cx = cx; t->cy = cy; t->rot = rot;
        t->ed_geom = true;
        sl->touched = true;
    };
}

static std::function<void()> restore_text(SlidePtr sl, int i, const TextBody& tb, bool has) {
    std::shared_ptr<TextBody> copy = std::make_shared<TextBody>(tb);
    return [sl, i, copy, has]() {
        if (i < 0 || i >= (int)sl->shapes.size()) return;
        ShapePtr t = sl->shapes[i];
        t->text = *copy;
        t->has_text = has;
        t->ed_text = true;
        sl->touched = true;
    };
}

static std::function<void()> restore_fill(SlidePtr sl, int i, const Fill& f) {
    std::shared_ptr<Fill> copy = std::make_shared<Fill>(f);
    return [sl, i, copy]() {
        if (i < 0 || i >= (int)sl->shapes.size()) return;
        ShapePtr t = sl->shapes[i];
        t->fill = *copy;
        t->ed_fill = true;
        sl->touched = true;
    };
}

PPT_API int pptx_shape_move(PptDoc doc, int si, int i, long long dx_emu, long long dy_emu) {
    ShapePtr s = top_shape(doc, si, i);
    if (!s) return -1;
    SlidePtr sl = slide_ptr(doc, si);
    long long ox = s->x, oy = s->y, cx = s->cx, cy = s->cy;
    double rot = s->rot;
    s->x += dx_emu; s->y += dy_emu;
    s->ed_geom = true;
    sl->touched = true;
    std::string key = "geom:" + std::to_string(si) + ":" + std::to_string(i);
    record_step(H(doc), key,
                restore_geom(sl, i, ox, oy, cx, cy, rot),
                restore_geom(sl, i, s->x, s->y, cx, cy, rot));
    return 0;
}

PPT_API int pptx_shape_resize(PptDoc doc, int si, int i, long long w_emu, long long h_emu) {
    ShapePtr s = top_shape(doc, si, i);
    if (!s) return -1;
    if (w_emu < 12700) w_emu = 12700;         // 最小 1pt
    if (h_emu < 12700) h_emu = 12700;
    SlidePtr sl = slide_ptr(doc, si);
    long long ox = s->x, oy = s->y, ocx = s->cx, ocy = s->cy;
    double rot = s->rot;
    s->cx = w_emu; s->cy = h_emu;
    s->ed_geom = true;
    sl->touched = true;
    std::string key = "geom:" + std::to_string(si) + ":" + std::to_string(i);
    record_step(H(doc), key,
                restore_geom(sl, i, ox, oy, ocx, ocy, rot),
                restore_geom(sl, i, ox, oy, s->cx, s->cy, rot));
    return 0;
}

PPT_API int pptx_shape_set_geom(PptDoc doc, int si, int i, long long x, long long y,
                                long long cx, long long cy) {
    ShapePtr s = top_shape(doc, si, i);
    if (!s) return -1;
    if (cx < 12700) cx = 12700;
    if (cy < 12700) cy = 12700;
    SlidePtr sl = slide_ptr(doc, si);
    long long ox = s->x, oy = s->y, ocx = s->cx, ocy = s->cy;
    double rot = s->rot;
    s->x = x; s->y = y; s->cx = cx; s->cy = cy;
    s->ed_geom = true;
    sl->touched = true;
    std::string key = "geom:" + std::to_string(si) + ":" + std::to_string(i);
    record_step(H(doc), key,
                restore_geom(sl, i, ox, oy, ocx, ocy, rot),
                restore_geom(sl, i, x, y, cx, cy, rot));
    return 0;
}

PPT_API int pptx_shape_set_rot(PptDoc doc, int si, int i, double rot_deg) {
    ShapePtr s = top_shape(doc, si, i);
    if (!s) return -1;
    while (rot_deg >= 360) rot_deg -= 360;
    while (rot_deg < 0) rot_deg += 360;
    SlidePtr sl = slide_ptr(doc, si);
    long long x = s->x, y = s->y, cx = s->cx, cy = s->cy;
    double old = s->rot;
    s->rot = rot_deg;
    s->ed_geom = true;
    sl->touched = true;
    std::string key = "rot:" + std::to_string(si) + ":" + std::to_string(i);
    record_step(H(doc), key,
                restore_geom(sl, i, x, y, cx, cy, old),
                restore_geom(sl, i, x, y, cx, cy, rot_deg));
    return 0;
}

PPT_API int pptx_shape_set_text(PptDoc doc, int si, int i, const char* utf8) {
    ShapePtr s = top_shape(doc, si, i);
    if (!s) return -1;
    SlidePtr sl = slide_ptr(doc, si);
    TextBody old = s->text;
    bool old_has = s->has_text;
    edit_text_keep_style(s->text, utf8 ? utf8 : "");
    s->has_text = !s->text.empty();
    s->ed_text = true;
    sl->touched = true;
    std::string key = "text:" + std::to_string(si) + ":" + std::to_string(i);
    record_step(H(doc), key,
                restore_text(sl, i, old, old_has),
                restore_text(sl, i, s->text, s->has_text));
    return 0;
}

PPT_API int pptx_shape_set_fontsize(PptDoc doc, int si, int i, double pt) {
    ShapePtr s = top_shape(doc, si, i);
    if (!s) return -1;
    if (pt < 1 || pt > 999) { set_err("字号超出范围"); return -2; }
    SlidePtr sl = slide_ptr(doc, si);
    TextBody old = s->text;
    bool old_has = s->has_text;
    bool any = false;
    for (TextParagraph& p : s->text.paras) {
        for (TextRun& r : p.runs) { r.props.sz_set = true; r.props.sz = pt * 100.0; any = true; }
        if (p.para_default_set) { p.para_default.sz_set = true; p.para_default.sz = pt * 100.0; }
    }
    if (!any) {
        TextParagraph& p = s->text.add_para();
        TextRun& r = s->text.add_run(p);
        r.props.sz_set = true; r.props.sz = pt * 100.0;
        s->has_text = true;
    }
    s->ed_text = true;
    sl->touched = true;
    std::string key = "text:" + std::to_string(si) + ":" + std::to_string(i);
    record_step(H(doc), key,
                restore_text(sl, i, old, old_has),
                restore_text(sl, i, s->text, s->has_text));
    return 0;
}

PPT_API int pptx_shape_set_fill(PptDoc doc, int si, int i, unsigned int rrggbb) {
    ShapePtr s = top_shape(doc, si, i);
    if (!s) return -1;
    SlidePtr sl = slide_ptr(doc, si);
    Fill old = s->fill;
    s->fill.kind = FillKind::Solid;
    s->fill.solid = ARGB(255, (uint8_t)((rrggbb >> 16) & 0xFF),
                              (uint8_t)((rrggbb >> 8) & 0xFF),
                              (uint8_t)(rrggbb & 0xFF));
    s->fill.solid_scheme.clear();
    s->ed_fill = true;
    sl->touched = true;
    std::string key = "fill:" + std::to_string(si) + ":" + std::to_string(i);
    record_step(H(doc), key,
                restore_fill(sl, i, old),
                restore_fill(sl, i, s->fill));
    return 0;
}

// 文字样式统一入口（颜色/对齐/加粗都改 txBody）
static int edit_text_style(PptDoc doc, int si, int i, const std::string& tag,
                           void (*fn)(Shape&, int), int arg) {
    ShapePtr s = top_shape(doc, si, i);
    if (!s) return -1;
    SlidePtr sl = slide_ptr(doc, si);
    TextBody old = s->text;
    bool old_has = s->has_text;
    fn(*s, arg);
    s->ed_text = true;
    sl->touched = true;
    std::string key = "text:" + std::to_string(si) + ":" + std::to_string(i);
    record_step(H(doc), key,
                restore_text(sl, i, old, old_has),
                restore_text(sl, i, s->text, s->has_text));
    return 0;
}

static void apply_textcolor(Shape& s, int rgb) {
    for (TextParagraph& p : s.text.paras) {
        for (TextRun& r : p.runs) {
            r.props.color_set = true;
            r.props.color = ARGB(255, (uint8_t)((rgb >> 16) & 0xFF),
                                      (uint8_t)((rgb >> 8) & 0xFF),
                                      (uint8_t)(rgb & 0xFF));
            r.props.color_scheme.clear();
        }
    }
    s.text.ensure_first();
}

static void apply_align(Shape& s, int a) {
    s.text.ensure_first();
    for (TextParagraph& p : s.text.paras) {
        p.props.algn_set = true;
        p.props.algn = a == 1 ? AlignL::Center : (a == 2 ? AlignL::Right : AlignL::Left);
    }
}

static void apply_bold(Shape& s, int b) {
    for (TextParagraph& p : s.text.paras)
        for (TextRun& r : p.runs) { r.props.b_set = true; r.props.b = (b != 0); }
    s.text.ensure_first();
}

PPT_API int pptx_shape_set_textcolor(PptDoc doc, int si, int i, unsigned int rrggbb) {
    return edit_text_style(doc, si, i, "color", apply_textcolor, (int)rrggbb);
}
PPT_API int pptx_shape_set_align(PptDoc doc, int si, int i, int align) {
    return edit_text_style(doc, si, i, "align", apply_align, align);
}
PPT_API int pptx_shape_set_bold(PptDoc doc, int si, int i, int bold) {
    return edit_text_style(doc, si, i, "bold", apply_bold, bold);
}

static void apply_italic(Shape& s, int v) {
    for (TextParagraph& p : s.text.paras)
        for (TextRun& r : p.runs) { r.props.i_set = true; r.props.i = (v != 0); }
    s.text.ensure_first();
}

static void apply_underline(Shape& s, int v) {
    for (TextParagraph& p : s.text.paras)
        for (TextRun& r : p.runs) { r.props.u_set = true; r.props.u = v ? "sng" : "none"; }
    s.text.ensure_first();
}

PPT_API int pptx_shape_set_italic(PptDoc doc, int si, int i, int on) {
    return edit_text_style(doc, si, i, "italic", apply_italic, on);
}

PPT_API int pptx_shape_set_underline(PptDoc doc, int si, int i, int on) {
    return edit_text_style(doc, si, i, "underline", apply_underline, on);
}

PPT_API int pptx_shape_set_fontname(PptDoc doc, int si, int i, const char* utf8_name) {
    ShapePtr s = top_shape(doc, si, i);
    if (!s) return -1;
    if (!utf8_name || !*utf8_name) { set_err("字体名为空"); return -2; }
    SlidePtr sl = slide_ptr(doc, si);
    TextBody old = s->text;
    bool old_has = s->has_text;
    for (TextParagraph& p : s->text.paras)
        for (TextRun& r : p.runs) { r.props.latin = utf8_name; r.props.ea = utf8_name; }
    s->text.ensure_first();
    for (TextParagraph& p : s->text.paras)
        for (TextRun& r : p.runs) { r.props.latin = utf8_name; r.props.ea = utf8_name; }
    s->has_text = true;
    s->ed_text = true;
    sl->touched = true;
    std::string key = "text:" + std::to_string(si) + ":" + std::to_string(i);
    record_step(H(doc), key,
                restore_text(sl, i, old, old_has),
                restore_text(sl, i, s->text, s->has_text));
    return 0;
}

PPT_API int pptx_shape_text_italic(PptDoc doc, int si, int i) {
    Slide* sl = get_slide(doc, si);
    if (!sl || i < 0 || i >= (int)sl->shapes.size()) return -1;
    ShapePtr s = sl->shapes[i];
    for (const TextParagraph& p : s->text.paras)
        for (const TextRun& r : p.runs)
            if (r.props.i_set) return r.props.i ? 1 : 0;
    return 0;
}

PPT_API int pptx_shape_text_underline(PptDoc doc, int si, int i) {
    Slide* sl = get_slide(doc, si);
    if (!sl || i < 0 || i >= (int)sl->shapes.size()) return -1;
    ShapePtr s = sl->shapes[i];
    for (const TextParagraph& p : s->text.paras)
        for (const TextRun& r : p.runs)
            if (r.props.u_set) return (r.props.u != "none" && !r.props.u.empty()) ? 1 : 0;
    return 0;
}

static std::function<void()> restore_line(SlidePtr sl, int i, const Line& ln) {
    std::shared_ptr<Line> copy = std::make_shared<Line>(ln);
    return [sl, i, copy]() {
        if (i < 0 || i >= (int)sl->shapes.size()) return;
        ShapePtr t = sl->shapes[i];
        t->line = *copy;
        t->ed_line = true;
        sl->touched = true;
    };
}

PPT_API int pptx_shape_set_line(PptDoc doc, int si, int i, unsigned int rrggbb, double width_pt) {
    ShapePtr s = top_shape(doc, si, i);
    if (!s) return -1;
    SlidePtr sl = slide_ptr(doc, si);
    Line old = s->line;
    if (rrggbb == 0) {
        s->line.inherit = false;
        s->line.explicit_none = true;
        s->line.color = ARGB(0, 0, 0, 0);
    } else {
        s->line.inherit = false;
        s->line.explicit_none = false;
        s->line.color = ARGB(255, (uint8_t)((rrggbb >> 16) & 0xFF),
                                  (uint8_t)((rrggbb >> 8) & 0xFF),
                                  (uint8_t)(rrggbb & 0xFF));
        if (width_pt > 0) s->line.w = width_pt * 12700.0;
    }
    s->ed_line = true;
    sl->touched = true;
    std::string key = "line:" + std::to_string(si) + ":" + std::to_string(i);
    record_step(H(doc), key,
                restore_line(sl, i, old),
                restore_line(sl, i, s->line));
    return 0;
}

PPT_API unsigned int pptx_shape_line(PptDoc doc, int si, int i) {
    Slide* sl = get_slide(doc, si);
    if (!sl || i < 0 || i >= (int)sl->shapes.size()) return 0;
    ShapePtr s = sl->shapes[i];
    if (s->line.explicit_none || s->line.color.a == 0) return 0;
    return (((unsigned)s->line.color.r) << 16) | (((unsigned)s->line.color.g) << 8) | s->line.color.b;
}

PPT_API int pptx_shape_text_color(PptDoc doc, int si, int i) {
    Slide* sl = get_slide(doc, si);
    if (!sl || i < 0 || i >= (int)sl->shapes.size()) return -1;
    ShapePtr s = sl->shapes[i];
    Presentation& p = H(doc)->pres;
    RenderTheme rt = make_render_theme(p, *sl);
    for (const TextParagraph& para : s->text.paras) {
        if (para.runs.empty()) continue;
        ResolvedRun rr = rt.resolve_run(*s, para, para.runs[0]);
        if (rr.color.a == 0) return -1;
        return (int)((rr.color.r << 16) | (rr.color.g << 8) | rr.color.b);
    }
    return -1;
}

PPT_API int pptx_shape_text_align(PptDoc doc, int si, int i) {
    Slide* sl = get_slide(doc, si);
    if (!sl || i < 0 || i >= (int)sl->shapes.size()) return -1;
    ShapePtr s = sl->shapes[i];
    for (const TextParagraph& p : s->text.paras)
        if (p.props.algn_set)
            return p.props.algn == AlignL::Center ? 1 : (p.props.algn == AlignL::Right ? 2 : 0);
    return 0;
}

PPT_API int pptx_shape_text_bold(PptDoc doc, int si, int i) {
    Slide* sl = get_slide(doc, si);
    if (!sl || i < 0 || i >= (int)sl->shapes.size()) return -1;
    ShapePtr s = sl->shapes[i];
    for (const TextParagraph& p : s->text.paras)
        for (const TextRun& r : p.runs)
            if (r.props.b_set) return r.props.b ? 1 : 0;
    return 0;
}

PPT_API int pptx_shape_delete(PptDoc doc, int si, int i) {
    Slide* sl = get_slide(doc, si, true);
    if (!sl) return -1;
    if (i < 0 || i >= (int)sl->shapes.size()) { set_err("形状索引越界"); return -2; }
    SlidePtr sp = slide_ptr(doc, si);
    ShapePtr victim = sl->shapes[i];
    sl->shapes.erase(sl->shapes.begin() + i);
    sl->touched = true;
    std::string key = "delshape:" + std::to_string(si) + ":" + std::to_string(i) + ":" + std::to_string(victim->id);
    record_step(H(doc), key,
        [sp, i, victim]() {
            if (i <= (int)sp->shapes.size()) sp->shapes.insert(sp->shapes.begin() + i, victim);
            sp->touched = true;
        },
        [sp, i]() {
            if (i >= 0 && i < (int)sp->shapes.size()) sp->shapes.erase(sp->shapes.begin() + i);
            sp->touched = true;
        }, false);
    return 0;
}

PPT_API int pptx_shape_duplicate(PptDoc doc, int si, int i) {
    Slide* sl = get_slide(doc, si, true);
    if (!sl) return -1;
    if (i < 0 || i >= (int)sl->shapes.size()) { set_err("形状索引越界"); return -2; }
    SlidePtr sp = slide_ptr(doc, si);
    ShapePtr copy = clone_shape(sl->shapes[i], (long long)sl->next_shape_id());
    copy->x += 200000; copy->y += 200000;
    int idx = (int)sl->shapes.size();
    sl->shapes.push_back(copy);
    sl->touched = true;
    std::string key = "addshape:" + std::to_string(si) + ":" + std::to_string(idx);
    record_step(H(doc), key,
        [sp, idx]() {
            if (idx >= 0 && idx < (int)sp->shapes.size()) sp->shapes.erase(sp->shapes.begin() + idx);
            sp->touched = true;
        },
        [sp, idx, copy]() {
            if (idx <= (int)sp->shapes.size()) sp->shapes.insert(sp->shapes.begin() + idx, copy);
            sp->touched = true;
        }, false);
    return idx;
}

PPT_API int pptx_shape_zorder(PptDoc doc, int si, int i, int dir) {
    Slide* sl = get_slide(doc, si, true);
    if (!sl) return -1;
    if (i < 0 || i >= (int)sl->shapes.size()) { set_err("形状索引越界"); return -2; }
    SlidePtr sp = slide_ptr(doc, si);
    int n = (int)sl->shapes.size();
    int to = i;
    if (dir == 1) to = std::min(n - 1, i + 1);
    else if (dir == -1) to = std::max(0, i - 1);
    else if (dir == 2) to = n - 1;
    else if (dir == -2) to = 0;
    if (to == i) return 0;
    ShapePtr s = sl->shapes[i];
    sl->shapes.erase(sl->shapes.begin() + i);
    sl->shapes.insert(sl->shapes.begin() + to, s);
    sl->touched = true;
    auto move_to = [sp, s](int from, int dst) {
        for (size_t k = 0; k < sp->shapes.size(); ++k)
            if (sp->shapes[k] == s) from = (int)k, sp->shapes.erase(sp->shapes.begin() + k), k--;
        sp->shapes.insert(sp->shapes.begin() + dst, s);
        sp->touched = true;
    };
    std::string key = "zorder:" + std::to_string(si);
    record_step(H(doc), key,
        [move_to, i, to]() { move_to(to, i); },
        [move_to, i, to]() { move_to(i, to); });
    return to;
}

PPT_API int pptx_shape_add_rect(PptDoc doc, int si, long long x, long long y,
                                long long cx, long long cy, const char* utf8_text) {
    Slide* sl = get_slide(doc, si, true);
    if (!sl) return -1;
    SlidePtr sp = slide_ptr(doc, si);
    ShapePtr s = std::make_shared<Shape>();
    s->id = (long long)sl->next_shape_id();
    s->name = "文本框";
    s->kind = ShapeKind::Rect;
    s->prst_geom = "rect";
    s->x = x; s->y = y;
    s->cx = (cx > 0 ? cx : 2000000);
    s->cy = (cy > 0 ? cy : 600000);
    s->fill.kind = FillKind::NoFill;
    s->line.explicit_none = true;
    std::string t = utf8_text ? utf8_text : "双击编辑文字";
    TextParagraph& p = s->text.add_para();
    TextRun& r = s->text.add_run(p);
    r.text = t;
    r.props.sz_set = true; r.props.sz = 1800;
    r.props.color_set = true;
    r.props.color = ARGB(255, 0x22, 0x22, 0x22);
    s->has_text = true;
    sl->shapes.push_back(s);
    sl->touched = true;
    int idx = (int)sl->shapes.size() - 1;
    std::string key = "addshape:" + std::to_string(si) + ":" + std::to_string(idx);
    record_step(H(doc), key,
        [sp, idx]() {
            if (idx >= 0 && idx < (int)sp->shapes.size()) sp->shapes.erase(sp->shapes.begin() + idx);
            sp->touched = true;
        },
        [sp, idx, s]() {
            if (idx <= (int)sp->shapes.size()) sp->shapes.insert(sp->shapes.begin() + idx, s);
            sp->touched = true;
        }, false);
    return idx;
}

PPT_API int pptx_shape_add_shape(PptDoc doc, int si, const char* prst,
                                 long long x, long long y, long long cx, long long cy,
                                 unsigned int fill_rgb, unsigned int line_rgb) {
    Slide* sl = get_slide(doc, si, true);
    if (!sl) return -1;
    SlidePtr sp = slide_ptr(doc, si);
    std::string geom = (prst && *prst) ? prst : "rect";
    ShapePtr s = std::make_shared<Shape>();
    s->id = (long long)sl->next_shape_id();
    s->prst_geom = geom;
    s->name = geom;
    s->kind = ShapeKind::Rect;
    if (geom == "line" || geom == "straightConnector1" || geom == "bentConnector3" ||
        geom == "bentConnector2" || geom == "curvedConnector3")
        s->kind = ShapeKind::Connector;
    else if (geom == "ellipse") s->kind = ShapeKind::Ellipse;
    else if (geom == "roundRect") s->kind = ShapeKind::RoundedRect;
    else if (geom == "triangle") s->kind = ShapeKind::Triangle;
    else if (geom == "hexagon") s->kind = ShapeKind::Hexagon;
    else if (geom == "rightArrow" || geom == "leftArrow" || geom == "upArrow" || geom == "downArrow")
        s->kind = ShapeKind::Arrow;
    else if (geom.rfind("star", 0) == 0) s->kind = ShapeKind::Star;

    s->x = x; s->y = y;
    s->cx = (cx > 0 ? cx : 1600000);
    s->cy = (cy > 0 ? cy : 1000000);
    if (fill_rgb == 0) {
        s->fill.kind = FillKind::NoFill;
    } else {
        s->fill.kind = FillKind::Solid;
        s->fill.solid = ARGB(255, (uint8_t)((fill_rgb >> 16) & 0xFF),
                                  (uint8_t)((fill_rgb >> 8) & 0xFF),
                                  (uint8_t)(fill_rgb & 0xFF));
    }
    s->line.inherit = false;
    if (line_rgb == 0) {
        s->line.explicit_none = true;
        s->line.color = ARGB(0, 0, 0, 0);
    } else {
        s->line.color = ARGB(255, (uint8_t)((line_rgb >> 16) & 0xFF),
                                  (uint8_t)((line_rgb >> 8) & 0xFF),
                                  (uint8_t)(line_rgb & 0xFF));
        s->line.w = 12700;      // 1pt
    }
    sl->shapes.push_back(s);
    sl->touched = true;
    int idx = (int)sl->shapes.size() - 1;
    std::string key = "addshape:" + std::to_string(si) + ":" + std::to_string(idx);
    record_step(H(doc), key,
        [sp, idx]() {
            if (idx >= 0 && idx < (int)sp->shapes.size()) sp->shapes.erase(sp->shapes.begin() + idx);
            sp->touched = true;
        },
        [sp, idx, s]() {
            if (idx <= (int)sp->shapes.size()) sp->shapes.insert(sp->shapes.begin() + idx, s);
            sp->touched = true;
        }, false);
    return idx;
}

PPT_API int pptx_shape_add_table(PptDoc doc, int si, long long x, long long y,
                                 long long cx, long long cy, int rows, int cols) {
    Slide* sl = get_slide(doc, si, true);
    if (!sl) return -1;
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;
    if (rows > 60) rows = 60;
    if (cols > 30) cols = 30;
    SlidePtr sp = slide_ptr(doc, si);

    ShapePtr s = std::make_shared<Shape>();
    s->id = (long long)sl->next_shape_id();
    s->kind = ShapeKind::Table;
    s->name = "表格";
    s->prst_geom = "rect";
    s->x = x; s->y = y;
    s->cx = (cx > 0 ? cx : (long long)cols * 1200000);
    s->cy = (cy > 0 ? cy : (long long)rows * 400000);

    auto tb = std::make_shared<Table>();
    tb->first_row = false;          // 默认不套主题蓝色表头，做成 Excel 那种白底细网格
    tb->band_row = false;
    long long cwd = s->cx / cols;
    long long chd = s->cy / rows;
    for (int c = 0; c < cols; ++c) tb->cols.push_back(cwd);
    for (int r = 0; r < rows; ++r) tb->rows.push_back(chd);
    tb->resize((size_t)rows, (size_t)cols);
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            TableCell& cell = tb->cells[r][c];
            for (int b = 0; b < 4; ++b) {
                cell.has_border[b] = true;
                cell.border_color[b] = ARGB(255, 0xC8, 0xC8, 0xC8);
                cell.border_w[b] = 6350;      // 0.5pt 细网格，像 Excel
            }
            if (r == 0) {                     // 表头：浅灰底
                cell.fill.kind = FillKind::Solid;
                cell.fill.solid = ARGB(255, 0xEF, 0xF2, 0xF5);
                cell.fill.solid_scheme.clear();
            }
            TextParagraph& p = cell.text.add_para();
            TextRun& run = cell.text.add_run(p);
            run.props.sz_set = true; run.props.sz = 1800;
            run.props.color_set = true;
            run.props.color = ARGB(255, 0x22, 0x22, 0x22);
            if (r == 0) { run.props.b_set = true; run.props.b = true; }
        }
    }
    s->table = tb;

    // 直接生成 graphicFrame 原始 XML（写回时原样保留）
    std::string xml = "<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id=\"" +
                      std::to_string(s->id) + "\" name=\"表格\"/><p:cNvGraphicFramePr>"
                      "<a:graphicFrameLocks noGrp=\"1\"/></p:cNvGraphicFramePr><p:nvPr/>"
                      "</p:nvGraphicFramePr><p:xfrm><a:off x=\"" + std::to_string(x) +
                      "\" y=\"" + std::to_string(y) + "\"/><a:ext cx=\"" + std::to_string(s->cx) +
                      "\" cy=\"" + std::to_string(s->cy) + "\"/></p:xfrm><a:graphic>"
                      "<a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/table\">"
                      "<a:tbl><a:tblPr firstRow=\"0\" bandRow=\"0\"/><a:tblGrid>";
    for (int c = 0; c < cols; ++c) xml += "<a:gridCol w=\"" + std::to_string(cwd) + "\"/>";
    xml += "</a:tblGrid>";
    for (int r = 0; r < rows; ++r) {
        xml += "<a:tr h=\"" + std::to_string(chd) + "\">";
        for (int c = 0; c < cols; ++c) {
            std::string txt = "";
            std::string clr = "222222";
            std::string fill = (r == 0) ? "<a:solidFill><a:srgbClr val=\"EFF2F5\"/></a:solidFill>" : "";
            std::string bold = (r == 0) ? " b=\"1\"" : "";
            xml += "<a:tc><a:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:rPr lang=\"zh-CN\" sz=\"1400\"" + bold + ">"
                   "<a:solidFill><a:srgbClr val=\"" + clr + "\"/></a:solidFill></a:rPr><a:t>" + txt +
                   "</a:t></a:r></a:p></a:txBody><a:tcPr marL=\"91440\" marR=\"91440\" marT=\"45720\" marB=\"45720\">" +
                   fill;
            for (const char* ln : {"a:lnL", "a:lnR", "a:lnT", "a:lnB"})
                xml += std::string("<") + ln + " w=\"6350\"><a:solidFill><a:srgbClr val=\"C8C8C8\"/>"
                       "</a:solidFill></" + ln + ">";
            xml += "</a:tcPr></a:tc>";
        }
        xml += "</a:tr>";
    }
    xml += "</a:tbl></a:graphicData></a:graphic></p:graphicFrame>\n";
    s->raw_xml = xml;
    s->raw_preserve = false;

    sl->shapes.push_back(s);
    sl->touched = true;
    int idx = (int)sl->shapes.size() - 1;
    std::string key = "addshape:" + std::to_string(si) + ":" + std::to_string(idx);
    record_step(H(doc), key,
        [sp, idx]() {
            if (idx >= 0 && idx < (int)sp->shapes.size()) sp->shapes.erase(sp->shapes.begin() + idx);
            sp->touched = true;
        },
        [sp, idx, s]() {
            if (idx <= (int)sp->shapes.size()) sp->shapes.insert(sp->shapes.begin() + idx, s);
            sp->touched = true;
        }, false);
    return idx;
}

PPT_API int pptx_table_size(PptDoc doc, int si, int i, int* rows, int* cols) {
    Slide* sl = get_slide(doc, si);
    if (!sl || i < 0 || i >= (int)sl->shapes.size()) return -1;
    ShapePtr s = sl->shapes[i];
    if (!s->table) return -1;
    if (rows) *rows = (int)s->table->nrows();
    if (cols) *cols = (int)s->table->ncols();
    return 0;
}

// 取表格形状在页面 EMU 坐标系里的盒子（占位符按继承解析）
static bool table_box(PptDoc doc, int si, int i, long long& x, long long& y,
                      long long& cx, long long& cy) {
    Slide* sl = get_slide(doc, si);
    if (!sl || i < 0 || i >= (int)sl->shapes.size()) return false;
    ShapePtr s = sl->shapes[i];
    if (!s->table) return false;
    x = s->x; y = s->y; cx = s->cx; cy = s->cy;
    if (cx <= 0 || cy <= 0) {
        Presentation& p = ((DocHandle*)doc)->pres;
        RenderTheme rt = make_render_theme(p, *sl);
        if (rt.placeholder_geometry(*s, x, y, cx, cy)) return true;
        x = s->x; y = s->y; cx = s->cx; cy = s->cy;
    }
    return cx > 0 && cy > 0;
}

PPT_API int pptx_table_cell_at(PptDoc doc, int si, int i, int px, int py, int cw, int ch) {
    long long bx, by, bcx, bcy;
    if (!table_box(doc, si, i, bx, by, bcx, bcy)) return -1;
    Slide* sl = get_slide(doc, si);
    ShapePtr s = sl->shapes[i];
    const Table& t = *s->table;
    if (t.cols.empty() || t.rows.empty()) return -1;
    Presentation& p = ((DocHandle*)doc)->pres;
    if (cw <= 0) cw = 1280;
    if (ch <= 0) ch = 720;
    long long emuX = (long long)((double)px * p.slide_w / cw);
    long long emuY = (long long)((double)py * p.slide_h / ch);
    long long lx = emuX - bx, ly = emuY - by;
    if (lx < 0 || ly < 0 || lx > bcx || ly > bcy) return -1;
    long long total_c = 0, total_r = 0;
    for (long long w : t.cols) total_c += w;
    for (long long h : t.rows) total_r += h;
    // 画布按形状盒子拉伸，先把相对坐标换算回表格自身坐标系
    long long ux = total_c > 0 ? (long long)((double)lx * total_c / bcx) : lx;
    long long uy = total_r > 0 ? (long long)((double)ly * total_r / bcy) : ly;
    int col = -1, row = -1;
    long long acc = 0;
    for (size_t k = 0; k < t.cols.size(); ++k) {
        if (ux >= acc && ux < acc + t.cols[k]) { col = (int)k; break; }
        acc += t.cols[k];
    }
    acc = 0;
    for (size_t k = 0; k < t.rows.size(); ++k) {
        if (uy >= acc && uy < acc + t.rows[k]) { row = (int)k; break; }
        acc += t.rows[k];
    }
    if (col < 0) col = (int)t.cols.size() - 1;
    if (row < 0) row = (int)t.rows.size() - 1;
    if (row < 0 || col < 0 || row >= (int)t.rows.size() || col >= (int)t.cols.size()) return -1;
    return row * (int)t.cols.size() + col;
}

PPT_API int pptx_table_cell_rect(PptDoc doc, int si, int i, int r, int c, int cw, int ch,
                                 int* out_x, int* out_y, int* out_w, int* out_h) {
    if (!out_x || !out_y || !out_w || !out_h) return -1;
    long long bx, by, bcx, bcy;
    if (!table_box(doc, si, i, bx, by, bcx, bcy)) return -1;
    Slide* sl = get_slide(doc, si);
    if (!sl || i < 0 || i >= (int)sl->shapes.size()) return -1;
    ShapePtr s = sl->shapes[i];
    if (!s->table) return -1;
    const Table& t = *s->table;
    if (t.cols.empty() || t.rows.empty()) return -1;
    if (r < 0 || c < 0 || r >= (int)t.rows.size() || c >= (int)t.cols.size()) return -1;
    Presentation& p = ((DocHandle*)doc)->pres;
    if (cw <= 0) cw = 1280;
    if (ch <= 0) ch = 720;
    long long total_c = 0, total_r = 0;
    for (long long w : t.cols) total_c += w;
    for (long long h : t.rows) total_r += h;
    if (total_c <= 0 || total_r <= 0) return -1;
    long long off = 0;
    for (int k = 0; k < c; ++k) off += t.cols[k];
    double sx = (double)bcx / (double)total_c;
    double sy = (double)bcy / (double)total_r;
    double x0 = (double)bx + (double)off * sx;
    double y0 = (double)by;
    for (int k = 0; k < r; ++k) y0 += (double)t.rows[k] * sy;
    double cw_emu = (double)t.cols[c] * sx;
    double ch_emu = (double)t.rows[r] * sy;
    *out_x = (int)(x0 * cw / (double)p.slide_w);
    *out_y = (int)(y0 * ch / (double)p.slide_h);
    *out_w = (int)(cw_emu * cw / (double)p.slide_w);
    *out_h = (int)(ch_emu * ch / (double)p.slide_h);
    return 0;
}

// ---- 表格行列增删（从模型重建，保存时整体写回）----
static int table_snapshot_edit(PptDoc doc, int si, int i,
                               const std::function<void(Table&)>& mut,
                               const std::string& key) {
    ShapePtr s = top_shape(doc, si, i);
    if (!s || !s->table) { set_err("该形状不是表格"); return -1; }
    SlidePtr sp = slide_ptr(doc, si);
    auto old_tbl = std::make_shared<Table>(*s->table);
    Table nw = *s->table;
    mut(nw);
    s->table = std::make_shared<Table>(nw);
    sp->touched = true;
    auto new_tbl = s->table;
    record_step(H(doc), key,
        [sp, s, old_tbl]() { s->table = old_tbl; sp->touched = true; },
        [sp, s, new_tbl]() { s->table = new_tbl; sp->touched = true; }, false);
    return 0;
}

PPT_API int pptx_table_row_add(PptDoc doc, int si, int i, int at) {
    return table_snapshot_edit(doc, si, i, [at](Table& t) mutable {
        int rows = (int)t.cells.size();
        if (at < 0 || at > rows) at = rows;
        std::vector<TableCell> row;
        if (rows > 0) row = t.cells[rows - 1];
        row.resize(t.cols.size());
        for (auto& c : row) { c.text = TextBody(); c.text.add_para(); }
        t.cells.insert(t.cells.begin() + at, row);
        if (!t.rows.empty()) {
            int h = t.rows.back();
            t.rows.insert(t.rows.begin() + std::min(at, (int)t.rows.size()), h);
        } else {
            t.rows.push_back(200000);
        }
    }, "table:" + std::to_string(si) + ":" + std::to_string(i));
}

PPT_API int pptx_table_row_del(PptDoc doc, int si, int i, int r) {
    return table_snapshot_edit(doc, si, i, [r](Table& t) {
        if (r < 0 || r >= (int)t.cells.size() || t.cells.size() <= 1) return;
        t.cells.erase(t.cells.begin() + r);
        if (r < (int)t.rows.size() && t.rows.size() > 1) t.rows.erase(t.rows.begin() + r);
    }, "table:" + std::to_string(si) + ":" + std::to_string(i));
}

PPT_API int pptx_table_col_add(PptDoc doc, int si, int i, int at) {
    return table_snapshot_edit(doc, si, i, [at](Table& t) mutable {
        int cols = (int)t.cols.size();
        if (at < 0 || at > cols) at = cols;
        for (auto& row : t.cells) {
            TableCell c;
            if (!row.empty()) {
                int src = at >= (int)row.size() ? (int)row.size() - 1 : std::max(0, at - 1);
                c = row[src];
            }
            c.text = TextBody(); c.text.add_para();
            row.insert(row.begin() + std::min<int>(at, (int)row.size()), c);
        }
        int w = t.cols.empty() ? 200000 : t.cols.back();
        t.cols.insert(t.cols.begin() + std::min(at, (int)t.cols.size()), w);
    }, "table:" + std::to_string(si) + ":" + std::to_string(i));
}

PPT_API int pptx_table_col_del(PptDoc doc, int si, int i, int c) {
    return table_snapshot_edit(doc, si, i, [c](Table& t) {
        if (c < 0 || c >= (int)t.cols.size() || t.cols.size() <= 1) return;
        for (auto& row : t.cells)
            if (c < (int)row.size()) row.erase(row.begin() + c);
        t.cols.erase(t.cols.begin() + c);
    }, "table:" + std::to_string(si) + ":" + std::to_string(i));
}

PPT_API int pptx_table_cell_get_text(PptDoc doc, int si, int i, int r, int c, char* out, int cap) {
    if (!out || cap <= 0) return -1;
    out[0] = 0;
    Slide* sl = get_slide(doc, si);
    if (!sl || i < 0 || i >= (int)sl->shapes.size()) return -1;
    ShapePtr s = sl->shapes[i];
    if (!s->table) return -1;
    if (r < 0 || c < 0 || r >= (int)s->table->cells.size() ||
        c >= (int)s->table->cells[r].size()) return -1;
    copy_utf8(out, (size_t)cap, s->table->cells[r][c].text.all_text());
    return 0;
}

PPT_API int pptx_table_cell_set_text(PptDoc doc, int si, int i, int r, int c, const char* utf8_text) {
    if (!utf8_text) return -1;
    ShapePtr s = top_shape(doc, si, i);
    if (!s || !s->table) return -1;
    if (r < 0 || c < 0 || r >= (int)s->table->cells.size() ||
        c >= (int)s->table->cells[r].size()) { set_err("单元格越界"); return -2; }
    SlidePtr sl = slide_ptr(doc, si);
    TableCell& cell = s->table->cells[r][c];
    TextBody old = cell.text;

    TextBody nb = cell.text;
    if (nb.paras.empty()) nb.add_para();
    TextParagraph& p = nb.paras[0];
    p.runs.resize(1);
    p.runs[0].text = utf8_text;
    nb.paras.resize(1);
    cell.text = nb;
    s->ed_table = true;
    sl->touched = true;

    std::string key = "table:" + std::to_string(si) + ":" + std::to_string(i) + ":" +
                      std::to_string(r) + ":" + std::to_string(c);
    auto restore_cell = [sl, i, r, c, old]() {
        if (i < 0 || i >= (int)sl->shapes.size()) return;
        ShapePtr t = sl->shapes[i];
        if (!t->table || r >= (int)t->table->cells.size() || c >= (int)t->table->cells[r].size()) return;
        t->table->cells[r][c].text = old;
        t->ed_table = true;
        sl->touched = true;
    };
    auto apply_new = [sl, i, r, c, nb]() {
        if (i < 0 || i >= (int)sl->shapes.size()) return;
        ShapePtr t = sl->shapes[i];
        if (!t->table || r >= (int)t->table->cells.size() || c >= (int)t->table->cells[r].size()) return;
        t->table->cells[r][c].text = nb;
        t->ed_table = true;
        sl->touched = true;
    };
    record_step(H(doc), key, restore_cell, apply_new);
    return 0;
}

PPT_API int pptx_shape_add_picture(PptDoc doc, int si, const char* utf8_path,                                   long long x, long long y, long long cx, long long cy) {
    if (!doc || !utf8_path) return -1;
    Slide* sl = get_slide(doc, si, true);
    if (!sl) return -1;
    if (!ensure_gdi()) return -2;

    // 读取图片字节
    std::vector<uint8_t> bytes;
    {
        FILE* f = fopen(utf8_path, "rb");
        if (!f) { set_err("图片打开失败"); return -3; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0) {
            bytes.resize((size_t)sz);
            size_t rd = fread(bytes.data(), 1, (size_t)sz, f);
            bytes.resize(rd);
        }
        fclose(f);
    }
    if (bytes.empty()) { set_err("图片为空"); return -4; }

    // 用 GDI+ 取原图尺寸，按比例定高宽
    int pw = 0, ph = 0;
    {
        IStream* st = nullptr;
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
        if (hg) {
            void* pv = GlobalLock(hg);
            memcpy(pv, bytes.data(), bytes.size());
            GlobalUnlock(hg);
            if (CreateStreamOnHGlobal(hg, TRUE, &st) == S_OK) {
                Gdiplus::Image img(st);
                if (img.GetLastStatus() == Gdiplus::Ok) { pw = (int)img.GetWidth(); ph = (int)img.GetHeight(); }
                st->Release();
            }
        }
    }
    std::string ext = "png";
    {
        std::string p = utf8_path;
        size_t dot = p.rfind('.');
        if (dot != std::string::npos) {
            std::string e = p.substr(dot + 1);
            for (char& c : e) c = (char)tolower(c);
            if (e == "jpg" || e == "jpeg") ext = "jpg";
            else if (e == "gif") ext = "gif";
            else if (e == "bmp") ext = "bmp";
        }
    }

    Presentation& pres = H(doc)->pres;
    std::string part;
    pres.add_media(ext, bytes, part);

    double ratio = (pw > 0 && ph > 0) ? (double)ph / pw : 0.5625;
    if (cx <= 0) cx = 4000000;
    if (cy <= 0) cy = (long long)(cx * ratio);

    ShapePtr s = std::make_shared<Shape>();
    s->id = (long long)sl->next_shape_id();
    s->name = "图片";
    s->kind = ShapeKind::Picture;
    s->x = x; s->y = y; s->cx = cx; s->cy = cy;
    s->pic_part = part;
    s->pic_ext = ext;
    // 建立 幻灯片 -> 图片 的关系，并生成该形状的原始 XML（回写时原样输出）
    std::string target = part;
    if (starts_with(target, "ppt/")) target = target.substr(4);
    std::string rid = pres.add_rel(sl->part, rel::IMAGE, "../" + target, false);
    {
        char b[64];
        std::string raw = "<p:pic><p:nvPicPr><p:cNvPr id=\"" + std::to_string(s->id) +
                          "\" name=\"图片\"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr>"
                          "<p:blipFill><a:blip r:embed=\"" + rid + "\"/><a:stretch><a:fillRect/></a:stretch></p:blipFill>"
                          "<p:spPr><a:xfrm><a:off x=\"" + std::to_string(x) + "\" y=\"" + std::to_string(y) +
                          "\"/><a:ext cx=\"" + std::to_string(cx) + "\" cy=\"" + std::to_string(cy) +
                          "\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>";
        (void)b;
        s->raw_xml = raw;
        s->raw_preserve = true;
    }
    sl->shapes.push_back(s);
    sl->touched = true;
    SlidePtr sp = H(doc)->pres.slides[si];
    int idx = (int)sl->shapes.size() - 1;
    std::string key = "addshape:" + std::to_string(si) + ":" + std::to_string(idx);
    record_step(H(doc), key,
        [sp, idx]() {
            if (idx >= 0 && idx < (int)sp->shapes.size()) sp->shapes.erase(sp->shapes.begin() + idx);
            sp->touched = true;
        },
        [sp, idx, s]() {
            if (idx <= (int)sp->shapes.size()) sp->shapes.insert(sp->shapes.begin() + idx, s);
            sp->touched = true;
        }, false);
    return idx;
}

// ---------------- 撤销 / 重做 ----------------
PPT_API int pptx_begin_edit(PptDoc doc) {
    DocHandle* h = H(doc);
    if (!h) return -1;
    h->txn.clear();
    h->txn_open = true;
    return 0;
}

PPT_API int pptx_end_edit(PptDoc doc) {
    DocHandle* h = H(doc);
    if (!h) return -1;
    h->txn_open = false;
    if (!h->txn.empty()) {
        // 多个不同字段的修改作为一个整体
        std::vector<UndoPair> merged;
        for (UndoPair& p : h->txn) merged.push_back(p);
        h->undo_stack.push_back(UndoPair{"txn", [merged]() {
            for (size_t k = merged.size(); k-- > 0;) merged[k].undo();
        }, [merged]() {
            for (size_t k = 0; k < merged.size(); ++k) merged[k].redo();
        }});
        if (h->undo_stack.size() > 80) h->undo_stack.erase(h->undo_stack.begin());
        h->redo_stack.clear();
        h->txn.clear();
    }
    return 0;
}

PPT_API int pptx_undo(PptDoc doc) {
    DocHandle* h = H(doc);
    if (!h || h->undo_stack.empty()) return -1;
    UndoPair p = h->undo_stack.back();
    h->undo_stack.pop_back();
    p.undo();
    h->redo_stack.push_back(p);
    h->pres.dirty = true;
    return 0;
}

PPT_API int pptx_redo(PptDoc doc) {
    DocHandle* h = H(doc);
    if (!h || h->redo_stack.empty()) return -1;
    UndoPair p = h->redo_stack.back();
    h->redo_stack.pop_back();
    p.redo();
    h->undo_stack.push_back(p);
    h->pres.dirty = true;
    return 0;
}

PPT_API int pptx_can_undo(PptDoc doc) { return (H(doc) && !H(doc)->undo_stack.empty()) ? 1 : 0; }
PPT_API int pptx_can_redo(PptDoc doc) { return (H(doc) && !H(doc)->redo_stack.empty()) ? 1 : 0; }


// ---------------- 幻灯片级 ----------------
PPT_API int pptx_slide_add(PptDoc doc, int after_idx) {
    if (!doc) { set_err("文档句柄为空"); return -1; }
    DocHandle* h = H(doc);
    Presentation& p = h->pres;
    SlidePtr after = nullptr;
    if (after_idx >= 0 && after_idx < (int)p.slides.size()) after = p.slides[after_idx];
    SlidePtr s = p.add_slide(after);
    p.dirty = true;
    int idx = (int)p.index_of(s);
    record_step(h, "slides", [h]() { /* 占位，由下面覆盖 */ }, [h]() {});
    // 用精确的增删动作替换刚记录的那一步
    h->undo_stack.back() = UndoPair{"slides",
        [h, s]() { for (size_t k = 0; k < h->pres.slides.size(); ++k)
                       if (h->pres.slides[k] == s) { h->pres.slides.erase(h->pres.slides.begin() + k); break; } },
        [h, s, idx]() {
            int at = idx < 0 ? 0 : (idx > (int)h->pres.slides.size() ? (int)h->pres.slides.size() : idx);
            h->pres.slides.insert(h->pres.slides.begin() + at, s);
        }};
    return idx;
}

PPT_API int pptx_slide_duplicate(PptDoc doc, int idx) {
    if (!doc) { set_err("文档句柄为空"); return -1; }
    DocHandle* h = H(doc);
    Presentation& p = h->pres;
    if (idx < 0 || idx >= (int)p.slides.size()) { set_err("页码越界"); return -2; }
    SlidePtr s = p.duplicate_slide((size_t)idx);
    if (!s) return -3;
    p.dirty = true;
    int ni = (int)p.index_of(s);
    record_step(h, "slides", [h, s]() {
        for (size_t k = 0; k < h->pres.slides.size(); ++k)
            if (h->pres.slides[k] == s) { h->pres.slides.erase(h->pres.slides.begin() + k); break; }
    }, [h, s, ni]() {
        int at = ni > (int)h->pres.slides.size() ? (int)h->pres.slides.size() : ni;
        h->pres.slides.insert(h->pres.slides.begin() + at, s);
    });
    return ni;
}

PPT_API int pptx_slide_remove(PptDoc doc, int idx) {
    if (!doc) { set_err("文档句柄为空"); return -1; }
    DocHandle* h = H(doc);
    Presentation& p = h->pres;
    if (p.slides.size() <= 1) { set_err("至少保留一页"); return -2; }
    if (idx < 0 || idx >= (int)p.slides.size()) { set_err("页码越界"); return -3; }
    SlidePtr victim = p.slides[(size_t)idx];
    p.remove_slide((size_t)idx);
    p.dirty = true;
    record_step(h, "slides", [h, victim, idx]() {
        int at = idx > (int)h->pres.slides.size() ? (int)h->pres.slides.size() : idx;
        h->pres.slides.insert(h->pres.slides.begin() + at, victim);
    }, [h, victim]() {
        for (size_t k = 0; k < h->pres.slides.size(); ++k)
            if (h->pres.slides[k] == victim) { h->pres.slides.erase(h->pres.slides.begin() + k); break; }
    });
    return 0;
}

PPT_API int pptx_slide_move(PptDoc doc, int from, int to) {
    if (!doc) { set_err("文档句柄为空"); return -1; }
    DocHandle* h = H(doc);
    Presentation& p = h->pres;
    if (from < 0 || from >= (int)p.slides.size() || to < 0 || to >= (int)p.slides.size()) {
        set_err("页码越界"); return -2;
    }
    SlidePtr s = p.slides[(size_t)from];
    p.move_slide((size_t)from, (size_t)to);
    p.dirty = true;
    record_step(h, "slides", [h, s, from]() {
        for (size_t k = 0; k < h->pres.slides.size(); ++k)
            if (h->pres.slides[k] == s) { h->pres.slides.erase(h->pres.slides.begin() + k); break; }
        int at = from > (int)h->pres.slides.size() ? (int)h->pres.slides.size() : from;
        h->pres.slides.insert(h->pres.slides.begin() + at, s);
    }, [h, s, to]() {
        for (size_t k = 0; k < h->pres.slides.size(); ++k)
            if (h->pres.slides[k] == s) { h->pres.slides.erase(h->pres.slides.begin() + k); break; }
        int at = to > (int)h->pres.slides.size() ? (int)h->pres.slides.size() : to;
        h->pres.slides.insert(h->pres.slides.begin() + at, s);
    });
    return 0;
}

// ---------------- 切换动画 / 入场动画 ----------------
PPT_API int pptx_slide_transition_get(PptDoc doc, int si, char* kind, int cap, int* dur_ms, int* dir) {
    if (!doc) return -1;
    Presentation& p = H(doc)->pres;
    if (si < 0 || si >= (int)p.slides.size()) return -2;
    const Transition& t = p.slides[(size_t)si]->trans;
    if (kind && cap > 0) copy_utf8(kind, cap, t.kind.empty() ? "none" : t.kind);
    if (dur_ms) *dur_ms = (int)(t.dur_ms > 0 ? t.dur_ms : 600);
    if (dir) *dir = t.dir;
    return 0;
}

// OOXML 切换速度只有 fast/med/slow 三档，统一吸附，保证存盘再打开一致
static long long snap_trans_dur(long long d) {
    if (d <= 0) return 600;
    if (d <= 450) return 300;
    if (d <= 900) return 600;
    return 1000;
}

static void set_one_transition(DocHandle* h, int si, const std::string& kind, long long dur, int dir) {
    SlidePtr sl = h->pres.slides[(size_t)si];
    sl->trans.kind = kind;
    sl->trans.dur_ms = dur;
    sl->trans.dir = dir;
    sl->touched = true;
    h->pres.dirty = true;
}

PPT_API int pptx_slide_transition_set(PptDoc doc, int si, const char* kind, int dur_ms, int dir) {
    if (!doc) return -1;
    DocHandle* h = H(doc);
    Presentation& p = h->pres;
    if (si < 0 || si >= (int)p.slides.size()) return -2;
    std::string nk = kind && *kind ? kind : "none";
    long long nd = snap_trans_dur(dur_ms);
    int odir = p.slides[(size_t)si]->trans.dir;
    std::string okind = p.slides[(size_t)si]->trans.kind;
    long long odur = p.slides[(size_t)si]->trans.dur_ms;
    set_one_transition(h, si, nk, nd, dir);
    record_step(h, "transition", [h, si, okind, odur, odir]() {
        set_one_transition(h, si, okind, odur, odir);
    }, [h, si, nk, nd, dir]() { set_one_transition(h, si, nk, nd, dir); });
    return 0;
}

PPT_API int pptx_transition_apply_all(PptDoc doc, const char* kind, int dur_ms, int dir) {
    if (!doc) return -1;
    DocHandle* h = H(doc);
    Presentation& p = h->pres;
    std::string nk = kind && *kind ? kind : "none";
    long long nd = snap_trans_dur(dur_ms);
    std::vector<Transition> old;
    for (const SlidePtr& sl : p.slides) old.push_back(sl->trans);
    for (size_t i = 0; i < p.slides.size(); ++i)
        set_one_transition(h, (int)i, nk, nd, dir);
    record_step(h, "transition-all", [h, old]() {
        for (size_t i = 0; i < h->pres.slides.size() && i < old.size(); ++i) {
            h->pres.slides[i]->trans = old[i];
            h->pres.slides[i]->touched = true;
        }
    }, [h, nk, nd, dir]() {
        for (size_t i = 0; i < h->pres.slides.size(); ++i)
            set_one_transition(h, (int)i, nk, nd, dir);
    });
    return 0;
}

PPT_API int pptx_shape_anim_get(PptDoc doc, int si, int shape_idx, char* kind, int cap, int* dur_ms) {
    if (!doc) return -1;
    Presentation& p = H(doc)->pres;
    if (si < 0 || si >= (int)p.slides.size()) return -2;
    SlidePtr sl = p.slides[(size_t)si];
    if (shape_idx < 0 || shape_idx >= (int)sl->shapes.size()) return -3;
    long long sid = sl->shapes[(size_t)shape_idx]->id;
    std::string k = "none";
    long long d = 500;
    for (const AnimNode& a : sl->trans.seq) {
        if (a.target_id == sid) { k = a.raw_prst.empty() ? "fade" : a.raw_prst; d = a.dur_ms; break; }
    }
    if (kind && cap > 0) copy_utf8(kind, cap, k);
    if (dur_ms) *dur_ms = (int)d;
    return 0;
}

PPT_API int pptx_shape_anim_set(PptDoc doc, int si, int shape_idx, const char* kind, int dur_ms) {
    if (!doc) return -1;
    DocHandle* h = H(doc);
    Presentation& p = h->pres;
    if (si < 0 || si >= (int)p.slides.size()) return -2;
    SlidePtr sl = p.slides[(size_t)si];
    if (shape_idx < 0 || shape_idx >= (int)sl->shapes.size()) return -3;
    long long sid = sl->shapes[(size_t)shape_idx]->id;
    std::string nk = kind && *kind ? kind : "none";
    long long nd = dur_ms > 0 ? dur_ms : 500;
    std::vector<AnimNode> old = sl->trans.seq;

    sl->trans.seq.erase(std::remove_if(sl->trans.seq.begin(), sl->trans.seq.end(),
        [sid](const AnimNode& a) { return a.target_id == sid; }), sl->trans.seq.end());
    if (nk != "none") {
        AnimNode a;
        a.effect = AnimEffect::Entrance;
        a.raw_prst = nk;
        a.target_id = sid;
        a.dur_ms = nd;
        a.trg = TrgKind::OnClick;
        sl->trans.seq.push_back(a);
    }
    sl->touched = true;
    p.dirty = true;
    auto apply_seq = [sl](const std::vector<AnimNode>& v) { sl->trans.seq = v; sl->touched = true; };
    std::vector<AnimNode> nseq = sl->trans.seq;
    record_step(h, "anim", [h, apply_seq, old]() { apply_seq(old); },
                [h, apply_seq, nseq]() { apply_seq(nseq); });
    return 0;
}

PPT_API int pptx_shape_anim_clear(PptDoc doc, int si) {
    if (!doc) return -1;
    DocHandle* h = H(doc);
    Presentation& p = h->pres;
    if (si < 0 || si >= (int)p.slides.size()) return -2;
    SlidePtr sl = p.slides[(size_t)si];
    std::vector<AnimNode> old = sl->trans.seq;
    sl->trans.seq.clear();
    sl->touched = true;
    p.dirty = true;
    record_step(h, "anim-clear", [h, sl, old]() { sl->trans.seq = old; sl->touched = true; },
                [h, sl]() { sl->trans.seq.clear(); sl->touched = true; });
    return 0;
}

// ---------------- 保存与导出 ----------------
PPT_API int pptx_save(PptDoc doc, const char* utf8_path) {
    if (!doc || !utf8_path || !*utf8_path) { set_err("参数非法"); return -1; }
    Presentation& p = ((DocHandle*)doc)->pres;
    std::string err;
    if (!pptx_write(utf8_path, p, err)) { set_err("保存失败: " + err); return -2; }
    p.dirty = false;
    return 0;
}

PPT_API int pptx_export_html(PptDoc doc, const char* utf8_path) {
    if (!doc || !utf8_path || !*utf8_path) { set_err("参数非法"); return -1; }
    Presentation& p = ((DocHandle*)doc)->pres;
    plugin_host_bind(&p);
    std::string err;
    HtmlOptions opt;
    opt.title = stem_name(utf8_path);
    if (!render_to_file(utf8_path, p, opt, &err)) { set_err("导出失败: " + err); return -2; }
    return 0;
}

// ---------------- 插件 ----------------
PPT_API int pptx_plugins_load(const char* utf8_dir) {
    std::string dir = (utf8_dir && *utf8_dir) ? std::string(utf8_dir) : default_plugin_dir();
    if (dir.empty()) { set_err("未找到插件目录"); return -1; }
    PluginHost::instance().unload_all();
    int n = PluginHost::instance().load_dir(dir);
    g_plugins_loaded = true;
    return n;
}

PPT_API int pptx_plugin_count() {
    return (int)PluginHost::instance().loaded().size();
}

PPT_API int pptx_plugin_info(int idx, char* name, int name_cap, char* desc, int desc_cap) {
    const auto& v = PluginHost::instance().loaded();
    if (idx < 0 || idx >= (int)v.size()) return -1;
    if (name && name_cap > 0) copy_utf8(name, name_cap, v[idx].name);
    if (desc && desc_cap > 0) copy_utf8(desc, desc_cap, v[idx].desc);
    return 0;
}

PPT_API int pptx_plugin_command(const char* cmd, const char* arg, char* out, int out_cap) {
    if (!cmd || !*cmd) { set_err("命令为空"); return -1; }
    std::string result;
    bool ok = PluginHost::instance().run_command(cmd, arg ? arg : "", result);
    if (out && out_cap > 0) copy_utf8(out, out_cap, result);
    return ok ? 1 : 0;
}

PPT_API const char* pptx_last_error() {
    return g_err.c_str();
}

} // extern "C"


