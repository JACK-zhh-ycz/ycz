# -*- coding: utf-8 -*-
"""表格单元格画面内直接输入"""
import os
import sys
import ctypes
import ctypes.wintypes as wt
from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "src", "pyui"))
from editor import App

ok = True


def check(name, cond, extra=""):
    global ok
    print(("[PASS] " if cond else "[FAIL] ") + name + (("  " + str(extra)) if extra else ""))
    if not cond:
        ok = False


def grab(hwnd, path):
    u = ctypes.windll.user32; g = ctypes.windll.gdi32

    class BIH(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                    ("biBitCount", wt.WORD), ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
                    ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                    ("biClrImportant", wt.DWORD)]

    class BI(ctypes.Structure):
        _fields_ = [("bmiHeader", BIH), ("bmiColors", wt.DWORD * 3)]

    root = u.GetAncestor(hwnd, 2); r = wt.RECT(); u.GetWindowRect(root, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    hdc = u.GetWindowDC(root); dc = g.CreateCompatibleDC(hdc); bmp = g.CreateCompatibleBitmap(hdc, w, h)
    g.SelectObject(dc, bmp); u.PrintWindow(root, dc, 2)
    bi = BI(); bi.bmiHeader.biSize = ctypes.sizeof(BIH); bi.bmiHeader.biWidth = w
    bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32
    buf = ctypes.create_string_buffer(w * h * 4); g.GetDIBits(dc, bmp, 0, h, buf, ctypes.byref(bi), 0)
    Image.frombuffer("RGBA", (w, h), buf, "raw", "BGRA", 0, 1).convert("RGB").save(path)
    g.DeleteObject(bmp); g.DeleteDC(dc); u.ReleaseDC(root, hdc)


app = App(None)


def run():
    app.on_new(); app.update()
    w, h = app.core.slide_w, app.core.slide_h
    i = app.core.shape_add_table(0, int(w * .1), int(h * .2), int(w * .75), int(h * .45), 3, 4)
    app.canvas.reload(keep_sel=False); app.update()

    rect = app.core.table_cell_rect(0, i, 0, 0, app.canvas.rw, app.canvas.rh)
    check("能算单元格矩形", rect is not None, rect)

    app.canvas.begin_cell_edit(i, 0, 0)
    app.update()
    check("单元格上出现输入框", app.canvas._edit is not None)
    app.canvas._edit.delete("1.0", "end")
    app.canvas._edit.insert("1.0", "直接写进单元格")
    grab(app.winfo_id(), "build/ui_table_inline.png")
    app.canvas.commit_text_edit(False)
    app.update()
    check("文字写回单元格", app.core.table_cell_text(0, i, 0, 0) == "直接写进单元格",
          app.core.table_cell_text(0, i, 0, 0))
    check("输入框已销毁", app.canvas._edit is None)

    app.canvas.begin_cell_edit(i, 1, 2)
    app.update()
    app.canvas._edit.delete("1.0", "end")
    app.canvas._edit.insert("1.0", "第二格")
    app.canvas.commit_text_edit(True)
    app.update()
    check("可以逐个单元格写", app.core.table_cell_text(0, i, 1, 2) == "第二格")
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "build", "table_inline.pptx")
    app.path = out
    app.on_save()
    check("存盘成功", os.path.exists(out) and os.path.getsize(out) > 0)

    app.destroy()


app.after(800, run)
app.mainloop()
print("\n=== 表格就地输入自测全部通过 ===" if ok else "\n=== 表格就地输入自测失败 ===")
sys.exit(0 if ok else 1)
