# -*- coding: utf-8 -*-
"""ycz 附加信息：形状脚本 / 视频音频 / 同目录程序，保存在 <演示文稿>.ycz.json 旁车文件"""
import json
import os
import urllib.parse

SIDE_SUFFIX = ".ycz.json"


def sidecar_path(pptx_path):
    if not pptx_path:
        return None
    base, _ = os.path.splitext(pptx_path)
    return base + SIDE_SUFFIX


def empty():
    return {"shapes": {}, "global_js": "", "slide_js": {}}


def load(pptx_path):
    p = sidecar_path(pptx_path)
    if not p or not os.path.exists(p):
        return empty()
    try:
        with open(p, "r", encoding="utf-8") as f:
            d = json.load(f)
    except Exception:
        return empty()
    if not isinstance(d, dict):
        return empty()
    d.setdefault("shapes", {})
    d.setdefault("global_js", "")
    d.setdefault("slide_js", {})
    return d


def save(pptx_path, data):
    p = sidecar_path(pptx_path)
    if not p:
        return False
    if not data.get("shapes") and not data.get("global_js") and not data.get("slide_js"):
        if os.path.exists(p):
            os.remove(p)
        return True
    with open(p, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    return True


def shape_entry(data, sid):
    return data.setdefault("shapes", {}).setdefault(str(sid), {})


def file_url(path):
    p = os.path.abspath(path).replace("\\", "/")
    if not p.startswith("/"):
        p = "/" + p
    return "file://" + urllib.parse.quote(p, safe="/:@")


def _js_str(s):
    return json.dumps(s, ensure_ascii=False)


def build_script(data):
    """生成注入到导出 HTML 末尾的脚本：形状脚本 + 视频/音频 + 程序按钮 + 全局脚本"""
    parts = []
    parts.append("window.YCZ={shape:function(id){return document.querySelector('[data-sid=\"'+id+'\"]');},"
                 "on:function(id,ev,fn){var el=this.shape(id);if(el)el.addEventListener(ev,fn);}};")

    media = {}
    for sid, ent in (data.get("shapes") or {}).items():
        m = ent.get("media")
        if m and m.get("path"):
            media[sid] = {"type": m.get("type", "video"), "src": file_url(m["path"]),
                          "auto": bool(m.get("autoplay"))}
    if media:
        parts.append("(function(){var M=" + _js_str(media) + ";"
                     "Object.keys(M).forEach(function(id){"
                     "var el=YCZ.shape(id);if(!el)return;var it=M[id];"
                     "var v;if(it.type==='audio'){v=document.createElement('audio');v.controls=true;"
                     "v.style.cssText='position:absolute;left:0;bottom:0;width:100%;z-index:5;';}"
                     "else{v=document.createElement('video');v.controls=true;v.playsInline=true;"
                     "v.style.cssText='position:absolute;left:0;top:0;width:100%;height:100%;object-fit:contain;z-index:5;background:#000;';}"
                     "v.src=it.src;if(it.auto){v.autoplay=true;v.muted=true;}"
                     "el.appendChild(v);});})();")

    progs = {}
    for sid, ent in (data.get("shapes") or {}).items():
        if ent.get("program"):
            progs[sid] = file_url(ent["program"])
    if progs:
        parts.append("(function(){var P=" + _js_str(progs) + ";"
                     "Object.keys(P).forEach(function(id){"
                     "var el=YCZ.shape(id);if(!el)return;el.style.cursor='pointer';"
                     "el.title='点击运行本地程序：'+P[id];"
                     "el.addEventListener('click',function(){window.open(P[id]);});});})();")

    for sid, ent in (data.get("shapes") or {}).items():
        if ent.get("js"):
            parts.append("(function(){var el=YCZ.shape(" + json.dumps(str(sid)) + ");\n" +
                         ent["js"] + "\n})();")
    for idx, js in (data.get("slide_js") or {}).items():
        if js:
            parts.append("(function(){var sl=document.getElementById('slide-" + str(idx) + "');\n" +
                         js + "\n})();")
    if data.get("global_js"):
        parts.append("(function(){\n" + data["global_js"] + "\n})();")

    return "\n".join(parts)


def inject(html_path, data, pptx_path=None):
    """把附加脚本插到导出 HTML 的 </body> 前；没有内容时不做改动"""
    script = build_script(data)
    if not (data.get("shapes") or data.get("global_js") or data.get("slide_js")):
        return False
    if not os.path.exists(html_path):
        return False
    with open(html_path, "r", encoding="utf-8") as f:
        html = f.read()
    marker = "<!-- ycz-attach -->"
    if marker in html:
        return False
    block = marker + "\n<script>\n" + script + "\n</script>\n"
    i = html.rfind("</body>")
    if i < 0:
        html += block
    else:
        html = html[:i] + block + html[i:]
    with open(html_path, "w", encoding="utf-8") as f:
        f.write(html)
    return True
