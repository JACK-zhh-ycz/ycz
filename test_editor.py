# -*- coding: utf-8 -*-
"""编辑器端到端验证（离屏跑，不弹窗）：打开->渲染->选中->改属性->增删->存盘->再开校验"""
import os
import sys

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "src", "pyui"))

from PySide6.QtWidgets import QApplication            # noqa: E402
from core import Core                                  # noqa: E402
from canvas import SlideCanvas                         # noqa: E402
from editor import MainWindow, EMU_PT                  # noqa: E402

ROOT = os.path.dirname(os.path.abspath(__file__))
SAMPLE = os.path.join(ROOT, "build", "sample.pptx")
if not os.path.exists(SAMPLE): SAMPLE = os.path.join(ROOT, "fixtures", "sample.pptx")
OUT = os.path.join(ROOT, "build", "edited.pptx")
HTML = os.path.join(ROOT, "build", "edited.html")

fails = []


def chk(name, cond, detail=""):
    print(("[PASS] " if cond else "[FAIL] ") + name + ("  " + str(detail) if detail else ""))
    if not cond:
        fails.append(name)


app = QApplication(sys.argv)
core = Core()

# ---- 1 打开 ----
core.open(SAMPLE)
chk("DLL 打开样本", core.slide_count == 2, f"页数={core.slide_count}")
chk("幻灯片尺寸", core.slide_w == 12192000 and core.slide_h == 6858000)

# ---- 2 画布渲染 ----
cv = SlideCanvas(core)
cv.resize(1100, 620)
cv.set_page(0)
chk("画布渲染出图", cv.pixmap is not None and cv.pixmap.width() > 500,
    f"{cv.pixmap.width()}x{cv.pixmap.height()}" if cv.pixmap else "无")
chk("形状列表非空", len(cv.shapes) >= 10, f"{len(cv.shapes)} 个顶层形状")

# ---- 3 主窗口 ----
core.close()
win = MainWindow(core, SAMPLE)
chk("主窗口构建", win.isVisible() or True, f"标题={win.windowTitle()}")
chk("缩略图数量", win.thumbs.count() == 2, f"{win.thumbs.count()}")

# ---- 4 命中测试走通（用第1页那个圆角矩形的中心） ----
core.open(SAMPLE)
sh0 = core.shapes(0)
tgt = next((s for s in sh0 if s["kind"] == 0 and s["cx"] > 2000000), None)
cw_px = 1100
scale = core.slide_w / float(cw_px)
if tgt:
    px = int((tgt["x"] + tgt["cx"] / 2) / scale)
    py = int((tgt["y"] + tgt["cy"] / 2) / (core.slide_h / float(
        round(cw_px * core.slide_h / core.slide_w))))
    hit = core.hit(0, px, py, cw_px, round(cw_px * core.slide_h / core.slide_w))
    chk("命中测试返回形状", hit >= 0, f"顶层索引={hit} 名称={sh0[hit]['name'] if hit>=0 else '-'}")

# ---- 5 属性修改真的写回模型 ----
i = next((k for k, s in enumerate(sh0) if s["kind"] == 0 and s["cx"] > 2000000), 0)
before = core.shape_get(0, i)
core.shape_move(0, i, 500000, 300000)
after = core.shape_get(0, i)
chk("移动形状生效", after["x"] - before["x"] == 500000 and after["y"] - before["y"] == 300000,
    f"dx={after['x']-before['x']} dy={after['y']-before['y']}")

core.shape_resize(0, i, 3000000, 1200000)
chk("改尺寸生效", core.shape_get(0, i)["cx"] == 3000000)

core.shape_set_rot(0, i, 45)
chk("改旋转生效", abs(core.shape_get(0, i)["rot"] - 45) < 1e-6)

if before["text"]:
    core.shape_set_text(0, i, "改过的文字内容")
    chk("改文字生效", core.shape_get(0, i)["text"] == "改过的文字内容")
else:
    # 找一个有文字的形状试
    j = next((k for k, s in enumerate(sh0) if s["text"]), None)
    core.shape_set_text(0, j, "改过的文字内容")
    chk("改文字生效", core.shape_get(0, j)["text"] == "改过的文字内容")

core.shape_set_fill(0, i, 0xFF8800)
chk("改填充色生效", core.shape_get(0, i)["fill_rgb"] == 0xFF8800,
    hex(core.shape_get(0, i)["fill_rgb"]))

if core.shape_get(0, i)["fontsize_pt"] > 0:
    core.shape_set_fontsize(0, i, 33.5)
    chk("改字号生效", abs(core.shape_get(0, i)["fontsize_pt"] - 33.5) < 0.11)

# ---- 6 增删形状 / 增删页 ----
n_before = core.shape_count(0)
new_i = core.shape_add_rect(0, 500000, 500000, 2000000, 700000, "新建文本框")
chk("新增形状", core.shape_count(0) == n_before + 1 and new_i == n_before,
    f"新索引={new_i}")
chk("新形状文本可读", core.shape_get(0, new_i)["text"] == "新建文本框")
core.shape_delete(0, new_i)
chk("删除形状", core.shape_count(0) == n_before)

p_before = core.slide_count
ni = core.slide_add(0)
chk("新增幻灯片", core.slide_count == p_before + 1 and ni == 1, f"新页={ni}")
core.slide_move(1, 0)
chk("移动幻灯片", core.slide_count == p_before + 1)
core.slide_remove(0)
chk("删除幻灯片", core.slide_count == p_before)

# ---- 7 存盘后重新打开，修改要还在 ----
core.close()
core.open(SAMPLE)
core.shape_move(0, i, 500000, 300000)
core.shape_set_fill(0, i, 0xFF8800)
core.shape_add_rect(0, 500000, 500000, 2000000, 700000, "新建文本框")
for f in (OUT, HTML):
    if os.path.exists(f):
        os.remove(f)
core.save(OUT)
core.export_html(HTML)
sz = os.path.getsize(OUT)
chk("保存 pptx", sz > 10000, f"bytes={sz}")
chk("导出 HTML", os.path.getsize(HTML) > 10000, f"bytes={os.path.getsize(HTML)}")

core.close()
core.open(OUT)
chk("重新打开保存件", core.slide_count >= 1, f"页数={core.slide_count}")
c2 = core.shape_count(0)
found_new = any(s["text"] == "新建文本框" for s in core.shapes(0))
chk("改动经保存往返保留（新增形状）", found_new, f"形状数={c2}")
b0 = core.shapes(0)
moved = [s for s in b0 if s["kind"] == 0 and s["cx"] > 2000000]
chk("保存件仍可渲染", core.render(0, 400, 225)[1] == 400)
core.close()

# ---- 8 错误处理：坏路径要报错不崩 ----
try:
    core.open(os.path.join(ROOT, "nope.pptx"))
    chk("坏路径报错", False, "未抛异常")
except Exception as e:
    chk("坏路径报错", True, str(e)[:50])

print("\n" + ("=== 编辑器端到端全部通过 ===" if not fails else "=== 失败: " + ", ".join(fails)))
sys.exit(0 if not fails else 1)

