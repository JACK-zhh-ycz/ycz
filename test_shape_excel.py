# -*- coding: utf-8 -*-
"""第十六轮：表格可拖动 / 形状库新形状 / Excel 导入"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "src", "pyui"))
import xlsx_export
import xlsx_import
import editor as ed
from editor import App

ok = True


def check(name, cond, extra=""):
    global ok
    print(("[PASS] " if cond else "[FAIL] ") + name + (("  " + str(extra)) if extra else ""))
    if not cond:
        ok = False


class Ev:
    def __init__(self, x, y, state=0):
        self.x = x
        self.y = y
        self.state = state


app = App(None)
XLSX = os.path.join(os.path.dirname(os.path.abspath(__file__)), "build", "import_demo.xlsx")


def run():
    app.on_new(); app.update()
    w, h = app.core.slide_w, app.core.slide_h
    i = app.core.shape_add_table(0, int(w * .1), int(h * .2), int(w * .7), int(h * .4), 3, 4)
    app.canvas.reload(); app.canvas.select(i); app.update()
    cv = app.canvas
    cx, cy = cv.ox + cv.rw // 2, cv.oy + cv.rh // 2

    # 1) 表格里按住拖动 → 整张表移动
    x0 = app.core.shape_get(0, i)["x"]
    cv._on_press(Ev(cx, cy))
    cv._on_motion(Ev(cx + 60, cy + 40))
    cv._on_release(Ev(cx + 60, cy + 40))
    app.update()
    x1 = app.core.shape_get(0, i)["x"]
    check("按住表格拖动可以移动整张表", x1 > x0, (x0, x1))
    check("拖动后不会残留单元格选中框", cv.cell_sel is None, cv.cell_sel)

    # 2) 表格里单击（不拖）→ 选单元格
    cv._on_press(Ev(cv.ox + cv.rw // 2, cv.oy + cv.rh // 2))
    cv._on_release(Ev(cv.ox + cv.rw // 2, cv.oy + cv.rh // 2))
    app.update()
    check("单击表格选中单元格", cv.cell_sel is not None, cv.cell_sel)

    # 3) 形状库：立体 / 曲线 / 星形
    n0 = len(app.core.shapes(0))
    for g in ("cube", "can", "pyramid", "sphere", "curvedConnector3", "star5", "heart"):
        app._insert_prst(g)
    app.update()
    n1 = len(app.core.shapes(0))
    check("形状库能插入 7 个新形状", n1 == n0 + 7, (n0, n1))
    geoms = [s["name"] for s in app.core.shapes(0)]
    check("立体/曲线形状确实进了模型", "cube" in geoms and "curvedConnector3" in geoms, geoms[-8:])

    # 4) Excel 导入
    xlsx_export.write_xlsx(XLSX, [["月份", "收入", "支出"],
                                  ["1月", "1200", "800"],
                                  ["2月", "1500", "900"]], sheet_name="账目")
    rows, sheet = xlsx_import.read_first_sheet(XLSX)
    check("能读出 Excel 内容", len(rows) == 3 and rows[1][0] == "1月", rows[:2])
    check("能读出表名", sheet == "账目", sheet)

    orig = ed.filedialog.askopenfilename
    ed.filedialog.askopenfilename = lambda *a, **k: XLSX
    try:
        app.on_import_excel()
    finally:
        ed.filedialog.askopenfilename = orig
    app.update()
    tables = [k for k, s in enumerate(app.core.shapes(0)) if s["kind"] == 2]
    check("导入后多了一张表", len(tables) == 2, tables)
    ti = tables[-1]
    check("导入的表格尺寸对", app.core.table_size(0, ti) == (3, 3), app.core.table_size(0, ti))
    check("导入的表格文字对", app.core.table_cell_text(0, ti, 1, 0) == "1月",
          app.core.table_cell_text(0, ti, 1, 0))

    app.destroy()


app.after(800, run)
app.mainloop()
print("\n=== 第十六轮自测全部通过 ===" if ok else "\n=== 第十六轮自测失败 ===")
sys.exit(0 if ok else 1)
