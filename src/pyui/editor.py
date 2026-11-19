# -*- coding: utf-8 -*-
"""ycz 编辑器主窗口（customtkinter）

布局：左缩略图 | 中画布 | 右属性面板
内核全部由 C++ 的 pptx-core.dll 承担，这里只做界面与交互。
"""
import ctypes
import os
import re
import subprocess
import sys
import traceback
import tkinter as tk
from tkinter import filedialog, messagebox, colorchooser

import customtkinter as ctk
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import attach as yc_attach                            # noqa: E402
import xlsx_export as yc_xlsx                         # noqa: E402
import xlsx_import as yc_xlsx_in                      # noqa: E402
from core import Core, EMU_PER_PT                      # noqa: E402
from canvas import SlideCanvas                        # noqa: E402
from web_snapshot import html_to_png                  # noqa: E402

ctk.set_appearance_mode("dark")
ctk.set_default_color_theme("blue")


def _enable_dpi_aware():
    """让窗口按物理像素显示，避免被系统缩放放大后超出屏幕"""
    try:
        import ctypes
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except Exception:
            pass


FONT = ("Microsoft YaHei UI", 12)
FONT_S = ("Microsoft YaHei UI", 11)
TEXT_MAIN = "#f1f5fb"
TEXT_SUB = "#c9d5e4"
TEXT_DIM = "#98a8bc"

# 切换动画 / 入场动画：界面中文 -> 内核 kind
TR_NAMES = {
    "无": "none", "淡入淡出": "fade", "推进": "push", "擦除": "wipe",
    "分割": "split", "覆盖": "cover", "显示": "uncover", "切出": "cut",
    "溶解": "dissolve", "百叶窗": "blinds", "棋盘": "checker", "圆形": "circle",
    "菱形": "diamond", "轮辐": "wheel", "随机": "random", "缩放": "zoom",
}
TR_NAMES_R = {v: k for k, v in TR_NAMES.items()}
ANIM_NAMES = {
    "无": "none", "出现": "appear", "淡入": "fade", "擦除": "wipe",
    "棋盘": "checker", "圆形": "circle", "菱形": "diamond", "溶解": "dissolve",
    "百叶窗": "blinds", "轮辐": "wheel", "缩放": "zoom",
}
ANIM_NAMES_R = {v: k for k, v in ANIM_NAMES.items()}
TWIPS_PER_SEC = 1000


def pil_from(data, w, h):
    return Image.frombytes("RGBA", (w, h), data)


def _app_root():
    """资源根目录：源码运行=工程根；PyInstaller 打包后=解包目录"""
    if getattr(sys, "frozen", False):
        return getattr(sys, "_MEIPASS", os.path.dirname(sys.executable))
    return os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def _set_taskbar_icon(win, ico_path):
    """把 ico 设成窗口大/小图标——任务栏读的就是它，Tk 的 iconphoto 改不到任务栏"""
    try:
        try:
            ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID("ycz.editor")
        except Exception:
            pass
        if not os.path.exists(ico_path):
            return
        win.update_idletasks()
        hwnd = ctypes.windll.user32.GetAncestor(win.winfo_id(), 2) or win.winfo_id()  # GA_ROOT
        IMAGE_ICON, LR_LOADFROMFILE = 1, 0x10
        big = ctypes.windll.user32.LoadImageW(0, ico_path, IMAGE_ICON, 0, 0, LR_LOADFROMFILE)
        small = ctypes.windll.user32.LoadImageW(0, ico_path, IMAGE_ICON, 16, 16, LR_LOADFROMFILE)
        if big:
            ctypes.windll.user32.SendMessageW(hwnd, 0x80, 1, big)    # WM_SETICON, ICON_BIG
        if small:
            ctypes.windll.user32.SendMessageW(hwnd, 0x80, 0, small)  # WM_SETICON, ICON_SMALL
    except Exception:
        pass


class App(ctk.CTk):
    def __init__(self, path=None):
        super().__init__()
        self.core = Core()
        self.path = None
        self._busy = False
        self._thumb_imgs = []

        self.title("ycz 编辑器")
        self.geometry("1300x820")
        self.minsize(820, 520)
        root = _app_root()
        try:                                   # 左上角图标
            p = os.path.join(root, "icon.png")
            if os.path.exists(p):
                self._icon_img = tk.PhotoImage(file=p)
                self.iconphoto(True, self._icon_img)
        except Exception:
            pass
        self.after(150, lambda: _set_taskbar_icon(self, os.path.join(root, "ycz.ico")))  # 任务栏图标
        self._center_window()
        self._build_ui()
        self._bind_keys()
        if path:
            self.load_file(path)

    def _center_window(self):
        """按物理像素算目标尺寸，再换算回 Tk 单位（高 DPI 下 1 单位 = scale 像素）"""
        self.update_idletasks()
        scale = max(1.0, self.winfo_fpixels("1i") / 96.0)
        sw = self.winfo_screenwidth() / scale      # 物理宽
        sh = self.winfo_screenheight() / scale     # 物理高
        w = min(1460, sw - 80)
        h = min(880, sh - 120)
        tw, th = int(w / scale), int(h / scale)
        self.geometry(f"{tw}x{th}+{max(0, int((sw - w) / 2 / scale))}+{max(0, int((sh - h) / 2 / scale))}")

    # ================= UI =================
    def _build_ui(self):
        # ---- 工具栏 ----
        bar = ctk.CTkFrame(self, height=46, corner_radius=0)
        bar.pack(fill="x", side="top")
        self.btn_undo = None
        self.btn_redo = None

        def cmd(text, fn, width=64):
            b = ctk.CTkButton(bar, text=text, width=width, height=30, font=FONT_S,
                              command=fn, fg_color="#39404a", hover_color="#4a525e")
            b.pack(side="left", padx=2, pady=8)
            return b

        def sep():
            ctk.CTkFrame(bar, width=2, height=26, fg_color="#5a626e").pack(side="left", padx=4)

        cmd("打开", self.on_open, 58)
        cmd("新建", self.on_new, 58)
        cmd("保存", self.on_save, 58)
        cmd("另存为", self.on_save_as, 66)
        cmd("导出HTML", self.on_export_html, 80)
        self.var_split_js = ctk.BooleanVar(value=False)
        ctk.CTkCheckBox(bar, text="分离JS", variable=self.var_split_js, width=78, height=26,
                        font=ctk.CTkFont(size=12), checkbox_width=18, checkbox_height=18,
                        fg_color="#3b82f6", hover_color="#2563eb").pack(side="left", padx=(6, 2))
        sep()
        self.btn_undo = cmd("撤销", self.on_undo, 58)
        self.btn_redo = cmd("重做", self.on_redo, 58)
        sep()
        cmd("新增页", self.on_add_slide, 64)
        cmd("复制页", self.on_dup_slide, 64)
        cmd("删除页", self.on_del_slide, 64)
        sep()
        cmd("文本框", self.on_add_text, 64)
        cmd("图片", self.on_add_picture, 54)
        cmd("删形状", self.on_del_shape, 64)

        # ---- 第二行功能区（字体/字形/颜色/对齐/插入）----
        bar2 = ctk.CTkFrame(self, height=42, corner_radius=0, fg_color="#33383f")
        bar2.pack(fill="x", side="top")
        ctk.CTkButton(bar2, text="插件", width=54, height=28, font=FONT_S,
                      fg_color="#39404a", hover_color="#4a525e",
                      command=self.on_plugins).pack(side="right", padx=(2, 10), pady=7)

        def sep2():
            ctk.CTkFrame(bar2, width=2, height=26, fg_color="#5a626e").pack(side="left", padx=5)

        ctk.CTkLabel(bar2, text="字体", font=FONT_S, text_color=TEXT_SUB,
                     width=34).pack(side="left", padx=(10, 2), pady=6)
        self.cb_font = ctk.CTkComboBox(bar2, width=132, height=28, font=FONT_S,
                                       values=["Microsoft YaHei", "宋体", "黑体", "微软雅黑",
                                               "等线", "楷体", "Arial", "Calibri", "Times New Roman"],
                                       command=lambda _v: self.on_fontname())
        self.cb_font.pack(side="left", pady=6)
        self.cb_font.bind("<Return>", lambda _e: self.on_fontname())

        self.cb_size = ctk.CTkComboBox(bar2, width=66, height=28, font=FONT_S,
                                       values=["12", "14", "16", "18", "20", "24", "28",
                                               "32", "36", "40", "44", "54", "66", "80", "96"],
                                       command=lambda _v: self.on_fontsize())
        self.cb_size.pack(side="left", padx=(6, 2), pady=6)
        self.cb_size.set("18")
        self.cb_size.bind("<Return>", lambda _e: self.on_fontsize())

        self.tb_bold = ctk.CTkButton(bar2, text="B", width=32, height=28,
                                     font=("Georgia", 13, "bold"), fg_color="#39404a",
                                     hover_color="#4a525e", command=lambda: self.on_style_toggle("bold"))
        self.tb_bold.pack(side="left", padx=1, pady=6)
        self.tb_italic = ctk.CTkButton(bar2, text="I", width=32, height=28,
                                       font=("Georgia", 13, "italic"), fg_color="#39404a",
                                       hover_color="#4a525e", command=lambda: self.on_style_toggle("italic"))
        self.tb_italic.pack(side="left", padx=1, pady=6)
        self.tb_underline = ctk.CTkButton(bar2, text="U", width=32, height=28,
                                          font=("Georgia", 13, "underline"), fg_color="#39404a",
                                          hover_color="#4a525e", command=lambda: self.on_style_toggle("underline"))
        self.tb_underline.pack(side="left", padx=1, pady=6)

        sep2()
        self.btn_tcolor = ctk.CTkButton(bar2, text="A 文字色", width=70, height=28, font=FONT_S,
                                        fg_color="#39404a", hover_color="#4a525e",
                                        command=self.on_pick_textcolor)
        self.btn_tcolor.pack(side="left", padx=2, pady=6)
        self.sw_tcolor2 = tk.Label(bar2, width=3, bg="#222222", relief="solid", bd=1)
        self.sw_tcolor2.pack(side="left", padx=(0, 6), pady=6)

        self.seg_align2 = ctk.CTkSegmentedButton(bar2, values=["左", "中", "右"], font=FONT_S,
                                                 width=112, command=self.on_align)
        self.seg_align2.pack(side="left", padx=4, pady=6)

        sep2()
        self.cb_shape = ctk.CTkComboBox(bar2, width=104, height=28, font=FONT_S,
                                        values=["矩形", "圆角矩形", "椭圆", "三角形", "菱形",
                                                "五边形", "六边形", "箭头", "五角星", "直线"],
                                        state="readonly")
        self.cb_shape.pack(side="left", padx=(4, 2), pady=6)
        self.cb_shape.set("矩形")
        ctk.CTkButton(bar2, text="插入形状", width=76, height=28, font=FONT_S,
                      fg_color="#2f6df6", hover_color="#3f7bff",
                      command=self.on_add_shape).pack(side="left", padx=2, pady=6)
        ctk.CTkButton(bar2, text="插入表格", width=76, height=28, font=FONT_S,
                      fg_color="#39404a", hover_color="#4a525e",
                      command=self.on_add_table).pack(side="left", padx=2, pady=6)
        ctk.CTkButton(bar2, text="导入Excel", width=80, height=28, font=FONT_S,
                      fg_color="#39404a", hover_color="#4a525e",
                      command=self.on_import_excel).pack(side="left", padx=2, pady=6)
        ctk.CTkButton(bar2, text="形状库…", width=74, height=28, font=FONT_S,
                      fg_color="#39404a", hover_color="#4a525e",
                      command=self.open_shape_library).pack(side="left", padx=2, pady=6)
        ctk.CTkButton(bar2, text="插入网页", width=76, height=28, font=FONT_S,
                      fg_color="#39404a", hover_color="#4a525e",
                      command=self.on_add_html).pack(side="left", padx=2, pady=6)
        ctk.CTkButton(bar2, text="复制形状", width=76, height=28, font=FONT_S,
                      fg_color="#39404a", hover_color="#4a525e",
                      command=self.on_dup_shape).pack(side="left", padx=2, pady=6)
        self.sw_tcolor2.configure(bg="#222222")

        # ---- 主体三栏 ----
        body = ctk.CTkFrame(self, corner_radius=0, fg_color="#2b2f36")
        body.pack(fill="both", expand=True)

        # 左：缩略图
        left = ctk.CTkFrame(body, width=196, corner_radius=0, fg_color="#33383f")
        left.pack(side="left", fill="y")
        left.pack_propagate(False)
        ctk.CTkLabel(left, text="幻灯片", font=("Microsoft YaHei UI", 13, "bold"),
                     anchor="w").pack(fill="x", padx=10, pady=(10, 4))
        self.thumb_box = ctk.CTkScrollableFrame(left, width=176, fg_color="#33383f")
        self.thumb_box.pack(fill="both", expand=True, padx=6, pady=(0, 8))

        # 右：属性（必须在中间画布之前 pack，否则会被 expand 的画布挤掉）
        right = ctk.CTkFrame(body, width=272, corner_radius=0, fg_color="#33383f")
        right.pack(side="right", fill="y")
        right.pack_propagate(False)
        self.props_scroll = ctk.CTkScrollableFrame(right, width=252, fg_color="#33383f",
                                                   scrollbar_button_color="#4a5563")
        self.props_scroll.pack(fill="both", expand=True)
        self._build_props(self.props_scroll)

        # 中：画布
        mid = ctk.CTkFrame(body, corner_radius=0, fg_color="#3f444b")
        mid.pack(side="left", fill="both", expand=True)
        self.canvas = SlideCanvas(mid, self.core, on_change=self._after_edit,
                                  on_select=self.on_shape_selected, status=self.set_status)
        self.canvas.pack(fill="both", expand=True, padx=8, pady=8)
        # 文字编辑框里按 Ctrl+S 时：先提交文字，再直接存盘
        self.canvas.save_cb = self.on_save
        # 形状附加信息（脚本 / 视频音频 / 程序）
        self.attach = yc_attach.empty()
        self.canvas.attach = self.attach["shapes"]
        self.canvas.menu_handler = self.on_shape_menu

        # ---- 状态栏 ----
        self.status_var = tk.StringVar(value="就绪 — 打开 .pptx 开始编辑")
        sb = ctk.CTkFrame(self, height=26, corner_radius=0, fg_color="#272b31")
        sb.pack(fill="x", side="bottom")
        ctk.CTkLabel(sb, textvariable=self.status_var, anchor="w", font=FONT_S,
                     text_color="#d6dfeb").pack(fill="both", padx=10)

    def _build_props(self, parent):
        pad = {"padx": 10}
        ctk.CTkLabel(parent, text="属性", font=("Microsoft YaHei UI", 13, "bold"),
                     anchor="w").pack(fill="x", pady=(10, 2), **pad)

        self.lb_info = ctk.CTkLabel(parent, text="未选中形状", anchor="w", justify="left",
                                    font=FONT_S, text_color="#9fb0c3", wraplength=250)
        self.lb_info.pack(fill="x", pady=(0, 8), **pad)

        def field(label, width=86):
            f = ctk.CTkFrame(parent, fg_color="transparent")
            f.pack(fill="x", pady=3, **pad)
            ctk.CTkLabel(f, text=label, width=52, anchor="w", font=FONT_S).pack(side="left")
            e = ctk.CTkEntry(f, width=width, height=28, font=FONT_S)
            e.pack(side="left")
            ctk.CTkLabel(f, text="pt", width=24, font=FONT_S, text_color="#8b98a8").pack(side="left")
            e.bind("<Return>", lambda _e: self.on_prop_apply())
            e.bind("<FocusOut>", lambda _e: self.on_prop_apply())
            return e

        self.e_x = field("X")
        self.e_y = field("Y")
        self.e_w = field("宽")
        self.e_h = field("高")

        f = ctk.CTkFrame(parent, fg_color="transparent")
        f.pack(fill="x", pady=3, **pad)
        ctk.CTkLabel(f, text="旋转", width=52, anchor="w", font=FONT_S).pack(side="left")
        self.e_rot = ctk.CTkEntry(f, width=86, height=28, font=FONT_S)
        self.e_rot.pack(side="left")
        ctk.CTkLabel(f, text="°", width=24, font=FONT_S, text_color="#8b98a8").pack(side="left")
        self.e_rot.bind("<Return>", lambda _e: self.on_prop_apply())
        self.e_rot.bind("<FocusOut>", lambda _e: self.on_prop_apply())

        f = ctk.CTkFrame(parent, fg_color="transparent")
        f.pack(fill="x", pady=3, **pad)
        ctk.CTkLabel(f, text="字号", width=52, anchor="w", font=FONT_S).pack(side="left")
        self.e_font = ctk.CTkEntry(f, width=86, height=28, font=FONT_S)
        self.e_font.pack(side="left")
        ctk.CTkLabel(f, text="磅", width=24, font=FONT_S, text_color="#8b98a8").pack(side="left")
        self.e_font.bind("<Return>", lambda _e: self.on_prop_apply())
        self.e_font.bind("<FocusOut>", lambda _e: self.on_prop_apply())

        ctk.CTkLabel(parent, text="文字对齐", anchor="w", font=FONT_S).pack(fill="x", pady=(6, 2), **pad)
        self.seg_align = ctk.CTkSegmentedButton(parent, values=["左", "中", "右"], font=FONT_S,
                                                command=self.on_align)
        self.seg_align.pack(fill="x", pady=(0, 6), **pad)

        ctk.CTkLabel(parent, text="形状对齐（Ctrl+单击可多选）", anchor="w",
                     font=FONT_S).pack(fill="x", pady=(6, 2), **pad)
        g1 = ctk.CTkFrame(parent, fg_color="transparent")
        g1.pack(fill="x", **pad)
        for txt, mode in (("左", "left"), ("水平居中", "hcenter"), ("右", "right")):
            ctk.CTkButton(g1, text=txt, height=28, font=FONT_S, fg_color="#4a5563",
                          hover_color="#5a6675", width=78,
                          command=lambda m=mode: self.on_align_shapes(m)).pack(side="left", padx=2)
        g2 = ctk.CTkFrame(parent, fg_color="transparent")
        g2.pack(fill="x", pady=(4, 2), **pad)
        for txt, mode in (("上", "top"), ("垂直居中", "vcenter"), ("下", "bottom")):
            ctk.CTkButton(g2, text=txt, height=28, font=FONT_S, fg_color="#4a5563",
                          hover_color="#5a6675", width=78,
                          command=lambda m=mode: self.on_align_shapes(m)).pack(side="left", padx=2)
        g3 = ctk.CTkFrame(parent, fg_color="transparent")
        g3.pack(fill="x", pady=(0, 4), **pad)
        ctk.CTkButton(g3, text="水平等距", height=28, font=FONT_S, fg_color="#4a5563",
                      hover_color="#5a6675", command=lambda: self.on_distribute("h")).pack(side="left", padx=2)
        ctk.CTkButton(g3, text="垂直等距", height=28, font=FONT_S, fg_color="#4a5563",
                      hover_color="#5a6675", command=lambda: self.on_distribute("v")).pack(side="left", padx=2)

        ctk.CTkLabel(parent, text="表格（双击格子后可在这里加删行列）", anchor="w",
                     font=FONT_S).pack(fill="x", pady=(6, 2), **pad)
        gt = ctk.CTkFrame(parent, fg_color="transparent")
        gt.pack(fill="x", **pad)
        ctk.CTkButton(gt, text="加一行", height=28, font=FONT_S, fg_color="#4a5563",
                      hover_color="#5a6675", width=78,
                      command=lambda: self.on_table_op("add_row")).pack(side="left", padx=2)
        ctk.CTkButton(gt, text="删一行", height=28, font=FONT_S, fg_color="#4a5563",
                      hover_color="#5a6675", width=78,
                      command=lambda: self.on_table_op("del_row")).pack(side="left", padx=2)
        gt2 = ctk.CTkFrame(parent, fg_color="transparent")
        gt2.pack(fill="x", pady=(4, 0), **pad)
        ctk.CTkButton(gt2, text="加一列", height=28, font=FONT_S, fg_color="#4a5563",
                      hover_color="#5a6675", width=78,
                      command=lambda: self.on_table_op("add_col")).pack(side="left", padx=2)
        ctk.CTkButton(gt2, text="删一列", height=28, font=FONT_S, fg_color="#4a5563",
                      hover_color="#5a6675", width=78,
                      command=lambda: self.on_table_op("del_col")).pack(side="left", padx=2)
        gt3 = ctk.CTkFrame(parent, fg_color="transparent")
        gt3.pack(fill="x", pady=(4, 0), **pad)
        ctk.CTkButton(gt3, text="导出该表格为 Excel…", height=28, font=FONT_S,
                      fg_color="#2f6df6", hover_color="#3f7dff", width=172,
                      command=lambda: self._export_table_xlsx(self._sel())).pack(side="left", padx=2)

        self.sw_bold = ctk.CTkSwitch(parent, text="加粗", font=FONT_S, command=self.on_bold)
        self.sw_bold.pack(fill="x", pady=4, **pad)

        ctk.CTkLabel(parent, text="填充色 / 文字色", anchor="w", font=FONT_S).pack(fill="x", pady=(6, 2), **pad)
        cf = ctk.CTkFrame(parent, fg_color="transparent")
        cf.pack(fill="x", **pad)
        self.btn_fill = ctk.CTkButton(cf, text="填充", width=76, height=28, font=FONT_S,
                                      fg_color="#4a5563", hover_color="#5a6675", command=self.on_pick_fill)
        self.btn_fill.pack(side="left", padx=(0, 4))
        self.sw_fill = tk.Label(cf, width=4, bg="#ffffff", relief="solid", bd=1)
        self.sw_fill.pack(side="left", padx=(0, 10))
        self.btn_text = ctk.CTkButton(cf, text="文字", width=76, height=28, font=FONT_S,
                                      fg_color="#4a5563", hover_color="#5a6675", command=self.on_pick_textcolor)
        self.btn_text.pack(side="left", padx=(0, 4))
        self.sw_text = tk.Label(cf, width=4, bg="#222222", relief="solid", bd=1)
        self.sw_text.pack(side="left")

        ctk.CTkLabel(parent, text="层级", anchor="w", font=FONT_S).pack(fill="x", pady=(10, 2), **pad)
        zf = ctk.CTkFrame(parent, fg_color="transparent")
        zf.pack(fill="x", **pad)
        for text, val in (("上移", 1), ("下移", -1), ("置顶", 2), ("置底", -2)):
            ctk.CTkButton(zf, text=text, width=58, height=28, font=FONT_S,
                          fg_color="#4a5563", hover_color="#5a6675",
                          command=lambda v=val: self.on_zorder(v)).pack(side="left", padx=(0, 4))

        ctk.CTkLabel(parent, text="本页切换动画", anchor="w", font=FONT_S).pack(fill="x", pady=(10, 2), **pad)
        tf = ctk.CTkFrame(parent, fg_color="transparent")
        tf.pack(fill="x", **pad)
        self.cb_tr = ctk.CTkComboBox(tf, values=[k for k in TR_NAMES.keys()], width=150, height=28,
                                     font=FONT_S, command=lambda _v: self.on_transition_preview())
        self.cb_tr.pack(side="left")
        tf2 = ctk.CTkFrame(parent, fg_color="transparent")
        tf2.pack(fill="x", pady=3, **pad)
        ctk.CTkLabel(tf2, text="方向", width=40, anchor="w", font=FONT_S).pack(side="left")
        self.cb_tdir = ctk.CTkComboBox(tf2, values=["左", "右", "上", "下"], width=62, height=28, font=FONT_S)
        self.cb_tdir.pack(side="left", padx=(0, 6))
        ctk.CTkLabel(tf2, text="时长", width=36, anchor="w", font=FONT_S).pack(side="left")
        self.e_tdur = ctk.CTkEntry(tf2, width=52, height=28, font=FONT_S)
        self.e_tdur.insert(0, "0.6")
        self.e_tdur.pack(side="left")
        ctk.CTkLabel(tf2, text="秒", width=20, font=FONT_S, text_color=TEXT_DIM).pack(side="left")
        tb = ctk.CTkFrame(parent, fg_color="transparent")
        tb.pack(fill="x", pady=(2, 4), **pad)
        ctk.CTkButton(tb, text="应用到本页", width=94, height=28, font=FONT_S,
                      fg_color="#4a5563", hover_color="#5a6675",
                      command=self.on_apply_transition).pack(side="left", padx=(0, 4))
        ctk.CTkButton(tb, text="应用到全部", width=94, height=28, font=FONT_S,
                      fg_color="#4a5563", hover_color="#5a6675",
                      command=self.on_apply_transition_all).pack(side="left")

        ctk.CTkLabel(parent, text="选中形状的入场动画", anchor="w", font=FONT_S).pack(fill="x", pady=(10, 2), **pad)
        af = ctk.CTkFrame(parent, fg_color="transparent")
        af.pack(fill="x", **pad)
        self.cb_anim = ctk.CTkComboBox(af, values=[k for k in ANIM_NAMES.keys()], width=110, height=28,
                                       font=FONT_S)
        self.cb_anim.pack(side="left", padx=(0, 4))
        self.e_adur = ctk.CTkEntry(af, width=52, height=28, font=FONT_S)
        self.e_adur.insert(0, "0.5")
        self.e_adur.pack(side="left")
        ctk.CTkLabel(af, text="秒", width=20, font=FONT_S, text_color=TEXT_DIM).pack(side="left")
        ab = ctk.CTkFrame(parent, fg_color="transparent")
        ab.pack(fill="x", pady=(2, 4), **pad)
        ctk.CTkButton(ab, text="应用动画", width=94, height=28, font=FONT_S,
                      fg_color="#4a5563", hover_color="#5a6675",
                      command=self.on_apply_anim).pack(side="left", padx=(0, 4))
        ctk.CTkButton(ab, text="清空本页", width=94, height=28, font=FONT_S,
                      fg_color="#4a5563", hover_color="#5a6675",
                      command=self.on_clear_anim).pack(side="left")

        ctk.CTkLabel(parent, text="提示：画布上拖动可移动，拖白点改大小，拖上方圆点旋转，\n双击改文字；Ctrl+滚轮缩放。导出的放映底部有画笔/荧光笔/橡皮。",
                     anchor="w", justify="left", font=("Microsoft YaHei UI", 10),
                     text_color=TEXT_DIM, wraplength=250).pack(fill="x", pady=(14, 6), **pad)
        self._brighten_labels(parent)

    def _brighten_labels(self, w):
        """暗色界面下统一提亮文字，避免灰底灰字看不清"""
        for c in w.winfo_children():
            try:
                if isinstance(c, (ctk.CTkLabel, ctk.CTkSwitch)):
                    c.configure(text_color=TEXT_SUB)
            except Exception:
                pass
            self._brighten_labels(c)

    def _bind_keys(self):
        self.bind("<Control-o>", lambda e: self.on_open())
        self.bind("<Control-s>", lambda e: self.on_save())
        self.bind("<Control-Shift-S>", lambda e: self.on_save_as())
        self.bind("<Control-z>", lambda e: self.on_undo())
        self.bind("<Control-y>", lambda e: self.on_redo())
        self.bind("<Control-t>", lambda e: self.on_add_text())
        self.bind("<Control-b>", lambda e: self.on_style_toggle("bold"))
        self.bind("<Control-i>", lambda e: self.on_style_toggle("italic"))
        self.bind("<Control-u>", lambda e: self.on_style_toggle("underline"))
        self.bind("<Control-d>", lambda e: self.on_dup_shape())
        self.bind("<Delete>", lambda e: self.on_del_shape())
        self.bind("<Prior>", lambda e: self.goto(self.canvas.slide_idx - 1))
        self.bind("<Next>", lambda e: self.goto(self.canvas.slide_idx + 1))
        self.bind("<Left>", lambda e: self._nudge(-1, 0))
        self.bind("<Right>", lambda e: self._nudge(1, 0))
        self.bind("<Up>", lambda e: self._nudge(0, -1))
        self.bind("<Down>", lambda e: self._nudge(0, 1))
        self.bind("<Escape>", lambda e: self.canvas.select(-1))

    # ================= 文件 =================
    def _blank_template(self):
        root = _app_root()
        for rel in (("templates", "空白.pptx"), ("build", "blank.pptx")):
            p = os.path.normpath(os.path.join(root, *rel))
            if os.path.exists(p):
                return p
        return None

    def on_new(self):
        tpl = self._blank_template()
        if not tpl:
            messagebox.showerror("新建失败", "缺少空白模板 templates\\空白.pptx", parent=self)
            return
        try:
            self.core.open(tpl)
            # 真空白：模板里残留的占位符全部删掉（否则新建稿会带“二级占位符文本”）
            while self.core.slide_count:
                n = len(self.core.shapes(0))
                if n <= 0:
                    break
                self.core.shape_delete(0, n - 1)
        except Exception as e:
            messagebox.showerror("新建失败", str(e), parent=self)
            return
        self.path = None                     # 未保存过：点“保存”会走另存为
        self.attach = yc_attach.empty()
        self.canvas.attach = self.attach["shapes"]
        self.title("ycz 编辑器 — 未命名")
        self.refresh_thumbs()
        self.goto(0)
        self._update_undo_buttons()
        self.set_status("已新建空白演示文稿（保存时另存为 .pptx）")

    def _need_doc(self):
        if self.core.doc:
            return True
        if messagebox.askyesno("还没有演示文稿", "当前没有打开的文件，要新建一个空白演示文稿吗？",
                               parent=self):
            self.on_new()
        return False

    def load_file(self, path):
        try:
            self.core.open(path)
        except Exception as e:
            messagebox.showerror("打开失败", str(e), parent=self)
            return
        self.path = path
        self.attach = yc_attach.load(path)
        self.canvas.attach = self.attach["shapes"]
        self.title(f"ycz 编辑器 — {os.path.basename(path)}")
        self.refresh_thumbs()
        self.goto(0)
        self._update_undo_buttons()
        self.set_status(f"已打开 {path}（{self.core.slide_count} 页）")

    def on_open(self):
        p = filedialog.askopenfilename(title="打开演示文稿",
                                       filetypes=[("PowerPoint", "*.pptx"), ("所有文件", "*.*")])
        if p:
            self.load_file(p)

    def on_save(self):
        if self.canvas._edit is not None:
            self.canvas.commit_text_edit(False)
        if not self.path:
            return self.on_save_as()
        try:
            self.core.save(self.path)
            yc_attach.save(self.path, self.attach)
            self.set_status("已保存 " + self.path)
        except Exception as e:
            messagebox.showerror("保存失败", str(e), parent=self)

    def on_save_as(self):
        p = filedialog.asksaveasfilename(title="另存为", defaultextension=".pptx",
                                         initialfile=os.path.basename(self.path or "演示文稿.pptx"),
                                         filetypes=[("PowerPoint", "*.pptx")])
        if not p:
            return
        try:
            self.core.save(p)
            self.path = p
            yc_attach.save(self.path, self.attach)
            self.title(f"ycz 编辑器 — {os.path.basename(p)}")
            self.set_status("已保存 " + p)
        except Exception as e:
            messagebox.showerror("保存失败", str(e), parent=self)

    def on_export_html(self):
        if not self._need_doc():
            return
        p = filedialog.asksaveasfilename(title="导出 HTML 放映", defaultextension=".html",
                                         initialfile=os.path.splitext(os.path.basename(self.path or "放映"))[0] + ".html",
                                         filetypes=[("HTML", "*.html")])
        if not p:
            return
        try:
            split = bool(self.var_split_js.get())
            self.core.export_html(p, split_js=split)
            injected = yc_attach.inject(p, self.attach)
            if split:
                self.set_status("已导出 " + p + "，脚本已分离为同名 _1.js（放一起才能播放）")
            else:
                self.set_status("已导出 " + p + ("（附加脚本已注入）" if injected else "（单文件）"))
        except Exception as e:
            messagebox.showerror("导出失败", str(e), parent=self)

    # ================= 插件 =================
    def on_plugins(self):
        if getattr(self, "_plug_win", None) is not None:
            try:
                if self._plug_win.winfo_exists():
                    self._plug_win.lift()
                    self._plug_win.focus()
                    return
            except Exception:
                pass
        win = ctk.CTkToplevel(self)
        self._plug_win = win
        win.title("插件")
        win.geometry("460x500")
        win.transient(self)
        win.configure(fg_color="#2b2f36")

        head = ctk.CTkFrame(win, fg_color="transparent")
        head.pack(fill="x", padx=14, pady=(12, 4))
        ctk.CTkLabel(head, text="已加载插件", font=("Microsoft YaHei UI", 13, "bold"),
                     text_color="#eef3fa").pack(side="left")
        self._plug_dir_lb = ctk.CTkLabel(head, text="", font=FONT_S, text_color="#9fb0c6")
        self._plug_dir_lb.pack(side="right")

        self._plug_list = ctk.CTkScrollableFrame(win, height=150, fg_color="#33383f")
        self._plug_list.pack(fill="both", expand=True, padx=14, pady=6)

        row = ctk.CTkFrame(win, fg_color="transparent")
        row.pack(fill="x", padx=14, pady=(4, 2))
        self._plug_cmd = ctk.CTkEntry(row, height=30, font=FONT_S,
                                      placeholder_text="命令，例如 stats 或 watermark 文字")
        self._plug_cmd.pack(side="left", fill="x", expand=True)
        ctk.CTkButton(row, text="执行", width=64, height=30, font=FONT_S,
                      command=self._run_plugin_cmd).pack(side="left", padx=(6, 0))

        self._plug_out = tk.Text(win, height=5, bg="#1f2328", fg="#dbe4ef",
                                 insertbackground="#dbe4ef", relief="flat",
                                 font=("Consolas", 10), wrap="word")
        self._plug_out.pack(fill="both", expand=False, padx=14, pady=(4, 6))

        bottom = ctk.CTkFrame(win, fg_color="transparent")
        bottom.pack(fill="x", padx=14, pady=(0, 12))
        ctk.CTkButton(bottom, text="重新加载插件", width=110, height=28, font=FONT_S,
                      fg_color="#4a5563", hover_color="#5a6675",
                      command=self._reload_plugins).pack(side="left")
        ctk.CTkButton(bottom, text="插件说明", width=90, height=28, font=FONT_S,
                      fg_color="#4a5563", hover_color="#5a6675",
                      command=self._plugin_help).pack(side="left", padx=6)

        self._refresh_plugin_list()

    def _plugin_dir(self):
        return os.path.join(_app_root(), "build", "plugins")

    def _refresh_plugin_list(self):
        for w in self._plug_list.winfo_children():
            w.destroy()
        try:
            tags = self.core.plugins()
        except Exception as e:
            tags = []
            self._plug_out.insert("end", f"插件读取失败：{e}\n")
        if not tags:
            ctk.CTkLabel(self._plug_list, text="未加载插件（检查 build\\plugins 目录）",
                         font=FONT_S, text_color="#c7d2e0").pack(anchor="w", padx=6, pady=6)
        for p in tags:
            box = ctk.CTkFrame(self._plug_list, fg_color="#3c434d", corner_radius=6)
            box.pack(fill="x", pady=3, padx=2)
            ctk.CTkLabel(box, text=p["name"], font=("Microsoft YaHei UI", 12, "bold"),
                         text_color="#eef3fa", anchor="w").pack(fill="x", padx=8, pady=(5, 0))
            ctk.CTkLabel(box, text=p["desc"], font=FONT_S, text_color="#c2cdda",
                         anchor="w", justify="left", wraplength=360).pack(fill="x", padx=8, pady=(0, 5))
        try:
            self._plug_dir_lb.configure(text=os.path.basename(self._plugin_dir()))
        except Exception:
            pass

    def _reload_plugins(self):
        try:
            n = self.core.plugins_load(self._plugin_dir())
            self._refresh_plugin_list()
            self._plug_out.insert("end", f"已重新加载 {n} 个插件\n")
        except Exception as e:
            self._plug_out.insert("end", f"加载失败：{e}\n")

    def _run_plugin_cmd(self):
        line = self._plug_cmd.get().strip()
        if not line:
            return
        cmd, _, arg = line.partition(" ")
        try:
            ok, text = self.core.plugin_command(cmd, arg)
            self._plug_out.insert("end", (text or "") + ("" if ok else f"[{cmd}] 没有插件处理\n"))
            self._plug_out.see("end")
        except Exception as e:
            self._plug_out.insert("end", f"执行失败：{e}\n")

    def _plugin_help(self):
        self._plug_out.insert(
            "end",
            "插件说明：\n"
            "· 进度条：导出的 HTML 底部显示翻页进度条\n"
            "· 水印：导出的 HTML 每页右下角加水印，命令 watermark 文字 可改\n"
            "· 统计：命令 stats 输出页数与每页标题概览\n"
            "· 网页嵌入：导出放映时把本地网页以 iframe 加载到某一页\n"
            "    命令 web 页码 文件.html 左% 上% 宽% 高%   （如 web 1 D:\\a.html 8 10 84 74）\n"
            "    命令 webdel 页码 删除、web clear 清空、web 查看当前列表\n"
            "    （编辑器里的【插入网页】会自动帮你设置好，一般不用手敲）\n"
            "把插件 DLL 放到 build\\plugins 目录即可被自动加载。\n")
        self._plug_out.see("end")

    # ================= 页面 =================
    def refresh_thumbs(self):
        for w in self.thumb_box.winfo_children():
            w.destroy()
        self._thumb_imgs = []
        if not self._need_doc():
            return
        for i in range(self.core.slide_count):
            try:
                data, w, h = self.core.render(i, 168, 95)
                img = ctk.CTkImage(light_image=pil_from(data, w, h), size=(168, 95))
            except Exception:
                img = None
            self._thumb_imgs.append(img)
            cell = ctk.CTkFrame(self.thumb_box, fg_color="#33383f")
            cell.pack(fill="x", pady=3)
            lb = ctk.CTkLabel(cell, text=f"  {i + 1}", image=img, compound="top",
                              font=FONT_S, text_color="#e2e9f3")
            lb.pack(fill="x")
            for wgt in (cell, lb):
                wgt.bind("<Button-1>", lambda e, k=i: self.goto(k))
            if i == self.canvas.slide_idx:
                cell.configure(fg_color="#3d5169")

    def goto(self, idx):
        if not self._need_doc():
            return
        idx = max(0, min(idx, self.core.slide_count - 1))
        changed = getattr(self, "_last_page", None) != idx
        self.canvas.set_page(idx)
        self._last_page = idx
        self._highlight_thumb(idx)
        self.on_shape_selected(-1)
        self._sync_transition()
        if changed:
            self.canvas.after(40, self.canvas.fade_in)

    def _highlight_thumb(self, idx):
        kids = self.thumb_box.winfo_children()
        for k, w in enumerate(kids):
            try:
                w.configure(fg_color="#3d5169" if k == idx else "#33383f")
            except Exception:
                pass

    def on_add_slide(self):
        if not self._need_doc():
            return
        i = self.core.slide_add(self.canvas.slide_idx)
        self.refresh_thumbs()
        self.goto(i)
        self._update_undo_buttons()
        self.set_status(f"已新增第 {i + 1} 页")

    def on_dup_slide(self):
        if not self._need_doc():
            return
        i = self.core.slide_duplicate(self.canvas.slide_idx)
        self.refresh_thumbs()
        self.goto(i)
        self._update_undo_buttons()
        self.set_status(f"已复制为第 {i + 1} 页")

    def on_del_slide(self):
        if not self._need_doc():
            return
        if self.core.slide_count <= 1:
            messagebox.showinfo("提示", "至少保留一页", parent=self)
            return
        i = self.canvas.slide_idx
        try:
            self.core.slide_remove(i)
        except Exception as e:
            messagebox.showerror("删除失败", str(e), parent=self)
            return
        self.refresh_thumbs()
        self.goto(min(i, self.core.slide_count - 1))
        self._update_undo_buttons()
        self.set_status("已删除一页")

    # ================= 形状 =================
    def _sel(self):
        return self.canvas.sel

    def on_add_text(self):
        if not self._need_doc():
            return
        w = self.core.slide_w
        h = self.core.slide_h
        i = self.core.shape_add_rect(self.canvas.slide_idx, int(w * 0.12), int(h * 0.42),
                                     int(w * 0.46), int(h * 0.13), "新文本框")
        self.canvas.reload(keep_sel=False)
        self.canvas.select(i)
        self._update_undo_buttons()
        self.set_status("已插入文本框，双击可改文字")

    def on_add_shape(self):
        if not self._need_doc():
            return
        name = self.cb_shape.get()
        geom = {"矩形": "rect", "圆角矩形": "roundRect", "椭圆": "ellipse", "三角形": "triangle",
                "菱形": "diamond", "五边形": "pentagon", "六边形": "hexagon",
                "箭头": "rightArrow", "五角星": "star5", "直线": "line"}.get(name, "rect")
        w, h = self.core.slide_w, self.core.slide_h
        cx, cy = int(w * 0.24), int(h * 0.24)
        if geom == "line":
            cy = 0
            cx = int(w * 0.34)
            fill, line = 0, 0x404040
        else:
            fill, line = 0x4472C4, 0
        x = int((w - cx) / 2)
        y = int((h - cy) / 2) if cy else int(h * 0.5)
        idx = self.core.shape_add_shape(self.canvas.slide_idx, geom, x, y, cx, cy, fill, line)
        self.canvas.reload(keep_sel=False)
        self.canvas.select(idx)
        self._update_undo_buttons()
        self.set_status(f"已插入{name}")

    def _insert_prst(self, geom, name=""):
        if not self._need_doc():
            return
        w, h = self.core.slide_w, self.core.slide_h
        line_like = geom in ("line", "straightConnector1", "bentConnector3",
                             "bentConnector2", "curvedConnector3", "arc")
        if geom == "line":
            cx, cy = int(w * 0.32), 0
            x, y = int((w - cx) / 2), int(h * 0.55)
        else:
            cx, cy = int(w * 0.26), int(h * 0.26)
            x, y = int((w - cx) / 2), int((h - cy) / 2)
        fill, line = (0, 0x404040) if line_like else (0x4472C4, 0)
        idx = self.core.shape_add_shape(self.canvas.slide_idx, geom, x, y, cx, cy, fill, line)
        self.canvas.reload(keep_sel=False)
        self.canvas.select(idx)
        self._update_undo_buttons()
        self.set_status("已插入%s" % (name or geom))

    def open_shape_library(self):
        if not self._need_doc():
            return
        groups = [
            ("基本形状", ["rect 矩形", "roundRect 圆角矩形", "ellipse 椭圆", "triangle 三角形",
                          "rtTriangle 直角三角形", "diamond 菱形", "trapezoid 梯形",
                          "parallelogram 平行四边形", "homePlate 五边形", "hexagon 六边形",
                          "star5 五角星", "heart 心形", "cloud 云形", "sun 太阳",
                          "moon 月亮", "lightningBolt 闪电", "smileyFace 笑脸",
                          "cloudCallout 标注"]),
            ("线条 / 箭头", ["line 直线", "bentConnector3 折线", "curvedConnector3 曲线",
                             "rightArrow 右箭头", "leftArrow 左箭头", "upArrow 上箭头",
                             "downArrow 下箭头", "arc 弧形", "plus 加号", "minus 减号",
                             "equal 等号", "mult 乘号", "divide 除号",
                             "checkmark 对号", "xmark 叉号"]),
            ("立体图形", ["cube 立方体", "can 圆柱", "pyramid 四棱锥", "sphere 球",
                          "prism3 三棱柱", "ribbon 缎带", "ribbon2 缎带(反)"]),
            ("流程图", ["flowChartProcess 处理", "flowChartDecision 判断",
                        "flowChartTerminator 起止", "flowChartDocument 文档",
                        "flowChartData 数据", "noSmoking 禁止"]),
        ]
        win = ctk.CTkToplevel(self)
        win.title("形状库 — 点一下就插入")
        win.geometry("620x560")
        win.configure(fg_color="#242a31")
        win.transient(self)
        ctk.CTkLabel(win, text="形状库（单击插入到当前页中央）",
                     font=("Microsoft YaHei UI", 13, "bold"),
                     text_color=TEXT_MAIN).pack(pady=(10, 4))
        sf = ctk.CTkScrollableFrame(win, fg_color="#20252c")
        sf.pack(fill="both", expand=True, padx=10, pady=(0, 10))
        for title, items in groups:
            ctk.CTkLabel(sf, text=title, font=FONT_S, text_color="#7fb4ff",
                         anchor="w").pack(fill="x", pady=(8, 4))
            grid = ctk.CTkFrame(sf, fg_color="transparent")
            grid.pack(fill="x")
            for n, it in enumerate(items):
                geom, name = it.split(" ", 1)
                ctk.CTkButton(grid, text=name, width=100, height=30, font=FONT_S,
                              fg_color="#39404a", hover_color="#4b5768",
                              command=lambda g=geom, nm=name: (self._insert_prst(g, nm),
                                                              win.destroy())
                              ).grid(row=n // 5, column=n % 5, padx=4, pady=4)
        ctk.CTkButton(win, text="关闭", width=90, height=30, font=FONT_S,
                      fg_color="#555f6d", command=win.destroy).pack(pady=(0, 10))

    def on_import_excel(self):
        if not self._need_doc():
            return
        p = filedialog.askopenfilename(title="导入 Excel 表格",
                                       filetypes=[("Excel 工作簿", "*.xlsx"), ("所有文件", "*.*")])
        if not p:
            return
        try:
            rows, sheet_name = yc_xlsx_in.read_first_sheet(p)
        except Exception as ex:
            messagebox.showerror("导入失败", "读不了这个 Excel：\n%s" % ex, parent=self)
            return
        if not rows:
            self.set_status("这个工作表是空的")
            return
        w, h = self.core.slide_w, self.core.slide_h
        nr = len(rows)
        nc = max(1, max(len(r) for r in rows))
        tw = min(int(w * 0.86), int(w * 0.085 * nc))
        th = min(int(h * 0.72), int(h * 0.055 * nr))
        idx = self.core.shape_add_table(self.canvas.slide_idx,
                                        int((w - tw) / 2), int((h - th) / 2), tw, th, nr, nc)
        for r, row in enumerate(rows):
            for c in range(nc):
                self.core.table_cell_set_text(self.canvas.slide_idx, idx, r, c,
                                              row[c] if c < len(row) else "")
        self.canvas.reload(keep_sel=False)
        self.canvas.select(idx)
        self._update_undo_buttons()
        self.set_status("已导入《%s》：%d 行 %d 列" % (sheet_name, nr, nc))
        self._toast("已导入 Excel 表格")

    def on_add_table(self):
        if not self._need_doc():
            return
        w, h = self.core.slide_w, self.core.slide_h
        rows, cols = 3, 4
        cx, cy = int(w * 0.7), int(h * 0.42)
        idx = self.core.shape_add_table(self.canvas.slide_idx, int(w * 0.15), int(h * 0.24),
                                        cx, cy, rows, cols)
        self.canvas.reload(keep_sel=False)
        self.canvas.select(idx)
        self._update_undo_buttons()
        self.set_status(f"已插入 {rows}行{cols}列 表格")

    def on_dup_shape(self):
        i = self._sel()
        if i < 0:
            return
        try:
            ni = self.core.shape_duplicate(self.canvas.slide_idx, i)
        except Exception as e:
            messagebox.showerror("复制失败", str(e), parent=self)
            return
        self.canvas.reload(keep_sel=False)
        self.canvas.select(ni)
        self._update_undo_buttons()
        self.set_status("已复制形状")

    def on_fontname(self):
        i = self._sel()
        if self._busy or i < 0 or not self.core.doc:
            return
        name = self.cb_font.get().strip()
        if not name:
            return
        try:
            self.core.shape_set_fontname(self.canvas.slide_idx, i, name)
            self.canvas.reload()
            self._update_undo_buttons()
            self._refresh_current_thumb()
            self.set_status("字体 → " + name)
        except Exception as e:
            self.set_status("设置字体失败: " + str(e))

    def on_fontsize(self):
        i = self._sel()
        if self._busy or i < 0 or not self.core.doc:
            return
        try:
            pt = float(self.cb_size.get())
        except Exception:
            return
        try:
            self.core.shape_set_fontsize(self.canvas.slide_idx, i, pt)
            self.canvas.reload()
            self._sync_props(self.canvas.selected_shape())
            self._update_undo_buttons()
            self._refresh_current_thumb()
            self.set_status(f"字号 → {pt:g} 磅")
        except Exception as e:
            self.set_status("设置字号失败: " + str(e))

    def on_style_toggle(self, kind):
        i = self._sel()
        if i < 0 or not self.core.doc:
            return
        si = self.canvas.slide_idx
        try:
            if kind == "bold":
                on = 0 if self.core.shape_text_bold(si, i) else 1
                self.core.shape_set_bold(si, i, on)
            elif kind == "italic":
                on = 0 if self.core.shape_text_italic(si, i) else 1
                self.core.shape_set_italic(si, i, on)
            else:
                on = 0 if self.core.shape_text_underline(si, i) else 1
                self.core.shape_set_underline(si, i, on)
            self.canvas.reload()
            self._sync_props(self.canvas.selected_shape())
            self._update_undo_buttons()
            self._refresh_current_thumb()
        except Exception as e:
            self.set_status("样式设置失败: " + str(e))

    def on_add_picture(self):
        if not self._need_doc():
            return
        p = filedialog.askopenfilename(
            title="插入图片（PNG/JPG/GIF/BMP/WEBP/TIFF/ICO 都能用）",
            filetypes=[("图片", "*.png *.jpg *.jpeg *.gif *.bmp *.webp *.tif *.tiff *.ico"),
                       ("所有文件", "*.*")])
        if not p:
            return
        src = p
        ext = os.path.splitext(p)[1].lower()
        if ext not in (".png", ".jpg", ".jpeg"):
            # 其他格式先转成 PNG，PowerPoint/网页播放兼容性最好
            try:
                from PIL import Image as _Img
                import tempfile
                im = _Img.open(p)
                if im.mode not in ("RGBA", "RGB"):
                    im = im.convert("RGBA")
                fd, src = tempfile.mkstemp(prefix="img_", suffix=".png")
                os.close(fd)
                im.save(src, "PNG")
            except Exception as e:
                messagebox.showerror("格式转换失败", "这个文件读不出图片数据：\n" + str(e), parent=self)
                return
        try:
            i = self.core.shape_add_picture(self.canvas.slide_idx, src,
                                            int(self.core.slide_w * 0.1), int(self.core.slide_h * 0.15))
        except Exception as e:
            messagebox.showerror("插入失败", str(e), parent=self)
            return
        self.canvas.reload(keep_sel=False)
        self.canvas.select(i)
        self._update_undo_buttons()
        self.set_status("已插入图片 " + os.path.basename(p))

    def on_add_html(self):
        """把本地 HTML 截成图片插入当前页（画布、pptx、放映都能看到）"""
        if not self._need_doc():
            return
        p = filedialog.askopenfilename(title="选择要放进幻灯片的网页",
                                       filetypes=[("网页", "*.html *.htm"), ("所有文件", "*.*")])
        if not p:
            return
        w = int(self.core.slide_w * 0.74)
        h = int(w * 0.5625)
        self.set_status("正在渲染网页…（用 Edge 无头模式截图）")
        self.update_idletasks()
        try:
            png = html_to_png(p, width=1600, height=int(1600 * h / w))
        except Exception as e:
            messagebox.showerror("网页渲染失败", str(e), parent=self)
            return
        try:
            x0 = int((self.core.slide_w - w) / 2)
            y0 = int((self.core.slide_h - h) / 2)
            i = self.core.shape_add_picture(self.canvas.slide_idx, png, x0, y0, w, h)
        except Exception as e:
            messagebox.showerror("插入失败", str(e), parent=self)
            return
        # 同时告诉“网页嵌入”插件：放映时这块区域用实时网页覆盖（可选）
        try:
            sw, sh = self.core.slide_w, self.core.slide_h
            self.core.plugin_command(
                "web", "%d %s %.2f %.2f %.2f %.2f" % (
                    self.canvas.slide_idx + 1, p, x0 / sw * 100, y0 / sh * 100,
                    w / sw * 100, h / sh * 100))
        except Exception:
            pass
        self.canvas.reload(keep_sel=False)
        self.canvas.select(i)
        self._update_undo_buttons()
        self.set_status("已把网页放入当前页：" + os.path.basename(p))

    def on_del_shape(self):
        i = self._sel()
        if i < 0:
            return
        try:
            self.core.shape_delete(self.canvas.slide_idx, i)
        except Exception as e:
            messagebox.showerror("删除失败", str(e), parent=self)
            return
        self.canvas.select(-1)
        self.canvas.reload(keep_sel=False)
        self._after_edit()
        self.set_status("已删除形状")

    def on_zorder(self, d):
        i = self._sel()
        if i < 0:
            return
        ni = self.core.shape_zorder(self.canvas.slide_idx, i, d)
        self.canvas.reload(keep_sel=False)
        self.canvas.select(ni)
        self._after_edit()

    def _nudge(self, dx, dy):
        i = self._sel()
        if i < 0 or not self.core.doc:
            return
        step = 12700 if dx or dy else 12700
        try:
            self.core.shape_move(self.canvas.slide_idx, i, dx * step, dy * step)
            self.canvas.reload()
            self._sync_props(self.canvas.selected_shape())
        except Exception:
            pass

    # ================= 属性 =================
    def on_shape_selected(self, i):
        if not self._need_doc():
            return
        sh = self.canvas.selected_shape()
        if not sh:
            self.lb_info.configure(text="未选中形状\n（单击画布中的形状可选中）")
            return
        self._sync_props(sh)

    def _sync_props(self, sh):
        if not sh:
            return
        self._busy = True
        self.lb_info.configure(text=f"[{self.canvas.sel}] {sh['name']} · {sh['kind_name']}\nid={sh['id']}")
        for e, v in ((self.e_x, sh["x"]), (self.e_y, sh["y"]),
                     (self.e_w, sh["cx"]), (self.e_h, sh["cy"])):
            e.delete(0, "end")
            e.insert(0, f"{v / EMU_PER_PT:.1f}")
        self.e_rot.delete(0, "end")
        self.e_rot.insert(0, f"{sh['rot']:.0f}")
        self.e_font.delete(0, "end")
        if sh["fontsize_pt"] and sh["fontsize_pt"] > 0:
            self.e_font.insert(0, f"{sh['fontsize_pt']:.1f}")
        else:
            self.e_font.insert(0, "-")
        rgb = sh["fill_rgb"] or 0xFFFFFF
        self.sw_fill.configure(bg=f"#{rgb:06x}")
        try:
            tc = self.core.shape_text_color(self.canvas.slide_idx, self.canvas.sel)
            if tc >= 0:
                self.sw_text.configure(bg=f"#{tc:06x}")
            align = self.core.shape_text_align(self.canvas.slide_idx, self.canvas.sel)
            self.seg_align.set({0: "左", 1: "中", 2: "右"}.get(align, "左"))
            try:
                self.seg_align2.set({0: "左", 1: "中", 2: "右"}.get(align, "左"))
            except Exception:
                pass
            bold = self.core.shape_text_bold(self.canvas.slide_idx, self.canvas.sel)
            if bold:
                self.sw_bold.select()
            else:
                self.sw_bold.deselect()
            self._paint_toggle(self.tb_bold, bool(bold))
            self._paint_toggle(self.tb_italic, bool(self.core.shape_text_italic(self.canvas.slide_idx, self.canvas.sel)))
            self._paint_toggle(self.tb_underline, bool(self.core.shape_text_underline(self.canvas.slide_idx, self.canvas.sel)))
            if tc >= 0:
                self.sw_tcolor2.configure(bg=f"#{tc:06x}")
            if sh.get("font_name"):
                self.cb_font.set(sh["font_name"])
            if sh["fontsize_pt"] and sh["fontsize_pt"] > 0:
                self.cb_size.set(f"{sh['fontsize_pt']:.0f}")
            ak, ad = self.core.shape_anim(self.canvas.slide_idx, self.canvas.sel)
            self.cb_anim.set(ANIM_NAMES_R.get(ak, "无"))
            self.e_adur.delete(0, "end")
            self.e_adur.insert(0, f"{ad / 1000.0:.1f}")
        except Exception:
            pass
        self._busy = False

    def _paint_toggle(self, btn, on):
        try:
            btn.configure(fg_color="#2f6df6" if on else "#39404a")
        except Exception:
            pass

    # -------- 切换动画 / 入场动画 --------
    def _sync_transition(self):
        if not self.core.doc:
            return
        try:
            kind, dur, d = self.core.slide_transition(self.canvas.slide_idx)
            self.cb_tr.set(TR_NAMES_R.get(kind, "无"))
            self.cb_tdir.set({1: "上", 2: "下", 3: "左", 4: "右"}.get(d, "左"))
            self.e_tdur.delete(0, "end")
            self.e_tdur.insert(0, f"{dur / 1000.0:.1f}")
        except Exception:
            pass

    def _transition_params(self):
        kind = TR_NAMES.get(self.cb_tr.get(), "none")
        d = {"左": 3, "右": 4, "上": 1, "下": 2}.get(self.cb_tdir.get(), 3)
        try:
            dur = int(max(0.1, min(5.0, float(self.e_tdur.get()))) * 1000)
        except Exception:
            dur = 600
        return kind, dur, d

    def on_transition_preview(self):
        """下拉一改就应用到当前页，跟 PowerPoint 的手感一致"""
        self.on_apply_transition(silent=True)

    def on_apply_transition(self, silent=False):
        if not self.core.doc:
            return
        kind, dur, d = self._transition_params()
        try:
            self.core.slide_set_transition(self.canvas.slide_idx, kind, dur, d)
        except Exception as e:
            self.set_status("设置切换失败: " + str(e))
            return
        self._update_undo_buttons()
        self._refresh_current_thumb()
        self._sync_transition()
        if not silent:
            self.set_status("切换动画已应用：" + TR_NAMES_R.get(kind, "无"))

    def on_apply_transition_all(self):
        if not self.core.doc:
            return
        kind, dur, d = self._transition_params()
        try:
            self.core.transition_apply_all(kind, dur, d)
        except Exception as e:
            self.set_status("批量设置失败: " + str(e))
            return
        self._update_undo_buttons()
        self.refresh_thumbs()
        self.set_status("已应用到全部页：" + TR_NAMES_R.get(kind, "无"))

    def _anim_params(self):
        kind = ANIM_NAMES.get(self.cb_anim.get(), "none")
        try:
            dur = int(max(0.1, min(5.0, float(self.e_adur.get()))) * 1000)
        except Exception:
            dur = 500
        return kind, dur

    def on_apply_anim(self):
        i = self._sel()
        if not self._need_doc():
            return
        if i < 0:
            self.set_status("请先在画布上选中一个形状，再设置入场动画")
            return
        kind, dur = self._anim_params()
        try:
            self.core.shape_set_anim(self.canvas.slide_idx, i, kind, dur)
        except Exception as e:
            self.set_status("设置动画失败: " + str(e))
            return
        self._update_undo_buttons()
        self.set_status("入场动画已应用：" + ANIM_NAMES_R.get(kind, "无"))

    def on_clear_anim(self):
        if not self._need_doc():
            return
        try:
            self.core.shape_anim_clear(self.canvas.slide_idx)
        except Exception as e:
            self.set_status("清空动画失败: " + str(e))
            return
        self._update_undo_buttons()
        self.set_status("已清空本页入场动画")

    def _val(self, entry, default=0.0):
        try:
            return float(entry.get())
        except Exception:
            return default

    def on_prop_apply(self):
        i = self._sel()
        if self._busy or i < 0 or not self.core.doc:
            return
        si = self.canvas.slide_idx
        sh = self.canvas.selected_shape()
        if not sh:
            return
        try:
            x = int(self._val(self.e_x) * EMU_PER_PT)
            y = int(self._val(self.e_y) * EMU_PER_PT)
            w = max(12700, int(self._val(self.e_w, 1) * EMU_PER_PT))
            h = max(12700, int(self._val(self.e_h, 1) * EMU_PER_PT))
            changed = (x, y, w, h) != (sh["x"], sh["y"], sh["cx"], sh["cy"])
            if changed:
                self.core.shape_set_geom(si, i, x, y, w, h)
            rot = self._val(self.e_rot, sh["rot"])
            if abs(rot - sh["rot"]) > 0.5:
                self.core.shape_set_rot(si, i, rot)
            fs = self._val(self.e_font, sh["fontsize_pt"] if sh["fontsize_pt"] > 0 else 18)
            if sh["fontsize_pt"] > 0 and abs(fs - sh["fontsize_pt"]) > 0.05:
                self.core.shape_set_fontsize(si, i, fs)
            self.canvas.reload()
            self._sync_props(self.canvas.selected_shape())
            self._update_undo_buttons()
            self._refresh_current_thumb()
        except Exception as e:
            self.set_status("属性应用失败: " + str(e))

    def on_align_shapes(self, mode):
        idxs = self.canvas.selected_indices()
        if self._busy or not idxs:
            return
        si = self.canvas.slide_idx
        shapes = self.canvas.shapes
        sw, sh = self.core.slide_w, self.core.slide_h
        if len(idxs) == 1:
            # 单个形状对齐到幻灯片
            L, T, R, B = 0, 0, sw, sh
        else:
            L = min(shapes[i]["x"] for i in idxs)
            T = min(shapes[i]["y"] for i in idxs)
            R = max(shapes[i]["x"] + shapes[i]["cx"] for i in idxs)
            B = max(shapes[i]["y"] + shapes[i]["cy"] for i in idxs)
        self.core.begin_edit()
        try:
            for i in idxs:
                s = shapes[i]
                x, y, cx, cy = s["x"], s["y"], s["cx"], s["cy"]
                if mode == "left":
                    x = L
                elif mode == "right":
                    x = R - cx
                elif mode == "hcenter":
                    x = (L + R) // 2 - cx // 2
                elif mode == "top":
                    y = T
                elif mode == "bottom":
                    y = B - cy
                elif mode == "vcenter":
                    y = (T + B) // 2 - cy // 2
                self.core.shape_set_geom(si, i, x, y, cx, cy)
        finally:
            self.core.end_edit()
        self.canvas.reload()
        self._update_undo_buttons()

    def on_distribute(self, axis):
        idxs = self.canvas.selected_indices()
        if self._busy or len(idxs) < 3:
            self.set_status("等距分布需要选中至少 3 个形状")
            return
        si = self.canvas.slide_idx
        shapes = self.canvas.shapes
        key = (lambda i: (shapes[i]["x"], shapes[i]["y"])) if axis == "h" else \
              (lambda i: (shapes[i]["y"], shapes[i]["x"]))
        order = sorted(idxs, key=key)
        self.core.begin_edit()
        try:
            if axis == "h":
                span = (shapes[order[-1]]["x"] + shapes[order[-1]]["cx"]) - shapes[order[0]]["x"]
                total = sum(shapes[i]["cx"] for i in order)
                gap = (span - total) // max(1, len(order) - 1)
                cur = shapes[order[0]]["x"]
                for i in order:
                    s = shapes[i]
                    self.core.shape_set_geom(si, i, cur, s["y"], s["cx"], s["cy"])
                    cur += s["cx"] + gap
            else:
                span = (shapes[order[-1]]["y"] + shapes[order[-1]]["cy"]) - shapes[order[0]]["y"]
                total = sum(shapes[i]["cy"] for i in order)
                gap = (span - total) // max(1, len(order) - 1)
                cur = shapes[order[0]]["y"]
                for i in order:
                    s = shapes[i]
                    self.core.shape_set_geom(si, i, s["x"], cur, s["cx"], s["cy"])
                    cur += s["cy"] + gap
        finally:
            self.core.end_edit()
        self.canvas.reload()
        self._update_undo_buttons()

    def on_table_op(self, op):
        if not self._need_doc():
            return
        i = self._sel()
        sh = self.canvas.selected_shape()
        if i < 0 or not sh or sh["kind"] != 2:
            self.set_status("请先选中一个表格形状")
            return
        si = self.canvas.slide_idx
        r, c = self.canvas.last_cell
        rows, cols = self.core.table_size(si, i)
        try:
            if op == "add_row":
                self.core.table_row_add(si, i, r + 1)
            elif op == "del_row":
                if rows <= 1:
                    self.set_status("只剩一行了，删不掉")
                    return
                self.core.table_row_del(si, i, r)
            elif op == "add_col":
                self.core.table_col_add(si, i, c + 1)
            elif op == "del_col":
                if cols <= 1:
                    self.set_status("只剩一列了，删不掉")
                    return
                self.core.table_col_del(si, i, c)
        except Exception as e:
            messagebox.showerror("表格操作失败", str(e), parent=self)
            return
        self.canvas.last_cell = (min(r, max(0, rows - (1 if op == "del_row" else 0))),
                                 min(c, max(0, cols - (1 if op == "del_col" else 0))))
        self.canvas.reload(keep_sel=True)
        self.canvas.select(i)
        self._update_undo_buttons()
        nr, nc = self.core.table_size(si, i)
        self.set_status("表格现在是 %d 行 %d 列（Ctrl+Z 可撤销）" % (nr, nc))

    def on_align(self, value):
        i = self._sel()
        if self._busy or i < 0:
            return
        a = {"左": 0, "中": 1, "右": 2}.get(value, 0)
        self.core.shape_set_align(self.canvas.slide_idx, i, a)
        self.canvas.reload()
        self._update_undo_buttons()
    def on_bold(self):
        i = self._sel()
        if self._busy or i < 0:
            return
        b = 1 if self.sw_bold.get() else 0
        self.core.shape_set_bold(self.canvas.slide_idx, i, b)
        self.canvas.reload()
        self._update_undo_buttons()

    def on_pick_fill(self):
        i = self._sel()
        if i < 0:
            return
        sh = self.canvas.selected_shape()
        cur = f"#{sh['fill_rgb'] or 0xFFFFFF:06x}"
        rgb, _ = colorchooser.askcolor(color=cur, parent=self, title="选择填充色")
        if not rgb:
            return
        v = (int(rgb[0]) << 16) | (int(rgb[1]) << 8) | int(rgb[2])
        self.core.shape_set_fill(self.canvas.slide_idx, i, v)
        self.sw_fill.configure(bg=f"#{v:06x}")
        self.canvas.reload()
        self._update_undo_buttons()
        self._refresh_current_thumb()

    def on_pick_textcolor(self):
        i = self._sel()
        if i < 0:
            return
        rgb, _ = colorchooser.askcolor(color="#222222", parent=self, title="选择文字颜色")
        if not rgb:
            return
        v = (int(rgb[0]) << 16) | (int(rgb[1]) << 8) | int(rgb[2])
        self.core.shape_set_textcolor(self.canvas.slide_idx, i, v)
        self.sw_text.configure(bg=f"#{v:06x}")
        self.canvas.reload()
        self._update_undo_buttons()
        self._refresh_current_thumb()

    # ================= 撤销 =================
    def on_undo(self):
        if not self._need_doc():
            return
        if self.core.undo() != 0:
            self.set_status("没有可撤销的操作")
            return
        self._after_undo_redo()

    def on_redo(self):
        if not self._need_doc():
            return
        if self.core.redo() != 0:
            self.set_status("没有可重做的操作")
            return
        self._after_undo_redo()

    def _after_undo_redo(self):
        if self.canvas.slide_idx >= self.core.slide_count:
            self.canvas.slide_idx = max(0, self.core.slide_count - 1)
        self.refresh_thumbs()
        self._highlight_thumb(self.canvas.slide_idx)
        self.canvas.reload(keep_sel=True)
        self._update_undo_buttons()
        self.set_status("撤销/重做已应用")

    def _update_undo_buttons(self):
        if not self.btn_undo:
            return
        try:
            self.btn_undo.configure(state="normal" if self.core.can_undo() else "disabled")
            self.btn_redo.configure(state="normal" if self.core.can_redo() else "disabled")
        except Exception:
            pass

    # ================= 杂项 =================
    # ================= 右键快捷菜单 / 附加元素 =================
    def _refresh_attach(self, reload=True):
        self.canvas.attach = self.attach["shapes"]
        if reload:
            self.canvas.reload(keep_sel=True)
        if self.path:
            try:
                yc_attach.save(self.path, self.attach)
            except Exception:
                pass

    def on_shape_menu(self, action, i):
        shapes = self.canvas.shapes
        has = 0 <= i < len(shapes)
        sid = str(shapes[i]["id"]) if has else None
        ent = self.attach["shapes"].get(sid, {}) if sid else {}

        if action == "edit_text":
            if has:
                self.canvas.begin_text_edit(i)
        elif action == "duplicate":
            self.canvas.select(i); self.on_dup_shape()
        elif action == "top":
            self.canvas.select(i); self.on_zorder(1)
        elif action == "bottom":
            self.canvas.select(i); self.on_zorder(-1)
        elif action == "delete":
            self.canvas.select(i); self.on_del_shape()
        elif action == "new_text":
            self.on_add_text()
        elif action == "new_picture":
            self.on_add_picture()
        elif action == "script" and has:
            self._edit_script(sid, "形状 JS 脚本（this 元素 = el）")
        elif action == "global_script":
            self._edit_global_script()
        elif action == "slide_script":
            self._edit_slide_script()
        elif action == "export_xlsx":
            self._export_table_xlsx(i)
        elif action == "clear_attach" and has:
            self.attach["shapes"].pop(sid, None)
            self._refresh_attach()
            self.set_status("已清除该形状的脚本/媒体/程序")
            self._toast("已清除附加信息")
        elif action in ("video", "audio") and has:
            self._attach_media(i, sid, action)
        elif action == "new_media":
            self._attach_media(-1, None, "video")
        elif action == "toggle_autoplay" and has:
            m = ent.get("media") or {}
            if not m.get("path"):
                return
            m["autoplay"] = not m.get("autoplay")
            self._refresh_attach()
            self._toast("自动播放：" + ("开" if m["autoplay"] else "关"))
        elif action == "program" and has:
            self._attach_program(sid)
        elif action == "run_program" and has:
            prog = ent.get("program")
            if prog and os.path.exists(prog):
                try:
                    subprocess.Popen([prog], cwd=os.path.dirname(prog))
                    self.set_status("已运行 " + os.path.basename(prog))
                except Exception as e:
                    messagebox.showerror("运行失败", str(e), parent=self)

    def _edit_script(self, sid, title):
        ent = self.attach["shapes"].setdefault(str(sid), {})
        dlg = ScriptDialog(self, title, ent.get("js", ""))
        self.wait_window(dlg)
        if dlg.result is None:
            return
        ent["js"] = dlg.result
        if not dlg.result.strip():
            ent.pop("js", None)
        if not ent:
            self.attach["shapes"].pop(str(sid), None)
        self._refresh_attach()
        self.set_status("脚本已保存（导出 HTML 时注入）" if dlg.result.strip() else "已清除脚本")

    def _export_table_xlsx(self, i):
        if not self._need_doc():
            return
        sh = None
        try:
            sh = self.core.shape_get(self.canvas.slide_idx, i)
        except Exception:
            pass
        if not sh or sh["kind"] != 2:
            self.set_status("这不是表格形状")
            return
        rows, cols = self.core.table_size(self.canvas.slide_idx, i)
        data = []
        for r in range(rows):
            line = []
            for c in range(cols):
                line.append(self.core.table_cell_text(self.canvas.slide_idx, i, r, c))
            data.append(line)
        if not data:
            self.set_status("表格是空的")
            return
        widths = []
        for c in range(cols):
            m = 4
            for r in range(rows):
                t = data[r][c]
                m = max(m, sum(2 if ord(ch) > 127 else 1 for ch in t))
            widths.append(min(46, max(8, m * 1.15)))
        base = os.path.splitext(os.path.basename(self.path or "演示文稿"))[0]
        init = os.path.dirname(self.path) if self.path else os.getcwd()
        p = filedialog.asksaveasfilename(title="导出该表格为 Excel",
                                        initialdir=init,
                                        initialfile=base + "_表格.xlsx",
                                        defaultextension=".xlsx",
                                        filetypes=[("Excel 工作簿", "*.xlsx")])
        if not p:
            return
        try:
            yc_xlsx.write_xlsx(p, data, sheet_name=base[:20] or "Sheet1", col_widths=widths)
        except Exception as ex:
            messagebox.showerror("导出失败", str(ex), parent=self)
            return
        self.set_status("已导出 Excel：%s（%d 行 %d 列）" % (p, rows, cols))
        self._toast("已导出 Excel")

    def _edit_slide_script(self):
        idx = str(self.canvas.slide_idx)
        dlg = ScriptDialog(self, "本页 JS 脚本（第 %d 页）" % (self.canvas.slide_idx + 1),
                           self.attach.setdefault("slide_js", {}).get(idx, ""))
        self.wait_window(dlg)
        if dlg.result is None:
            return
        self.attach.setdefault("slide_js", {})[idx] = dlg.result
        if not dlg.result.strip():
            self.attach["slide_js"].pop(idx, None)
        if self.path:
            yc_attach.save(self.path, self.attach)
        self.set_status("本页脚本已保存" if dlg.result.strip() else "已清除本页脚本")
        self._toast("本页脚本已保存" if dlg.result.strip() else "已清除本页脚本")

    def _edit_global_script(self):
        dlg = ScriptDialog(self, "全局 JS 脚本", self.attach.get("global_js", ""))
        self.wait_window(dlg)
        if dlg.result is None:
            return
        self.attach["global_js"] = dlg.result
        if self.path:
            yc_attach.save(self.path, self.attach)
        self.set_status("全局脚本已保存")

    def _attach_media(self, i, sid, kind):
        init = os.path.dirname(self.path) if self.path else os.getcwd()
        if kind == "audio":
            ft = [("音频", "*.mp3 *.wav *.m4a *.ogg *.aac"), ("所有文件", "*.*")]
            title = "选择音频文件"
        else:
            ft = [("视频", "*.mp4 *.webm *.ogg *.mov *.m4v"), ("所有文件", "*.*")]
            title = "选择视频文件"
        p = filedialog.askopenfilename(title=title, initialdir=init, filetypes=ft)
        if not p:
            return
        if not has:  # 新建一个媒体块
            sw, sh = self.core.slide_w, self.core.slide_h
            idx = self.core.shape_add_rect(self.canvas.slide_idx, int(sw * .08), int(sh * .12),
                                           int(sw * .84), int(sh * .7))
            self.canvas.reload(keep_sel=False)
            self.canvas.select(idx)
            i = idx
            sid = str(self.canvas.shapes[idx]["id"])
        ent = self.attach["shapes"].setdefault(str(sid), {})
        ent["media"] = {"type": "audio" if kind == "audio" else "video", "path": p}
        self._refresh_attach()
        self.set_status(("音频" if kind == "audio" else "视频") + "已附加：" + os.path.basename(p))
        self._toast(("♪ 音频" if kind == "audio" else "▶ 视频") + " 已附加到形状")

    def _attach_program(self, sid):
        init = os.path.dirname(self.path) if self.path else os.getcwd()
        p = filedialog.askopenfilename(title="选择要嵌入的程序", initialdir=init,
                                       filetypes=[("程序", "*.exe *.bat *.cmd *.py"), ("所有文件", "*.*")])
        if not p:
            return
        ent = self.attach["shapes"].setdefault(str(sid), {})
        ent["program"] = p
        self._refresh_attach()
        self.set_status("已关联程序：" + os.path.basename(p) + "（右键“运行该程序”或双击运行）")
        self._toast("⚙ 已关联程序：" + os.path.basename(p))

    def _after_edit(self):
        self._sync_props(self.canvas.selected_shape())
        self._update_undo_buttons()
        self._refresh_current_thumb()

    def _refresh_current_thumb(self):
        i = self.canvas.slide_idx
        kids = self.thumb_box.winfo_children()
        if i < 0 or i >= len(kids):
            return
        try:
            data, w, h = self.core.render(i, 168, 95)
            img = ctk.CTkImage(light_image=pil_from(data, w, h), size=(168, 95))
            self._thumb_imgs[i] = img
            lb = kids[i].winfo_children()[0]
            lb.configure(image=img)
        except Exception:
            pass

    def set_status(self, text):
        self.status_var.set(text)

    def _toast(self, text):
        """轻量提示条：从底部往上浮一小段后消失"""
        try:
            if getattr(self, "_toast_lbl", None) is not None:
                self._toast_lbl.destroy()
            lbl = tk.Label(self, text=text, bg="#2f6df6", fg="#ffffff",
                           font=("Microsoft YaHei UI", 10), padx=14, pady=6)
            self._toast_lbl = lbl
            step = {"i": 0}

            def anim():
                if not lbl.winfo_exists():
                    return
                step["i"] += 1
                lbl.place(relx=0.5, rely=0.985 - step["i"] * 0.008, anchor="s")
                if step["i"] < 6:
                    lbl.after(45, anim)
                else:
                    lbl.after(900, lambda: lbl.destroy() if lbl.winfo_exists() else None)

            lbl.place(relx=0.5, rely=0.985, anchor="s")
            lbl.after(45, anim)
        except Exception:
            pass


class ScriptDialog(ctk.CTkToplevel):
    """形状 / 全局 JS 脚本编辑器；返回 result=None 表示取消"""

    SAMPLE = ("// el 就是被附加脚本的形状元素\n"
              "el.style.transition = 'transform .35s ease';\n"
              "el.addEventListener('click', function () {\n"
              "  el.style.transform = 'scale(1.08)';\n"
              "});\n")

    def __init__(self, master, title, code):
        super().__init__(master)
        self.title(title)
        self.geometry("660x470")
        self.result = None
        self.transient(master)
        self.configure(fg_color="#1e242b")

        bar = ctk.CTkFrame(self, fg_color="transparent")
        bar.pack(side="bottom", fill="x", padx=12, pady=10)
        ctk.CTkLabel(bar, text="Ctrl+Enter 保存 · Esc 取消", font=FONT_S,
                     text_color="#9aa7b8").pack(side="left")
        ctk.CTkButton(bar, text="取消", width=88, height=30, fg_color="#4a5563",
                      hover_color="#5a6675", command=self._cancel).pack(side="right", padx=(6, 0))
        ctk.CTkButton(bar, text="清空", width=76, height=30, fg_color="#7a4a4a",
                      hover_color="#8a5555", command=self._clear).pack(side="right", padx=(6, 0))
        ctk.CTkButton(bar, text="保存", width=88, height=30, command=self._ok).pack(side="right")

        ctk.CTkLabel(self, text="JS 脚本只作用于导出后的 HTML 放映页（内置 YCZ.shape 便捷函数）",
                     font=FONT_S, text_color="#93a1b4", anchor="w").pack(fill="x", padx=12, pady=(8, 4))
        self.txt = tk.Text(self, bg="#161a20", fg="#e6e9ef", insertbackground="#ffffff",
                           font=("Consolas", 11), relief="flat", padx=10, pady=8, undo=True,
                           highlightthickness=1, highlightbackground="#39424e")
        self.txt.pack(fill="both", expand=True, padx=12, pady=(0, 4))
        self._hl_job = None
        self._setup_tags()
        self.txt.insert("1.0", code if code and code.strip() else self.SAMPLE)
        self._highlight()
        self.txt.bind("<KeyRelease>", self._schedule_highlight)
        self.txt.bind("<Control-v>", lambda e: self.after(30, self._highlight))
        self.txt.bind("<Control-V>", lambda e: self.after(30, self._highlight))
        self.txt.focus_set()
        self.bind("<Control-Return>", lambda e: (self._ok(), "break")[1])
        self.bind("<Control-s>", lambda e: (self._ok(), "break")[1])
        self.bind("<Escape>", lambda e: (self._cancel(), "break")[1])
        self.after(60, self.focus_force)
        self.grab_set()

    # ---- 简易 JS 语法高亮（VS Code 暗色配色）----
    KW = set("""var let const function return if else for while do switch case break continue
                new delete typeof instanceof in of this class extends super try catch finally throw
                true false null undefined async await yield void""".split())
    GLB = set("""window document console Math JSON Object Array String Number Boolean Promise
                 Date RegExp Error setTimeout setInterval clearTimeout clearInterval
                 parseInt parseFloat isNaN alert YCZ""".split())

    def _setup_tags(self):
        conf = {
            "kw": ("#569cd6", None),
            "glb": ("#4ec9b0", None),
            "str": ("#ce9178", None),
            "com": ("#6a9955", "italic"),
            "num": ("#b5cea8", None),
            "fn": ("#dcdcaa", None),
            "prop": ("#9cdcfe", None),
        }
        self._tags = []
        for name, (color, slant) in conf.items():
            font = ("Consolas", 11, slant) if slant else ("Consolas", 11)
            self.txt.tag_configure(name, foreground=color, font=font)
            self._tags.append(name)

    def _add_tag(self, tag, m):
        self.txt.tag_add(tag, "1.0+%dc" % m.start(), "1.0+%dc" % m.end())

    def _schedule_highlight(self, _ev=None):
        if self._hl_job:
            try:
                self.txt.after_cancel(self._hl_job)
            except Exception:
                pass
        self._hl_job = self.txt.after(90, self._highlight)

    def _highlight(self, _ev=None):
        self._hl_job = None
        try:
            bounds = self.txt.yview()
            txt = self.txt.get("1.0", "end-1c")
            for t in self._tags:
                self.txt.tag_remove(t, "1.0", "end")
            spans = []
            for m in re.finditer(r"//[^\n]*|/\*[\s\S]*?\*/", txt):
                spans.append(m.span())
                self._add_tag("com", m)
            for m in re.finditer(r"'(?:\\.|[^'\\\n])*'|\"(?:\\.|[^\"\\\n])*\"|`(?:\\.|[^`\\])*`", txt):
                spans.append(m.span())
                self._add_tag("str", m)

            def blocked(a, b):
                return any(s <= a and b <= e for s, e in spans)

            for m in re.finditer(r"\b\d+(?:\.\d+)?\b", txt):
                if not blocked(*m.span()):
                    self._add_tag("num", m)
            for m in re.finditer(r"[A-Za-z_$][\w$]*", txt):
                a, b = m.span()
                if blocked(a, b):
                    continue
                w = m.group(0)
                if w in self.KW:
                    self._add_tag("kw", m)
                elif w in self.GLB:
                    self._add_tag("glb", m)
                elif txt[b:b + 1] == "(":
                    self._add_tag("fn", m)
                elif txt[a - 1:a] == ".":
                    self._add_tag("prop", m)
            for t in self._tags:          # 关键字、字符串盖在属性/函数色之上
                self.txt.tag_raise(t)
            self.txt.tag_raise("str")
            self.txt.tag_raise("com")
            self.txt.yview_moveto(bounds[0])
        except Exception:
            pass

    def _ok(self):
        self.result = self.txt.get("1.0", "end-1c")
        self.destroy()

    def _clear(self):
        self.result = ""
        self.destroy()

    def _cancel(self):
        self.result = None
        self.destroy()


def main():
    _enable_dpi_aware()
    path = None
    for a in sys.argv[1:]:
        if a.lower().endswith(".pptx") and os.path.exists(a):
            path = a
            break
    app = App(path)
    app.mainloop()


if __name__ == "__main__":
    try:
        main()
    except Exception:
        traceback.print_exc()
        input("启动失败，按回车退出…")
