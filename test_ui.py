# -*- coding: utf-8 -*-
"""界面冒烟测试：真正建窗口、加载样本、切页、选中、改属性，然后自动退出。

跑法：python test_ui.py
"""
import os
import sys
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "src", "pyui"))

from editor import App, EMU_PER_PT      # noqa: E402

SAMPLE = os.path.join(HERE, "build", "sample.pptx")
if not os.path.exists(SAMPLE): SAMPLE = os.path.join(HERE, "fixtures", "sample.pptx")
fails = []


def chk(name, cond, detail=""):
    print(("[PASS] " if cond else "[FAIL] ") + name + ("  " + str(detail) if detail else ""))
    if not cond:
        fails.append(name)


app = App(None)
try:
    app.update()
    chk("主窗口创建", app.winfo_exists() == 1)
    app.load_file(SAMPLE)
    app.update()
    chk("缩略图数量", len(app.thumb_box.winfo_children()) == 2,
        len(app.thumb_box.winfo_children()))
    chk("画布渲染", app.canvas.tkimg is not None,
        f"{app.canvas.rw}x{app.canvas.rh}" if app.canvas.tkimg else "无")

    # 选中第一页第一个大形状
    app.canvas.select(0)
    app.update()
    sh = app.canvas.selected_shape()
    chk("选中形状", sh is not None, sh["name"] if sh else "")
    chk("属性面板回填", app.e_w.get() not in ("", "0.0"), app.e_w.get())

    # 改 X 坐标走属性面板逻辑
    if sh:
        old_x = sh["x"]
        app.e_x.delete(0, "end")
        app.e_x.insert(0, f"{(old_x + 254000) / EMU_PER_PT:.1f}")
        app.on_prop_apply()
        app.update()
        new_shape = app.canvas.selected_shape()
        chk("属性改位置生效", abs(new_shape["x"] - (old_x + 254000)) < 3000,
            f"{old_x} -> {new_shape['x']}")
        chk("撤销按钮可用", app.core.can_undo())
        app.on_undo()
        app.update()
        chk("撤销后位置复原", abs(app.canvas.selected_shape()["x"] - old_x) < 3000)

    # 切页
    app.goto(1)
    app.update()
    chk("切换页面", app.canvas.slide_idx == 1 and app.canvas.tkimg is not None)
    chk("第二页有形状", len(app.canvas.shapes) > 0, len(app.canvas.shapes))
finally:
    try:
        app.destroy()
    except Exception:
        traceback.print_exc()

print("\n" + ("=== 界面冒烟测试通过 ===" if not fails else "=== 失败: " + ", ".join(fails)))
sys.exit(0 if not fails else 1)

