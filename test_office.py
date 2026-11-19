# -*- coding: utf-8 -*-
"""Office 化第二轮功能自测：字体/斜体/下划线/边框/预设形状/表格/保存保真"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "src", "pyui"))
from core import Core, EMU_PER_PT                                # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
SAMPLE = os.path.join(HERE, "build", "sample.pptx")
if not os.path.exists(SAMPLE): SAMPLE = os.path.join(HERE, "fixtures", "sample.pptx")
OUT = os.path.join(HERE, "build", "office_test.pptx")
OUT_HTML = os.path.join(HERE, "build", "office_test.html")

fails = []


def check(cond, name, extra=""):
    print(("[PASS] " if cond else "[FAIL] ") + name + ("  " + str(extra) if extra else ""))
    if not cond:
        fails.append(name)


c = Core()
c.open(SAMPLE)
si = 0

# 1) 找第一个有文字的顶层形状
idx_text = None
for i, sh in enumerate(c.shapes(si)):
    if sh["text"].strip():
        idx_text = i
        break
check(idx_text is not None, "找到带文字形状", idx_text)

old = c.shape_get(si, idx_text)
print("  原样式: 字号=%s 粗=%s 斜=%s 下划=%s 字体=%r"
      % (old["fontsize_pt"], c.shape_text_bold(si, idx_text),
         old["italic"], old["underline"], old["font_name"]))

# 2) 字体名
c.shape_set_fontname(si, idx_text, "微软雅黑")
check(c.shape_get(si, idx_text)["font_name"] in ("微软雅黑", ""),
      "设置字体名", c.shape_get(si, idx_text)["font_name"])

# 3) 斜体 / 下划线开关
c.shape_set_italic(si, idx_text, 1)
check(c.shape_text_italic(si, idx_text) == 1, "开启斜体")
c.shape_set_italic(si, idx_text, 0)
check(c.shape_text_italic(si, idx_text) == 0, "关闭斜体")
c.shape_set_underline(si, idx_text, 1)
check(c.shape_text_underline(si, idx_text) == 1, "开启下划线")
c.shape_set_underline(si, idx_text, 0)
check(c.shape_text_underline(si, idx_text) == 0, "关闭下划线")

# 4) 字号
c.shape_set_fontsize(si, idx_text, 28)
check(abs(c.shape_get(si, idx_text)["fontsize_pt"] - 28) < 0.1, "字号 28")

# 5) 边框线
c.shape_set_line(si, idx_text, 0xE74C3C, 2.0)
check(c.shape_line(si, idx_text) == 0xE74C3C, "设置边框色", hex(c.shape_line(si, idx_text)))
c.shape_set_line(si, idx_text, 0)
check(c.shape_line(si, idx_text) == 0, "取消边框")

# 6) 插入预设形状
shapes = [
    ("roundRect", 0x4472C4, 0),
    ("ellipse", 0xED7D31, 0x7F7F7F),
    ("star5", 0xFFC000, 0),
    ("rightArrow", 0x70AD47, 0),
    ("line", 0, 0x404040),
]
created = []
w, h = c.slide_w, c.slide_h
base_count = c.shape_count(si)
for k, (geom, fill, line) in enumerate(shapes):
    ix = c.shape_add_shape(si, geom, 200000 + k * 900000, 500000, 800000, 600000, fill, line)
    created.append(ix)
    sh = c.shape_get(si, ix)
    check(sh["name"] == geom, "新增形状 " + geom, sh["name"])
check(c.shape_count(si) == base_count + len(created), "形状数量增长",
      "%d -> %d" % (base_count, c.shape_count(si)))

# 7) 插入表格
it = c.shape_add_table(si, int(w * 0.1), int(h * 0.55), int(w * 0.6), int(h * 0.35), 3, 4)
check("表格" in c.shape_get(si, it)["text"], "新增表格", c.shape_get(si, it)["kind_name"])
check(c.shape_get(si, it)["kind_name"] == "表格", "表格类型正确")

# 8) 撤销 / 重做（表格）
c.undo()
check(c.shape_count(si) == it, "撤销表格", c.shape_count(si))
c.redo()
check(c.shape_count(si) == it + 1, "重做表格", c.shape_count(si))

# 9) 表格单元格文字
rows, cols = c.table_size(si, it)
check((rows, cols) == (3, 4), "表格尺寸", (rows, cols))
cell = c.table_cell_at(si, it, int((c.shape_get(si, it)["x"] + 200000) / c.slide_w * 900),
                       int((c.shape_get(si, it)["y"] + 100000) / c.slide_h * 500), 900, 500)
check(cell >= 0, "像素命中单元格", cell)
c.table_cell_set_text(si, it, 0, 0, "产品")
c.table_cell_set_text(si, it, 1, 1, "第二行第二列")
check(c.table_cell_text(si, it, 0, 0) == "产品", "写入单元格", c.table_cell_text(si, it, 0, 0))
check(c.table_cell_text(si, it, 1, 1) == "第二行第二列", "写入另一单元格",
      c.table_cell_text(si, it, 1, 1))

# 9) 保存 + 重开
c.save(OUT)
check(os.path.exists(OUT), "保存", str(os.path.getsize(OUT)))
c2 = Core()
c2.open(OUT)
check(c2.slide_count == c.slide_count, "页数不变")
check(c2.shape_count(si) == c.shape_count(si), "第一页形状数一致",
      "%d vs %d" % (c2.shape_count(si), c.shape_count(si)))
st = c2.shape_get(si, idx_text)
check(abs(st["fontsize_pt"] - 28) < 0.1, "保存后字号保留", st["fontsize_pt"])
check(st["font_name"] in ("微软雅黑", ""), "保存后字体保留", st["font_name"])
check(any(c2.shape_get(si, i)["kind_name"] == "表格" for i in range(c2.shape_count(si))),
      "保存后表格仍在")
# 单元格文字保存后仍在
ok = False
for i in range(c2.shape_count(si)):
    if c2.shape_get(si, i)["kind_name"] == "表格":
        ok = (c2.table_cell_text(si, i, 0, 0) == "产品"
              and c2.table_cell_text(si, i, 1, 1) == "第二行第二列")
        break
check(ok, "保存后单元格文字保留")

# 10) 导出 HTML
c2.export_html(OUT_HTML)
check(os.path.exists(OUT_HTML) and os.path.getsize(OUT_HTML) > 10000, "导出 HTML",
      str(os.path.getsize(OUT_HTML)))

print("\n=== Office 功能自测" + ("全部通过 ===" if not fails else "失败: " + ", ".join(fails) + " ==="))
sys.exit(1 if fails else 0)

