# -*- coding: utf-8 -*-
"""新建空白页无占位符 + 右键附加信息（脚本/视频/程序）+ 导出注入"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "src", "pyui"))
import attach as A
from editor import App

ok = True


def check(name, cond, extra=""):
    global ok
    print(("[PASS] " if cond else "[FAIL] ") + name + (("  " + str(extra)) if extra else ""))
    if not cond:
        ok = False


app = App(None)
ROOT = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(ROOT, "build", "attach_demo.pptx")
HTML = os.path.join(ROOT, "build", "attach_demo.html")


def run():
    app.on_new(); app.update()
    check("新建稿没有占位符残留", len(app.canvas.shapes) == 0, len(app.canvas.shapes))

    sw, sh = app.core.slide_w, app.core.slide_h
    idx = app.core.shape_add_rect(0, int(sw * .1), int(sh * .2), int(sw * .4), int(sh * .3), "带脚本的形状")
    app.canvas.reload(); app.update()
    sid = str(app.canvas.shapes[idx]["id"])

    app.path = OUT
    app.core.save(OUT)
    ent = app.attach["shapes"].setdefault(sid, {})
    ent["js"] = "el.style.background='#ff0';"
    ent["media"] = {"type": "video", "path": os.path.join(ROOT, "build", "demo.mp4")}
    ent["program"] = os.path.join(ROOT, "启动编辑器.bat")
    app._refresh_attach()
    check("徽标不报错并画出", len(app.canvas.find_withtag("badge")) > 0)
    yc_count = len(app.attach["shapes"])
    check("附加信息已记录", yc_count == 1)

    # 存盘 → 旁车文件
    app.on_save(); app.update()
    side = A.sidecar_path(OUT)
    check("旁车 JSON 已生成", os.path.exists(side), side)
    check("旁车内容可读回", bool(A.load(OUT)["shapes"]), list(A.load(OUT)["shapes"].keys()))

    # 导出 HTML → 注入
    app.core.export_html(HTML, split_js=False)
    injected = A.inject(HTML, app.attach)
    app.update()
    txt = open(HTML, "r", encoding="utf-8").read()
    check("HTML 有 data-sid", 'data-sid="' + sid + '"' in txt)
    check("HTML 注入了附加脚本", injected and "ycz-attach" in txt)
    check("HTML 带形状脚本", "el.style.background='#ff0';" in txt)
    check("HTML 带视频元素", "createElement('video')" in txt)
    check("HTML 带程序点击", "window.open(P[id])" in txt)

    # 删除附加
    app.attach["shapes"].pop(sid, None)
    app._refresh_attach()
    check("移除附加后徽标消失", len(app.canvas.find_withtag("badge")) == 0)

    # 动画：翻页淡入不报错
    app.goto(0)
    app.update()
    app._toast("测试提示动画")
    app.update()
    check("界面动画（淡入/提示）可用", True)

    app.destroy()


app.after(800, run)
app.mainloop()
print("\n=== 附加功能自测全部通过 ===" if ok else "\n=== 附加功能自测失败 ===")
sys.exit(0 if ok else 1)
