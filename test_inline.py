# -*- coding: utf-8 -*-
"""画面内就地编辑文字回归"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "src", "pyui"))
from editor import App

ok = True


def check(name, cond, extra=""):
    global ok
    print(("[PASS] " if cond else "[FAIL] ") + name + (("  " + str(extra)) if extra else ""))
    if not cond:
        ok = False


app = App(None)
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "build", "text_inline.pptx")


def run():
    app.on_new(); app.update()
    sw, sh = app.core.slide_w, app.core.slide_h
    app.core.shape_add_rect(0, int(sw * .1), int(sh * .2), int(sw * .5), int(sh * .15), "原始文字")
    app.canvas.reload(keep_sel=False); app.update()
    app.path = out

    app.canvas.begin_text_edit(0)
    app.update()
    check("输入层出现在画布上", app.canvas._edit is not None)
    check("输入层在形状内部", app.canvas._edit is not None and
          app.canvas._edit.winfo_width() > 10)

    app.canvas._edit.delete("1.0", "end")
    app.canvas._edit.insert("1.0", "画面内直接改的文字")
    app.canvas.commit_text_edit(True)
    app.update()
    check("文字写回模型", app.canvas.shapes[0]["text"] == "画面内直接改的文字",
          app.canvas.shapes[0]["text"])
    check("输入层已销毁", app.canvas._edit is None)
    check("Ctrl+S 路径已存盘", os.path.exists(out) and os.path.getsize(out) > 0)

    # 空文本也能提交（用于清空）
    app.canvas.begin_text_edit(0)
    app.update()
    app.canvas._edit.delete("1.0", "end")
    app.canvas.commit_text_edit(False)
    app.update()
    check("可以清空文字", app.canvas.shapes[0]["text"] == "", repr(app.canvas.shapes[0]["text"]))

    app.destroy()


app.after(700, run)
app.mainloop()
print("\n=== 就地编辑自测全部通过 ===" if ok else "\n=== 就地编辑自测失败 ===")
sys.exit(0 if ok else 1)
