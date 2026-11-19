# -*- coding: utf-8 -*-
"""中间画布：显示 C++ 渲染结果 + 叠加选择框，处理点选 / 拖动 / 缩放 / 旋转 / 双击改字

坐标约定：
  - 内核按像素渲染整页，返回 RGBA；
  - 画布把这张图居中贴在 tk.Canvas 上，EMU 与像素的换算是单一标量 self.k；
  - 鼠标事件先换成图片内像素坐标，再交给内核做命中测试。
"""
import math
import tkinter as tk
from tkinter import simpledialog

from PIL import Image, ImageTk

EMU_PER_PT = 12700.0
EMU_PER_PX = 914400.0 / 96.0

HANDLES = ["tl", "tr", "bl", "br", "tm", "bm", "ml", "mr"]


class SlideCanvas(tk.Canvas):
    def __init__(self, master, core, on_change=None, on_select=None, status=None):
        super().__init__(master, bg="#3f444b", highlightthickness=0, takefocus=1)
        self.core = core
        self.on_change = on_change
        self.on_select = on_select
        self.status = status

        self.slide_idx = 0
        self.shapes = []
        self.sel = -1
        self.multi = set()            # 多选（不含主选 sel）
        self.zoom = 1.0
        self.pil = None
        self.tkimg = None
        self.rw = self.rh = 0
        self.ox = self.oy = 0
        self.k = 1.0                 # EMU / 像素

        self.mode = None             # "move" / "resize" / "rotate"
        self.handle = None
        self.drag0 = (0, 0)
        self.geom0 = None
        self._render_job = None
        self._edit = None            # 就地文字编辑框（tk.Text 叠在画布上）
        self._edit_i = -1
        self._edit_cell = None       # 表格单元格就地编辑：(形状索引, 行, 列)
        self._committing = False
        self.attach = {}             # 形状附加信息：{str(id): {"js"/"media"/"program"}}
        self.last_cell = (0, 0)      # 最近编辑的表格单元格（行、列）
        self.cell_sel = None         # Excel 式单元格选中：(形状索引, 行, 列)
        self.menu_handler = None     # 右键菜单动作回调 (action, shape_index) -> bool

        self.bind("<Configure>", lambda e: self._schedule_render())
        self.bind("<ButtonPress-1>", self._on_press)
        self.bind("<Button-3>", self._on_right)
        self.bind("<B1-Motion>", self._on_motion)
        self.bind("<ButtonRelease-1>", self._on_release)
        self.bind("<Double-Button-1>", self._on_double)
        self.bind("<Key>", self._on_cell_key)
        self.bind("<Control-MouseWheel>", self._on_wheel)
        self.bind("<MouseWheel>", self._on_wheel_norm)

    # ---------------- 数据 ----------------
    def set_page(self, idx, keep_sel=False):
        self.commit_text_edit()
        self.slide_idx = idx
        if not keep_sel:
            self.sel = -1
        self.multi.clear()
        try:
            self.shapes = self.core.shapes(idx)
        except Exception:
            self.shapes = []
        self.redraw()
        self._notify_select()

    def reload(self, keep_sel=True):
        old = self.sel
        old_multi = set(self.multi)
        try:
            self.shapes = self.core.shapes(self.slide_idx)
        except Exception:
            self.shapes = []
        if keep_sel and 0 <= old < len(self.shapes):
            self.sel = old
        else:
            self.sel = -1
        self.multi = {i for i in old_multi if 0 <= i < len(self.shapes) and i != self.sel}
        self.redraw()
        self._notify_select()

    def select(self, i, add=False):
        if add and 0 <= i < len(self.shapes):
            if i == self.sel:
                return
            if i in self.multi:
                self.multi.discard(i)
            else:
                self.multi.add(i)
        else:
            self.sel = i if 0 <= i < len(self.shapes) else -1
            self.multi.clear()
        self._draw_selection()
        self._notify_select()

    def selected_indices(self):
        """主选 + 多选，按层级顺序返回"""
        out = [i for i in self.multi if 0 <= i < len(self.shapes)]
        if 0 <= self.sel < len(self.shapes):
            out.append(self.sel)
        return sorted(set(out))

    def selected_shape(self):
        if 0 <= self.sel < len(self.shapes):
            return self.shapes[self.sel]
        return None

    # ---------------- 渲染 ----------------
    def _schedule_idle_render(self):
        """拖动/缩放时延后重画：手停下来才重新渲染整页"""
        if getattr(self, "_idle_job", None):
            self.after_cancel(self._idle_job)
        self._idle_job = self.after(140, self._idle_render)

    def _idle_render(self):
        self._idle_job = None
        try:
            self.redraw()
        except Exception:
            pass

    def _schedule_render(self):
        if self._render_job:
            self.after_cancel(self._render_job)
        # 拖动/缩放时降频，松手后再出高清图
        self._render_job = self.after(90 if self.mode else 30, self.redraw)

    def redraw(self):
        self._render_job = None
        self.delete("all")
        if not self.core.doc:
            self.create_text(self.winfo_width() // 2, self.winfo_height() // 2,
                             text="还没有打开演示文稿\n点左上角【新建】开始，或【打开】一个 .pptx",
                             fill="#c8ced8", justify="center",
                             font=("Microsoft YaHei UI", 14))
            return
        aw = max(80, self.winfo_width() - 36)
        ah = max(60, self.winfo_height() - 36)
        nat_w = self.core.slide_w / EMU_PER_PX
        nat_h = self.core.slide_h / EMU_PER_PX
        fit = min(aw / nat_w, ah / nat_h)
        rw = max(64, int(nat_w * fit * self.zoom))
        rh = max(36, int(nat_h * fit * self.zoom))
        try:
            if getattr(self, "_fast", False):     # 拖动中：半分辨率渲染再放大，明显更快
                fw = max(64, int(rw * 0.55))
                fh = max(36, int(rh * 0.55))
                data, w, h = self.core.render(self.slide_idx, fw, fh)
                base = Image.frombytes("RGBA", (w, h), data)
                self.pil = base.resize((rw, rh), Image.BILINEAR)
            else:
                data, w, h = self.core.render(self.slide_idx, rw, rh)
                self.pil = Image.frombytes("RGBA", (w, h), data)
        except Exception as e:
            self.create_text(self.winfo_width() // 2, self.winfo_height() // 2,
                             text=str(e), fill="#f88", font=("Microsoft YaHei UI", 12))
            return
        self.tkimg = ImageTk.PhotoImage(self.pil)
        self.rw, self.rh = rw, rh
        w, h = rw, rh
        self.ox = (self.winfo_width() - w) // 2
        self.oy = (self.winfo_height() - h) // 2
        self.k = self.core.slide_w / float(w)
        self.create_image(self.ox, self.oy, anchor="nw", image=self.tkimg, tags="page")
        self.create_rectangle(self.ox - 1, self.oy - 1, self.ox + w + 1, self.oy + h + 1,
                              outline="#1e242b", width=1, tags="page")
        self._draw_selection()
        self._draw_cell_sel()
        self._draw_badges()

    def _draw_cell_sel(self):
        """Excel 式单元格选中框：蓝色粗边 + 右下角小方块"""
        self.delete("cellsel")
        if not self.cell_sel:
            return
        i, r, c = self.cell_sel
        if not (0 <= i < len(self.shapes)):
            return
        rect = self.core.table_cell_rect(self.slide_idx, i, r, c, self.rw, self.rh)
        if not rect:
            return
        x, y, w, h = rect
        x0, y0 = self.ox + x, self.oy + y
        self.create_rectangle(x0 - 1, y0 - 1, x0 + w + 1, y0 + h + 1,
                              outline="#2f6df6", width=3, tags="cellsel")
        self.create_rectangle(x0 + w - 4, y0 + h - 4, x0 + w + 4, y0 + h + 4,
                              fill="#2f6df6", outline="#2f6df6", tags="cellsel")

    def _cell_at(self, i, px, py):
        """画布坐标 → 表格单元格 (r,c)，不在表格内返回 None"""
        if not (0 <= i < len(self.shapes)):
            return None
        sh = self.shapes[i]
        if sh["kind"] != 2:
            return None
        if not (self.ox <= px <= self.ox + self.rw and self.oy <= py <= self.oy + self.rh):
            return None
        idx = self.core.table_cell_at(self.slide_idx, i, px - self.ox, py - self.oy,
                                      self.rw, self.rh)
        if idx < 0:
            return None
        cols = self.core.table_size(self.slide_idx, i)[1]
        if cols <= 0:
            return None
        return divmod(idx, cols)

    def select_cell(self, i, r, c):
        self.cell_sel = (i, r, c)
        self.last_cell = (r, c)
        self._draw_cell_sel()
        if self.status:
            rows, cols = self.core.table_size(self.slide_idx, i)
            self.status("第%d行第%d列（共%d行%d列）｜直接打字=覆盖，Enter下一行，Tab右一格，"
                        "Delete清空，Ctrl+V可粘贴Excel内容" % (r + 1, c + 1, rows, cols))

    def _move_cell(self, dr, dc):
        if not self.cell_sel:
            return
        i, r, c = self.cell_sel
        rows, cols = self.core.table_size(self.slide_idx, i)
        r2 = max(0, min(rows - 1, r + dr))
        c2 = max(0, min(cols - 1, c + dc))
        self.commit_text_edit()
        self.select_cell(i, r2, c2)
        self.focus_set()

    def _on_cell_key(self, ev):
        """画布上的 Excel 式键盘操作（只在选中单元格时生效）"""
        if not self.cell_sel or self._edit is not None:
            return None
        i, r, c = self.cell_sel
        ks = ev.keysym
        ctrl = bool(ev.state & 0x0004)
        if ks == "Up":
            self._move_cell(-1, 0); return "break"
        if ks == "Down":
            self._move_cell(1, 0); return "break"
        if ks == "Left":
            self._move_cell(0, -1); return "break"
        if ks == "Right":
            self._move_cell(0, 1); return "break"
        if ks == "Tab":
            self._move_cell(0, 1); return "break"
        if ks in ("ISO_Left_Tab",):
            self._move_cell(0, -1); return "break"
        if ks in ("Return", "KP_Enter"):
            self._move_cell(1, 0); return "break"
        if ks in ("Delete", "BackSpace"):
            self.core.table_cell_set_text(self.slide_idx, i, r, c, "")
            self.shapes[i] = self.core.shape_get(self.slide_idx, i)
            self.redraw()
            if self.on_change:
                self.on_change()
            return "break"
        if ks == "F2":
            self.begin_cell_edit(i, r, c); return "break"
        if ks == "Escape":
            self.cell_sel = None
            self._draw_cell_sel()
            return "break"
        if ctrl and ks.lower() == "v":
            return self._paste_into_cell()
        if ctrl:
            return None
        ch = getattr(ev, "char", "") or ""
        if len(ch) == 1 and ch.isprintable():
            self.begin_cell_edit(i, r, c)
            if self._edit is not None:
                self._edit.delete("1.0", "end")
                self._edit.insert("1.0", ch)
                self._edit.tag_remove("sel", "1.0", "end")
            return "break"
        return None

    def _clip_text(self):
        try:
            import pyperclip
            return pyperclip.paste() or ""
        except Exception:
            try:
                return self.clipboard_get()
            except Exception:
                return ""

    def _paste_into_cell(self):
        """把 Excel 里复制的一片（制表符/换行分隔）整块粘到当前单元格开始的位置"""
        if not self.cell_sel:
            return None
        i, r0, c0 = self.cell_sel
        txt = self._clip_text()
        if not txt.strip():
            return "break"
        lines = txt.replace("\r", "").split("\n")
        while lines and not lines[-1].strip():
            lines.pop()
        if not lines:
            return "break"
        block = [ln.split("\t") for ln in lines]
        need_r = r0 + len(block)
        need_c = c0 + max(len(b) for b in block)
        rows, cols = self.core.table_size(self.slide_idx, i)
        while rows < need_r:
            self.core.table_row_add(self.slide_idx, i, rows)
            rows, cols = self.core.table_size(self.slide_idx, i)
        while cols < need_c:
            self.core.table_col_add(self.slide_idx, i, cols)
            rows, cols = self.core.table_size(self.slide_idx, i)
        for dr, row in enumerate(block):
            for dc, val in enumerate(row):
                self.core.table_cell_set_text(self.slide_idx, i, r0 + dr, c0 + dc, val)
        self.shapes[i] = self.core.shape_get(self.slide_idx, i)
        self.redraw()
        if self.on_change:
            self.on_change()
        if self.status:
            self.status("已粘贴 %d 行 %d 列（Ctrl+Z 可撤销）" % (len(block), max(len(b) for b in block)))
        return "break"

    def _draw_badges(self):
        """在带脚本/视频/音频/程序的形状左上角画一个小徽标"""
        self.delete("badge")
        for sh in self.shapes:
            ent = self.attach.get(str(sh["id"]))
            if not ent:
                continue
            tags = []
            m = ent.get("media") or {}
            if m.get("path"):
                tags.append("▶ 视频" if m.get("type") != "audio" else "♪ 音频")
            if ent.get("program"):
                tags.append("⚙ 程序")
            if ent.get("js"):
                tags.append("</> 脚本")
            if not tags:
                continue
            x, y, w, h = self._shape_px_box(sh)
            bx = self.ox + x + 2
            by = self.oy + y - 4
            anchor = "sw"
            if by - 16 < self.oy + 2:     # 形状贴页顶：徽标放到形状内部左上角
                by = self.oy + y + 18
                anchor = "nw"
            txt = " ".join(tags)
            tid = self.create_text(bx + 5, by, text=txt, anchor=anchor, tags="badge",
                                   fill="#ffe9a8", font=("Microsoft YaHei UI", 9, "bold"))
            bb = self.bbox(tid)
            self.create_rectangle(bb[0] - 4, bb[1] - 1, bb[2] + 4, bb[3] + 1,
                                  fill="#2b3340", outline="#7d8ea6", tags="badge")
            self.tag_raise(tid)

    def fade_in(self):
        """翻页时做一次简单淡入：用点状遮罩逐级变稀"""
        if self.rw <= 0 or self.rh <= 0:
            return
        self.delete("fade")
        rid = self.create_rectangle(self.ox, self.oy, self.ox + self.rw, self.oy + self.rh,
                                    fill="#262b33", outline="", stipple="gray75", tags="fade")
        steps = [("gray75", 55), ("gray50", 55), ("gray25", 55), ("gray12", 45)]

        def step(i=0):
            if i >= len(steps):
                self.delete("fade")
                return
            st, ms = steps[i]
            self.itemconfigure(rid, stipple=st)
            self.after(ms, lambda: step(i + 1))

        step()

    # ---------------- 选择框 ----------------
    def _shape_px_box(self, sh):
        return (sh["x"] / self.k, sh["y"] / self.k,
                sh["cx"] / self.k, sh["cy"] / self.k)

    def _rot_pts(self, cx, cy, w, h, rot, pts):
        """把形状局部坐标点旋转到画布坐标"""
        if not rot:
            return [(cx + x, cy + y) for x, y in pts]
        c = math.cos(math.radians(rot))
        s = math.sin(math.radians(rot))
        mx, my = cx + w / 2, cy + h / 2
        out = []
        for x, y in pts:
            dx, dy = cx + x - mx, cy + y - my
            out.append((mx + dx * c - dy * s, my + dx * s + dy * c))
        return out

    def _handle_pts(self, cx, cy, w, h, rot):
        pts = [(0, 0), (w, 0), (0, h), (w, h), (w / 2, 0), (w / 2, h), (0, h / 2), (w, h / 2)]
        return self._rot_pts(cx, cy, w, h, rot, pts)

    def _rot_handle(self, cx, cy, w, h, rot):
        p = self._rot_pts(cx, cy, w, h, rot, [(w / 2, -26)])[0]
        return p

    def _draw_selection(self):
        self.delete("sel")
        self.delete("guide")
        for i in self.multi:
            if not (0 <= i < len(self.shapes)) or i == self.sel:
                continue
            sh = self.shapes[i]
            x, y, w, h = self._shape_px_box(sh)
            cx, cy = self.ox + x, self.oy + y
            pts = self._rot_pts(cx, cy, w, h, sh.get("rot", 0.0), [(0, 0), (w, 0), (w, h), (0, h)])
            self.create_polygon([c for p in pts for c in p], outline="#4ea1ff", width=1,
                                dash=(3, 3), fill="", tags="sel")
        sh = self.selected_shape()
        if not sh:
            return
        x, y, w, h = self._shape_px_box(sh)
        cx, cy = self.ox + x, self.oy + y
        rot = sh.get("rot", 0.0)
        pts = self._rot_pts(cx, cy, w, h, rot, [(0, 0), (w, 0), (w, h), (0, h)])
        if self.mode == "move":
            # 拖动时整页渲染会延后，用一个半透明“影子”提示形状要去的位置
            self.create_rectangle(cx, cy, cx + w, cy + h, fill="#2f6df6",
                                  stipple="gray12", outline="", tags="sel")
        self.create_polygon([c for p in pts for c in p], outline="#1e88ff", width=2,
                            dash=(4, 3), fill="", tags="sel")
        hp = self._handle_pts(cx, cy, w, h, rot)
        for (hx, hy) in hp:
            self.create_rectangle(hx - 4, hy - 4, hx + 4, hy + 4,
                                  fill="#ffffff", outline="#1e88ff", tags="sel")
        rx, ry = self._rot_handle(cx, cy, w, h, rot)
        mx, my = self._rot_pts(cx, cy, w, h, rot, [(w / 2, 0)])[0]
        self.create_line(mx, my, rx, ry, fill="#1e88ff", dash=(2, 2), tags="sel")
        self.create_oval(rx - 5, ry - 5, rx + 5, ry + 5, fill="#1e88ff", outline="#0b5cad", tags="sel")

    def _notify_select(self):
        if self.on_select:
            self.on_select(self.sel)

    # ---------------- 命中 ----------------
    def _pick(self, ev_x, ev_y):
        """返回 (形状索引, 句柄名)；句柄仅对已选中的形状生效"""
        sh = self.selected_shape()
        if sh:
            x, y, w, h = self._shape_px_box(sh)
            cx, cy = self.ox + x, self.oy + y
            rot = sh.get("rot", 0.0)
            rx, ry = self._rot_handle(cx, cy, w, h, rot)
            if abs(ev_x - rx) <= 8 and abs(ev_y - ry) <= 8:
                return self.sel, "rot"
            for name, (hx, hy) in zip(HANDLES, self._handle_pts(cx, cy, w, h, rot)):
                if abs(ev_x - hx) <= 6 and abs(ev_y - hy) <= 6:
                    return self.sel, name
        px, py = ev_x - self.ox, ev_y - self.oy
        if not (0 <= px < self.rw and 0 <= py < self.rh):
            return -1, None
        try:
            return self.core.hit(self.slide_idx, px, py, self.rw, self.rh), None
        except Exception:
            return -1, None

    # ---------------- 交互 ----------------
    def _snap_move(self, nx, ny, cx, cy):
        """拖动时吸附到幻灯片中线 / 其它形状的边和中线，并画出参考线"""
        self.delete("guide")
        if self.k <= 0:
            return nx, ny
        w, h = cx / self.k, cy / self.k
        x, y = nx / self.k, ny / self.k
        vts = [self.rw / 2.0]
        hts = [self.rh / 2.0]
        for j, sh in enumerate(self.shapes):
            if j == self.sel or j in self.multi:
                continue
            sx, sy = sh["x"] / self.k, sh["y"] / self.k
            sw, shh = sh["cx"] / self.k, sh["cy"] / self.k
            vts += [sx, sx + sw / 2, sx + sw]
            hts += [sy, sy + shh / 2, sy + shh]
        bv = bh = None
        for ex in (x, x + w / 2, x + w):
            for t in vts:
                d = t - ex
                if abs(d) <= 6 and (bv is None or abs(d) < abs(bv[0])):
                    bv = (d, t)
        for ey in (y, y + h / 2, y + h):
            for t in hts:
                d = t - ey
                if abs(d) <= 6 and (bh is None or abs(d) < abs(bh[0])):
                    bh = (d, t)
        if bv:
            nx = int(round(nx + bv[0] * self.k))
            gx = self.ox + bv[1]
            self.create_line(gx, self.oy, gx, self.oy + self.rh, fill="#ff5fa2",
                             dash=(2, 4), tags="guide")
        if bh:
            ny = int(round(ny + bh[0] * self.k))
            gy = self.oy + bh[1]
            self.create_line(self.ox, gy, self.ox + self.rw, gy, fill="#ff5fa2",
                             dash=(2, 4), tags="guide")
        return nx, ny

    def _on_press(self, ev):
        self.focus_set()
        ctrl = bool(ev.state & 0x0004)
        i, handle = self._pick(ev.x, ev.y)
        if ctrl and i >= 0 and i != self.sel:
            # Ctrl+单击：加选 / 取消加选，不进入拖动
            self.select(i, add=True)
            return
        if i != self.sel:
            if i in self.multi:
                # 点在已加选的形状上：保持多选，整体拖动
                pass
            else:
                self.sel = i
                self.multi.clear()
        if i < 0:
            self.multi.clear()
            self.cell_sel = None
            self._draw_selection()
            self._draw_cell_sel()
            self._notify_select()
            return
        sh = self.shapes[i]
        if self._edit is not None:
            self.commit_text_edit()
        self._fast = False
        self._tbl_press = None
        if not ctrl and sh.get("kind") == 2 and not handle:
            cell = self._cell_at(i, ev.x, ev.y)
            if cell:
                # 表格：按下先记住格子，拖动超过 4px 才是移动整张表
                self._tbl_press = (i, cell[0], cell[1], ev.x, ev.y)
        self.cell_sel = None
        self._draw_cell_sel()
        self.drag0 = (ev.x, ev.y)
        self.geom0 = (sh["x"], sh["y"], sh["cx"], sh["cy"], sh["rot"])
        self.geom0_all = {}
        for j in self.selected_indices():
            sj = self.shapes[j]
            self.geom0_all[j] = (sj["x"], sj["y"], sj["cx"], sj["cy"], sj["rot"])
        self.guides = []
        if handle == "rot":
            self.mode = "rotate"
        elif handle:
            self.mode = "resize"
            self.handle = handle
        else:
            self.mode = "move"
        self.core.begin_edit()
        self._set_cursor(handle)
        self._draw_selection()
        self._notify_select()

    def _on_motion(self, ev):
        if not self.mode or not self.selected_shape():
            _, handle = self._pick(ev.x, ev.y)
            self._set_cursor(handle if self.selected_shape() else None)
            if handle is None:
                i2, _h2 = self._pick(ev.x, ev.y)
                if 0 <= i2 < len(self.shapes) and self.shapes[i2]["kind"] == 2:
                    self.configure(cursor="fleur")     # 表格：可按住拖动整张表
            return
        dx = ev.x - self.drag0[0]
        dy = ev.y - self.drag0[1]
        if getattr(self, "_tbl_press", None):
            if abs(dx) < 4 and abs(dy) < 4:
                return                      # 还没拖开，先不动，松手就当“选单元格”
            self._tbl_press = None
            self.cell_sel = None
            self._draw_cell_sel()
        ox, oy, ocx, ocy, orot = self.geom0
        if self.mode == "move":
            if len(getattr(self, "geom0_all", {}) or {}) > 1:
                # 多选整体拖动：所有形状同一位移，不做吸附
                for j, g in self.geom0_all.items():
                    self.core.shape_set_geom(self.slide_idx, j,
                                             g[0] + int(dx * self.k), g[1] + int(dy * self.k),
                                             g[2], g[3])
                    self.shapes[j] = self.core.shape_get(self.slide_idx, j)
                self._draw_selection()
                self._schedule_idle_render()
                return
            nx = ox + int(dx * self.k)
            ny = oy + int(dy * self.k)
            nx, ny = self._snap_move(nx, ny, ocx, ocy)
            self.core.shape_set_geom(self.slide_idx, self.sel, nx, ny, ocx, ocy)
        elif self.mode == "rotate":
            sh = self.selected_shape()
            cx = self.ox + (sh["x"] + sh["cx"] / 2) / self.k
            cy = self.oy + (sh["y"] + sh["cy"] / 2) / self.k
            ang = math.degrees(math.atan2(ev.y - cy, ev.x - cx)) + 90
            self.core.shape_set_rot(self.slide_idx, self.sel, ang % 360)
        else:
            nx, ny, ncx, ncy = ox, oy, ocx, ocy
            ddx = int(dx * self.k)
            ddy = int(dy * self.k)
            if "l" in self.handle:
                nx = ox + ddx
                ncx = ocx - ddx
            if "r" in self.handle:
                ncx = ocx + ddx
            if "t" in self.handle:
                ny = oy + ddy
                ncy = ocy - ddy
            if "b" in self.handle:
                ncy = ocy + ddy
            ncx = max(int(8 * 12700), ncx)
            ncy = max(int(8 * 12700), ncy)
            self.core.shape_set_geom(self.slide_idx, self.sel, nx, ny, ncx, ncy)
        self.shapes[self.sel] = self.core.shape_get(self.slide_idx, self.sel)
        self._draw_selection()
        self._schedule_idle_render()      # 拖动时先只跟手画选框，停止 140ms 后才重画整页
        if self.status:
            sh = self.shapes[self.sel]
            self.status(f"x={sh['x']/EMU_PER_PT:.1f}pt  y={sh['y']/EMU_PER_PT:.1f}pt  "
                        f"宽={sh['cx']/EMU_PER_PT:.1f}pt 高={sh['cy']/EMU_PER_PT:.1f}pt")

    def _on_release(self, ev):
        tbl = getattr(self, "_tbl_press", None)
        self._tbl_press = None
        self._fast = False
        if getattr(self, "_idle_job", None):
            self.after_cancel(self._idle_job)
            self._idle_job = None
        if self.mode:
            self.core.end_edit()
            self.mode = None
            self.handle = None
            self.reload()
            if self.on_change:
                self.on_change()
        if tbl:
            self.select_cell(tbl[0], tbl[1], tbl[2])
            self.focus_set()
        self._set_cursor(None)

    def _on_double(self, ev):
        i, _ = self._pick(ev.x, ev.y)
        if i < 0:
            return
        self.sel = i
        sh = self.shapes[i]
        if sh["kind"] == 2:                       # 表格：双击改单元格文字
            idx = self.core.table_cell_at(self.slide_idx, i, ev.x - self.ox, ev.y - self.oy,
                                          self.rw, self.rh)
            if idx < 0:
                if self.status:
                    self.status("没点中单元格，请点在表格范围内")
                return
            rows, cols = self.core.table_size(self.slide_idx, i)
            if cols <= 0:
                return
            r, c = divmod(idx, cols)
            self.last_cell = (r, c)
            self.begin_cell_edit(i, r, c)
            return
        ent = self.attach.get(str(sh["id"]), {})
        if ent.get("program"):
            self.menu_call("run_program", i)
            return
        if sh["kind"] in (1, 3, 4):
            if self.status:
                self.status("图片 / 图表 / 媒体暂不支持直接改文字")
            return
        self.begin_text_edit(i)

    # ---------------- 右键快捷菜单 ----------------
    def _on_right(self, ev):
        i, _ = self._pick(ev.x, ev.y)
        if i is not None and i >= 0:
            self.select(i)
            self.focus_set()
        else:
            i = self.sel
        self._popup_menu(i)

    def _popup_menu(self, i):
        m = self._build_menu(i)
        try:
            m.tk_popup(self.winfo_pointerx(), self.winfo_pointery())
        finally:
            m.grab_release()

    def _build_menu(self, i):
        m = tk.Menu(self, tearoff=0, bg="#2b3340", fg="#e6e9ef",
                    activebackground="#3d6fd6", activeforeground="#ffffff",
                    bd=0)
        has = 0 <= i < len(self.shapes)
        sid = str(self.shapes[i]["id"]) if has else ""
        ent = self.attach.get(sid, {}) if has else {}
        if has:
            m.add_command(label="编辑文字", command=lambda: self.menu_call("edit_text", i))
            if self.shapes[i]["kind"] == 2:
                m.add_command(label="导出该表格为 Excel…",
                              command=lambda: self.menu_call("export_xlsx", i))
            m.add_command(label="复制形状", command=lambda: self.menu_call("duplicate", i))
            m.add_command(label="置于顶层", command=lambda: self.menu_call("top", i))
            m.add_command(label="置于底层", command=lambda: self.menu_call("bottom", i))
            m.add_separator()
            js_label = "编辑 JS 脚本…" if ent.get("js") else "添加 JS 脚本…"
            m.add_command(label=js_label, command=lambda: self.menu_call("script", i))
            m.add_command(label="插入视频…", command=lambda: self.menu_call("video", i))
            m.add_command(label="插入音频…", command=lambda: self.menu_call("audio", i))
            if (ent.get("media") or {}).get("path"):
                ap = "开" if ent["media"].get("autoplay") else "关"
                m.add_command(label="切换自动播放（当前：%s）" % ap,
                              command=lambda: self.menu_call("toggle_autoplay", i))
            m.add_command(label="选择同目录程序…", command=lambda: self.menu_call("program", i))
            if ent.get("program"):
                m.add_command(label="▶ 运行该程序", command=lambda: self.menu_call("run_program", i))
            if ent:
                m.add_command(label="清除该形状的附加信息", command=lambda: self.menu_call("clear_attach", i))
            m.add_separator()
            m.add_command(label="删除形状", command=lambda: self.menu_call("delete", i))
        m.add_separator()
        m.add_command(label="插入文本框", command=lambda: self.menu_call("new_text", i))
        m.add_command(label="插入图片…", command=lambda: self.menu_call("new_picture", i))
        m.add_command(label="新建视频 / 音频块", command=lambda: self.menu_call("new_media", i))
        m.add_command(label="本页 JS 脚本…", command=lambda: self.menu_call("slide_script", i))
        m.add_command(label="全局 JS 脚本…", command=lambda: self.menu_call("global_script", i))
        return m

    def menu_call(self, action, i):
        if self.menu_handler:
            self.menu_handler(action, i)

    # ---------------- 就地文字编辑 ----------------
    def _font_px(self, sh):
        fs = sh.get("fontsize_pt") or 18.0
        return max(7, min(200, int(round(fs * 12700.0 / max(1.0, self.k)))))

    def begin_text_edit(self, i):
        self.commit_text_edit()
        if not (0 <= i < len(self.shapes)):
            return
        sh = self.shapes[i]
        x, y, w, h = self._shape_px_box(sh)
        fill = sh.get("fill_rgb")
        bg = "#ffffff" if not fill else "#%06x" % fill
        self._edit = tk.Text(self, wrap="word", undo=True,
                             font=("Microsoft YaHei UI", self._font_px(sh)),
                             bg=bg, fg="#111111", insertbackground="#111111",
                             relief="solid", bd=1, padx=3, pady=2,
                             highlightthickness=1, highlightbackground="#2f6df6")
        self._edit.place(x=self.ox + x, y=self.oy + y, width=max(20, w), height=max(20, h))
        self._edit.insert("1.0", sh.get("text") or "")
        self._edit.focus_set()
        self._edit.bind("<Escape>", lambda e: (self.commit_text_edit(False), "break")[1])
        self._edit.bind("<Control-Return>", lambda e: (self.commit_text_edit(False), "break")[1])
        self._edit.bind("<Control-s>", lambda e: (self.commit_text_edit(True), "break")[1])
        self._edit.bind("<Control-S>", lambda e: (self.commit_text_edit(True), "break")[1])
        self._edit.bind("<FocusOut>", lambda e: self.commit_text_edit(False))
        self._edit_i = i
        if self.status:
            self.status("正在编辑文字：Ctrl+Enter / 点空白处 完成，Ctrl+S 完成并存盘，Esc 取消")

    def begin_cell_edit(self, i, r, c):
        """表格单元格就地编辑：把输入框盖在单元格上"""
        self.commit_text_edit()
        if not (0 <= i < len(self.shapes)):
            return
        rect = self.core.table_cell_rect(self.slide_idx, i, r, c, self.rw, self.rh)
        if not rect:
            if self.status:
                self.status("读不到单元格位置")
            return
        x, y, w, h = rect
        self._edit = tk.Text(self, wrap="word", undo=True,
                             font=("Microsoft YaHei UI", 16),
                             bg="#ffffff", fg="#111111", insertbackground="#111111",
                             relief="solid", bd=1, padx=2, pady=1,
                             highlightthickness=1, highlightbackground="#2f6df6")
        self._edit.place(x=self.ox + x + 1, y=self.oy + y + 1,
                         width=max(16, w - 2), height=max(16, h - 2))
        self._edit.insert("1.0", self.core.table_cell_text(self.slide_idx, i, r, c))
        self._edit.tag_add("sel", "1.0", "end")
        self._edit.focus_set()
        self._edit.bind("<Escape>", lambda e: (self.commit_text_edit(False), "break")[1])
        self._edit.bind("<Control-Return>", lambda e: (self.commit_text_edit(False), "break")[1])
        self._edit.bind("<Control-s>", lambda e: (self.commit_text_edit(True), "break")[1])
        self._edit.bind("<Control-S>", lambda e: (self.commit_text_edit(True), "break")[1])
        self._edit.bind("<FocusOut>", lambda e: self.commit_text_edit(False))
        self._edit.bind("<Return>", lambda e: (self.commit_text_edit(False),
                                               self._move_cell(1, 0), "break")[2])
        self._edit.bind("<KP_Enter>", lambda e: (self.commit_text_edit(False),
                                                 self._move_cell(1, 0), "break")[2])
        self._edit.bind("<Tab>", lambda e: (self.commit_text_edit(False),
                                            self._move_cell(0, 1), "break")[2])
        self._edit.bind("<Shift-Tab>", lambda e: (self.commit_text_edit(False),
                                                  self._move_cell(0, -1), "break")[2])
        self._edit_i = i
        self._edit_cell = (i, r, c)
        self.last_cell = (r, c)
        self.cell_sel = (i, r, c)
        self._draw_cell_sel()
        if self.status:
            self.status("正在编辑第%d行第%d列：Ctrl+Enter/Tab 完成，Ctrl+S 存盘，Esc 取消" % (r + 1, c + 1))

    def commit_text_edit(self, save=False):
        if self._committing or self._edit is None:
            return
        self._committing = True
        try:
            txt = self._edit.get("1.0", "end-1c")
            i = self._edit_i
            cell = self._edit_cell
            self._edit.destroy()
            self._edit = None
            self._edit_i = -1
            self._edit_cell = None
            if cell is not None:
                ci, r, c = cell
                if 0 <= ci < len(self.shapes):
                    self.core.table_cell_set_text(self.slide_idx, ci, r, c, txt)
                    self.shapes[ci] = self.core.shape_get(self.slide_idx, ci)
                    self.reload()
                    if self.on_change:
                        self.on_change()
                    if save and getattr(self, "save_cb", None):
                        self.save_cb()
            elif 0 <= i < len(self.shapes):
                self.core.shape_set_text(self.slide_idx, i, txt)
                self.shapes[i] = self.core.shape_get(self.slide_idx, i)
                self.reload()
                if self.on_change:
                    self.on_change()
                if save and getattr(self, "save_cb", None):
                    self.save_cb()
        finally:
            self._committing = False


    def _on_wheel(self, ev):
        self.zoom = min(3.0, max(0.2, self.zoom + (0.1 if ev.delta > 0 else -0.1)))
        self._schedule_idle_render()

    def _on_wheel_norm(self, ev):
        # 普通滚轮用于滚动，画布内不处理
        pass

    def _set_cursor(self, handle):
        cur = "arrow"
        if handle == "rot":
            cur = "exchange"
        elif handle in ("ml", "mr"):
            cur = "sb_h_double_arrow"
        elif handle in ("tm", "bm"):
            cur = "sb_v_double_arrow"
        elif handle in ("tl", "br"):
            cur = "size_nw_se"
        elif handle in ("tr", "bl"):
            cur = "size_ne_sw"
        elif self.mode == "move":
            cur = "fleur"
        else:
            cur = "hand2"
        try:
            self.configure(cursor=cur)
        except Exception:
            pass


class TextDialog(tk.Toplevel):
    """多行文字编辑框（替代 PySide6 的 QInputDialog）

    快捷键：Ctrl+Enter / Ctrl+S 确定并写回幻灯片（Ctrl+S 还会顺手存盘），Esc 取消。
    """

    def __init__(self, master, title, text):
        super().__init__(master)
        self.title(title)
        self.result = None
        self.want_save = False
        self.geometry("560x400")
        self.configure(bg="#23272e")
        self.transient(master.winfo_toplevel())
        tk.Label(self, text="内容（回车换行，多段即多行）", anchor="w",
                 bg="#23272e", fg="#dbe4ef").pack(fill="x", padx=10, pady=(10, 2))
        self.txt = tk.Text(self, wrap="word", font=("Microsoft YaHei UI", 12), undo=True,
                           bg="#2b3038", fg="#eef4fb", insertbackground="#eef4fb",
                           relief="flat", padx=6, pady=6)
        # 先占位底部按钮条，再放会 expand 的文本框；否则按钮会被挤到窗口外面
        bar = tk.Frame(self, bg="#23272e")
        bar.pack(side="bottom", fill="x", pady=8, padx=10)
        tk.Label(bar, text="Ctrl+Enter 或 Ctrl+S 确定  ·  Esc 取消", anchor="w",
                 bg="#23272e", fg="#9fb0c6").pack(side="left")
        tk.Button(bar, text="取消", width=10, command=self.destroy,
                  bg="#39404a", fg="#dfe7f1", activebackground="#4a525e",
                  relief="flat").pack(side="right", padx=4)
        tk.Button(bar, text="确定", width=10, command=self._ok,
                  bg="#2f6df6", fg="#ffffff", activebackground="#3f7bff",
                  relief="flat").pack(side="right")
        self.txt.pack(fill="both", expand=True, padx=10)
        self.txt.insert("1.0", text)
        self.txt.tag_add("sel", "1.0", "end-1c")
        self.txt.focus_set()
        self.txt.mark_set("insert", "1.0")
        self.txt.see("insert")
        # 对话框是 grab 模态，编辑器主窗口的 Ctrl+S 收不到，必须在框内自己接。
        # 只绑在 Toplevel 一级（Text 的 bindtags 包含它），避免同一次按键被处理两遍。
        self.bind("<Control-Return>", self._ok_key)
        self.bind("<Control-KP_Enter>", self._ok_key)
        self.bind("<Control-s>", self._ok_save)
        self.bind("<Control-S>", self._ok_save)
        self.bind("<Escape>", self._cancel_key)
        self.protocol("WM_DELETE_WINDOW", self.destroy)
        self.grab_set()
        self.wait_window(self)

    def _ok_key(self, _ev=None):
        self._ok()
        return "break"

    def _ok_save(self, _ev=None):
        self._ok(with_save=True)
        return "break"

    def _cancel_key(self, _ev=None):
        self.destroy()
        return "break"

    def _ok(self, with_save=False):
        self.result = self.txt.get("1.0", "end-1c")
        self.want_save = bool(with_save)
        self.destroy()
