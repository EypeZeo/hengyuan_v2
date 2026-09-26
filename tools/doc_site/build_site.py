# -*- coding: utf-8 -*-
"""从权威 Markdown 生成单文件、离线自包含的交互式 HTML 站点视图。

设计约束：
  1. Markdown 是唯一权威数据源。站点图、卡片、时间轴、可开工性判定、故障筛选与任务卡片
     全部由 Markdown 解析得出，不引入任何手工数据，从机制上避免与正文漂移。
  2. 产物为单文件，零外部请求（无 CDN、无字体、无外链脚本），离线可直接打开。
  3. 无 JavaScript 时页面仍完整可读；脚本只做增强（筛选、提示气泡、主题、动画开关、目录跟随）。
  4. 不新增、不重命名、不重排 Markdown 的章节标题与锚点；只插入无标题的展示型面板，
     以保证与 docs/HengYuan_v2_...TODOLIST.md 的双向可追溯性。
  5. 产物不含生成时间戳，保证同一输入逐字节可复现。

用法：python tools/doc_site/build_site.py [--check]
"""
from __future__ import annotations

import collections
import datetime
import hashlib
import html as _html
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "docs", "HengYuan_v2_全自动量化实盘系统实施指南与TODOLIST.md")
OUT = os.path.join(ROOT, "docs", "HengYuan_v2_全自动量化实盘系统实施指南与TODOLIST.html")

BASELINE_DATE = datetime.date(2026, 9, 26)      # 与 0.1 节同步时间一致
EXTERNAL_REF = re.compile(r'(?:src|href)\s*=\s*"(?:https?:)?//', re.I)


# --------------------------------------------------------------------------- #
# 基础工具
# --------------------------------------------------------------------------- #

def esc(text: str) -> str:
    return _html.escape(text, quote=False)


def attr(text: str) -> str:
    return _html.escape(text, quote=True)


def slug(heading: str) -> str:
    """与 GitHub 一致的标题锚点规则，必须与 Markdown 内部链接逐字符相同。"""
    s = re.sub(r"<[^>]+>", "", heading).strip().lower()
    s = re.sub(r"[^\w\s-]", "", s, flags=re.UNICODE)
    s = re.sub(r"\s+", "-", s)
    s = re.sub(r"-{2,}", "-", s)
    return s.strip("-")


def text_width(text: str, fs: float) -> float:
    return sum(fs if ord(ch) > 0x2E80 else fs * 0.56 for ch in text)


def wrap_cjk(text: str, max_px: float, fs: float) -> list:
    out = []
    for seg in re.split(r"<br\s*/?>", text):
        seg = seg.strip()
        if not seg:
            continue
        line = ""
        for ch in seg:
            if line and text_width(line + ch, fs) > max_px:
                out.append(line)
                line = ch
            else:
                line += ch
        if line:
            out.append(line)
    return out or [""]


# --------------------------------------------------------------------------- #
# LaTeX 子集 -> MathML（离线原生渲染）
# --------------------------------------------------------------------------- #

MATH_SYMS = {
    r"\cdot": "⋅", r"\times": "×", r"\%": "%", r"\rightarrow": "→",
    r"\longrightarrow": "⟶", r"\to": "→", r"\le": "≤", r"\ge": "≥",
    r"\neq": "≠", r"\pm": "±", r"\sum": "∑", r"\prod": "∏", r"\infty": "∞",
    r"\in": "∈", r"\forall": "∀", r"\exists": "∃", r"\approx": "≈",
    r"\Rightarrow": "⇒", r"\quad": "\u2003", r"\,": "\u2009", r"\;": "\u2009",
}


def _math_tokens(src: str) -> list:
    toks, i = [], 0
    while i < len(src):
        ch = src[i]
        if ch == "\\":
            m = re.match(r"\\[A-Za-z]+", src[i:])
            if m:
                toks.append(("cmd", m.group(0)))
                i += len(m.group(0))
            else:
                toks.append(("cmd", src[i:i + 2]))
                i += 2
            continue
        if ch in "{}_^":
            toks.append((ch, ch))
            i += 1
            continue
        if ch.isspace():
            toks.append(("sp", " "))
            i += 1
            continue
        m = re.match(r"\d+(?:\.\d+)?", src[i:])
        if m:
            toks.append(("num", m.group(0)))
            i += len(m.group(0))
            continue
        m = re.match(r"[A-Za-z]+", src[i:])
        if m:
            toks.append(("id", m.group(0)))
            i += len(m.group(0))
            continue
        toks.append(("op", ch))
        i += 1
    return toks


def latex_to_mathml(src: str, display: bool = False) -> str:
    toks = _math_tokens(src)
    pos = [0]

    def peek():
        return toks[pos[0]] if pos[0] < len(toks) else (None, None)

    def take():
        t = peek()
        pos[0] += 1
        return t

    def read_text():
        buf = []
        if peek()[0] == "{":
            take()
            while peek()[0] not in (None, "}"):
                buf.append(take()[1])
            if peek()[0] == "}":
                take()
        return "".join(buf)

    def parse_seq(stop_brace=False):
        out = []
        while True:
            k, v = peek()
            if k is None:
                break
            if k == "}":
                if stop_brace:
                    break
                take()
                continue
            if k == "{":
                take()
                out.append(parse_seq(True))
                continue
            if k == "cmd":
                take()
                if v == r"\text":
                    out.append("<mtext>%s</mtext>" % esc(read_text()))
                elif v == r"\frac":
                    out.append("<mfrac>%s%s</mfrac>" % (parse_atom(), parse_atom()))
                elif v in MATH_SYMS:
                    out.append("<mo>%s</mo>" % esc(MATH_SYMS[v]))
                else:
                    out.append("<mi>%s</mi>" % esc(v.lstrip("\\")))
                continue
            if k in ("_", "^"):
                take()
                base = out.pop() if out else "<mi></mi>"
                sub = parse_atom()
                tag = "msub" if k == "_" else "msup"
                out.append("<%s>%s%s</%s>" % (tag, base, sub, tag))
                continue
            take()
            if k == "num":
                out.append("<mn>%s</mn>" % v)
            elif k == "id":
                out.append("<mi>%s</mi>" % v)
            elif k == "sp":
                continue
            else:
                out.append("<mo>%s</mo>" % esc(v))
        return "".join(out)

    def parse_atom():
        k, v = peek()
        if k is None:
            return "<mi></mi>"
        if k == "{":
            take()
            return parse_seq(True)
        if k == "sp":
            take()
            return parse_atom()
        if k == "cmd":
            take()
            if v == r"\text":
                return "<mtext>%s</mtext>" % esc(read_text())
            if v == r"\frac":
                return "<mfrac>%s%s</mfrac>" % (parse_atom(), parse_atom())
            if v in MATH_SYMS:
                return "<mo>%s</mo>" % esc(MATH_SYMS[v])
            return "<mi>%s</mi>" % esc(v.lstrip("\\"))
        take()
        if k == "num":
            return "<mn>%s</mn>" % v
        if k == "id":
            return "<mi>%s</mi>" % v
        return "<mo>%s</mo>" % esc(v)

    body = parse_seq()
    open_tag = ('<math xmlns="http://www.w3.org/1998/Math/MathML" display="block">'
                if display else '<math xmlns="http://www.w3.org/1998/Math/MathML">')
    return "%s<mrow>%s</mrow></math>" % (open_tag, body)


# --------------------------------------------------------------------------- #
# 解析
# --------------------------------------------------------------------------- #

NODE_DEF = re.compile(
    r'([A-Za-z_]\w*)\s*\(\s*\[\s*"(.*?)"\s*\]\s*\)'
    r'|([A-Za-z_]\w*)\s*\[\s*"(.*?)"\s*\]', re.S)
EDGE_DEF = re.compile(
    r'([A-Za-z_]\w*)\s*(-\.->|-->|---)\s*'
    r'(?:\|\s*([^|]*?)\s*\|\s*)?([A-Za-z_]\w*)')
SUBGRAPH_DEF = re.compile(r'^subgraph\s+(\w+)\s*\[\s*"(.*?)"\s*\]\s*$')
GRAPH_DIR = re.compile(r'^(?:graph|flowchart)\s+(LR|TD|TB|RL|BT)\s*$')

STATUS_MARKS = [
    ("DONE", re.compile(r"^\[DONE\]")),
    ("WIP", re.compile(r"^\[WIP:")),
    ("TODO", re.compile(r"^\[TODO:")),
    ("BLOCKED", re.compile(r"^\[BLOCKED\]")),
    ("DROPPED", re.compile(r"^\[DROPPED\]")),
]
GLYPH = {"DONE": "●", "WIP": "◉", "TODO": "○", "BLOCKED": "⊘", "DROPPED": "⊘"}
STATUS_LABEL = {"DONE": "已完工", "WIP": "进行中", "TODO": "待实现",
                "BLOCKED": "被阻断", "DROPPED": "已废弃"}
# 线路配色由 CSS 变量 --lane-a..--lane-e 单独持有，且深浅主题各一份；
# 生成端只输出 lane-A..lane-E 类名，不重复定义颜色，避免两处真值源。
LANE_TITLE = {"A": "治理与门禁", "B": "现货安全闭环", "C": "合约产品",
              "D": "数据与投研", "E": "账务与运维"}
LANE_Y = {"A": 118.0, "B": 300.0, "C": 496.0, "D": 676.0, "E": 856.0}
LANE_X0 = {"A": 168.0, "B": 168.0, "C": 372.0, "D": 268.0, "E": 428.0}
LANE_DX = {"A": 330.0, "B": 146.0, "C": 300.0, "D": 320.0, "E": 360.0}
SPUR = {"BD": (300.0, 420.0)}
MAP_W, MAP_H = 1620, 940
INTERCHANGE = {"A1", "B5", "B8"}
CROSS_ARCS = [("A1", "B6"), ("B5", "D1"), ("B5", "E0"), ("B8", "E2")]
LANE_ORDER = {}


def split_row(row: str) -> list:
    r = row.strip()
    if r.startswith("|"):
        r = r[1:]
    if r.endswith("|"):
        r = r[:-1]
    return [c.strip() for c in r.split("|")]


def is_sep(row: str) -> bool:
    return bool(re.match(r"^\s*\|[\s:|-]+\|\s*$", row))


def parse_heading_index(lines: list) -> list:
    out, seen = [], collections.Counter()
    for i, ln in enumerate(lines):
        m = re.match(r"^(#{1,6})\s+(.*?)\s*$", ln)
        if not m:
            continue
        s = slug(m.group(2))
        seen[s] += 1
        if seen[s] > 1:
            s = "%s-%d" % (s, seen[s] - 1)
        out.append({"line": i, "level": len(m.group(1)), "title": m.group(2), "id": s})
    return out


def parse_mermaid_blocks(lines: list) -> list:
    blocks, i = [], 0
    while i < len(lines):
        if lines[i].strip().startswith("```mermaid"):
            j = i + 1
            while j < len(lines) and lines[j].strip() != "```":
                j += 1
            blocks.append({"start": i, "end": j, "body": lines[i + 1:j]})
            i = j + 1
            continue
        i += 1
    return blocks


def parse_mermaid(body: list) -> dict:
    direction, order, nodes, edges, subs, cur = "TD", [], {}, [], [], None
    stadium = set()
    for raw in body:
        s = raw.strip()
        if not s or s.startswith("%%"):
            continue
        if s.startswith(("classDef", "class ", "linkStyle", "style ", "direction")):
            continue
        m = GRAPH_DIR.match(s)
        if m:
            direction = m.group(1)
            continue
        m = SUBGRAPH_DEF.match(s)
        if m:
            cur = {"id": m.group(1), "label": m.group(2), "nodes": []}
            subs.append(cur)
            continue
        if s == "end":
            cur = None
            continue

        def take_node(mo):
            nid = mo.group(1) or mo.group(3)
            label = mo.group(2) if mo.group(1) else mo.group(4)
            if nid not in nodes:
                nodes[nid] = {"id": nid, "label": label, "sub": cur["id"] if cur else None}
                order.append(nid)
                if cur is not None:
                    cur["nodes"].append(nid)
            if mo.group(1):
                stadium.add(nid)
            return nid

        skeleton = NODE_DEF.sub(take_node, s)
        for mo in EDGE_DEF.finditer(skeleton):
            edges.append({"src": mo.group(1), "dst": mo.group(4),
                          "dashed": mo.group(2) == "-.->", "label": (mo.group(3) or "").strip()})
    for n in nodes.values():
        n["stadium"] = n["id"] in stadium
    return {"direction": direction, "nodes": nodes, "order": order,
            "edges": edges, "subgraphs": subs}


def parse_stations(lines: list, mermaid: dict) -> list:
    lane_of, label_of = {}, {}
    for sub in mermaid["subgraphs"]:
        lane = sub["id"][1:] if re.match(r"^L[A-Z]$", sub["id"]) else sub["id"]
        for nid in sub["nodes"]:
            lane_of[nid] = lane
            label_of[nid] = mermaid["nodes"][nid]["label"]
    start = next((i for i, ln in enumerate(lines) if ln.startswith("### 1.3 ")), None)
    if start is None:
        return []
    i = start + 1
    while i < len(lines) and not lines[i].strip().startswith("|"):
        i += 1
    rows = []
    while i < len(lines) and lines[i].strip().startswith("|"):
        if not is_sep(lines[i]):
            rows.append(split_row(lines[i]))
        i += 1
    stations = []
    for cells in rows[1:]:
        if len(cells) < 10:
            continue
        sid, marker = cells[0], cells[1].strip("`")
        status = next((n for n, rx in STATUS_MARKS if rx.search(marker)), "TODO")
        link = re.search(r"\(#([^)]+)\)", cells[9])
        stations.append({
            "id": sid, "marker": marker, "status": status, "glyph": GLYPH[status],
            "name": cells[2], "date": cells[3], "owner": cells[4], "due": cells[5],
            "priority": cells[6], "deps": cells[7], "effort": cells[8],
            "anchor": link.group(1) if link else "",
            "lane": lane_of.get(sid, ""), "label": label_of.get(sid, cells[2]),
        })
    return stations


def parse_fi(lines: list) -> tuple:
    """返回 (用例列表, 需要跳过的原始表格行区间列表)。"""
    gate, out, ranges = None, [], []
    i, n = 0, len(lines)
    while i < n:
        m = re.match(r"^### 7\.\d\s+(PRE_D3|PRE_OWNER_LIVE|CANARY)\s", lines[i])
        if m:
            gate = m.group(1)
            i += 1
            continue
        if re.match(r"^## ", lines[i]):
            gate = None
        if gate and lines[i].strip().startswith("|"):
            start = i
            while i < n and lines[i].strip().startswith("|"):
                if not is_sep(lines[i]):
                    cells = split_row(lines[i])
                    if cells and re.match(r"^\*\*FI-\d{3}\*\*$", cells[0]):
                        out.append({
                            "id": cells[0].strip("*"), "gate": gate, "scope": cells[1],
                            "stage": cells[2], "level": cells[3], "impl": cells[4],
                            "scene": cells[5], "invariant": cells[6] if len(cells) > 6 else "",
                            "artifact": cells[7] if len(cells) > 7 else "",
                            "base": cells[8] if len(cells) > 8 else "",
                        })
                i += 1
            ranges.append((start, i - 1))
            continue
        i += 1
    return out, ranges


TASK_RE = re.compile(r"^- \[([ x])\] \*\*任务\s+([^ ]+)\s+(.*?)\*\*\s*(.*)$")


def parse_tasks(lines: list, heads: list) -> tuple:
    """返回 (任务列表, 行号->任务 的映射)。"""
    out, byline, section, section_id = [], {}, "", ""
    headmap = {h["line"]: h for h in heads}
    for i, ln in enumerate(lines):
        h = headmap.get(i)
        if h and h["level"] in (2, 3):
            section, section_id = h["title"], h["id"]
        m = TASK_RE.match(ln)
        if not m:
            continue
        tail = m.group(4)
        mm = re.search(r"`(\[(?:TODO|WIP|BLOCKED|DONE|DROPPED)[^`]*)`", tail)
        marker = mm.group(1)[1:-1] if mm else ""
        status = next((n for n, rx in STATUS_MARKS if rx.search("[" + marker + "]")), "TODO")
        pri = re.search(r"P[012]", marker)
        fields, j = [], i + 1
        while j < len(lines):
            nl = lines[j]
            if TASK_RE.match(nl) or re.match(r"^#{1,6}\s", nl) or re.match(r"^-{3,}\s*$", nl):
                break
            fm = re.match(r"^\s+-\s+\*\*(.+?)\*\*[：:]\s*(.*)$", nl)
            if fm:
                fields.append({"k": fm.group(1), "v": fm.group(2)})
            elif fields and nl.strip() and nl.startswith("    "):
                fields[-1]["v"] += " " + nl.strip()
            j += 1
        task = {
            "id": m.group(2), "title": m.group(3).strip(), "status": status,
            "marker": marker or "[TODO: P0]", "section": section, "section_id": section_id,
            "priority": pri.group(0) if pri else "P0", "fields": fields,
            "extra": re.sub(r"`\[[^`]*\]`", "", tail).strip(),
            "end": j - 1,
        }
        out.append(task)
        byline[i] = task
    return out, byline


def parse_annotation_lines(lines: list) -> list:
    out, in_fence, lang = [], False, ""
    for ln in lines:
        if ln.startswith("```"):
            if not in_fence:
                in_fence, lang = True, ln[3:].strip()
            else:
                in_fence, lang = False, ""
            continue
        if in_fence and lang == "html" and ln.strip().startswith("<!--"):
            out.append(ln.strip())
    return out


def parse_table_after(lines: list, prefix: str) -> list:
    start = next((i for i, ln in enumerate(lines) if ln.startswith(prefix)), None)
    if start is None:
        return []
    i = start + 1
    while i < len(lines) and not lines[i].strip().startswith("|"):
        i += 1
    rows = []
    while i < len(lines) and lines[i].strip().startswith("|"):
        if not is_sep(lines[i]):
            rows.append(split_row(lines[i]))
        i += 1
    return rows[1:] if rows else []


# --------------------------------------------------------------------------- #
# Markdown 渲染
# --------------------------------------------------------------------------- #

def inline(s: str) -> str:
    out = esc(s)
    codes: list = []

    def stash(m):
        codes.append(m.group(1))
        return "\x00%d\x00" % (len(codes) - 1)

    out = re.sub(r"`([^`]+)`", stash, out)
    out = re.sub(r"\[([^\]]+)\]\(([^)]+)\)",
                 lambda m: '<a href="%s">%s</a>' % (attr(m.group(2)), m.group(1)), out)
    out = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", out)
    out = re.sub(r"(?<![*\w])\*([^*\n]+)\*(?!\*)", r"<em>\1</em>", out)
    out = re.sub(r"\$\$([^$]+)\$\$", lambda m: latex_to_mathml(m.group(1), True), out)
    out = re.sub(r"\$([^$\n]+)\$", lambda m: latex_to_mathml(m.group(1), False), out)
    return re.sub("\x00(\\d+)\x00", lambda m: "<code>%s</code>" % codes[int(m.group(1))], out)


def render_table(rows: list) -> str:
    if not rows:
        return ""
    out = ['<div class="tw"><table>',
           "<thead><tr>%s</tr></thead><tbody>"
           % "".join("<th>%s</th>" % inline(c) for c in rows[0])]
    for r in rows[1:]:
        out.append("<tr>%s</tr>" % "".join("<td>%s</td>" % inline(c) for c in r))
    out.append("</tbody></table></div>")
    return "".join(out)


def render_list(items: list) -> str:
    def build(idx, indent):
        tag = "ol" if items[idx][2] == "ol" else "ul"
        parts = ["<%s>" % tag]
        while idx < len(items) and items[idx][0] == indent:
            _, text, _ = items[idx]
            task_item = text.startswith("\x01")
            inner = text[1:] if task_item else text
            idx += 1
            cls = ' class="task"' if task_item else ""
            if idx < len(items) and items[idx][0] > indent:
                sub, idx = build(idx, items[idx][0])
                parts.append("<li%s>%s%s</li>" % (cls, inline(inner), sub))
            else:
                parts.append("<li%s>%s</li>" % (cls, inline(inner)))
        parts.append("</%s>" % tag)
        return "".join(parts), idx

    return build(0, items[0][0])[0]


def render_task_cards(tasks: list) -> str:
    out = ['<div class="tasks">']
    for t in tasks:
        fields = "".join('<div><dt>%s</dt><dd>%s</dd></div>' % (esc(f["k"]), inline(f["v"]))
                         for f in t["fields"])
        out.append(
            '<article class="task t-%s" data-priority="%s" data-status="%s" data-text="%s">'
            '<header><span class="tid">%s</span><span class="tp tp-%s">%s</span>'
            '<span class="tm">%s</span></header><h6>%s%s</h6><dl>%s</dl></article>'
            % (t["status"].lower(), attr(t["priority"]), t["status"].lower(),
               attr((t["id"] + t["title"]).lower()), esc(t["id"]), t["priority"].lower(),
               esc(t["priority"]), esc(t["marker"]), esc(t["title"]),
               ("<span class=\"textra\">%s</span>" % esc(t["extra"]) if t["extra"] else ""),
               fields or "<div><dt>说明</dt><dd>详见对应章节</dd></div>"))
    out.append("</div>")
    return "".join(out)


def fi_table_html(rows: list, fi_by_id: dict) -> str:
    """按原始九列结构渲染故障表，仅追加筛选属性与可折叠的不变量列。"""
    out = ['<div class="tw tall"><table class="fi-table"><thead><tr>%s</tr></thead><tbody>'
           % "".join("<th>%s</th>" % inline(c) for c in rows[0])]
    for r in rows[1:]:
        fid = re.match(r"^\*\*(FI-\d{3})\*\*$", r[0] if r else "")
        rec = fi_by_id.get(fid.group(1)) if fid else None
        cells = []
        for k, c in enumerate(r):
            if k == 4 and rec:
                cls = {"PARTIAL": "t-part", "NOT_TESTABLE_YET": "t-not",
                       "IMPLEMENTED": "t-ok"}.get(rec["impl"], "t-not")
                text = '<span class="tag %s">%s</span>' % (cls, inline(c))
            elif k == 6 and len(c) > 90:
                text = '<details><summary>%s</summary><p class="inv">%s</p></details>' \
                       % (inline(c[:88]) + "…", inline(c))
            else:
                text = inline(c)
            cells.append("<td>%s</td>" % text)
        if rec:
            out.append('<tr data-impl="%s" data-gate="%s" data-text="%s">%s</tr>'
                       % (attr(rec["impl"]), attr(rec["gate"]),
                          attr((rec["id"] + rec["scene"] + rec["invariant"]).lower()),
                          "".join(cells)))
        else:
            out.append("<tr>%s</tr>" % "".join(cells))
    out.append("</tbody></table></div>")
    return "".join(out)


def render_body(lines: list, heads: list, tasks_by_line: dict,
                special_tables: dict) -> str:
    headmap = {h["line"]: h for h in heads}
    out, i, n = [], 0, len(lines)
    fence_open, fence_lang = False, ""

    while i < n:
        ln = lines[i]
        h = headmap.get(i)
        if h and not fence_open:
            lvl = h["level"]
            out.append('<h%d id="%s">%s<a class="anchor" href="#%s" aria-label="本节链接">#</a></h%d>'
                       % (lvl, attr(h["id"]), inline(h["title"]), attr(h["id"]), lvl))
            i += 1
            continue
        if ln.strip().startswith("```"):
            if not fence_open:
                fence_open, fence_lang = True, ln.strip()[3:].strip()
                out.append('<pre class="annot"><code>')
            else:
                fence_open, fence_lang = False, ""
                out.append("</code></pre>")
            i += 1
            continue
        if fence_open:
            out.append(esc(ln) + "\n")
            i += 1
            continue
        if ln.strip() == "":
            i += 1
            continue
        if re.match(r"^-{3,}\s*$", ln):
            out.append("<hr>")
            i += 1
            continue
        if ln.strip().startswith(">"):
            buf = []
            while i < n and lines[i].strip().startswith(">"):
                buf.append(lines[i].strip()[1:].strip())
                i += 1
            out.append("<blockquote>%s</blockquote>" % inline(" ".join(buf)))
            continue
        if ln.strip().startswith("|"):
            start, rows = i, []
            while i < n and lines[i].strip().startswith("|"):
                if not is_sep(lines[i]):
                    rows.append(split_row(lines[i]))
                i += 1
            if start in special_tables:
                out.append(special_tables[start](rows))
            else:
                out.append(render_table(rows))
            continue
        if i in tasks_by_line:
            ids, j = [], i
            while j < n:
                if TASK_RE.match(lines[j]):
                    ids.append(j)
                    j = tasks_by_line[j]["end"] + 1
                    continue
                if lines[j].strip() == "" or lines[j].startswith(("  ", "\t")):
                    j += 1
                    continue
                break
            out.append(render_task_cards([tasks_by_line[k] for k in ids]))
            i = j
            continue
        if re.match(r"^\s*-\s+", ln) or re.match(r"^\s*\d+\.\s+", ln):
            items = []
            while i < n and (re.match(r"^\s*-\s+", lines[i]) or re.match(r"^\s*\d+\.\s+", lines[i])):
                m = re.match(r"^(\s*)(-|\d+\.)\s+(.*)$", lines[i])
                text = m.group(3)
                if text.startswith(("[ ] ", "[x] ")):
                    text = "\x01" + text[4:]
                items.append((len(m.group(1)), text, "ol" if m.group(2) != "-" else "ul"))
                i += 1
            out.append(render_list(items))
            continue
        buf, i = [ln.strip()], i + 1
        while (i < n and lines[i].strip() and not headmap.get(i)
               and i not in tasks_by_line
               and not lines[i].strip().startswith(("|", ">", "```"))
               and not re.match(r"^\s*-\s+", lines[i])
               and not re.match(r"^\s*\d+\.\s+", lines[i])
               and not re.match(r"^-{3,}\s*$", lines[i])):
            buf.append(lines[i].strip())
            i += 1
        out.append("<p>%s</p>" % inline(" ".join(buf)))
    return "\n".join(out)


# --------------------------------------------------------------------------- #
# Mermaid 子集 -> SVG
# --------------------------------------------------------------------------- #

def layout_dag(dia: dict) -> dict:
    nodes, order, edges = dia["nodes"], dia["order"], dia["edges"]
    adj = collections.defaultdict(list)
    for e in edges:
        if e["src"] in nodes and e["dst"] in nodes:
            adj[e["src"]].append(e["dst"])
    layer = {nid: 0 for nid in order}
    for _ in range(len(order) + 2):
        changed = False
        for nid in order:
            for m in adj[nid]:
                if layer[m] < layer[nid] + 1:
                    layer[m] = layer[nid] + 1
                    changed = True
        if not changed:
            break

    # 层内排序：以相邻层邻居位置的重心做两轮双向扫描，显著减少连线交叉
    preds = collections.defaultdict(list)
    for e in edges:
        if e["src"] in nodes and e["dst"] in nodes:
            preds[e["dst"]].append(e["src"])
    levels = sorted(set(layer.values()))
    groups = {lv: [nid for nid in order if layer[nid] == lv] for lv in levels}
    idx = {}
    for lv in levels:
        for i, nid in enumerate(groups[lv]):
            idx[nid] = i

    def bary(nid, neighbours):
        vals = [idx[m] for m in neighbours if m in idx]
        return sum(vals) / len(vals) if vals else idx[nid]

    for _ in range(4):
        for lv in levels[1:]:
            groups[lv].sort(key=lambda n: bary(n, preds[n]))
            for i, nid in enumerate(groups[lv]):
                idx[nid] = i
        for lv in reversed(levels[:-1]):
            groups[lv].sort(key=lambda n: bary(n, adj[n]))
            for i, nid in enumerate(groups[lv]):
                idx[nid] = i

    fs, pad, gapx, gapy = 12.5, 14, 46, 22
    for nid in order:
        nd = nodes[nid]
        nd["lines"] = wrap_cjk(nd["label"], 210, fs)
        nd["w"] = max(112.0, max(text_width(t, fs) for t in nd["lines"]) + pad * 2)
        nd["h"] = max(38.0, len(nd["lines"]) * (fs + 5) + pad * 1.6)

    subs = dia["subgraphs"]
    unplaced = [nid for nid in order if nodes[nid]["sub"] is None]
    rows = []
    for sub in subs:
        ids = [nid for nid in sub["nodes"] if nid in nodes]
        if ids:
            rows.append((sub, sorted(ids, key=lambda x: (layer[x], idx.get(x, 0)))))
    if unplaced:
        rows.append(({"id": "_rest", "label": "其他节点",
                      "nodes": unplaced},
                     sorted(unplaced, key=lambda x: (layer[x], idx.get(x, 0)))))

    if len(rows) > 1 or (subs and not unplaced and len(subs) == 1):
        y = 26.0
        for meta, ids in rows:
            hmax = max(nodes[nid]["h"] for nid in ids)
            x = 26.0
            for nid in ids:
                nodes[nid]["x"] = x
                nodes[nid]["y"] = y + (hmax - nodes[nid]["h"]) / 2
                x += nodes[nid]["w"] + gapx
            meta["x"], meta["y"] = 10.0, y - 12.0
            meta["w"], meta["h"] = x - 26.0 - gapx + gapx + 18.0, hmax + 24.0
            y += hmax + gapy + 36.0
        height = y + 6.0
        width = max([m["w"] for m, _ in rows] or [320.0]) + 42.0
    else:
        if dia["direction"] in ("LR", "RL"):
            x, hsum = 26.0, 0.0
            for lv in levels:
                wmax = max(nodes[nid]["w"] for nid in groups[lv])
                y = 26.0
                for nid in groups[lv]:
                    nodes[nid]["x"] = x
                    nodes[nid]["y"] = y
                    y += nodes[nid]["h"] + gapy
                hsum = max(hsum, y)
                x += wmax + gapx
            width, height = x + 16.0, hsum + 24.0
        else:
            y, wsum = 26.0, 0.0
            for lv in levels:
                hmax = max(nodes[nid]["h"] for nid in groups[lv])
                x = 26.0
                for nid in groups[lv]:
                    nodes[nid]["x"] = x
                    nodes[nid]["y"] = y
                    x += nodes[nid]["w"] + gapx
                wsum = max(wsum, x)
                y += hmax + gapy
            width, height = wsum + 16.0, y + 6.0
    return {"w": max(width, 320.0), "h": max(height, 170.0)}


def render_dag_svg(dia: dict, uid: str) -> str:
    box = layout_dag(dia)
    p = ['<svg class="diagram" viewBox="0 0 %d %d" role="img" aria-label="依赖关系图" '
         'preserveAspectRatio="xMidYMid meet"><defs>' % (int(box["w"]), int(box["h"]))]
    for name, color in (("arrow", "#5b6675"), ("arrowd", "#3f4854")):
        p.append('<marker id="%s-%s" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" '
                 'markerHeight="7" orient="auto-start-reverse">'
                 '<path d="M 0 0 L 10 5 L 0 10 z" fill="%s"/></marker>' % (uid, name, color))
    p.append("</defs>")

    for sub in dia["subgraphs"]:
        if "w" not in sub:
            continue
        p.append('<g class="subgraph"><rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="14"/>'
                 '<text class="sub-label" x="%.1f" y="%.1f">%s</text></g>'
                 % (sub["x"], sub["y"], sub["w"], sub["h"], sub["x"] + 14, sub["y"] + 19,
                    esc(sub["label"])))

    for e in dia["edges"]:
        s, d = dia["nodes"].get(e["src"]), dia["nodes"].get(e["dst"])
        if not s or not d or "x" not in s or "x" not in d:
            continue
        sx, sy = s["x"] + s["w"], s["y"] + s["h"] / 2
        dx, dy = d["x"], d["y"] + d["h"] / 2
        if d["y"] > s["y"] + s["h"] - 2:
            sx, sy = s["x"] + s["w"] / 2, s["y"] + s["h"]
            dx, dy = d["x"] + d["w"] / 2, d["y"]
        elif d["y"] + d["h"] < s["y"] + 2:
            sx, sy = s["x"] + s["w"] / 2, s["y"]
            dx, dy = d["x"] + d["w"] / 2, d["y"] + d["h"]
        mx, my = (sx + dx) / 2, (sy + dy) / 2
        dash = ' stroke-dasharray="5 4"' if e["dashed"] else ""
        marker = "arrowd" if e["dashed"] else "arrow"
        p.append('<path class="edge" d="M %.1f %.1f C %.1f %.1f %.1f %.1f %.1f %.1f"%s '
                 'marker-end="url(#%s-%s)"/>'
                 % (sx, sy, mx, sy, mx, dy, dx, dy, dash, uid, marker))
        if e["label"]:
            p.append('<text class="edge-label" x="%.1f" y="%.1f">%s</text>'
                     % (mx, my - 4, esc(e["label"])))

    for nid in dia["order"]:
        nd = dia["nodes"][nid]
        if "x" not in nd:
            continue
        rx = nd["h"] / 2 if nd["stadium"] else 8
        p.append('<g class="dnode"><rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="%.1f"/>'
                 % (nd["x"], nd["y"], nd["w"], nd["h"], rx))
        ty = nd["y"] + nd["h"] / 2 - (len(nd["lines"]) - 1) * 7.5 + 4.5
        for k, t in enumerate(nd["lines"]):
            p.append('<text x="%.1f" y="%.1f">%s</text>'
                     % (nd["x"] + nd["w"] / 2, ty + k * 15, esc(t)))
        p.append("</g>")
    p.append("</svg>")
    return "".join(p)


# --------------------------------------------------------------------------- #
# 站点图
# --------------------------------------------------------------------------- #

def station_pos(st: dict) -> tuple:
    if st["id"] in SPUR:
        return SPUR[st["id"]]
    lane = st["lane"] or "A"
    ids = [i for i in LANE_ORDER.get(lane, []) if i not in SPUR]
    idx = ids.index(st["id"]) if st["id"] in ids else 0
    return (LANE_X0[lane] + idx * LANE_DX[lane], LANE_Y[lane])


def render_station_node(st: dict, ready: bool) -> str:
    x, y = station_pos(st)
    aria = "%s %s；%s；负责人 %s；预期完成 %s" % (
        st["id"], st["name"], STATUS_LABEL[st["status"]],
        st["owner"] or "未指派", st["due"] or "未定")
    cls = ["st", "st-" + st["status"].lower()]
    if st["id"] in SPUR:
        cls.append("st-spur")
    out = ['<g class="%s" tabindex="0" role="button" data-station="%s" data-status="%s" '
           'data-lane="%s" data-anchor="%s" aria-label="%s">'
           % (" ".join(cls), attr(st["id"]), st["status"].lower(), attr(st["lane"]),
              attr(st["anchor"]), attr(aria))]
    if ready:
        out.append('<circle class="frontier" cx="%.1f" cy="%.1f" r="15"/>' % (x, y))
    if st["status"] == "WIP":
        out.append('<circle class="halo" cx="%.1f" cy="%.1f" r="13" fill="none"/>' % (x, y))
    if st["id"] in INTERCHANGE:
        out.append('<circle class="interchange" cx="%.1f" cy="%.1f" r="19"/>' % (x, y))
    out.append('<circle class="held" cx="%.1f" cy="%.1f" r="22"/>' % (x, y))
    out.append('<circle class="dot" cx="%.1f" cy="%.1f" r="12"/>' % (x, y))
    if st["id"] in SPUR or st["status"] == "BLOCKED":
        out.append('<path class="slash" d="M %.1f %.1f L %.1f %.1f"/>' % (x - 7, y + 7, x + 7, y - 7))
    out.append('<text class="glyph" x="%.1f" y="%.1f">%s</text>' % (x, y + 5, st["glyph"]))
    out.append('<text class="code" x="%.1f" y="%.1f">%s</text>' % (x, y + 34, esc(st["id"])))
    out.append("</g>")
    return "".join(out)


def render_station_map(stations: list, ready_ids: set) -> str:
    byid = {s["id"]: s for s in stations}
    p = ['<svg id="metro-svg" class="metro-svg" viewBox="0 0 %d %d" role="img" '
         'xmlns:xlink="http://www.w3.org/1999/xlink" '
         'aria-label="技术路线站点图：五条线路的站点状态与依赖关系" '
         'preserveAspectRatio="xMidYMid meet"><defs>' % (MAP_W, MAP_H)]
    # 注意：本段为字面量拼接，不经 % 格式化，故 SVG 的百分比长度直接写单个 %。
    p.append('<filter id="glow" x="-60%" y="-60%" width="220%" height="220%">'
             '<feGaussianBlur stdDeviation="4" result="b"/>'
             '<feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge></filter>')
    p.append('<marker id="arcarrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" '
             'markerHeight="6" orient="auto-start-reverse">'
             '<path d="M 0 0 L 10 5 L 0 10 z"/></marker></defs>')

    for lane in ("A", "B", "C", "D", "E"):
        ids = [i for i in LANE_ORDER.get(lane, []) if i in byid and i not in SPUR]
        if not ids:
            continue
        pts = [station_pos(byid[i]) for i in ids]
        d = "M %.1f %.1f" % (pts[0][0] - 74, pts[0][1])
        for x, y in pts:
            d += " L %.1f %.1f" % (x, y)
        p.append('<path id="lane-%s" class="lane lane-%s" d="%s"/>' % (lane, lane, d))
        p.append('<path class="lane-flow lane-%s" d="%s"/>' % (lane, d))
        p.append('<text class="lane-label" x="52" y="%.1f">线路 %s</text>' % (LANE_Y[lane] - 26, lane))
        p.append('<text class="lane-sub" x="52" y="%.1f">%s</text>'
                 % (LANE_Y[lane] - 8, esc(LANE_TITLE[lane])))

    if "BD" in byid and "B1" in byid:
        bx, by = SPUR["BD"]
        b1 = station_pos(byid["B1"])
        p.append('<path class="lane spur" d="M %.1f %.1f L %.1f %.1f"/>' % (b1[0], b1[1] + 16, bx, by - 16))

    for src, dst in CROSS_ARCS:
        if src not in byid or dst not in byid:
            continue
        sx, sy = station_pos(byid[src])
        dx, dy = station_pos(byid[dst])
        p.append('<path class="xarc" d="M %.1f %.1f Q %.1f %.1f %.1f %.1f" '
                 'marker-end="url(#arcarrow)"/>' % (sx, sy, (sx + dx) / 2, (sy + dy) / 2, dx, dy))

    for lane in ("A", "B", "C", "D", "E"):
        for nid in [i for i in LANE_ORDER.get(lane, []) if i in byid and i not in SPUR]:
            p.append(render_station_node(byid[nid], nid in ready_ids))
    if "BD" in byid:
        p.append(render_station_node(byid["BD"], False))

    for lane in ("A", "B", "C", "D", "E"):
        ids = [i for i in LANE_ORDER.get(lane, []) if i in byid and i not in SPUR]
        frontier = 0
        for k, nid in enumerate(ids):
            if byid[nid]["status"] in ("DONE", "WIP"):
                frontier = k
        pts = [station_pos(byid[i]) for i in ids[:frontier + 1]]
        if not pts:
            continue
        d = "M %.1f %.1f" % (pts[0][0] - 74, pts[0][1])
        for x, y in pts:
            d += " L %.1f %.1f" % (x, y)
        p.append('<path id="run-%s" d="%s" fill="none" stroke="none"/>' % (lane, d))
        p.append('<circle class="train train-%s" r="6.5" filter="url(#glow)">'
                 '<animateMotion dur="%.1fs" repeatCount="indefinite" '
                 'keyPoints="0;1;1" keyTimes="0;0.78;1" calcMode="linear">'
                 '<mpath href="#run-%s" xlink:href="#run-%s"/>'
                 '</animateMotion></circle>' % (lane, max(4.2, 1.2 + 0.55 * len(pts)), lane, lane))
    p.append("</svg>")
    return "".join(p)


# --------------------------------------------------------------------------- #
# 面板
# --------------------------------------------------------------------------- #

def kpi_panel(stations: list, fi: list, tasks: list) -> str:
    cnt = collections.Counter(s["status"] for s in stations)
    impl = collections.Counter(f["impl"] for f in fi)
    todo_p0 = sum(1 for s in stations if s["status"] == "TODO" and s["priority"] == "P0")
    lanes = {lane: [s for s in stations if s["lane"] == lane] for lane in "ABCDE"}
    done_lanes = sum(1 for v in lanes.values() if v and all(s["status"] == "DONE" for s in v))
    cards = [
        ("done", "已完工站点", cnt["DONE"], "共 %d 站" % len(stations)),
        ("wip", "进行中站点", cnt["WIP"], "由三个并行工作树推进"),
        ("todo", "待实现站点", cnt["TODO"], "其中 P0 %d 站" % todo_p0),
        ("blocked", "被阻断站点", cnt["BLOCKED"], "受实盘授权与门禁约束"),
        ("dropped", "已废弃站点", cnt["DROPPED"], "经评估后终止"),
        ("not", "未通过故障用例", impl["NOT_TESTABLE_YET"] + impl["PARTIAL"],
         "共 %d 条，部分实装 %d 条" % (len(fi), impl["PARTIAL"])),
        ("wip", "待办任务条目", len(tasks), "其中 P0 %d 条" % sum(1 for t in tasks if t["priority"] == "P0")),
        ("done", "线路已贯通", done_lanes, "五条线路中全线完工者"),
    ]
    out = ['<div class="kpis">']
    for kind, label, value, note in cards:
        out.append('<div class="kpi k-%s"><div class="kv">%s</div><div class="kl">%s</div>'
                   '<div class="kn">%s</div></div>' % (kind, value, esc(label), esc(note)))
    out.append("</div>")
    return "".join(out)


def station_cards(stations: list, ready_ids: set) -> str:
    out = ['<div class="cards">']
    for s in stations:
        ready = s["id"] in ready_ids
        out.append(
            '<article class="card c-%s" id="card-%s" tabindex="0" role="button" '
            'data-station="%s" data-status="%s" '
            'data-lane="%s" data-priority="%s" data-anchor="%s" data-text="%s" '
            'aria-label="%s %s，%s">'
            '<header><span class="cg">%s</span><span class="cid">%s</span>'
            '<span class="cmk">%s</span></header><h4>%s</h4><dl>'
            '<div><dt>线路</dt><dd>%s %s</dd></div>'
            '<div><dt>负责人</dt><dd>%s</dd></div>'
            '<div><dt>预期完成</dt><dd>%s</dd></div>'
            '<div><dt>依赖</dt><dd>%s</dd></div>'
            '<div><dt>预估</dt><dd>%s</dd></div>'
            '</dl>%s<p class="csec">所属章节：%s</p>'
            '<a class="jump" href="#%s">跳转该章节</a></article>'
            % (s["status"].lower(), attr(s["id"]), attr(s["id"]), s["status"].lower(),
               attr(s["lane"]), attr(s["priority"]), attr(s["anchor"]),
               attr((s["name"] + " " + s["id"] + " " + s["owner"] + " " + s["deps"]).lower()),
               attr(s["id"]), attr(s["name"]), attr(STATUS_LABEL[s["status"]]),
               s["glyph"], esc(s["id"]), esc(s["marker"]), esc(s["name"]),
               esc(s["lane"]), esc(LANE_TITLE.get(s["lane"], "")), esc(s["owner"] or "—"),
               esc(s["due"] or "—"), esc(s["deps"] or "—"), esc(s["effort"] or "—"),
               '<span class="tag t-wait">可开工</span>' if ready else "",
               esc(s["section"] or "—"), attr(s["anchor"])))
    out.append("</div>")
    return "".join(out)


def parse_effort_days(text: str) -> float:
    nums = [float(x) for x in re.findall(r"\d+(?:\.\d+)?", text or "")]
    return max(nums) if nums else 0.0


def timeline_panel(stations: list) -> str:
    rows = [s for s in stations if re.match(r"^\d{4}-\d{2}-\d{2}$", s["due"])]
    if not rows:
        return ""
    span_end = max(datetime.date.fromisoformat(s["due"]) for s in rows) + datetime.timedelta(days=8)
    span_start = BASELINE_DATE - datetime.timedelta(days=6)
    total = (span_end - span_start).days
    w, row_h, left, right = 1420.0, 30.0, 300.0, 46.0
    scale = (w - left - right) / total
    rows.sort(key=lambda s: (s["due"], s["id"]))
    h = 46 + row_h * len(rows)
    p = ['<svg class="gantt" viewBox="0 0 %d %d" role="img" aria-label="站点预期完成时间轴">'
         % (int(w), int(h))]
    cur = datetime.date(span_start.year, span_start.month, 1)
    while cur <= span_end:
        if cur >= span_start:
            x = left + (cur - span_start).days * scale
            p.append('<line class="grid" x1="%.1f" y1="30" x2="%.1f" y2="%d"/>' % (x, x, int(h) - 12))
            p.append('<text class="grid-label" x="%.1f" y="22">%d-%02d</text>'
                     % (x + 4, cur.year, cur.month))
        nxt = cur.month + 1
        cur = datetime.date(cur.year + (1 if nxt > 12 else 0), 1 if nxt > 12 else nxt, 1)
    tx = left + (BASELINE_DATE - span_start).days * scale
    p.append('<line class="today" x1="%.1f" y1="28" x2="%.1f" y2="%d"/>' % (tx, tx, int(h) - 12))
    p.append('<text class="today-label" x="%.1f" y="%d">基线日 %s</text>'
             % (tx + 5, int(h) - 16, BASELINE_DATE.isoformat()))
    for k, s in enumerate(rows):
        y = 44 + k * row_h
        due = datetime.date.fromisoformat(s["due"])
        days = parse_effort_days(s["effort"]) or 1.0
        x0 = left + max(0.0, (due - datetime.timedelta(days=days) - span_start).days) * scale
        x1 = left + (due - span_start).days * scale
        p.append('<text class="row-label" x="12" y="%.1f">%s %s</text>'
                 % (y + 15, esc(s["id"]),
                    esc(re.sub(r"[（(].*?[）)]", "", s["name"])[:16].strip())))
        p.append('<rect class="bar bar-%s lane-%s" x="%.1f" y="%.1f" width="%.1f" height="15" '
                 'rx="7" tabindex="0" role="button" data-station="%s" data-lane="%s" '
                 'data-anchor="%s" aria-label="%s %s，预期完成 %s"/>'
                 % (s["status"].lower(), s["lane"] or "A", x0, y + 3, max(6.0, x1 - x0),
                    attr(s["id"]), attr(s["lane"]), attr(s["anchor"]),
                    attr(s["id"]), attr(s["name"]), attr(s["due"])))
        p.append('<text class="bar-label" x="%.1f" y="%.1f">%s</text>' % (x1 + 6, y + 15, esc(s["due"])))
    p.append("</svg>")
    return "".join(p)


def dependency_panel(stations: list) -> str:
    byid = {s["id"]: s for s in stations}
    out = ['<div class="tw"><table><thead><tr><th>站点</th><th>状态</th><th>依赖文本中出现的站点</th>'
           '<th>前置站点状态</th><th>可开工性</th></tr></thead><tbody>']
    for s in stations:
        if s["status"] in ("DONE", "DROPPED"):
            continue
        refs = [r for r in re.findall(r"[A-E]\d|BD", s["deps"] or "")
                if r in byid and r != s["id"]]
        if not refs:
            verdict = '<span class="tag t-ok">无站点级前置</span>'
        elif all(byid[r]["status"] == "DONE" for r in refs):
            verdict = '<span class="tag t-ok">前置已完工，可开工</span>'
        else:
            pending = [r for r in refs if byid[r]["status"] != "DONE"]
            verdict = '<span class="tag t-wait">等待 %s</span>' % esc("、".join(pending))
        out.append("<tr><td><b>%s</b></td><td>%s %s</td><td>%s</td><td>%s</td><td>%s</td></tr>"
                   % (esc(s["id"]), s["glyph"], esc(STATUS_LABEL[s["status"]]),
                      esc("、".join(refs) or "—"),
                      esc("　".join("%s %s" % (r, byid[r]["glyph"]) for r in refs) or "—"),
                      verdict))
    out.append("</tbody></table></div>")
    return "".join(out)



# --------------------------------------------------------------------------- #
# 组装
# --------------------------------------------------------------------------- #

# 主题解析的唯一所有者：在 <head> 中同步执行，保证首帧即为最终主题，避免浅色偏好用户看到深色闪屏。
# 主脚本只读取 data-theme，不再重复这段判定逻辑。
THEME_BOOT = r"""
(function(){var d=document.documentElement,s=null;
try{s=localStorage.getItem('hy-theme')}catch(e){}
if(s!=='light'&&s!=='dark'){
  s=(window.matchMedia&&window.matchMedia('(prefers-color-scheme: light)').matches)?'light':'dark';}
d.setAttribute('data-theme',s);
try{d.style.colorScheme=s}catch(e){}})();
"""

CSS = r"""
*,*::before,*::after{box-sizing:border-box}
:root{
  color-scheme:dark;
  --bg:#0d1117;--panel:#161d26;--panel2:#1b232e;--line:#242e3a;--line2:#2f3b49;
  --fg:#dbe3ed;--fg2:#9dabb9;--fg3:#6f7d8c;--accent:#4da3ff;--done:#2e9e5b;--wip:#f08a24;
  --todo:#7f8b99;--blocked:#e04b4b;--dropped:#6b7280;--ok:#2e9e5b;--part:#d99a2b;--not:#c2554f;
  /* 状态描边：与状态填充同族的深色描边，浅色主题下需换成更暗的一档 */
  --done-edge:#0f5c2f;--wip-edge:#8f4a06;--blocked-edge:#8d1f1f;
  /* 图形装饰色 */
  --arc:#7c8899;--edge:#5b6675;--hit:#ffffff;--ring:rgba(77,163,255,.9);
  --shadow:0 10px 30px rgba(0,0,0,.45);--shadow-sm:0 2px 8px rgba(0,0,0,.35);
  --topbar:rgba(13,17,23,.93);--mark:#7a5a00;--mark-fg:#ffe9a8;
  --line-op:.26;--line-op-hit:.9;
  --lane-a:#4da3ff;--lane-b:#2e9e5b;--lane-c:#b06bff;--lane-d:#22c3c9;--lane-e:#f0a13a;
  --p0:#ff7b72;--p1:#d29922;--p2:#8b96a3;
  --mono:"Cascadia Code","JetBrains Mono",Consolas,"SF Mono",monospace;
  --sans:-apple-system,BlinkMacSystemFont,"Segoe UI","Microsoft YaHei","PingFang SC","Noto Sans CJK SC",sans-serif;
}
[data-theme="light"]{
  color-scheme:light;
  --bg:#f6f8fa;--panel:#fff;--panel2:#f2f5f8;--line:#dfe5ec;--line2:#c9d3de;
  --fg:#1c2530;--fg2:#5a6875;--fg3:#6b7784;--accent:#0b62c4;--done:#1e7a45;--wip:#b35f10;
  --todo:#8b98a6;--blocked:#c62828;--dropped:#6b7683;--ok:#1e7a45;--part:#96690f;--not:#b03a34;
  --done-edge:#134f2c;--wip-edge:#7a3f06;--blocked-edge:#8d1f1f;
  --arc:#8b96a3;--edge:#98a3b0;--hit:#0b62c4;--ring:rgba(11,98,196,.85);
  --shadow:0 10px 28px rgba(16,24,40,.16);--shadow-sm:0 2px 8px rgba(16,24,40,.10);
  --topbar:rgba(255,255,255,.94);--mark:#ffe9a8;--mark-fg:#3d2c00;
  /* 浅色底上需要更饱和的线路色与更高的线宽不透明度，否则轨道几乎不可见 */
  --line-op:.34;--line-op-hit:.92;
  --lane-a:#1565c0;--lane-b:#1b7a43;--lane-c:#7b3fc4;--lane-d:#0e8b93;--lane-e:#b86e12;
  --p0:#c0392b;--p1:#8a6400;--p2:#6b7683;
}
html{scroll-behavior:smooth;scrollbar-color:var(--line2) var(--bg)}
::selection{background:var(--accent);color:#fff}
body{margin:0;background:var(--bg);color:var(--fg);font-family:var(--sans);font-size:15px;line-height:1.75}
a{color:var(--accent);text-decoration:none}
a:hover{text-decoration:underline}
code{font-family:var(--mono);font-size:.88em;background:var(--panel2);border:1px solid var(--line);
  border-radius:4px;padding:.1em .38em}
kbd{font-family:var(--mono);font-size:11px;background:var(--panel2);border:1px solid var(--line2);
  border-bottom-width:2px;border-radius:5px;padding:1px 5px;color:var(--fg2)}
mark{background:var(--mark);color:var(--mark-fg);border-radius:3px;padding:0 1px}
pre.annot{margin:12px 0;padding:12px 14px;background:var(--panel2);border:1px solid var(--line);
  border-radius:10px;overflow-x:auto}
pre.annot code{background:none;border:0;padding:0;font-size:12.5px;line-height:1.7;white-space:pre}
.skip{position:absolute;left:-9999px}
.skip:focus{left:12px;top:12px;z-index:99;background:var(--panel);padding:8px 14px;
  border:1px solid var(--accent);border-radius:6px}
.prog{position:fixed;top:0;left:0;height:2.5px;width:0;background:var(--accent);z-index:70;
  transition:width .08s linear}
.totop{position:fixed;right:20px;bottom:22px;z-index:50;width:38px;height:38px;border-radius:50%;
  border:1px solid var(--line2);background:var(--panel);color:var(--fg2);font-size:16px;cursor:pointer;
  box-shadow:var(--shadow-sm);opacity:0;pointer-events:none;transition:opacity .18s}
.totop.on{opacity:1;pointer-events:auto}
.totop:hover{border-color:var(--accent);color:var(--accent)}
.topbar{position:sticky;top:0;z-index:40;background:var(--topbar);backdrop-filter:blur(10px);
  border-bottom:1px solid var(--line);padding:9px 18px}
.topbar .row{display:flex;flex-wrap:wrap;align-items:center;gap:10px;max-width:1780px;margin:0 auto}
.topbar .row.sub{display:none;margin-top:8px;padding-top:8px;border-top:1px dashed var(--line)}
.topbar .row.sub.on{display:flex}
.topbar .row.sub .lbl{line-height:1.7}
.search kbd{margin-left:6px;flex:0 0 auto}
.sicon{font-size:11.5px;color:var(--fg3);flex:0 0 auto;padding-right:6px;border-right:1px solid var(--line)}
.brand{font-weight:700;white-space:nowrap}
.chip{font-family:var(--mono);font-size:11.5px;color:var(--fg2);border:1px solid var(--line2);
  background:var(--panel);border-radius:999px;padding:2px 10px;white-space:nowrap}
.chip b{color:var(--fg)}
.spacer{flex:1 1 auto}
.search{display:flex;align-items:center;background:var(--panel);border:1px solid var(--line2);
  border-radius:8px;padding:4px 9px;min-width:240px}
.search input{background:none;border:0;color:var(--fg);font-family:var(--sans);font-size:13px;
  outline:none;width:100%}
.btn{background:var(--panel);border:1px solid var(--line2);color:var(--fg2);border-radius:8px;
  padding:5px 11px;font-size:12.5px;font-family:var(--sans);cursor:pointer}
.btn:hover{border-color:var(--accent);color:var(--fg)}
.btn[aria-pressed="true"]{border-color:var(--accent);color:var(--accent)}
.shell{display:flex;gap:26px;max-width:1780px;margin:0 auto;padding:0 18px 90px}
nav.toc{position:sticky;top:64px;align-self:flex-start;width:288px;max-height:calc(100vh - 84px);
  overflow:auto;padding:16px 4px 40px;font-size:13px;flex:0 0 auto}
nav.toc a{display:block;color:var(--fg2);padding:2.5px 9px;border-left:2px solid transparent;
  border-radius:0 5px 5px 0;line-height:1.45}
nav.toc a:hover{color:var(--fg);background:var(--panel);text-decoration:none}
nav.toc a.lv2{color:var(--fg);font-weight:600;margin-top:8px}
nav.toc a.lv3{padding-left:20px}
nav.toc a.lv4{padding-left:32px;font-size:12.5px}
nav.toc a.active{border-left-color:var(--accent);color:var(--accent);background:var(--panel)}
main{flex:1 1 auto;min-width:0;max-width:1280px;padding-top:16px}
main h1{font-size:27px;line-height:1.35;margin:14px 0 20px;padding-bottom:14px;border-bottom:2px solid var(--line)}
main h2{font-size:21px;margin:44px 0 14px;padding:9px 0 9px 13px;border-left:4px solid var(--accent)}
main h3{font-size:17.5px;margin:30px 0 10px}
main h4{font-size:15.5px;margin:22px 0 8px;color:var(--fg2)}
main h5{font-size:14.5px;margin:0}
main p{margin:9px 0}
main ul,main ol{margin:9px 0;padding-left:24px}
main li{margin:3px 0}
main li.task{list-style:none;margin-left:-20px;padding-left:22px;position:relative}
main li.task::before{content:"";position:absolute;left:0;top:.55em;width:11px;height:11px;
  border:1.5px solid var(--fg3);border-radius:3px}
main blockquote{margin:11px 0;padding:9px 15px;background:var(--panel);border-left:3px solid var(--accent);
  border-radius:0 8px 8px 0;color:var(--fg2)}
main hr{border:0;border-top:1px solid var(--line);margin:34px 0}
.anchor{float:right;opacity:0;font-weight:400;color:var(--fg3);font-size:.72em;padding-left:10px}
h1:hover .anchor,h2:hover .anchor,h3:hover .anchor,h4:hover .anchor{opacity:1}
math{font-size:1.03em}
.tw{overflow-x:auto;margin:13px 0;border:1px solid var(--line);border-radius:10px;background:var(--panel)}
table{border-collapse:collapse;width:100%;font-size:13.5px}
th,td{padding:8px 11px;text-align:left;vertical-align:top;border-bottom:1px solid var(--line)}
th{background:var(--panel2);font-weight:600;color:var(--fg);white-space:nowrap;font-size:12.5px}
tr:last-child td{border-bottom:0}
tbody tr:hover{background:var(--panel2)}
.panel{margin:20px 0;border:1px solid var(--line);border-radius:14px;background:var(--panel);overflow:hidden}
.panel>.ptitle{display:flex;flex-wrap:wrap;align-items:center;gap:10px;padding:11px 16px;
  background:var(--panel2);border-bottom:1px solid var(--line);font-size:13.5px;font-weight:600}
.panel>.ptitle .hint{font-weight:400;color:var(--fg3);font-size:12.5px}
.panel>.pbody{padding:16px}
.kpis{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:11px;padding:16px}
.kpi{border:1px solid var(--line);border-radius:11px;padding:11px 13px;background:var(--panel2);
  border-top:3px solid var(--fg3)}
.kpi .kv{font-size:25px;font-weight:700;font-family:var(--mono);line-height:1.15}
.kpi .kl{font-size:12.5px;font-weight:600}
.kpi .kn{font-size:11.5px;color:var(--fg3);line-height:1.45}
.k-done{border-top-color:var(--done)}.k-done .kv{color:var(--done)}
.k-wip{border-top-color:var(--wip)}.k-wip .kv{color:var(--wip)}
.k-todo{border-top-color:var(--todo)}.k-todo .kv{color:var(--fg2)}
.k-blocked{border-top-color:var(--blocked)}.k-blocked .kv{color:var(--blocked)}
.k-dropped{border-top-color:var(--dropped)}.k-dropped .kv{color:var(--dropped)}
.k-not{border-top-color:var(--not)}.k-not .kv{color:var(--not)}
.toolbar{display:flex;flex-wrap:wrap;gap:8px;align-items:center;padding:10px 16px;background:var(--panel2);
  border-bottom:1px solid var(--line)}
.toolbar .lbl{font-size:12.5px;color:var(--fg3)}
.chipbtn{background:var(--panel);border:1px solid var(--line2);color:var(--fg2);border-radius:999px;
  padding:3px 11px;font-size:12px;cursor:pointer;font-family:var(--sans)}
.chipbtn:hover{border-color:var(--accent);color:var(--fg)}
.chipbtn[aria-pressed="true"]{border-color:var(--accent);color:var(--accent);background:rgba(77,163,255,.1)}
.metro-wrap{padding:8px 10px 4px;overflow-x:auto}
.metro-svg{width:100%;min-width:1120px;height:auto;display:block}
.lane{fill:none;stroke-width:9;stroke-linecap:round;stroke-linejoin:round;opacity:var(--line-op)}
.lane-flow{fill:none;stroke-width:3;stroke-dasharray:11 15;
  animation:flow 1.7s linear infinite;opacity:var(--line-op-hit)}
.lane.spur{stroke:var(--dropped);stroke-dasharray:7 6;opacity:.5;stroke-width:3;fill:none}
.lane-A{stroke:var(--lane-a)}.lane-B{stroke:var(--lane-b)}.lane-C{stroke:var(--lane-c)}
.lane-D{stroke:var(--lane-d)}.lane-E{stroke:var(--lane-e)}
.train-A{fill:var(--lane-a)}.train-B{fill:var(--lane-b)}.train-C{fill:var(--lane-c)}
.train-D{fill:var(--lane-d)}.train-E{fill:var(--lane-e)}
.bar.lane-A{fill:var(--lane-a)}.bar.lane-B{fill:var(--lane-b)}.bar.lane-C{fill:var(--lane-c)}
.bar.lane-D{fill:var(--lane-d)}.bar.lane-E{fill:var(--lane-e)}
@keyframes flow{to{stroke-dashoffset:-52}}
.lane-label{fill:var(--fg3);font-size:12px;font-family:var(--mono);letter-spacing:1.5px}
.lane-sub{fill:var(--fg2);font-size:12.5px}
.xarc{fill:none;stroke:var(--arc);stroke-width:1.6;stroke-dasharray:6 5;opacity:.75}
#arcarrow path{fill:var(--arc)}
.train{opacity:.95}
/* 暂停：CSS 动画归 animation-play-state 管，SMIL（列车 animateMotion）只能由脚本
   pauseAnimations() 停；两者必须同时生效，见主脚本 setAnim。 */
body.anim-off .lane-flow,body.anim-off .halo,body.anim-off .frontier,
body.anim-hold .lane-flow,body.anim-hold .halo,body.anim-hold .frontier{
  animation-play-state:paused}
.st{cursor:pointer;outline:none}
.st .dot{fill:var(--panel);stroke:var(--fg3);stroke-width:2.5}
.st .code{fill:var(--fg2);font-size:12.5px;font-family:var(--mono);text-anchor:middle}
.st .glyph{font-size:13px;text-anchor:middle;fill:var(--fg2);pointer-events:none}
.st-done .dot{fill:var(--done);stroke:var(--done-edge);stroke-width:2.5}
.st-done .glyph{fill:#fff}
.st-wip .dot{fill:var(--wip);stroke:var(--wip-edge);stroke-width:3}
.st-wip .glyph{fill:#fff}
.st-wip .code{fill:var(--wip);font-weight:700}
.st-todo .dot{fill:var(--panel);stroke:var(--todo);stroke-width:2.2;stroke-dasharray:4 3}
.st-todo .glyph{fill:var(--todo)}
.st-blocked .dot{fill:var(--blocked);stroke:var(--blocked-edge);stroke-width:2.5}
.st-blocked .glyph{display:none}
.st-blocked .slash{stroke:#fff;stroke-width:2.6;stroke-linecap:round}
.st-blocked .code{fill:var(--blocked);font-weight:700}
.st-dropped .dot{fill:var(--panel);stroke:var(--dropped);stroke-width:2.2}
.st-dropped .glyph{fill:var(--dropped)}
.st-dropped .slash{display:none}
.st .slash{display:none}
.st-spur .slash{display:block;stroke:var(--dropped)}
.st .interchange{fill:none;stroke:var(--fg3);stroke-width:1.2;stroke-dasharray:3 3;opacity:.7}
.st .halo{stroke:var(--wip);stroke-width:2.5;animation:pulse 1.9s ease-out infinite}
@keyframes pulse{0%{r:12;opacity:.95}70%{r:24;opacity:0}100%{r:24;opacity:0}}
.st .frontier{fill:none;stroke:var(--accent);stroke-width:2;stroke-dasharray:4 4;
  transform-box:fill-box;transform-origin:center;animation:spin 6s linear infinite}
@keyframes spin{to{transform:rotate(360deg)}}
/* 点击进行中站点时的短停顿：静态锁定环，替代脉冲环 */
.st .held{fill:none;stroke:none}
.st.held .held{stroke:var(--wip);stroke-width:3;opacity:.95}
.st.held .halo{display:none}
.st.held .dot{stroke:var(--wip);stroke-width:4}
.st:hover .dot,.st:focus-visible .dot{stroke:var(--accent);stroke-width:4.5}
.st:focus-visible .dot{stroke:var(--ring)}
.st.dim{opacity:.15}
.st.hit .dot{stroke:var(--hit);stroke-width:5}
body.anim-off .halo,body.anim-hold .halo{opacity:.35}
/* 动画关闭时给进行中站点一个静态提示环，否则脉冲消失后与待实现站点难以区分。
   正在短停顿的站点用 :not(.held) 排除，避免这条规则的更高优先级盖掉锁定环。 */
body.anim-off .st-wip:not(.held) .held,
body.anim-hold .st-wip:not(.held) .held{stroke:var(--wip);stroke-width:2;opacity:.55}
.maphint{margin:2px 16px 12px;color:var(--fg3);font-size:12.5px}
.diagram{width:100%;height:auto;display:block}
.diagram .subgraph rect{fill:none;stroke:var(--line2);stroke-dasharray:7 5}
.diagram .sub-label{fill:var(--fg3);font-size:12.5px}
.diagram .edge{fill:none;stroke:var(--edge);stroke-width:1.5}
.diagram .edge-label{fill:var(--fg3);font-size:11.5px;text-anchor:middle}
.diagram .dnode rect{fill:var(--panel2);stroke:var(--line2);stroke-width:1.3}
.diagram .dnode text{fill:var(--fg);font-size:12.5px;text-anchor:middle}
.cards{display:grid;grid-template-columns:repeat(auto-fill,minmax(282px,1fr));gap:12px}
.card{border:1px solid var(--line);border-radius:12px;background:var(--panel2);padding:12px 14px;
  border-left:4px solid var(--fg3);transition:box-shadow .18s;outline:none}
.card:hover{border-color:var(--line2)}
.card:focus-visible{box-shadow:0 0 0 2px var(--ring)}
.card.c-done{border-left-color:var(--done)}
.card.c-wip{border-left-color:var(--wip)}
.card.c-todo{border-left-color:var(--todo)}
.card.c-blocked{border-left-color:var(--blocked)}
.card.c-dropped{border-left-color:var(--dropped)}
.card.hit{box-shadow:0 0 0 2px var(--accent)}
.card header{display:flex;align-items:center;gap:8px;font-size:12px}
.card .cg{font-size:15px;color:var(--fg2)}
.card .cid{font-family:var(--mono);font-weight:700}
.card .cmk{font-family:var(--mono);color:var(--fg3);margin-left:auto;font-size:11px}
.card h4{margin:7px 0 9px;font-size:14.5px;line-height:1.5;font-weight:600}
.card dl{margin:0;display:grid;gap:4px;font-size:12.5px}
.card dl>div{display:flex;gap:8px}
.card dt{color:var(--fg3);flex:0 0 62px}
.card dd{margin:0;color:var(--fg2);flex:1 1 auto;min-width:0}
.card .csec{margin:9px 0 0;font-size:12px;color:var(--fg3);line-height:1.5}
.card .jump{display:inline-block;margin-top:7px;font-size:12px}
.gantt{width:100%;height:auto;display:block;min-width:900px}
.gantt .grid{stroke:var(--line);stroke-width:1}
.gantt .grid-label{fill:var(--fg3);font-size:11.5px;font-family:var(--mono)}
.gantt .today{stroke:var(--accent);stroke-width:1.6;stroke-dasharray:5 4}
.gantt .today-label{fill:var(--accent);font-size:11.5px}
.gantt .row-label{fill:var(--fg2);font-size:12.5px}
.gantt .bar{opacity:.9;cursor:pointer;outline:none}
.gantt .bar:hover,.gantt .bar:focus-visible{opacity:1;stroke:var(--accent);stroke-width:2.5}
.gantt .bar-todo{opacity:.45;stroke:var(--fg3);stroke-dasharray:4 3}
.gantt .bar-blocked{fill:var(--blocked)!important;opacity:.85}
.gantt .bar-dropped{fill:var(--dropped)!important;opacity:.4}
.gantt .bar.dim{opacity:.12}
.gantt .bar-label{fill:var(--fg3);font-size:11.5px;font-family:var(--mono)}
.tag{display:inline-block;font-family:var(--mono);font-size:11.5px;border-radius:999px;padding:1.5px 9px;
  border:1px solid var(--line2);white-space:nowrap}
.fi-table td:first-child,.fi-table th:first-child{white-space:nowrap}
.t-ok{color:var(--ok);border-color:var(--ok)}
.t-part{color:var(--part);border-color:var(--part)}
.t-not{color:var(--not);border-color:var(--not)}
.t-wait{color:var(--wip);border-color:var(--wip)}
details summary{cursor:pointer;color:var(--fg2)}
details[open] summary{color:var(--fg);margin-bottom:5px}
.inv{margin:0;color:var(--fg2);font-size:13px}
.tasks{display:grid;gap:12px;margin:12px 0 18px}
.task{border:1px solid var(--line);border-radius:11px;background:var(--panel);padding:11px 15px;
  border-left:3px solid var(--fg3)}
.task.t-wip{border-left-color:var(--wip)}
.task.t-blocked{border-left-color:var(--blocked)}
.task header{display:flex;flex-wrap:wrap;gap:9px;align-items:center;font-size:12px}
.task .tid{font-family:var(--mono);font-weight:700}
.task .tm{font-family:var(--mono);color:var(--fg3);font-size:11.5px}
.task h6{margin:5px 0 8px;font-size:14.5px;font-weight:600}
.task .textra{color:var(--fg3);font-weight:400;font-size:13px;margin-left:6px}
.task dl{margin:0;display:grid;gap:5px;font-size:13px}
.task dl>div{display:flex;gap:10px}
.task dt{color:var(--fg3);flex:0 0 76px}
.task dd{margin:0;color:var(--fg2);flex:1 1 auto;min-width:0}
.task.hidden,.card.hidden,tr.hidden,.empty[hidden]{display:none}
.tp{font-family:var(--mono);font-size:11px;border-radius:5px;padding:1px 7px;border:1px solid}
.tp-p0{color:var(--p0);border-color:var(--p0)}
.tp-p1{color:var(--p1);border-color:var(--p1)}
.tp-p2{color:var(--p2);border-color:var(--line2)}
/* 悬浮摘要：结构化卡片，始终 pointer-events:none，因此不会与鼠标路径竞争、不会自遮挡 */
.tip{position:fixed;z-index:60;pointer-events:none;width:max-content;max-width:340px;
  background:var(--panel);border:1px solid var(--line2);border-radius:10px;padding:10px 13px;
  font-size:12.5px;line-height:1.6;box-shadow:var(--shadow);opacity:0;visibility:hidden;
  transition:opacity .12s ease,visibility .12s}
.tip.on{opacity:1;visibility:visible}
.tip-h{display:flex;align-items:center;gap:7px;font-size:12px;margin-bottom:5px}
.tip-glyph{font-size:14px}
.tip-id{font-family:var(--mono);font-weight:700}
.tip-badge{font-size:11px;border:1px solid var(--line2);border-radius:999px;padding:0 7px;
  margin-left:auto;color:var(--fg2)}
.tip-name{font-weight:600;font-size:13.5px;line-height:1.5;margin-bottom:7px}
.tip-meta{margin:0;display:grid;gap:3px;font-size:12px}
.tip-meta>div{display:flex;gap:8px}
.tip-meta dt{color:var(--fg3);flex:0 0 58px}
.tip-meta dd{margin:0;color:var(--fg2);flex:1 1 auto;min-width:0}
.tip-hint{margin-top:8px;padding-top:7px;border-top:1px solid var(--line);color:var(--fg3);font-size:11.5px}
.foot{margin-top:50px;padding-top:16px;border-top:1px solid var(--line);color:var(--fg3);font-size:12.5px}
/* 筛选反馈 */
.toolbar .spacer{flex:1 1 auto}
.chipbtn .n{font-family:var(--mono);font-size:10.5px;opacity:.75;margin-left:5px}
.chipbtn[disabled]{opacity:.4;cursor:not-allowed}
.chipbtn[disabled]:hover{border-color:var(--line2);color:var(--fg2)}
.seg{display:inline-flex;border:1px solid var(--line2);border-radius:8px;overflow:hidden}
.segbtn{background:var(--panel);border:0;color:var(--fg2);font-family:var(--sans);font-size:12px;
  padding:4px 11px;cursor:pointer}
.segbtn+.segbtn{border-left:1px solid var(--line2)}
.segbtn:hover{color:var(--fg)}
.segbtn[aria-pressed="true"]{background:var(--accent);color:#fff}
.count{font-size:12px;color:var(--fg3);font-variant-numeric:tabular-nums}
.count.hit{color:var(--accent)}
.count.note{color:var(--wip)}
.gantt .bar.hit{stroke:var(--hit);stroke-width:3}
.btn.mini{padding:3px 9px;font-size:12px}
.empty{margin:10px 0 4px;padding:14px 16px;border:1px dashed var(--line2);border-radius:10px;
  color:var(--fg3);font-size:13px;text-align:center}
.empty button{margin-left:8px}
/* 命中高亮：跳转后短暂标记目标标题 */
.head-hit{animation:headhit 1.8s ease-out 1}
@keyframes headhit{0%,40%{background:var(--mark);box-shadow:0 0 0 14px var(--mark)}
  100%{background:transparent;box-shadow:0 0 0 14px transparent}}
/* 大表表头吸顶，长表格滚动时列名不丢失 */
.tw.tall{max-height:74vh;overflow:auto}
.tw.tall thead th{position:sticky;top:0;z-index:2;background:var(--panel2);
  box-shadow:inset 0 -1px 0 var(--line)}
.tw.tall thead th:first-child{background:var(--panel2)}
/* 锚点跳转必须避开吸顶导航，否则目标标题被压在顶栏之下 */
main h1,main h2,main h3,main h4,main h5,main h6{scroll-margin-top:78px}
.foot,.panel,.tw{scroll-margin-top:78px}
a:focus-visible,button:focus-visible,input:focus-visible,summary:focus-visible,
[tabindex]:focus-visible{outline:2px solid var(--ring);outline-offset:2px;border-radius:4px}
@media (max-width:1100px){nav.toc{display:none}.shell{padding:0 12px 60px}}
@media (max-width:900px){.kpis{grid-template-columns:repeat(2,minmax(0,1fr))}
  .topbar .row.sub .lbl{font-size:11.5px}}
@media (prefers-reduced-motion:reduce){
  html{scroll-behavior:auto}
  .lane-flow,.halo,.frontier,.prog{animation:none!important;transition:none!important}
}
@media print{
  .topbar,nav.toc,.toolbar,.tip,.totop,.prog,.seg,.empty button{display:none!important}
  body{background:#fff;color:#000}
  .panel,.tw,.card,.task{break-inside:avoid}
  .lane-flow,.train,.halo,.frontier{animation:none!important}
  .tw.tall{max-height:none;overflow:visible}
  .card.hidden,.task.hidden,tr.hidden{display:revert!important}
  mark{background:none;color:inherit;font-weight:700}
  main{max-width:none}
}
"""

JS = r"""
(function(){
'use strict';
var doc=document,root=doc.documentElement,body=doc.body;
var DEBOUNCE_MS=300;   // 检索防抖：用户停止输入 0.3 秒后才执行筛选
var HOLD_MS=2600;      // 单击进行中站点后的停顿时长
var TIP_DELAY=140;     // 悬浮摘要延迟，避免扫过站点时闪烁

var store={
  get:function(k){try{return localStorage.getItem(k)}catch(e){return null}},
  set:function(k,v){try{localStorage.setItem(k,v)}catch(e){}},
  del:function(k){try{localStorage.removeItem(k)}catch(e){}}
};

/* ---------------- 数据岛 ---------------- */
var DATA={stations:[],fi:[],tasks:[],labels:{}};
try{DATA=JSON.parse(doc.getElementById('site-data').textContent)||DATA;}catch(e){}
var ST={};
(DATA.stations||[]).forEach(function(s){ST[s.id]=s;});
var LABEL=DATA.labels||{};

function $(sel,ctx){return (ctx||doc).querySelector(sel);}
function $$(sel,ctx){return Array.prototype.slice.call((ctx||doc).querySelectorAll(sel));}
function on(el,ev,fn,opt){if(el)el.addEventListener(ev,fn,opt||false);}
function txt(el,s){if(el)el.textContent=s;}
function norm(s){return (s||'').toLowerCase().replace(/\s+/g,' ').trim();}

/* 状态提示通道：动画停顿、筛选清除、复制链接共用同一 aria-live 区域 */
var note=$('#status-note'),noteTimer=0;
function say(msg,keep){
  if(!note)return;
  txt(note,msg);
  note.classList.toggle('hit',!!msg);
  if(noteTimer)clearTimeout(noteTimer);
  if(msg&&!keep)noteTimer=setTimeout(function(){txt(note,'');note.classList.remove('hit');},HOLD_MS);
}

/* ---------------- 主题 ---------------- */
/* data-theme 已由 <head> 中的引导脚本在首帧前写好，此处只负责切换与持久化。 */
var tb=doc.getElementById('btn-theme');
function setTheme(t,save){
  root.setAttribute('data-theme',t);
  try{root.style.colorScheme=t}catch(e){}
  if(save)store.set('hy-theme',t);
}
function paintTheme(){
  var t=root.getAttribute('data-theme');
  txt(tb,t==='dark'?'浅色':'深色');
  tb.setAttribute('aria-pressed',t==='light'?'true':'false');
  tb.title=t==='dark'?'切换到浅色主题（t）':'切换到深色主题（t）';
}
paintTheme();
on(tb,'click',function(){
  setTheme(root.getAttribute('data-theme')==='dark'?'light':'dark',true);paintTheme();});

/* ---------------- 动画 ---------------- */
/* 三件事必须同时成立动画才算真的停了：
   1) CSS 动画（轨道光流、脉冲环、旋转环）由 body 类上的 animation-play-state 控制；
   2) 列车用的是 SMIL animateMotion，CSS 完全管不到，只能靠 svg.pauseAnimations()；
   3) 视图外或短停顿时也应停，避免后台持续绘制。 */
var svg=doc.getElementById('metro-svg'),ab=doc.getElementById('btn-anim');
var anim={manual:store.get('hy-anim')==='off',offscreen:false,hold:false,
          holdTimer:0,holdNode:null,holdStart:0};
function paused(){return anim.manual||anim.offscreen||anim.hold;}
function applyAnim(){
  body.classList.toggle('anim-off',anim.manual||anim.offscreen);
  body.classList.toggle('anim-hold',anim.hold&&!anim.manual&&!anim.offscreen);
  if(svg&&svg.pauseAnimations){if(paused())svg.pauseAnimations();else svg.unpauseAnimations();}
  paintAnim();
}
function paintAnim(){
  txt(ab,anim.manual?'播放动画':'暂停动画');
  ab.setAttribute('aria-pressed',anim.manual?'true':'false');
  ab.title=anim.manual?'恢复线路图动画（p）':'暂停线路图动画（p）';
}
function setManual(v){anim.manual=v;store.set('hy-anim',v?'off':'on');applyAnim();
  say(v?'动画已暂停':'');}
on(ab,'click',function(){setManual(!anim.manual);});
applyAnim();

/* 视图外暂停：站点图滚出视口后不再消耗绘制与电池 */
var panel=doc.getElementById('metro-panel');
if(panel&&'IntersectionObserver' in window){
  new IntersectionObserver(function(es){
    es.forEach(function(en){anim.offscreen=!en.isIntersecting;});
    /* 回到视口时补收尾：停顿可能是在离开视口期间到期的 */
    if(anim.hold)scheduleHoldEnd();else applyAnim();
  },{threshold:0}).observe(panel);
}

/* 单击进行中站点的短停顿。
   难点在于这次单击同时还会跳转到所属章节，站点图随即离开视口。
   若严格按 2.6 秒计时，用户返回时停顿早已结束、等于什么都没发生；
   因此规则定为：至少停顿 2.6 秒，且到期时若站点图不在视口内则保持停顿，
   直到用户返回站点图才恢复。这样停顿一定可见，又不会永久停住动画。 */
function holdPause(id){
  if(anim.holdTimer){clearTimeout(anim.holdTimer);anim.holdTimer=0;}
  if(anim.holdNode)anim.holdNode.classList.remove('held');
  var node=$('.st[data-station="'+id+'"]');
  anim.hold=true;anim.holdStart=Date.now();anim.holdNode=node;
  if(node)node.classList.add('held');
  applyAnim();
  say('动画已停顿，约 2.6 秒后自动恢复');
  scheduleHoldEnd();
}
function scheduleHoldEnd(){
  if(anim.holdTimer){clearTimeout(anim.holdTimer);anim.holdTimer=0;}
  var left=HOLD_MS-(Date.now()-anim.holdStart);
  if(left<=0){
    if(!anim.offscreen)endHold();   // 视图外则挂起，等 IntersectionObserver 唤醒
    return;
  }
  anim.holdTimer=setTimeout(scheduleHoldEnd,left+40);
}
function endHold(){
  if(anim.holdTimer){clearTimeout(anim.holdTimer);anim.holdTimer=0;}
  anim.hold=false;anim.holdStart=0;
  if(anim.holdNode){anim.holdNode.classList.remove('held');anim.holdNode=null;}
  applyAnim();
}

/* ---------------- 悬浮摘要 ---------------- */
var tip=doc.getElementById('tip'),
    tipGlyph=$('.tip-glyph',tip),tipId=$('.tip-id',tip),tipBadge=$('.tip-badge',tip),
    tipName=$('.tip-name',tip),tipMeta=$('.tip-meta',tip),tipHint=$('.tip-hint',tip);
var tipTimer=0,tipFor=null,last={x:0,y:0};
var clickMode=store.get('hy-click')==='card'?'card':'section';

function metaRow(k,v){
  var d=doc.createElement('div'),a=doc.createElement('dt'),b=doc.createElement('dd');
  a.textContent=k;b.textContent=v;d.appendChild(a);d.appendChild(b);return d;
}
function clearTip(){
  tipGlyph.textContent='';tipId.textContent='';tipBadge.textContent='';
  tipName.textContent='';tipHint.textContent='';tipMeta.textContent='';
}
function buildStationTip(id){
  var s=ST[id];if(!s)return false;
  clearTip();
  tipGlyph.textContent=s.glyph||'';
  tipId.textContent=s.id;
  tipBadge.textContent=LABEL[s.status]||s.status;
  tipName.textContent=s.name||'';
  [['线路','线路 '+s.lane+(s.laneTitle?' '+s.laneTitle:'')],
   ['负责人',s.owner||'未指派'],['预期完成',s.due||'未定'],
   ['依赖',s.deps||'—'],['预估',s.effort||'—']
  ].forEach(function(r){tipMeta.appendChild(metaRow(r[0],r[1]));});
  if(s.section)tipMeta.appendChild(metaRow('所属章节',s.section));
  if(s.status==='WIP')tipMeta.appendChild(metaRow('动画','单击可停顿约 2.6 秒'));
  tipHint.textContent='单击'+(clickMode==='section'?'跳转所属章节':'定位站点卡片')+'，Alt 单击取反';
  return true;
}
function buildTextTip(s){clearTip();tipName.textContent=s;return true;}

function placeTip(){
  tip.classList.add('on');tip.setAttribute('aria-hidden','false');
  var r=tip.getBoundingClientRect(),vw=window.innerWidth,vh=window.innerHeight;
  var left=last.x+16;if(left+r.width>vw-10)left=last.x-r.width-16;if(left<8)left=8;
  var top=last.y+18;if(top+r.height>vh-10)top=last.y-r.height-18;if(top<8)top=8;
  tip.style.left=Math.round(left)+'px';tip.style.top=Math.round(top)+'px';
}
function hideTip(){
  if(tipTimer){clearTimeout(tipTimer);tipTimer=0;}
  tipFor=null;tip.classList.remove('on');tip.setAttribute('aria-hidden','true');
}
function targetOf(e){
  return (e.target&&e.target.closest)?e.target.closest('[data-station],[data-tip]'):null;
}
function showFor(el){
  var id=el.getAttribute('data-station');
  var ok=id?buildStationTip(id):(el.getAttribute('data-tip')?buildTextTip(el.getAttribute('data-tip')):false);
  if(!ok){hideTip();return;}
  tipFor=el;
  if(tipTimer)clearTimeout(tipTimer);
  tipTimer=setTimeout(function(){tipTimer=0;placeTip();},TIP_DELAY);
}
on(doc,'mousemove',function(e){last.x=e.clientX;last.y=e.clientY;});
on(doc,'mouseover',function(e){
  var t=targetOf(e);
  if(!t){if(tipFor)hideTip();return;}
  if(t===tipFor)return;           // 同一目标内部移动：不重建，杜绝闪烁
  last.x=e.clientX;last.y=e.clientY;showFor(t);
});
on(doc,'mouseout',function(e){
  var t=targetOf(e);if(!t)return;
  var to=e.relatedTarget;
  if(to&&t.contains&&t.contains(to))return;   // 移到同一目标内部：保持显示
  if(t===tipFor)hideTip();
});
on(doc,'focusin',function(e){
  var t=targetOf(e);if(!t)return;
  var r=t.getBoundingClientRect();
  last.x=r.left+r.width/2;last.y=r.bottom;showFor(t);
});
on(doc,'focusout',hideTip);
/* 滚动时不能一律收起：平滑滚动会持续派发 scroll，若直接隐藏，
   用户在页面仍在滚动时把鼠标移到站点上，摘要会被尾随的 scroll 事件立刻抹掉。
   摘要锚定光标而非元素，因此滚动本身不影响其位置；只有当光标下已不是原目标时才收起。 */
var scrollTick=false;
on(window,'scroll',function(){
  if(scrollTick||!tipFor)return;
  scrollTick=true;
  requestAnimationFrame(function(){
    scrollTick=false;
    if(!tipFor)return;
    var under=doc.elementFromPoint(last.x,last.y);
    if(!under||!(tipFor===under||tipFor.contains(under)))hideTip();
  });
},{passive:true});

/* ---------------- 单击跳转 ---------------- */
var segs=$$('[data-clickmode]');
function paintSeg(){
  segs.forEach(function(b){
    b.setAttribute('aria-pressed',b.getAttribute('data-clickmode')===clickMode?'true':'false');});
}
function setClickMode(m){
  clickMode=m==='card'?'card':'section';store.set('hy-click',clickMode);paintSeg();
  say('单击站点行为：'+(clickMode==='section'?'跳转章节':'定位卡片'));
}
segs.forEach(function(b){on(b,'click',function(){setClickMode(b.getAttribute('data-clickmode'));});});
paintSeg();

function flash(el,cls,ms){
  if(!el)return;
  el.classList.add(cls);
  setTimeout(function(){el.classList.remove(cls);},ms);
}
function jump(hash,where){
  var t=hash?doc.getElementById(hash):null;
  if(!t)return false;
  try{t.scrollIntoView({behavior:'smooth',block:where})}catch(e){t.scrollIntoView();}
  flash(t,where==='start'?'head-hit':'hit',1900);
  try{history.replaceState(null,'','#'+hash)}catch(e){}
  return true;
}
function focusStation(id,mode){
  var s=ST[id];
  flash($('.st[data-station="'+id+'"]'),'hit',1600);
  if(mode==='card'){
    var c=doc.getElementById('card-'+id);
    if(c&&c.classList.contains('hidden')){clearFilters('all');}
    if(jump('card-'+id,'center'))return;
  }
  var anchor=s&&s.anchor?s.anchor:'';
  if(anchor&&jump(anchor,'start'))return;
  jump('card-'+id,'center');
}
on(doc,'click',function(e){
  if(!e.target.closest)return;
  if(e.target.closest('a,button,input,select,summary,label,details'))return;  // 交给原生控件
  var t=targetOf(e);if(!t)return;
  var id=t.getAttribute('data-station');if(!id)return;
  e.preventDefault();
  if(ST[id]&&ST[id].status==='WIP')holdPause(id);
  focusStation(id,e.altKey?(clickMode==='section'?'card':'section'):clickMode);
});
on(doc,'keydown',function(e){
  if(e.key!=='Enter'&&e.key!==' '&&e.key!=='Spacebar')return;
  var t=e.target&&e.target.closest?e.target.closest('[data-station][tabindex]'):null;
  if(!t)return;
  var id=t.getAttribute('data-station');if(!id)return;
  e.preventDefault();
  if(ST[id]&&ST[id].status==='WIP')holdPause(id);
  focusStation(id,e.altKey?(clickMode==='section'?'card':'section'):clickMode);
});

/* ---------------- 筛选与检索 ---------------- */
var statusF=new Set(),laneF=new Set(),taskF=new Set(),implF=new Set(),gateF=new Set();
var input=doc.getElementById('q'),debTimer=0;

/* 元素清单与检索索引在初始化时各建一次，避免每次输入都重新查询 DOM */
var EL={
  st:$$('.st'),
  bars:$$('.gantt .bar'),
  cards:$$('.card'),
  tasks:$$('.task'),
  fi:$$('.fi-table tbody tr'),
  rows:$$('tbody tr').filter(function(r){return !r.closest('.fi-table');})
};
function indexOf(el,extra){
  el.__q=norm([extra||'',el.textContent||''].join(' '));
}
EL.st.forEach(function(g){var s=ST[g.getAttribute('data-station')];
  indexOf(g,s?[s.id,s.name,s.owner,s.deps,s.lane,s.laneTitle,s.section].join(' '):'');});
EL.bars.forEach(function(b){indexOf(b,'');});
EL.cards.concat(EL.tasks).concat(EL.fi).concat(EL.rows).forEach(function(el){indexOf(el,'');});

function query(){return norm(input.value);}
function activeFilters(){return statusF.size+laneF.size+taskF.size+implF.size+gateF.size;}
function anyFilter(){return !!activeFilters()||!!query();}
function hit(el,q){return !q||(el.__q||'').indexOf(q)>=0;}

function clearFilters(scope){
  if(scope==='task'){taskF.clear();}
  else if(scope==='fi'){implF.clear();gateF.clear();}
  else{statusF.clear();laneF.clear();taskF.clear();implF.clear();gateF.clear();}
  if(scope==='all'||!scope){input.value='';}
  syncChips();applyAll();
  say(scope==='all'||!scope?'已清除全部筛选':'已清除该区筛选');
}

function syncChips(){
  [['[data-fstatus]','data-fstatus',statusF],['[data-flane]','data-flane',laneF],
   ['[data-ftask]','data-ftask',taskF],['[data-fimpl]','data-fimpl',implF],
   ['[data-fgate]','data-fgate',gateF]].forEach(function(t){
    $$(t[0]).forEach(function(b){
      b.setAttribute('aria-pressed',t[2].has(b.getAttribute(t[1]))?'true':'false');});
  });
  $$('[data-clear]').forEach(function(b){
    var s=b.getAttribute('data-clear');
    var on_=s==='task'?taskF.size:(s==='fi'?(implF.size+gateF.size):(activeFilters()+ (query()?1:0)));
    b.hidden=!on_;
  });
}

/* 命中高亮：只处理文本节点，先撤旧标记再打新标记，容器数量有限所以开销可控 */
function clearMarks(list){
  list.forEach(function(rootEl){
    var ms=$$('mark',rootEl);
    if(!ms.length)return;
    ms.forEach(function(m){var p=m.parentNode;if(!p)return;
      p.replaceChild(doc.createTextNode(m.textContent),m);});
    rootEl.normalize();
  });
}
function markIn(rootEl,q){
  if(!q||q.length<2)return;         // 单字符命中过泛，标记会淹没正文
  var walker=doc.createTreeWalker(rootEl,NodeFilter.SHOW_TEXT,null),nodes=[],n;
  while((n=walker.nextNode()))nodes.push(n);
  nodes.forEach(function(node){
    var p=node.parentNode;
    if(!p||/^(SCRIPT|STYLE|MARK|TEXTAREA|OPTION)$/.test(p.nodeName))return;
    var s=node.nodeValue,low=s.toLowerCase(),idx=low.indexOf(q);
    if(idx<0)return;
    var frag=doc.createDocumentFragment(),cur=0;
    while(idx>=0){
      if(idx>cur)frag.appendChild(doc.createTextNode(s.slice(cur,idx)));
      var mk=doc.createElement('mark');
      mk.textContent=s.slice(idx,idx+q.length);
      frag.appendChild(mk);
      cur=idx+q.length;idx=low.indexOf(q,cur);
    }
    if(cur<s.length)frag.appendChild(doc.createTextNode(s.slice(cur)));
    p.replaceChild(frag,node);
  });
}

function emptyBox(afterEl,msg){
  if(!afterEl)return null;
  var box=afterEl.__empty;
  if(!box){
    box=doc.createElement('div');
    box.className='empty';box.hidden=true;
    box.appendChild(doc.createTextNode(msg));
    var btn=doc.createElement('button');
    btn.className='btn mini';btn.type='button';btn.textContent='清除筛选';
    on(btn,'click',function(){clearFilters('all');});
    box.appendChild(btn);
    afterEl.parentNode.insertBefore(box,afterEl.nextSibling);
    afterEl.__empty=box;
  }
  return box;
}
var cardsWrap=$('.cards'),tasksWrap=$('.tasks'),fiWrap=$('.fi-table');
var cardsEmpty=emptyBox(cardsWrap,'没有匹配当前筛选条件的站点。'),
    tasksEmpty=emptyBox(tasksWrap,'没有匹配当前筛选条件的任务。'),
    fiEmpty=emptyBox(fiWrap,'没有匹配当前筛选条件的故障用例。');

function setCount(name,shown,total,unit){
  var el=$('[data-count="'+name+'"]');
  if(!el)return;
  el.textContent=anyFilter()?'显示 '+shown+' / '+total+' '+unit:unit+'共 '+total+' 条';
  el.classList.toggle('hit',shown!==total);
}

/* 分面计数：片上数字反映"当前查询与其它分面"下的可得数量，而不是静态总数 */
function facet(){
  var q=query();
  function n(sel,attr,keep,test){
    $$(sel).forEach(function(b){
      var v=b.getAttribute(attr),c=0;
      test(v,function(){c++;});
      var span=$('.n',b);
      if(span)txt(span,c);
      b.disabled=(c===0&&!keep.has(v));
    });
  }
  n('[data-fstatus]','data-fstatus',statusF,function(v,inc){
    EL.st.forEach(function(g){
      if(laneF.size&&!laneF.has(g.getAttribute('data-lane')))return;
      if(!hit(g,q))return;
      if(ST[g.getAttribute('data-station')]&&ST[g.getAttribute('data-station')].status===v)inc();
    });
  });
  n('[data-flane]','data-flane',laneF,function(v,inc){
    EL.st.forEach(function(g){
      if(statusF.size&&!statusF.has(g.getAttribute('data-status')))return;
      if(!hit(g,q))return;
      if(g.getAttribute('data-lane')===v)inc();
    });
  });
  n('[data-ftask]','data-ftask',taskF,function(v,inc){
    EL.tasks.forEach(function(t){
      if(hit(t,q)&&t.getAttribute('data-priority')===v)inc();});
  });
  n('[data-fgate]','data-fgate',gateF,function(v,inc){
    EL.fi.forEach(function(r){
      if(implF.size&&!implF.has(r.getAttribute('data-impl')))return;
      if(hit(r,q)&&r.getAttribute('data-gate')===v)inc();});
  });
  n('[data-fimpl]','data-fimpl',implF,function(v,inc){
    EL.fi.forEach(function(r){
      if(gateF.size&&!gateF.has(r.getAttribute('data-gate')))return;
      if(hit(r,q)&&r.getAttribute('data-impl')===v)inc();});
  });
}

function applyAll(){
  var q=query();
  clearMarks(EL.cards.concat(EL.tasks).concat(EL.fi).concat(EL.rows.filter(function(r){
    return !r.closest('.fi-table');})));

  var stShown=0;
  EL.st.forEach(function(g){
    var ok=(!statusF.size||statusF.has(g.getAttribute('data-status')))
          &&(!laneF.size||laneF.has(g.getAttribute('data-lane')))
          &&hit(g,q);
    g.classList.toggle('dim',!ok);if(ok)stShown++;
  });
  EL.bars.forEach(function(b){
    var ok=(!statusF.size||statusF.has(b.getAttribute('data-status')))
          &&(!laneF.size||laneF.has(b.getAttribute('data-lane')))
          &&hit(b,q);
    b.classList.toggle('dim',!ok);
  });

  var cardShown=0;
  EL.cards.forEach(function(c){
    var ok=(!statusF.size||statusF.has(c.getAttribute('data-status')))
          &&(!laneF.size||laneF.has(c.getAttribute('data-lane')))
          &&hit(c,q);
    c.classList.toggle('hidden',!ok);
    if(ok){cardShown++;markIn(c,q);}
  });

  var taskShown=0;
  EL.tasks.forEach(function(t){
    var ok=(!taskF.size||taskF.has(t.getAttribute('data-priority')))&&hit(t,q);
    t.classList.toggle('hidden',!ok);
    if(ok){taskShown++;markIn(t,q);}
  });

  var fiShown=0;
  EL.fi.forEach(function(r){
    var ok=(!implF.size||implF.has(r.getAttribute('data-impl')))
          &&(!gateF.size||gateF.has(r.getAttribute('data-gate')))
          &&hit(r,q);
    r.classList.toggle('hidden',!ok);
    if(ok){fiShown++;markIn(r,q);}
  });

  var rowShown=0;
  EL.rows.forEach(function(r){
    if(r.closest('.fi-table'))return;
    var ok=hit(r,q);
    r.classList.toggle('hidden',!ok);if(ok)rowShown++;
  });

  if(cardsEmpty)cardsEmpty.hidden=cardShown>0;
  if(tasksEmpty)tasksEmpty.hidden=taskShown>0;
  if(fiEmpty)fiEmpty.hidden=fiShown>0;
  setCount('metro',cardShown,EL.cards.length,'站点');
  setCount('task',taskShown,EL.tasks.length,'条');
  setCount('fi',fiShown,EL.fi.length,'条');
  facet();syncChips();
}

/* 防抖：最后一次输入后 0.3 秒才真正筛选；清空与回车走 search 事件立即生效 */
on(input,'input',function(){
  if(debTimer)clearTimeout(debTimer);
  debTimer=setTimeout(function(){debTimer=0;applyAll();},DEBOUNCE_MS);
});
on(input,'search',function(){
  if(debTimer){clearTimeout(debTimer);debTimer=0;}
  applyAll();
});

[['[data-fstatus]','data-fstatus',statusF],['[data-flane]','data-flane',laneF],
 ['[data-ftask]','data-ftask',taskF],['[data-fimpl]','data-fimpl',implF],
 ['[data-fgate]','data-fgate',gateF]].forEach(function(t){
  $$(t[0]).forEach(function(b){
    on(b,'click',function(){
      if(b.disabled)return;
      var v=b.getAttribute(t[1]);
      if(t[2].has(v))t[2].delete(v);else t[2].add(v);
      b.setAttribute('aria-pressed',t[2].has(v)?'true':'false');
      applyAll();
    });
  });
});
$$('[data-clear]').forEach(function(b){
  on(b,'click',function(){clearFilters(b.getAttribute('data-clear'));});});

/* ---------------- 目录跟随与全局快捷键 ---------------- */
var links={};
$$('nav.toc a').forEach(function(a){links[a.getAttribute('href').slice(1)]=a;});
if('IntersectionObserver' in window){
  var io=new IntersectionObserver(function(es){
    es.forEach(function(en){
      if(!en.isIntersecting)return;
      var id=en.target.id;if(!links[id])return;
      $$('nav.toc a.active').forEach(function(a){a.classList.remove('active');});
      links[id].classList.add('active');
    });
  },{rootMargin:'-70px 0px -78% 0px',threshold:0});
  $$('main h2[id],main h3[id]').forEach(function(h){io.observe(h);});
}

var hintRow=$('.topbar .row.sub');
on(doc,'keydown',function(e){
  var typing=/^(INPUT|TEXTAREA|SELECT)$/.test((e.target&&e.target.tagName)||'');
  if(e.key==='Escape'){
    if(typing&&input.value){input.value='';applyAll();say('已清空检索');}
    else hideTip();
    return;
  }
  if(e.key==='/'&&!typing){e.preventDefault();input.focus();input.select();return;}
  if(typing||e.ctrlKey||e.metaKey||e.altKey)return;
  /* 快捷键行以大写字母标示（P/T），故匹配统一转小写：Shift+P 得到 'P'、
     大小写锁定得到 'P'，只比对 'p' 都会漏判。多字符键名（如 'Dead'）不受影响。 */
  var k=e.key.toLowerCase();
  if(k==='p'){e.preventDefault();setManual(!anim.manual);}
  else if(k==='t'){e.preventDefault();setTheme(root.getAttribute('data-theme')==='dark'?'light':'dark',true);paintTheme();}
  else if(k==='?'){e.preventDefault();toggleHint();}
});
/* 快捷键行的展开状态也持久化：首次访问默认展开，之后记住用户的选择 */
function toggleHint(){
  if(!hintRow)return;
  var on=!hintRow.classList.contains('on');
  hintRow.classList.toggle('on',on);
  store.set('hy-hint',on?'on':'off');
}
if(hintRow)hintRow.classList.toggle('on',store.get('hy-hint')!=='off');

/* 标题锚点：保留原生跳转，同时把完整链接写入剪贴板 */
on(doc,'click',function(e){
  var a=e.target.closest?e.target.closest('a.anchor'):null;
  if(!a||!navigator.clipboard||!navigator.clipboard.writeText)return;
  var url=location.href.split('#')[0]+a.getAttribute('href');
  navigator.clipboard.writeText(url).then(function(){say('已复制本节链接');},function(){});
});

/* ---------------- 阅读进度与回到顶部 ---------------- */
var prog=doc.getElementById('prog'),totop=doc.getElementById('totop'),ticking=false;
function onScroll(){
  if(ticking)return;ticking=true;
  requestAnimationFrame(function(){
    ticking=false;
    var max=doc.documentElement.scrollHeight-window.innerHeight;
    var p=max>0?Math.min(100,Math.max(0,window.scrollY/max*100)):0;
    if(prog){prog.style.width=p.toFixed(2)+'%';prog.setAttribute('aria-valuenow',Math.round(p));}
    if(totop)totop.classList.toggle('on',window.scrollY>700);
  });
}
on(window,'scroll',onScroll,{passive:true});
on(window,'resize',onScroll);
if(totop)on(totop,'click',function(){window.scrollTo({top:0,behavior:'smooth'});});
onScroll();

applyAll();
})();
"""


def build() -> str:
    global LANE_ORDER
    # 源哈希必须针对磁盘原始字节计算，使外部工具（Get-FileHash / sha256sum）可独立复核；
    # 解析则统一在归一化换行后的文本上进行，避免 CRLF 污染单元格与标记。
    raw = io.open(SRC, "rb").read()
    sha = hashlib.sha256(raw).hexdigest()
    text = raw.decode("utf-8").replace("\r\n", "\n").replace("\r", "\n")
    lines = text.split("\n")
    heads = parse_heading_index(lines)
    mblocks = parse_mermaid_blocks(lines)
    station_mermaid = parse_mermaid(mblocks[0]["body"]) if mblocks else None
    LANE_ORDER = {sub["id"][1:] if re.match(r"^L[A-Z]$", sub["id"]) else sub["id"]: sub["nodes"]
                  for sub in station_mermaid["subgraphs"]} if station_mermaid else {}
    stations = parse_stations(lines, station_mermaid) if station_mermaid else []
    # 站点所属章节的标题，供悬浮摘要与卡片展示；锚点缺失时留空而不报错。
    head_title = {h["id"]: h["title"] for h in heads}
    for _s in stations:
        _s["section"] = head_title.get(_s["anchor"], "")
    fi, fi_ranges = parse_fi(lines)
    tasks, tasks_by_line = parse_tasks(lines, heads)
    review = parse_table_after(lines, "## 附录 C")
    limits = parse_table_after(lines, "## 附录 D")
    annotations = parse_annotation_lines(lines)

    byid = {s["id"]: s for s in stations}
    ready = []
    for s in stations:
        if s["status"] != "TODO":
            continue
        refs = [r for r in re.findall(r"[A-E]\d|BD", s["deps"] or "") if r in byid and r != s["id"]]
        if all(byid[r]["status"] == "DONE" for r in refs):
            ready.append(s["id"])
    ready_set = set(ready)

    # 故障表就地升级为可筛选表格，筛选条插入 7.2 之前
    fi_by_id = {f["id"]: f for f in fi}
    special_tables = {}
    for start, _end in fi_ranges:
        special_tables[start] = (lambda rows, _m=fi_by_id: fi_table_html(rows, _m))

    # 正文：分段渲染，mermaid 块替换为 SVG
    def rel_heads(lo, hi):
        return [{"line": h["line"] - lo, "level": h["level"], "title": h["title"], "id": h["id"]}
                for h in heads if lo <= h["line"] < hi]

    def rel_tasks(lo, hi):
        # end 在 parse_tasks 中记录的是绝对行号，此处必须一并转为段内相对行号：
        # 否则任务分支会以绝对下标越界、直接跳出循环，导致该段其后全部内容丢失。
        return {k - lo: dict(v, end=v["end"] - lo)
                for k, v in tasks_by_line.items() if lo <= k < hi}

    def rel_special(lo, hi):
        return {k - lo: v for k, v in special_tables.items() if lo <= k < hi}

    parts, seg = [], 0
    for idx, b in enumerate(mblocks):
        parts.append(render_body(lines[seg:b["start"]], rel_heads(seg, b["start"]),
                                rel_tasks(seg, b["start"]), rel_special(seg, b["start"])))
        if idx == 0 and station_mermaid:
            st_cnt = collections.Counter(s["status"] for s in stations)
            ln_cnt = collections.Counter(s["lane"] for s in stations)
            parts.append(
                '<div class="panel" id="metro-panel"><div class="ptitle">技术路线站点图'
                '<span class="hint">列车位置等于该线路已达进度；脉冲环为进行中站点；'
                '虚线旋转环为下一个可开工站点。悬浮查看摘要，单击跳转所属章节</span></div>'
                + kpi_panel(stations, fi, tasks)
                + '<div class="toolbar" data-filterbar="metro"><span class="lbl">状态</span>'
                + "".join('<button class="chipbtn" data-fstatus="%s" aria-pressed="false" '
                          'data-facet="status"><span class="cg">%s</span>%s'
                          '<span class="n" data-n="status:%s">%d</span></button>'
                          % (k, GLYPH[k], STATUS_LABEL[k], k, st_cnt[k])
                          for k in ("DONE", "WIP", "TODO", "BLOCKED", "DROPPED"))
                + '<span class="lbl">线路</span>'
                + "".join('<button class="chipbtn" data-flane="%s" aria-pressed="false" '
                          'data-facet="lane">线路 %s<span class="n" data-n="lane:%s">%d</span></button>'
                          % (l, l, l, ln_cnt[l]) for l in ("A", "B", "C", "D", "E"))
                + '<span class="spacer"></span>'
                + '<span class="lbl">单击站点</span><span class="seg" role="group" '
                  'aria-label="单击站点的跳转目标">'
                  '<button class="segbtn" data-clickmode="section" aria-pressed="true">跳转章节</button>'
                  '<button class="segbtn" data-clickmode="card" aria-pressed="false">定位卡片</button>'
                  '</span>'
                + '<button class="btn mini" data-clear="all" hidden>清除筛选</button>'
                + '<span class="count" data-count="metro" aria-live="polite"></span>'
                + '<span class="count note" id="status-note" aria-live="polite"></span>'
                + '</div><div class="metro-wrap">'
                + render_station_map(stations, ready_set)
                + '</div><p class="maphint">按住 <kbd>Alt</kbd> 单击可在“跳转章节”与“定位卡片”'
                  '之间临时取反；单击进行中站点会停顿动画，回到本图时自动恢复。</p></div>')
        else:
            parts.append('<figure class="panel"><div class="pbody">'
                         + render_dag_svg(parse_mermaid(b["body"]), "d%d" % b["start"])
                         + "</div></figure>")
        seg = b["end"] + 1
    parts.append(render_body(lines[seg:], rel_heads(seg, len(lines)),
                             rel_tasks(seg, len(lines)), rel_special(seg, len(lines))))
    body = "\n".join(p for p in parts if p)

    anchor = '<h3 id="14-站点图更新协议">'
    if anchor not in body:
        raise SystemExit("未找到 1.4 锚点，无法插入扩展面板")
    body = body.replace(anchor, (
        '<div class="panel"><div class="ptitle">站点卡片'
        '<span class="hint">随状态与线路筛选联动；悬浮查看摘要，单击按当前"单击站点"设置跳转</span></div>'
        '<div class="pbody">' + station_cards(stations, ready_set) + '</div></div>'
        '<div class="panel"><div class="ptitle">预期完成时间轴'
        '<span class="hint">条形起点按预估人日上界回推，仅示意相对先后；单击条形跳转所属章节</span></div>'
        '<div class="pbody" style="overflow-x:auto">' + timeline_panel(stations) + '</div></div>'
        '<div class="panel"><div class="ptitle">可开工性判定'
        '<span class="hint">依据各站点依赖字段中出现的站点编号逐条判定</span></div>'
        '<div class="pbody">' + dependency_panel(stations) + '</div></div>') + anchor, 1)

    task_bar = ('<div class="toolbar" data-filterbar="task"><span class="lbl">优先级</span>'
                + "".join('<button class="chipbtn" data-ftask="%s" aria-pressed="false" '
                          'data-facet="task">%s<span class="n" data-n="task:%s">%d</span></button>'
                          % (p, p, p, sum(1 for t in tasks if t["priority"] == p))
                          for p in ("P0", "P1", "P2"))
                + '<span class="spacer"></span>'
                + '<button class="btn mini" data-clear="task" hidden>清除筛选</button>'
                + '<span class="count" data-count="task" aria-live="polite">共 %d 条任务</span>'
                  '</div>' % len(tasks))
    body = body.replace('<h2 id="卷-6-全阶段实施路线">',
                        '<h2 id="卷-6-全阶段实施路线">' + task_bar, 1)

    fi_bar = ('<div class="toolbar" data-filterbar="fi"><span class="lbl">门禁</span>'
              + "".join('<button class="chipbtn" data-fgate="%s" aria-pressed="false" '
                        'data-facet="gate">%s<span class="n" data-n="gate:%s">%d</span></button>'
                        % (g, g, g, sum(1 for f in fi if f["gate"] == g))
                        for g in ("PRE_D3", "PRE_OWNER_LIVE", "CANARY"))
              + '<span class="lbl">实装状态</span>'
              + "".join('<button class="chipbtn" data-fimpl="%s" aria-pressed="false" '
                        'data-facet="impl">%s<span class="n" data-n="impl:%s">%d</span></button>'
                        % (k, k, k, sum(1 for f in fi if f["impl"] == k))
                        for k in ("PARTIAL", "NOT_TESTABLE_YET"))
              + '<span class="spacer"></span>'
              + '<button class="btn mini" data-clear="fi" hidden>清除筛选</button>'
              + '<span class="count" data-count="fi" aria-live="polite">共 %d 条</span></div>'
                % len(fi))
    if '<h3 id="72-pre_d3-门禁用例">' not in body:
        raise SystemExit("未找到 7.2 锚点，无法插入故障筛选条")
    body = body.replace('<h3 id="72-pre_d3-门禁用例">',
                        fi_bar + '<h3 id="72-pre_d3-门禁用例">', 1)

    toc = ['<nav class="toc" aria-label="目录">']
    for h in heads:
        if h["level"] in (1, 2, 3, 4):
            toc.append('<a class="lv%d" href="#%s">%s</a>' % (h["level"], attr(h["id"]), esc(h["title"])))
    toc.append("</nav>")

    ver = re.search(r"（v([\d.]+)）", lines[0])
    ver = ver.group(1) if ver else "0"
    head = (
        '<!DOCTYPE html>\n<html lang="zh-CN" data-theme="dark">\n<head>\n<meta charset="utf-8">\n'
        '<meta name="viewport" content="width=device-width, initial-scale=1">\n'
        '<meta name="color-scheme" content="dark light">\n'
        '<title>HengYuan v2 全自动加密货币量化实盘系统实施指南与完整 TODO LIST（v' + ver + '）</title>\n'
        '<meta name="generator" content="tools/doc_site/build_site.py">\n'
        '<meta name="source-sha256" content="' + sha + '">\n'
        # 主题必须在首帧前落定，否则浅色偏好用户会先看到一帧深色。该脚本是主题的唯一解析点。
        '<script>' + THEME_BOOT + '</script>\n'
        '<style>' + CSS + '</style>\n</head>\n<body>\n'
        '<div class="prog" id="prog" role="progressbar" aria-label="阅读进度" '
        'aria-valuemin="0" aria-valuemax="100" aria-valuenow="0"></div>\n'
        '<a class="skip" href="#main">跳到正文</a>\n'
        '<header class="topbar"><div class="row">\n'
        '<span class="brand">HengYuan v2 实施指南与 TODO LIST</span>\n'
        '<span class="chip">v<b>' + ver + '</b></span>\n'
        '<span class="chip">基线 <b>' + BASELINE_DATE.isoformat() + '</b></span>\n'
        '<span class="chip">站点 <b>' + str(len(stations)) + '</b></span>\n'
        '<span class="chip">故障用例 <b>' + str(len(fi)) + '</b></span>\n'
        '<span class="chip">任务 <b>' + str(len(tasks)) + '</b></span>\n'
        '<span class="chip">源哈希 <b>' + sha[:12] + '</b></span>\n'
        '<span class="spacer"></span>\n'
        '<label class="search"><span class="sicon" aria-hidden="true">检索</span>'
        '<input id="q" type="search" autocomplete="off" aria-keyshortcuts="/" '
        'placeholder="筛选站点、用例与任务（按 / 聚焦）" aria-label="筛选站点、用例与任务" '
        'aria-describedby="qhint"><kbd class="skbd">/</kbd></label>\n'
        '<button class="btn" id="btn-anim" aria-pressed="false" aria-keyshortcuts="P">暂停动画</button>\n'
        '<button class="btn" id="btn-theme" aria-keyshortcuts="T">浅色</button>\n'
        '</div><div class="row sub"><span id="qhint" class="lbl">'
        '输入即筛选：站点图、站点卡片、故障注册表与任务卡片同步收敛；查询 0.3 秒后生效。'
        '快捷键：<kbd>/</kbd> 聚焦检索、<kbd>Esc</kbd> 清空、<kbd>P</kbd> 暂停动画、'
        '<kbd>T</kbd> 切换主题、<kbd>?</kbd> 显示或隐藏本行</span></div></header>\n<div class="shell">\n')

    data = json.dumps({
        "meta": {"version": ver, "sha256": sha, "baseline": BASELINE_DATE.isoformat(),
                 "stations": len(stations), "fi": len(fi), "tasks": len(tasks),
                 "annotations": len(annotations), "review": len(review), "limits": len(limits)},
        "labels": dict(STATUS_LABEL),
        "stations": [{"id": s["id"], "status": s["status"], "lane": s["lane"],
                      "laneTitle": LANE_TITLE.get(s["lane"], ""),
                      "due": s["due"], "anchor": s["anchor"], "section": s["section"],
                      "name": s["name"], "owner": s["owner"], "deps": s["deps"],
                      "effort": s["effort"], "priority": s["priority"],
                      "marker": s["marker"], "glyph": s["glyph"]} for s in stations],
        "fi": [{"id": f["id"], "gate": f["gate"], "impl": f["impl"]} for f in fi],
        "tasks": [{"id": t["id"], "priority": t["priority"], "status": t["status"]} for t in tasks],
    }, ensure_ascii=False, separators=(",", ":"))

    foot = (
        '\n<div class="foot">本页由 <code>tools/doc_site/build_site.py</code> 从权威 Markdown 生成，'
        '源文件 SHA-256 前 12 位 <code>' + sha[:12] + '</code>；全部数据由脚本解析得出，未经手工录入，'
        '与 <code>docs/HengYuan_v2_全自动量化实盘系统实施指南与TODOLIST.md</code> 同源。'
        '产物不含生成时间戳，同一输入可逐字节复现。'
        '交互状态仅将主题、动画开关与单击站点行为写入本机 localStorage，不联网、不外发。</div>\n</main>\n</div>\n'
        '<button class="totop" id="totop" type="button" aria-label="回到顶部">↑</button>\n'
        '<div class="tip" id="tip" role="tooltip" aria-hidden="true">'
        '<div class="tip-h"><span class="tip-glyph"></span><span class="tip-id"></span>'
        '<span class="tip-badge"></span></div>'
        '<div class="tip-name"></div><dl class="tip-meta"></dl>'
        '<div class="tip-hint"></div></div>\n'
        '<script id="site-data" type="application/json">' + data + '</script>\n'
        '<script>' + JS + '</script>\n</body>\n</html>\n')

    return head + "".join(toc) + '<main id="main">' + body + foot


def main() -> int:
    html_out = build()
    bad = EXTERNAL_REF.findall(html_out)
    if bad:
        print("外部引用检查失败：%s" % bad[:5])
        return 2
    if "--check" in sys.argv:
        print("构建校验通过：无外部引用，%d 字节" % len(html_out.encode("utf-8")))
        return 0
    io.open(OUT, "w", encoding="utf-8", newline="\n").write(html_out)
    print("已生成 %s（%d 字节）" % (OUT, len(html_out.encode("utf-8"))))
    return 0


if __name__ == "__main__":
    sys.exit(main())

