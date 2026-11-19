# -*- coding: utf-8 -*-
"""把本地 HTML 渲染成图片（用 Edge 无头模式截图）

编辑画布是 GDI+ 画的，不能直接显示网页；
这里先把网页截成 PNG，再作为图片形状插入幻灯片，
这样画布、保存后的 pptx、导出的 HTML 放映都能看到内容。
"""
import os
import subprocess
import tempfile

EDGE_CANDIDATES = [
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
]


def find_browser():
    for p in EDGE_CANDIDATES:
        if os.path.exists(p):
            return p
    return None


def html_to_png(html_path, out_png=None, width=1600, height=900, wait_ms=1200):
    """把 html_path 截成 PNG，返回图片绝对路径；失败抛 RuntimeError"""
    browser = find_browser()
    if not browser:
        raise RuntimeError("找不到 Edge/Chrome，无法渲染网页")
    html_path = os.path.abspath(html_path)
    if not os.path.exists(html_path):
        raise RuntimeError("网页文件不存在: " + html_path)
    if out_png is None:
        fd, out_png = tempfile.mkstemp(prefix="web_", suffix=".png")
        os.close(fd)
    out_png = os.path.abspath(out_png)
    args = [
        browser, "--headless=new", "--disable-gpu", "--hide-scrollbars",
        "--force-device-scale-factor=1",
        "--virtual-time-budget=%d" % int(wait_ms),
        "--window-size=%d,%d" % (int(width), int(height)),
        "--screenshot=" + out_png,
        "file:///" + html_path.replace("\\", "/").lstrip("/"),
    ]
    try:
        subprocess.run(args, capture_output=True, timeout=40)
    except subprocess.TimeoutExpired:
        raise RuntimeError("渲染网页超时（HTML 里可能有很慢的脚本）")
    if not os.path.exists(out_png) or os.path.getsize(out_png) < 1000:
        raise RuntimeError("渲染失败：没有生成截图")
    return out_png


if __name__ == "__main__":
    import sys
    print(html_to_png(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None))
