# -*- coding: utf-8 -*-
"""内核 + ctypes 封装的无界面自测：编辑、撤销、回写保真、导出。

用法：python test_core.py
"""
import os
import sys
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "src", "pyui"))
from core import Core                      # noqa: E402

ROOT = os.path.dirname(os.path.abspath(__file__))
SAMPLE = os.path.join(ROOT, "build", "sample.pptx")
if not os.path.exists(SAMPLE): SAMPLE = os.path.join(ROOT, "fixtures", "sample.pptx")
OUT = os.path.join(ROOT, "build", "edited_surgical.pptx")
HTML = os.path.join(ROOT, "build", "edited_surgical.html")

fails = []


def chk(name, cond, detail=""):
    print(("[PASS] " if cond else "[FAIL] ") + name + ("  " + str(detail) if detail else ""))
    if not cond:
        fails.append(name)


c = Core()
c.open(SAMPLE)
chk("打开样本", c.slide_count == 2, f"{c.slide_count} 页")
chk("页面尺寸", c.slide_w == 12192000 and c.slide_h == 6858000, f"{c.slide_w}x{c.slide_h}")

shapes = c.shapes(0)
chk("形状列表", len(shapes) >= 10, f"{len(shapes)} 个")

# ---- 移动 + 撤销 ----
i = next(k for k, s in enumerate(shapes) if s["kind"] == 0 and s["cx"] > 2000000)
x0, y0 = shapes[i]["x"], shapes[i]["y"]
c.begin_edit()
for _ in range(5):
    c.shape_move(0, i, 20000, 10000)          # 模拟拖拽的多次增量
c.end_edit()
now = c.shape_get(0, i)
chk("多次拖拽生效", now["x"] == x0 + 100000 and now["y"] == y0 + 50000,
    f"dx={now['x'] - x0} dy={now['y'] - y0}")
c.undo()
back = c.shape_get(0, i)
chk("撤销恢复到拖动前", back["x"] == x0 and back["y"] == y0, f"x={back['x']} y={back['y']}")
c.redo()
chk("重做回到拖动后", c.shape_get(0, i)["x"] == x0 + 100000)
c.undo()

# ---- 文字样式 ----
has_text = next((k for k, s in enumerate(shapes) if s["text"]), None)
chk("存在文字形状", has_text is not None)
before_align = c.shape_text_align(0, has_text)
c.shape_set_align(0, has_text, 1)
chk("改对齐生效", c.shape_text_align(0, has_text) == 1, f"{before_align} -> {c.shape_text_align(0, has_text)}")
c.shape_set_bold(0, has_text, 1)
chk("加粗生效", c.shape_text_bold(0, has_text) == 1)
c.shape_set_textcolor(0, has_text, 0xFF3300)
tc = c.shape_text_color(0, has_text)
chk("文字颜色生效", tc == 0xFF3300, hex(tc) if tc >= 0 else tc)
c.undo()
chk("文字颜色可撤销", c.shape_text_color(0, has_text) != 0xFF3300)

# ---- 增删形状 ----
n0 = c.shape_count(0)
ni = c.shape_add_rect(0, 300000, 300000, 1500000, 500000, "撤销测试框")
chk("新增文本框", c.shape_count(0) == n0 + 1 and c.shape_get(0, ni)["text"] == "撤销测试框")
c.undo()
chk("撤销新增", c.shape_count(0) == n0)
c.redo()
chk("重做新增", c.shape_count(0) == n0 + 1)
victim_text = c.shape_get(0, ni)["text"]
c.shape_delete(0, ni)
chk("删除形状", c.shape_count(0) == n0)
c.undo()
chk("撤销删除", any(s["text"] == victim_text for s in c.shapes(0)), victim_text)
c.undo()   # 回到 n0

# ---- 页面操作 ----
p0 = c.slide_count
ni2 = c.slide_add(0)
chk("新增页", c.slide_count == p0 + 1 and ni2 == 1, f"新页={ni2}")
c.slide_duplicate(0)
chk("复制页", c.slide_count == p0 + 2)
c.undo()
c.undo()
chk("两次撤销回到原页数", c.slide_count == p0, f"{c.slide_count}")

# ---- 手术式回写保真 ----
if os.path.exists(OUT):
    os.remove(OUT)
c.save(OUT)
chk("保存成功", os.path.exists(OUT) and os.path.getsize(OUT) > 10000, f"{os.path.getsize(OUT)} bytes")

with zipfile.ZipFile(SAMPLE) as z0, zipfile.ZipFile(OUT) as z1:
    n0s = set(z0.namelist())
    n1s = set(z1.namelist())
    missing = n0s - n1s
    chk("保存后无部件丢失", not missing, f"丢失 {sorted(missing)[:6]}")
    media0 = [n for n in n0s if n.startswith("ppt/media/")]
    media1 = [n for n in n1s if n.startswith("ppt/media/")]
    chk("图片部件保留", media0 == media1, f"{media1}")
    if media0:
        same = z0.read(media0[0]) == z1.read(media0[0])
        chk("图片字节完全一致", same, f"{len(z0.read(media0[0]))} bytes")
    # 母版/主题/版式应逐字节一致
    for pref in ("ppt/slideMasters/", "ppt/slideLayouts/", "ppt/theme/"):
        p0l = [n for n in n0s if n.startswith(pref)]
        p1l = [n for n in n1s if n.startswith(pref)]
        ok = p0l == p1l and all(z0.read(n) == z1.read(n) for n in p0l)
        chk(f"{pref} 原样保留", ok, f"{len(p0l)} 个")

c.export_html(HTML)
chk("导出 HTML", os.path.getsize(HTML) > 10000, f"{os.path.getsize(HTML)} bytes")

# ---- 插件 ----
plugs = c.plugins()
chk("插件已加载", len(plugs) >= 3, ", ".join(p["name"] for p in plugs))
ok, text = c.plugin_command("stats")
chk("插件命令 stats", ok == 1 and "页数" in text, text.replace("\n", " | ")[:60])
with open(HTML, "r", encoding="utf-8") as f:
    html_txt = f.read()
chk("插件注入 HTML（水印）", "wm-layer" in html_txt)
chk("HTML 画布尺寸读取不依赖 offsetWidth", "parseFloat(ss[0].style.width)" in html_txt)

c.close()

# ---- 重新打开保存件 ----
c2 = Core()
c2.open(OUT)
chk("重新打开保存件", c2.slide_count == p0, f"{c2.slide_count} 页")
chk("保存件仍可渲染", c2.render(0, 400, 225)[1] == 400)
chk("图片形状仍在", any(s["kind"] == 1 for s in c2.shapes(0)))
chk("表格形状仍在", any(s["kind"] == 2 for s in c2.shapes(1)))
c2.close()

# ---- 错误处理 ----
try:
    c3 = Core()
    c3.open(os.path.join(ROOT, "不存在.pptx"))
    chk("坏路径报错", False, "未抛异常")
except Exception as e:
    chk("坏路径报错", True, str(e)[:40])

print("\n" + ("=== 内核自测全部通过 ===" if not fails else "=== 失败: " + ", ".join(fails)))
sys.exit(0 if not fails else 1)

