# -*- coding: utf-8 -*-
"""文字编辑框回归：确定 / Ctrl+Enter / Ctrl+S（提交并标记保存） / Esc

headless 下 event_generate 送不进按键，这里直接校验绑定存在 + 回调行为，
并按画布的真实处理顺序（先写回、再保存）验证 want_save 标记。
"""
import os
import sys
import tkinter as tk

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "src", "pyui"))
from canvas import TextDialog

ok = True


def check(name, cond, extra=""):
    global ok
    print(("[PASS] " if cond else "[FAIL] ") + name + (("  " + str(extra)) if extra else ""))
    if not cond:
        ok = False


def run_case(title, text, action):
    root = tk.Tk()
    root.withdraw()
    bind = {}

    def poke():
        for w in root.winfo_children():
            if isinstance(w, tk.Toplevel):
                for seq in ("<Control-Return>", "<Control-s>", "<Escape>"):
                    bind[seq] = bool(w.bind(seq))
                action(w)
                return

    root.after(250, poke)
    dlg = TextDialog(root, title, text)
    root.update()
    root.destroy()
    dlg.bindings = bind
    return dlg


# 1) 点“确定”
r1 = run_case("t1", "hello", lambda d: d._ok())
check("点确定返回文本", r1.result == "hello", r1.result)
check("普通确定不请求保存", r1.want_save is False)

# 2) Ctrl+Enter 绑定 + 提交
r2 = run_case("t2", "line1", lambda d: d._ok_key())
check("Ctrl+Enter 已绑定", r2.bindings.get("<Control-Return>"))
check("Ctrl+Enter 提交", r2.result == "line1", r2.result)

# 3) Ctrl+S：改内容 + 提交 + 请求保存（画布随后写回并保存）
r3 = run_case("t3", "old", lambda d: (d.txt.delete("1.0", "end"),
                                      d.txt.insert("1.0", "new text"),
                                      d._ok_save()))
check("Ctrl+S 已绑定", r3.bindings.get("<Control-s>"))
check("Ctrl+S 提交新文本", r3.result == "new text", r3.result)
check("Ctrl+S 请求保存", r3.want_save is True)

# 4) Esc 取消
r4 = run_case("t4", "cancel me", lambda d: d._cancel_key())
check("Esc 已绑定", r4.bindings.get("<Escape>"))
check("Esc 取消返回 None", r4.result is None, r4.result)

print("\n=== 文字编辑自测全部通过 ===" if ok else "\n=== 文字编辑自测失败 ===")
sys.exit(0 if ok else 1)
