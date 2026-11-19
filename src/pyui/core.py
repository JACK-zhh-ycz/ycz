# -*- coding: utf-8 -*-
"""pptx-core.dll 的 ctypes 封装（编辑器前端唯一入口）

内核（C++）负责：解析 / 渲染 / 排版 / 保存 / 导出；前端（Python）只做交互。
所有字符串进出均为 UTF-8。
"""
import ctypes
import os
import re
import sys


def split_html_scripts(path):
    """把导出 HTML 里的内联 <script> 抽成同名 .js 文件并改成外链。

    目的：单文件 HTML 里塞大段脚本，容易被杀软按“网页挂马”启发式误报；
    拆开后 .html 只剩标签和样式，脚本是普通的本地 .js。
    返回 (html路径, [生成的 js 文件])。
    """
    path = str(path)
    with open(path, "r", encoding="utf-8") as f:
        html = f.read()
    base, _ext = os.path.splitext(path)
    pat = re.compile(r"<script(?![^>]*\bsrc=)[^>]*>(.*?)</script>", re.I | re.S)
    made = []

    def repl(m):
        code = m.group(1)
        idx = len(made) + 1
        js_path = "%s_%d.js" % (base, idx)
        with open(js_path, "w", encoding="utf-8") as f:
            f.write(code)
        made.append(js_path)
        return '<script src="%s"></script>' % os.path.basename(js_path)

    html = pat.sub(repl, html)
    with open(path, "w", encoding="utf-8") as f:
        f.write(html)
    return path, made


class ShapeInfo(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_longlong),
        ("kind", ctypes.c_int),
        ("x", ctypes.c_longlong), ("y", ctypes.c_longlong),
        ("cx", ctypes.c_longlong), ("cy", ctypes.c_longlong),
        ("rot", ctypes.c_double),
        ("name", ctypes.c_char * 128),
        ("text", ctypes.c_char * 1024),
        ("fontsize_pt", ctypes.c_double),
        ("fill_rgb", ctypes.c_uint),
        ("italic", ctypes.c_int),
        ("underline", ctypes.c_int),
        ("line_rgb", ctypes.c_uint),
        ("font_name", ctypes.c_char * 64),
    ]


EMU_PER_PT = 12700.0
EMU_PER_PX = 914400.0 / 96.0          # 96 DPI 下 1 像素 = 9525 EMU


def find_dll():
    here = os.path.dirname(os.path.abspath(__file__))
    roots = [here, os.path.join(here, "build"),
             os.path.normpath(os.path.join(here, "..", "build")),
             os.path.normpath(os.path.join(here, "..", "..", "build")),
             os.path.normpath(os.path.join(here, "..", "..", "..", "build"))]
    # PyInstaller 打包后：资源在 sys._MEIPASS
    if getattr(sys, "frozen", False):
        base = getattr(sys, "_MEIPASS", os.path.dirname(sys.executable))
        roots.insert(0, os.path.join(base, "build"))
        roots.insert(0, base)
    for r in roots:
        p = os.path.join(r, "pptx-core.dll")
        if os.path.exists(p):
            return p
    raise FileNotFoundError("找不到 pptx-core.dll，请先运行 build.ps1 构建")


class Core:
    """一层薄封装；参数都用人类单位（pt / EMU 由调用方决定）"""

    KIND = {0: "形状", 1: "图片", 2: "表格", 3: "图表", 4: "媒体", 5: "组合"}

    def __init__(self, dll_path=None):
        self.lib = ctypes.CDLL(dll_path or find_dll())
        L = self.lib
        vp, ci, cl, cd, cp = ctypes.c_void_p, ctypes.c_int, ctypes.c_longlong, ctypes.c_double, ctypes.c_char_p
        poi = ctypes.POINTER(ctypes.c_int)

        def sig(name, res, args):
            f = getattr(L, name)
            f.restype = res
            f.argtypes = args
            return f

        self._open = sig("pptx_open", vp, [cp])
        self._close = sig("pptx_close", None, [vp])
        self._slide_count = sig("pptx_slide_count", ci, [vp])
        self._slide_size = sig("pptx_slide_size", None, [vp, ctypes.POINTER(cl), ctypes.POINTER(cl)])
        self._render = sig("pptx_render_rgba", ci, [vp, ci, ci, ci, ctypes.POINTER(vp),
                                                    ctypes.POINTER(ci), ctypes.POINTER(ci)])
        self._free = sig("pptx_free_buf", None, [vp])
        self._shape_count = sig("pptx_shape_count", ci, [vp, ci])
        self._shape_get = sig("pptx_shape_get", ci, [vp, ci, ci, ctypes.POINTER(ShapeInfo)])
        self._hit = sig("pptx_hit", ci, [vp, ci, ci, ci, ci, ci])
        self._move = sig("pptx_shape_move", ci, [vp, ci, ci, cl, cl])
        self._resize = sig("pptx_shape_resize", ci, [vp, ci, ci, cl, cl])
        self._set_geom = sig("pptx_shape_set_geom", ci, [vp, ci, ci, cl, cl, cl, cl])
        self._set_rot = sig("pptx_shape_set_rot", ci, [vp, ci, ci, cd])
        self._set_text = sig("pptx_shape_set_text", ci, [vp, ci, ci, cp])
        self._set_fontsize = sig("pptx_shape_set_fontsize", ci, [vp, ci, ci, cd])
        self._set_fill = sig("pptx_shape_set_fill", ci, [vp, ci, ci, ctypes.c_uint])
        self._set_textcolor = sig("pptx_shape_set_textcolor", ci, [vp, ci, ci, ctypes.c_uint])
        self._set_align = sig("pptx_shape_set_align", ci, [vp, ci, ci, ci])
        self._set_bold = sig("pptx_shape_set_bold", ci, [vp, ci, ci, ci])
        self._text_color = sig("pptx_shape_text_color", ci, [vp, ci, ci])
        self._text_align = sig("pptx_shape_text_align", ci, [vp, ci, ci])
        self._text_bold = sig("pptx_shape_text_bold", ci, [vp, ci, ci])
        self._delete = sig("pptx_shape_delete", ci, [vp, ci, ci])
        self._duplicate = sig("pptx_shape_duplicate", ci, [vp, ci, ci])
        self._zorder = sig("pptx_shape_zorder", ci, [vp, ci, ci, ci])
        self._add_rect = sig("pptx_shape_add_rect", ci, [vp, ci, cl, cl, cl, cl, cp])
        self._set_italic = sig("pptx_shape_set_italic", ci, [vp, ci, ci, ci])
        self._set_underline = sig("pptx_shape_set_underline", ci, [vp, ci, ci, ci])
        self._set_fontname = sig("pptx_shape_set_fontname", ci, [vp, ci, ci, cp])
        self._text_italic = sig("pptx_shape_text_italic", ci, [vp, ci, ci])
        self._text_underline = sig("pptx_shape_text_underline", ci, [vp, ci, ci])
        self._set_line = sig("pptx_shape_set_line", ci, [vp, ci, ci, ctypes.c_uint, cd])
        self._line = sig("pptx_shape_line", ctypes.c_uint, [vp, ci, ci])
        self._add_shape = sig("pptx_shape_add_shape", ci,
                              [vp, ci, cp, cl, cl, cl, cl, ctypes.c_uint, ctypes.c_uint])
        self._add_table = sig("pptx_shape_add_table", ci, [vp, ci, cl, cl, cl, cl, ci, ci])
        self._table_size = sig("pptx_table_size", ci, [vp, ci, ci, ctypes.POINTER(ci), ctypes.POINTER(ci)])
        self._table_cell_at = sig("pptx_table_cell_at", ci, [vp, ci, ci, ci, ci, ci, ci])
        self._table_cell_get = sig("pptx_table_cell_get_text", ci, [vp, ci, ci, ci, ci, cp, ci])
        self._table_cell_set = sig("pptx_table_cell_set_text", ci, [vp, ci, ci, ci, ci, cp])
        self._table_cell_rect = sig("pptx_table_cell_rect", ci,
                                    [vp, ci, ci, ci, ci, ci, ci, poi, poi, poi, poi])
        self._table_row_add = sig("pptx_table_row_add", ci, [vp, ci, ci, ci])
        self._table_row_del = sig("pptx_table_row_del", ci, [vp, ci, ci, ci])
        self._table_col_add = sig("pptx_table_col_add", ci, [vp, ci, ci, ci])
        self._table_col_del = sig("pptx_table_col_del", ci, [vp, ci, ci, ci])
        self._add_pic = sig("pptx_shape_add_picture", ci, [vp, ci, cp, cl, cl, cl, cl])
        self._begin_edit = sig("pptx_begin_edit", ci, [vp])
        self._end_edit = sig("pptx_end_edit", ci, [vp])
        self._undo = sig("pptx_undo", ci, [vp])
        self._redo = sig("pptx_redo", ci, [vp])
        self._can_undo = sig("pptx_can_undo", ci, [vp])
        self._can_redo = sig("pptx_can_redo", ci, [vp])
        self._slide_add = sig("pptx_slide_add", ci, [vp, ci])
        self._slide_dup = sig("pptx_slide_duplicate", ci, [vp, ci])
        self._slide_remove = sig("pptx_slide_remove", ci, [vp, ci])
        self._slide_move = sig("pptx_slide_move", ci, [vp, ci, ci])
        self._tr_get = sig("pptx_slide_transition_get", ci, [vp, ci, cp, ci, ctypes.POINTER(ci), ctypes.POINTER(ci)])
        self._tr_set = sig("pptx_slide_transition_set", ci, [vp, ci, cp, ci, ci])
        self._tr_all = sig("pptx_transition_apply_all", ci, [vp, cp, ci, ci])
        self._anim_get = sig("pptx_shape_anim_get", ci, [vp, ci, ci, cp, ci, ctypes.POINTER(ci)])
        self._anim_set = sig("pptx_shape_anim_set", ci, [vp, ci, ci, cp, ci])
        self._anim_clear = sig("pptx_shape_anim_clear", ci, [vp, ci])
        self._save = sig("pptx_save", ci, [vp, cp])
        self._export = sig("pptx_export_html", ci, [vp, cp])
        self._last_error = sig("pptx_last_error", cp, [])
        self.doc = None
        self.slide_w = self.slide_h = 0

    # ---------- 生命周期 ----------
    def open(self, path):
        self.close()
        d = self._open(os.fsencode(path))
        if not d:
            raise RuntimeError(self.last_error() or "打开失败")
        self.doc = d
        w, h = ctypes.c_longlong(), ctypes.c_longlong()
        self._slide_size(d, ctypes.byref(w), ctypes.byref(h))
        self.slide_w, self.slide_h = w.value, h.value
        return self

    def close(self):
        if self.doc:
            self._close(self.doc)
            self.doc = None
            self.slide_w = self.slide_h = 0

    def last_error(self):
        e = self._last_error()
        return e.decode("utf-8", "replace") if e else ""

    def _chk(self, rc, what="操作"):
        if rc != 0:
            raise RuntimeError(self.last_error() or f"{what}失败 rc={rc}")
        return rc

    # ---------- 页面 ----------
    @property
    def slide_count(self):
        return self._slide_count(self.doc) if self.doc else 0

    def render(self, idx, w, h):
        buf, ow, oh = ctypes.c_void_p(), ctypes.c_int(), ctypes.c_int()
        rc = self._render(self.doc, idx, w, h,
                          ctypes.byref(buf), ctypes.byref(ow), ctypes.byref(oh))
        if rc != 0 or not buf:
            raise RuntimeError(self.last_error() or f"渲染失败 rc={rc}")
        try:
            return ctypes.string_at(buf, ow.value * oh.value * 4), ow.value, oh.value
        finally:
            self._free(buf)

    # ---------- 形状 ----------
    def _shape(self, si, i):
        info = ShapeInfo()
        self._chk(self._shape_get(self.doc, si, i, ctypes.byref(info)), "读取形状")
        return {
            "id": info.id, "kind": info.kind, "kind_name": self.KIND.get(info.kind, "形状"),
            "x": info.x, "y": info.y, "cx": info.cx, "cy": info.cy, "rot": info.rot,
            "name": info.name.decode("utf-8", "replace"),
            "text": info.text.decode("utf-8", "replace"),
            "fontsize_pt": info.fontsize_pt, "fill_rgb": info.fill_rgb,
            "italic": info.italic, "underline": info.underline,
            "line_rgb": info.line_rgb,
            "font_name": info.font_name.decode("utf-8", "replace"),
        }

    def shape_count(self, si):
        return self._shape_count(self.doc, si)

    def shape_get(self, si, i):
        return self._shape(si, i)

    def shapes(self, si):
        return [self._shape(si, i) for i in range(self.shape_count(si))]

    def hit(self, si, px, py, cw, ch):
        return self._hit(self.doc, si, px, py, cw, ch)

    def shape_move(self, si, i, dx, dy): self._chk(self._move(self.doc, si, i, dx, dy))
    def shape_resize(self, si, i, w, h): self._chk(self._resize(self.doc, si, i, w, h))
    def shape_set_geom(self, si, i, x, y, cx, cy): self._chk(self._set_geom(self.doc, si, i, x, y, cx, cy))
    def shape_set_rot(self, si, i, rot): self._chk(self._set_rot(self.doc, si, i, rot))
    def shape_set_text(self, si, i, t): self._chk(self._set_text(self.doc, si, i, t.encode("utf-8")))
    def shape_set_fontsize(self, si, i, pt): self._chk(self._set_fontsize(self.doc, si, i, pt))
    def shape_set_fill(self, si, i, rgb): self._chk(self._set_fill(self.doc, si, i, rgb))
    def shape_set_textcolor(self, si, i, rgb): self._chk(self._set_textcolor(self.doc, si, i, rgb))
    def shape_set_align(self, si, i, a): self._chk(self._set_align(self.doc, si, i, a))
    def shape_set_bold(self, si, i, b): self._chk(self._set_bold(self.doc, si, i, b))
    def shape_text_color(self, si, i): return self._text_color(self.doc, si, i)
    def shape_text_align(self, si, i): return self._text_align(self.doc, si, i)
    def shape_text_bold(self, si, i): return self._text_bold(self.doc, si, i)
    def shape_set_italic(self, si, i, v): self._chk(self._set_italic(self.doc, si, i, 1 if v else 0))
    def shape_set_underline(self, si, i, v): self._chk(self._set_underline(self.doc, si, i, 1 if v else 0))
    def shape_set_fontname(self, si, i, name): self._chk(self._set_fontname(self.doc, si, i, name.encode("utf-8")))
    def shape_text_italic(self, si, i): return self._text_italic(self.doc, si, i)
    def shape_text_underline(self, si, i): return self._text_underline(self.doc, si, i)
    def shape_set_line(self, si, i, rgb, pt=1.0): self._chk(self._set_line(self.doc, si, i, rgb, pt))
    def shape_line(self, si, i): return self._line(self.doc, si, i)

    def shape_add_shape(self, si, prst, x, y, cx, cy, fill_rgb=0, line_rgb=0):
        return self._add_shape(self.doc, si, prst.encode("utf-8"), x, y, cx, cy, fill_rgb, line_rgb)
    def shape_add_table(self, si, x, y, cx, cy, rows, cols):
        return self._add_table(self.doc, si, x, y, cx, cy, rows, cols)

    # ---- 表格单元格 ----
    def table_size(self, si, i):
        r, c = ctypes.c_int(), ctypes.c_int()
        if self._table_size(self.doc, si, i, ctypes.byref(r), ctypes.byref(c)) != 0:
            return (0, 0)
        return (r.value, c.value)

    def table_cell_at(self, si, i, px, py, cw, ch):
        return self._table_cell_at(self.doc, si, i, int(px), int(py), int(cw), int(ch))

    def table_cell_text(self, si, i, r, c):
        buf = ctypes.create_string_buffer(2048)
        if self._table_cell_get(self.doc, si, i, r, c, buf, len(buf)) != 0:
            return ""
        return buf.value.decode("utf-8", "replace")

    def table_cell_set_text(self, si, i, r, c, text):
        self._chk(self._table_cell_set(self.doc, si, i, r, c, text.encode("utf-8")), "写单元格")

    def table_cell_rect(self, si, i, r, c, cw, ch):
        """返回单元格在画布像素里的 (x, y, w, h)，失败返回 None"""
        x = ctypes.c_int(); y = ctypes.c_int(); w = ctypes.c_int(); h = ctypes.c_int()
        rc = self._table_cell_rect(self.doc, si, i, r, c, int(cw), int(ch),
                                   ctypes.byref(x), ctypes.byref(y),
                                   ctypes.byref(w), ctypes.byref(h))
        if rc != 0:
            return None
        return x.value, y.value, w.value, h.value

    # ---- 表格行列增删（at / r / c 都按 0 基；-1 表示追加到末尾）----
    def table_row_add(self, si, i, at=-1): return self._table_row_add(self.doc, si, i, at)
    def table_row_del(self, si, i, r): return self._table_row_del(self.doc, si, i, r)
    def table_col_add(self, si, i, at=-1): return self._table_col_add(self.doc, si, i, at)
    def table_col_del(self, si, i, c): return self._table_col_del(self.doc, si, i, c)

    def shape_delete(self, si, i): self._chk(self._delete(self.doc, si, i))
    def shape_duplicate(self, si, i): return self._duplicate(self.doc, si, i)
    def shape_zorder(self, si, i, d): return self._zorder(self.doc, si, i, d)
    def shape_add_rect(self, si, x, y, cx, cy, text=""):
        return self._add_rect(self.doc, si, x, y, cx, cy, text.encode("utf-8"))
    def shape_add_picture(self, si, path, x, y, cx=0, cy=0):
        return self._add_pic(self.doc, si, os.fsencode(path), x, y, cx, cy)

    # ---------- 撤销 ----------
    def begin_edit(self): self._begin_edit(self.doc)
    def end_edit(self): self._end_edit(self.doc)
    def undo(self): return self._undo(self.doc)
    def redo(self): return self._redo(self.doc)
    def can_undo(self): return self._can_undo(self.doc) > 0
    def can_redo(self): return self._can_redo(self.doc) > 0

    # ---------- 页面操作 ----------
    def slide_add(self, after=-1): return self._slide_add(self.doc, after)
    def slide_duplicate(self, idx): return self._slide_dup(self.doc, idx)
    def slide_remove(self, idx): self._chk(self._slide_remove(self.doc, idx))
    def slide_move(self, f, t): self._chk(self._slide_move(self.doc, f, t))

    # ---------- 切换动画 / 入场动画 ----------
    def slide_transition(self, si):
        kind = ctypes.create_string_buffer(64)
        dur = ctypes.c_int(600)
        d = ctypes.c_int(0)
        self._tr_get(self.doc, si, kind, len(kind), ctypes.byref(dur), ctypes.byref(d))
        return kind.value.decode("utf-8", "replace"), dur.value, d.value

    def slide_set_transition(self, si, kind, dur_ms=600, direction=0):
        self._chk(self._tr_set(self.doc, si, kind.encode("utf-8"), int(dur_ms), int(direction)),
                  "设置切换动画")

    def transition_apply_all(self, kind, dur_ms=600, direction=0):
        self._chk(self._tr_all(self.doc, kind.encode("utf-8"), int(dur_ms), int(direction)),
                  "批量设置切换")

    def shape_anim(self, si, i):
        kind = ctypes.create_string_buffer(64)
        dur = ctypes.c_int(500)
        self._anim_get(self.doc, si, i, kind, len(kind), ctypes.byref(dur))
        return kind.value.decode("utf-8", "replace"), dur.value

    def shape_set_anim(self, si, i, kind, dur_ms=500):
        self._chk(self._anim_set(self.doc, si, i, kind.encode("utf-8"), int(dur_ms)), "设置动画")

    def shape_anim_clear(self, si):
        self._chk(self._anim_clear(self.doc, si), "清除动画")

    # ---------- 存盘 ----------
    def save(self, path): self._chk(self._save(self.doc, os.fsencode(path)), "保存")
    def export_html(self, path, split_js=False):
        self._chk(self._export(self.doc, os.fsencode(path)), "导出HTML")
        if split_js:
            split_html_scripts(path)

    # ---------- 插件 ----------
    def plugins_load(self, dll_dir=None):
        """加载插件目录（默认 dll 同级 plugins），返回成功数量"""
        self._plugins_load = getattr(self, "_plugins_load", None) or self.lib.pptx_plugins_load
        self._plugins_load.restype = ctypes.c_int
        self._plugins_load.argtypes = [ctypes.c_char_p]
        n = self._plugins_load(os.fsencode(dll_dir) if dll_dir else None)
        return n

    def plugin_count(self):
        self.lib.pptx_plugin_count.restype = ctypes.c_int
        self.lib.pptx_plugin_count.argtypes = []
        return self.lib.pptx_plugin_count()

    def plugin_info(self, idx):
        f = self.lib.pptx_plugin_info
        f.restype = ctypes.c_int
        f.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_char), ctypes.c_int,
                      ctypes.POINTER(ctypes.c_char), ctypes.c_int]
        nb, db = ctypes.create_string_buffer(256), ctypes.create_string_buffer(1024)
        f(idx, nb, 256, db, 1024)
        return {"name": nb.value.decode("utf-8", "replace"),
                "desc": db.value.decode("utf-8", "replace")}

    def plugins(self):
        try:
            return [self.plugin_info(i) for i in range(self.plugin_count())]
        except AttributeError:
            return []

    def plugin_command(self, cmd, arg=""):
        """向插件广播命令，返回 (是否处理, 结果文本)"""
        f = self.lib.pptx_plugin_command
        f.restype = ctypes.c_int
        f.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_char), ctypes.c_int]
        out = ctypes.create_string_buffer(4096)
        rc = f(cmd.encode("utf-8"), arg.encode("utf-8"), out, 4096)
        return rc, out.value.decode("utf-8", "replace")
