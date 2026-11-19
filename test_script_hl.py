# -*- coding: utf-8 -*-
"""JS 脚本对话框语法高亮自测"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "src", "pyui"))
from editor import App, ScriptDialog

ok = True


def check(name, cond, extra=""):
    global ok
    print(("[PASS] " if cond else "[FAIL] ") + name + (("  " + str(extra)) if extra else ""))
    if not cond:
        ok = False


app = App(None)
CODE = "// 注释\nvar n = 42;\nfunction go(el) {\n  el.style.opacity = '0.5';\n  console.log(\"done\");\n}\n"


def run():
    d = ScriptDialog(app, "语法高亮测试", CODE)
    app.update()
    app.update()

    def ranges(tag):
        return len(d.txt.tag_ranges(tag)) // 2

    check("关键字着色", ranges("kw") >= 2, ranges("kw"))
    check("字符串着色", ranges("str") >= 2, ranges("str"))
    check("注释着色", ranges("com") >= 1, ranges("com"))
    check("数字着色", ranges("num") >= 1, ranges("num"))
    check("属性着色", ranges("prop") >= 1, ranges("prop"))
    check("改了字还能重新着色", d.txt.tag_ranges("kw") and d.txt.tag_ranges("str"))

    d.txt.insert("end", "\nlet more = true;\n")
    d._highlight()
    app.update()
    check("追加代码后仍高亮", ranges("kw") >= 3, ranges("kw"))

    # 截图
    import ctypes
    import ctypes.wintypes as wt
    from PIL import Image
    u = ctypes.windll.user32
    g = ctypes.windll.gdi32

    class BIH(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                    ("biBitCount", wt.WORD), ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
                    ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                    ("biClrImportant", wt.DWORD)]

    class BI(ctypes.Structure):
        _fields_ = [("bmiHeader", BIH), ("bmiColors", wt.DWORD * 3)]

    root = u.GetAncestor(d.winfo_id(), 2)
    r = wt.RECT(); u.GetWindowRect(root, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    hdc = u.GetWindowDC(root); dc = g.CreateCompatibleDC(hdc); bmp = g.CreateCompatibleBitmap(hdc, w, h)
    g.SelectObject(dc, bmp); u.PrintWindow(root, dc, 2)
    bi = BI(); bi.bmiHeader.biSize = ctypes.sizeof(BIH); bi.bmiHeader.biWidth = w
    bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32
    buf = ctypes.create_string_buffer(w * h * 4)
    g.GetDIBits(dc, bmp, 0, h, buf, ctypes.byref(bi), 0)
    Image.frombuffer("RGBA", (w, h), buf, "raw", "BGRA", 0, 1).convert("RGB").save("build/ui_hl.png")
    g.DeleteObject(bmp); g.DeleteDC(dc); u.ReleaseDC(root, hdc)

    d._cancel()
    app.destroy()


app.after(800, run)
app.mainloop()
print("\n=== 语法高亮自测全部通过 ===" if ok else "\n=== 语法高亮自测失败 ===")
sys.exit(0 if ok else 1)
