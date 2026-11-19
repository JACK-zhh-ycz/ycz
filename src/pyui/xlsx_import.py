# -*- coding: utf-8 -*-
"""读取 .xlsx 的第一个工作表（只用标准库 zipfile + xml）。"""
import re
import zipfile
from xml.etree import ElementTree as ET

NS = "{http://schemas.openxmlformats.org/spreadsheetml/2006/main}"


def _col_index(ref):
    m = re.match(r"([A-Z]+)", ref or "")
    if not m:
        return -1
    n = 0
    for ch in m.group(1):
        n = n * 26 + (ord(ch) - 64)
    return n - 1


def read_first_sheet(path, max_rows=60, max_cols=30):
    """返回 (rows, sheet_name)，rows 是 [["文字", ...], ...]，空单元格为 ""。"""
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        # 共享字符串
        shared = []
        if "xl/sharedStrings.xml" in names:
            root = ET.fromstring(z.read("xl/sharedStrings.xml"))
            for si in root.findall(NS + "si"):
                shared.append("".join(t.text or "" for t in si.iter(NS + "t")))
        # 表名
        sheet_name = "Sheet1"
        try:
            wb = ET.fromstring(z.read("xl/workbook.xml"))
            first = wb.find(NS + "sheets")
            if first is not None and len(first):
                sheet_name = first[0].get("name") or "Sheet1"
        except Exception:
            pass
        # 第一张表
        sheets = sorted(n for n in names if n.startswith("xl/worksheets/sheet") and n.endswith(".xml"))
        if not sheets:
            raise ValueError("这个文件里没有工作表")
        root = ET.fromstring(z.read(sheets[0]))

    rows = []
    for r_el in root.iter(NS + "row"):
        cells = {}
        for c in r_el.findall(NS + "c"):
            ci = _col_index(c.get("r"))
            if ci < 0:
                continue
            t = c.get("t")
            v = ""
            if t == "inlineStr":
                is_el = c.find(NS + "is")
                if is_el is not None:
                    v = "".join(x.text or "" for x in is_el.iter(NS + "t"))
            else:
                v_el = c.find(NS + "v")
                if v_el is not None:
                    v = v_el.text or ""
                    if t == "s" and v.strip().isdigit():
                        k = int(v)
                        v = shared[k] if 0 <= k < len(shared) else ""
            cells[ci] = v
        rows.append(cells)

    # 归一化为矩形，去掉尾部空行列
    width = 0
    for cells in rows:
        if cells:
            width = max(width, max(cells) + 1)
    width = min(width, max_cols)
    data = []
    for cells in rows[:max_rows]:
        data.append([cells.get(c, "") for c in range(width)])
    while data and not any((x or "").strip() for x in data[-1]):
        data.pop()
    return data, sheet_name
