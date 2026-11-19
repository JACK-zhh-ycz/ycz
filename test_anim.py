# -*- coding: utf-8 -*-
"""切换动画 / 入场动画 自测"""
import os
import sys
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "src", "pyui"))
from core import Core                                     # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
BLANK = os.path.join(HERE, "templates", "空白.pptx")
OUT = os.path.join(HERE, "build", "anim_test.pptx")
OUT_HTML = os.path.join(HERE, "build", "anim_test.html")
fails = []


def check(cond, name, extra=""):
    print(("[PASS] " if cond else "[FAIL] ") + name + ("  " + str(extra) if extra else ""))
    if not cond:
        fails.append(name)


c = Core()
c.open(BLANK)
w, h = c.slide_w, c.slide_h
i0 = c.shape_add_rect(0, int(w * .08), int(h * .12), int(w * .4), int(h * .15), "入场标题")
i1 = c.shape_add_rect(0, int(w * .08), int(h * .4), int(w * .4), int(h * .15), "第二条")

# 切换
c.slide_set_transition(0, "push", 800, 3)
kind, dur, d = c.slide_transition(0)
check(kind == "push" and dur == 600 and d == 3, "切换动画写入模型", (kind, dur, d))

# 入场动画
c.shape_set_anim(0, 0, "fade", 500)
c.shape_set_anim(0, 1, "zoom", 700)
check(c.shape_anim(0, 0)[0] == "fade", "形状0 淡入")
check(c.shape_anim(0, 1)[0] == "zoom", "形状1 缩放")

c.save(OUT)
# 文件里应有 p:transition / p:timing
with zipfile.ZipFile(OUT) as z:
    xml = z.read("ppt/slides/slide1.xml").decode("utf-8")
check("<p:transition" in xml and "<p:push" in xml, "pptx 含切换 XML")
check("<p:timing" in xml and "presetID=\"10\"" in xml, "pptx 含动画 XML")

# 重新打开
c2 = Core()
c2.open(OUT)
k2, d2, dir2 = c2.slide_transition(0)
check(k2 == "push" and d2 == 600 and dir2 == 3, "重开切换保留", (k2, d2, dir2))
check(c2.shape_anim(0, 0)[0] == "fade", "重开动画保留 shape0")
check(c2.shape_anim(0, 1)[0] == "zoom", "重开动画保留 shape1")

# 导出 HTML
c2.export_html(OUT_HTML)
html = open(OUT_HTML, encoding="utf-8").read()
check('data-tr="push"' in html, "HTML 带切换标记")
check('data-a="fade"' in html and 'data-a="zoom"' in html, "HTML 带入场动画标记")
check("inkbar" in html and "ink-pen" in html and "pointerdown" in html, "HTML 带批注工具栏")

# 清空动画
c2.shape_anim_clear(0)
c2.save(OUT)
with zipfile.ZipFile(OUT) as z:
    xml = z.read("ppt/slides/slide1.xml").decode("utf-8")
check("<p:timing" not in xml, "清空动画后 timing 被移除")
check("<p:transition" in xml, "切换仍保留")

print("\n=== 动画自测" + ("全部通过 ===" if not fails else "失败: " + ", ".join(fails) + " ==="))
sys.exit(1 if fails else 0)
