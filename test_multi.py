# -*- coding: utf-8 -*-
"""多选 / 形状对齐 / 等距分布 / 吸附参考线 回归"""
import os
import sys
import tkinter as tk

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "src", "pyui"))
from editor import App

ok = True


def check(name, cond, extra=""):
    global ok
    print(("[PASS] " if cond else "[FAIL] ") + name + (("  " + str(extra)) if extra else ""))
    if not cond:
        ok = False


app = App(None)


def run():
    app.on_new(); app.update()
    sw, sh = app.core.slide_w, app.core.slide_h
    for i, (fx, fy, fw, fh) in enumerate([(0.1, 0.1, 0.2, 0.1), (0.4, 0.3, 0.2, 0.1), (0.7, 0.5, 0.2, 0.1)]):
        app.core.shape_add_rect(0, int(sw * fx), int(sh * fy), int(sw * fw), int(sh * fh), f"S{i}")
    app.canvas.reload(keep_sel=False); app.update()

    # 多选
    app.canvas.select(0)
    app.canvas.select(1, add=True)
    app.canvas.select(2, add=True)
    idxs = app.canvas.selected_indices()
    check("Ctrl 多选 3 个", idxs == [0, 1, 2], idxs)

    # 左对齐
    app.on_align_shapes("left")
    xs = [app.canvas.shapes[i]["x"] for i in idxs]
    check("左对齐 x 相同", len(set(xs)) == 1, xs)

    # 垂直居中
    app.on_align_shapes("vcenter")
    cys = [app.canvas.shapes[i]["y"] + app.canvas.shapes[i]["cy"] / 2 for i in idxs]
    check("垂直居中同一中线", max(cys) - min(cys) <= 1, cys)

    # 重新摆成不等距，再等距
    app.canvas.select(0); app.canvas.select(1, add=True); app.canvas.select(2, add=True)
    for i, x in enumerate([0, int(sw * 0.2), int(sw * 0.85)]):
        s = app.canvas.shapes[i]
        app.core.shape_set_geom(0, i, x, s["y"], s["cx"], s["cy"])
    app.canvas.reload(keep_sel=True)
    app.canvas.select(0); app.canvas.select(1, add=True); app.canvas.select(2, add=True)
    app.on_distribute("h")
    a = app.canvas.shapes
    gaps = [a[1]["x"] - (a[0]["x"] + a[0]["cx"]), a[2]["x"] - (a[1]["x"] + a[1]["cx"])]
    check("水平等距", abs(gaps[0] - gaps[1]) <= 2, gaps)

    # 吸附：把形状拖到幻灯片中线附近，应被吸附
    app.canvas.select(0)
    s = app.canvas.shapes[0]
    w, h = s["cx"], s["cy"]
    nx = int((app.canvas.rw / 2.0 - w / app.canvas.k / 2) * app.canvas.k) + 900   # 差 0.1px 左右
    nx2, ny2 = app.canvas._snap_move(nx, s["y"], w, h)
    center = nx2 / app.canvas.k + (w / app.canvas.k) / 2
    check("吸附到中线", abs(center - app.canvas.rw / 2) <= 1, (center, app.canvas.rw / 2))

    app.destroy()


app.after(700, run)
app.mainloop()
print("\n=== 多选/对齐自测全部通过 ===" if ok else "\n=== 多选/对齐自测失败 ===")
sys.exit(0 if ok else 1)
