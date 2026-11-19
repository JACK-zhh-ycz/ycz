# -*- coding: utf-8 -*-
"""网页/多格式图片支持自测"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "src", "pyui"))
from core import Core                                    # noqa: E402
from web_snapshot import html_to_png                      # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
BLANK = os.path.join(HERE, "templates", "空白.pptx")
DEMO = os.path.join(HERE, "build", "demo.html")
SHOT = os.path.join(HERE, "build", "demo_shot.png")
OUT = os.path.join(HERE, "build", "web_test.pptx")
OUT_HTML = os.path.join(HERE, "build", "web_test.html")

fails = []


def check(cond, name, extra=""):
    print(("[PASS] " if cond else "[FAIL] ") + name + ("  " + str(extra) if extra else ""))
    if not cond:
        fails.append(name)


# 1) HTML -> PNG
png = html_to_png(DEMO, SHOT, width=1600, height=900)
check(os.path.exists(png) and os.path.getsize(png) > 10000, "网页截图", os.path.getsize(png))

# 2) 插入空白稿
c = Core()
c.open(BLANK)
w, h = c.slide_w, c.slide_h
idx = c.shape_add_picture(0, png, 0, 0, w, h)
check(c.shape_count(0) == 1 and c.shape_get(0, 0)["kind_name"] == "图片", "网页截图插入幻灯片")
c.save(OUT)

# 3) 插件与命令
n = c.plugins_load(os.path.join(HERE, "build", "plugins"))
names = [p["name"] for p in c.plugins()]
check("网页嵌入" in names, "网页嵌入插件已加载", names)
rc, out = c.plugin_command("web", "%d %s 8 10 84 74" % (1, DEMO))
check(rc > 0 and "已嵌入" in out, "插件命令设置嵌入", out.strip())
rc2, out2 = c.plugin_command("web", "")
check(rc2 > 0 and "第1页" in out2, "插件状态可查", out2.strip())

# 4) 导出 HTML 带 iframe
c.export_html(OUT_HTML)
html = open(OUT_HTML, encoding="utf-8").read()
check("pt-webembed" in html, "导出 HTML 含嵌入层")
check("<iframe" in html, "导出 HTML 含 iframe")
check("demo.html" in html, "iframe 指向网页")

# 5) 保存后打开
c2 = Core()
c2.open(OUT)
check(c2.shape_count(0) == 1, "重新打开图片仍在")

print("\n=== 网页/多格式自测" + ("全部通过 ===" if not fails else "失败: " + ", ".join(fails) + " ==="))
sys.exit(1 if fails else 0)
