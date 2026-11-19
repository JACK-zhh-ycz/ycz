# -*- coding: utf-8 -*-
"""表格加行/删行/加列/删列 + 持久化 + 撤销"""
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
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "build", "table_ops.pptx")


def run():
    app.on_new(); app.update()
    w, h = app.core.slide_w, app.core.slide_h
    i = app.core.shape_add_table(0, int(w * .1), int(h * .2), int(w * .75), int(h * .45), 3, 4)
    app.canvas.reload(); app.canvas.select(i); app.update()
    app.core.table_cell_set_text(0, i, 0, 0, "标题1")
    app.canvas.reload(keep_sel=True); app.canvas.select(i); app.update()
    app.path = OUT

    check("初始 3 行 4 列", app.core.table_size(0, i) == (3, 4), app.core.table_size(0, i))

    app.canvas.last_cell = (0, 1)
    app.on_table_op("add_row")
    check("加一行 → 4 行", app.core.table_size(0, i) == (4, 4), app.core.table_size(0, i))
    check("原有单元格文字还在", app.core.table_cell_text(0, i, 0, 0) == "标题1")

    app.canvas.last_cell = (1, 1)
    app.on_table_op("add_col")
    check("加一列 → 5 列", app.core.table_size(0, i) == (4, 5), app.core.table_size(0, i))

    app.canvas.last_cell = (1, 1)
    app.on_table_op("del_row")
    check("删一行 → 3 行", app.core.table_size(0, i) == (3, 5), app.core.table_size(0, i))

    app.canvas.last_cell = (0, 1)
    app.on_table_op("del_col")
    check("删一列 → 4 列", app.core.table_size(0, i) == (3, 4), app.core.table_size(0, i))

    # 撤销
    app.on_undo()
    app.update()
    check("撤销后回到 3x5", app.core.table_size(0, i) == (3, 5), app.core.table_size(0, i))

    # 存盘重开
    app.on_save()
    check("存盘成功", os.path.exists(OUT) and os.path.getsize(OUT) > 0)
    app.load_file(OUT)
    app.update()
    st = app.core.shapes(0)
    ti = [k for k, s in enumerate(st) if s["kind"] == 2]
    check("重开后表格还在", len(ti) == 1, ti)
    if ti:
        check("重开后 3x5 行列正确", app.core.table_size(0, ti[0]) == (3, 5),
              app.core.table_size(0, ti[0]))

    app.destroy()


app.after(800, run)
app.mainloop()
print("\n=== 表格行列自测全部通过 ===" if ok else "\n=== 表格行列自测失败 ===")
sys.exit(0 if ok else 1)
