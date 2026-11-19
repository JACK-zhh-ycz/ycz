# -*- coding: utf-8 -*-
"""Excel 式表格编辑：单元格选中 / 键盘导航 / 打字覆盖 / 粘贴整块 / 导出 xlsx"""
import os
import sys
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "src", "pyui"))
from editor import App

ok = True


def check(name, cond, extra=""):
    global ok
    print(("[PASS] " if cond else "[FAIL] ") + name + (("  " + str(extra)) if extra else ""))
    if not cond:
        ok = False


class Key:
    def __init__(self, keysym, char="", state=0):
        self.keysym = keysym
        self.char = char
        self.state = state


app = App(None)
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "build", "excel_demo.xlsx")


def run():
    app.on_new(); app.update()
    w, h = app.core.slide_w, app.core.slide_h
    i = app.core.shape_add_table(0, int(w * .08), int(h * .18), int(w * .8), int(h * .5), 3, 4)
    app.canvas.reload(); app.canvas.select(i); app.update()

    cv = app.canvas
    check("单击表格能定位到单元格", cv._cell_at(i, cv.ox + cv.rw // 2, cv.oy + cv.rh // 2) is not None)
    cv.select_cell(i, 0, 0)
    check("选中框出现", cv.cell_sel == (i, 0, 0))

    cv._on_cell_key(Key("Down"))
    check("↓ 下移一行", cv.cell_sel == (i, 1, 0), cv.cell_sel)
    cv._on_cell_key(Key("Right"))
    cv._on_cell_key(Key("Tab"))
    check("→ 与 Tab 右移", cv.cell_sel == (i, 1, 2), cv.cell_sel)

    cv._on_cell_key(Key("??", char="A"))
    app.update()
    check("直接打字即进入编辑", cv._edit is not None)
    cv._edit.event_generate("<Return>")
    app.update()
    check("Enter 提交并下移", cv._edit is None and cv.cell_sel[1] == 2, cv.cell_sel)
    check("字写进了格子", app.core.table_cell_text(0, i, 1, 2) == "A",
          app.core.table_cell_text(0, i, 1, 2))

    cv.select_cell(i, 0, 0)
    cv._on_cell_key(Key("Delete"))
    app.update()
    app.core.table_cell_set_text(0, i, 0, 0, "标题1")
    cv.select_cell(i, 0, 0)
    cv._on_cell_key(Key("Delete"))
    check("Delete 清空格子", app.core.table_cell_text(0, i, 0, 0) == "")

    # 粘贴 Excel 复制的一片（制表符分隔，超出原表大小 → 自动加行列）
    cv.select_cell(i, 1, 1)
    cv._clip_text = lambda: "甲\t乙\r\n丙\t丁"
    cv._on_cell_key(Key("v", state=0x0004))
    app.update()
    check("粘贴后格子有内容", app.core.table_cell_text(0, i, 1, 1) == "甲"
          and app.core.table_cell_text(0, i, 2, 2) == "丁")

    # 导出 xlsx
    cv.select_cell(i, 0, 0)
    app.core.table_cell_set_text(0, i, 0, 0, "标题1")
    app.path = OUT.replace(".xlsx", ".pptx")
    import xlsx_export
    rows, cols = app.core.table_size(0, i)
    data = [[app.core.table_cell_text(0, i, r, c) for c in range(cols)] for r in range(rows)]
    xlsx_export.write_xlsx(OUT, data, sheet_name="测试", col_widths=[12] * cols)
    check("xlsx 文件生成", os.path.exists(OUT) and os.path.getsize(OUT) > 0)
    with zipfile.ZipFile(OUT) as z:
        names = z.namelist()
        sheet = z.read("xl/worksheets/sheet1.xml").decode("utf-8")
    check("xlsx 结构完整", "xl/workbook.xml" in names and "xl/styles.xml" in names, names)
    check("单元格内容在 xlsx 里", "标题1" in sheet and "甲" in sheet)
    print("xlsx 大小", os.path.getsize(OUT), "字节")

    app.destroy()


app.after(800, run)
app.mainloop()
print("\n=== Excel 式表格自测全部通过 ===" if ok else "\n=== Excel 式表格自测失败 ===")
sys.exit(0 if ok else 1)
