"""Relocate raw HTML annotation comments out of the reading flow.

Reason: renderers with raw HTML disabled (html:false) display `<!-- ... -->`
literally. The document must render correctly in both HTML-enabled and
HTML-disabled pipelines, so the three annotation kinds move into a fenced,
machine-readable appendix while remaining byte-identical in syntax.
"""
import re
import pathlib

P = pathlib.Path(r"D:\My_Projects\hengyuan_v2\docs"
                 r"\HengYuan_v2_全自动量化实盘系统实施指南与TODOLIST.md")
text = P.read_text(encoding="utf-8")
lines = text.split("\n")

ANN_RE = re.compile(r"^<!--\s+(MODULE_BOUNDARY|RATIONALE|EXTENSION_POINT):\s*(.*?)\s*-->\s*$")
KIND_ZH = {
    "MODULE_BOUNDARY": "模块边界",
    "RATIONALE": "设计依据",
    "EXTENSION_POINT": "扩展接入点",
}


def slug(h):
    s = h.strip().lower()
    s = re.sub(r"[^\w\s\u4e00-\u9fff-]", "", s)
    return s.replace(" ", "-")


headings = []  # (index, level, text)
for i, l in enumerate(lines):
    m = re.match(r"^(#{2,4})\s+(.*?)\s*$", l)
    if m:
        headings.append((i, len(m.group(1)), m.group(2)))

mb_lines = []
for i, l in enumerate(lines):
    m = ANN_RE.match(l)
    if m and m.group(1) == "MODULE_BOUNDARY":
        mb_lines.append(i)

annotations = []  # dicts
for i, l in enumerate(lines):
    m = ANN_RE.match(l)
    if not m:
        continue
    kind, body = m.group(1), m.group(2)
    prev = [h for h in headings if h[0] < i]
    nxt = [h for h in headings if h[0] > i]

    if kind == "MODULE_BOUNDARY":
        # a MODULE_BOUNDARY annotates the module declared in its own `name`,
        # and sits immediately BEFORE that module's heading.
        nm = re.search(r'name:\s*"([^"]+)"', body)
        if nm and not nm.group(1).startswith("附录 A 至"):
            owner = None
            owner_text = nm.group(1)
        else:
            owner = nxt[0] if nxt else (prev[-1] if prev else None)
            owner_text = owner[2] if owner else ""
    elif kind == "EXTENSION_POINT":
        # an EXTENSION_POINT placed between a MODULE_BOUNDARY and the heading it
        # introduces belongs to that following module, not to the previous one.
        prev_h = prev[-1] if prev else None
        sandwiched = prev_h is not None and any(prev_h[0] < k < i for k in mb_lines)
        if sandwiched and nxt:
            owner_text = nxt[0][2]
        else:
            owner = prev_h if prev_h else (nxt[0] if nxt else None)
            owner_text = owner[2] if owner else ""
    else:
        owner = prev[-1] if prev else (nxt[0] if nxt else None)
        owner_text = owner[2] if owner else ""
    annotations.append({
        "line": i,
        "kind": kind,
        "body": body,
        "raw": l,
        "owner": owner_text,
        "anchor": slug(owner_text),
    })

print("annotations found:", len(annotations))
for a in annotations:
    print("  L%-5d %-16s -> %s" % (a["line"] + 1, a["kind"], a["owner"]))

# ---- identifier extraction for the index table -------------------------------
def ident(a):
    b = a["body"]
    if a["kind"] == "EXTENSION_POINT":
        m = re.search(r'id:\s*"([^"]+)"', b)
        return m.group(1) if m else "—"
    if a["kind"] == "MODULE_BOUNDARY":
        m = re.search(r'name:\s*"([^"]+)"', b)
        return m.group(1) if m else "—"
    # RATIONALE has no id: derive a short label from the first alternative
    m = re.search(r'alternatives_considered:\s*\["([^"]+)"', b)
    return ("决策：" + m.group(1)) if m else "—"


# ---- build appendix E --------------------------------------------------------
kind_order = ["MODULE_BOUNDARY", "RATIONALE", "EXTENSION_POINT"]
counts = {k: sum(1 for a in annotations if a["kind"] == k) for k in kind_order}

apx = []
apx.append("## 附录 E 结构化标注索引")
apx.append("")
apx.append("本附录是三类结构化标注的机器可读权威副本。标注不参与正文叙述，"
           "也不在章节内就地呈现，原因见 `0.5` 节：原始 HTML 注释在禁用"
           "原始 HTML 的渲染管线中会被逐字显示，破坏正文可读性。")
apx.append("")
apx.append("### E.1 标注类型")
apx.append("")
apx.append("| 类型 | 语义 | 实例数 |")
apx.append("| :--- | :--- | :---: |")
apx.append("| `MODULE_BOUNDARY` | 卷或附录集合的模块边界，记录名称、依赖与被依赖，用于安全地增删模块 | %d |" % counts["MODULE_BOUNDARY"])
apx.append("| `RATIONALE` | 关键决策的设计依据，记录备选方案、取舍与可逆路径，供后续重构时还原意图 | %d |" % counts["RATIONALE"])
apx.append("| `EXTENSION_POINT` | 官方扩展接入点，记录标识、上下文与预期用法，是后续增补内容的约定入口 | %d |" % counts["EXTENSION_POINT"])
apx.append("")
apx.append("### E.2 标注归属索引")
apx.append("")
apx.append("| 类型 | 标识 | 所属章节 |")
apx.append("| :--- | :--- | :--- |")
for a in annotations:
    apx.append("| `%s` | %s | [%s](#%s) |" % (a["kind"], ident(a), a["owner"], a["anchor"]))
apx.append("")
apx.append("### E.3 标注原文")
apx.append("")
apx.append("以下代码围栏逐行保留标注原文，保持字符级一致，可直接被脚本解析。"
           "围栏内不随正文渲染为可见注释，因此在任何渲染管线中均不产生显示副作用。")
apx.append("")
apx.append("```html")
for a in annotations:
    raw = a["raw"]
    if a["kind"] == "MODULE_BOUNDARY" and 'name: "附录 A 至 D"' in raw:
        raw = raw.replace('name: "附录 A 至 D"', 'name: "附录 A 至 E"')
    apx.append(raw)
apx.append("```")
apx.append("")
apx.append("> **维护约定**：新增卷、附录或扩展接入点时，须在同一次提交内更新 `E.2` 的归属索引与 `E.3` 的逐行原文；"
           "两者的条目集合必须与正文的实际章节集合保持一致。")
apx.append("")

# ---- assemble body with annotations stripped --------------------------------
kept = [l for i, l in enumerate(lines) if not ANN_RE.match(l)]

# appendix insertion point: before the final document-effect note
tail_idx = None
for i, l in enumerate(kept):
    if l.startswith("> **文档效力说明**"):
        tail_idx = i
        break
assert tail_idx is not None, "document-effect note not found"
# keep the trailing '---' that precedes the note with the appendix, then the note
insert_at = tail_idx - 1
while insert_at > 0 and kept[insert_at].strip() == "":
    insert_at -= 1
if kept[insert_at].strip() == "---":
    insert_at -= 1
    while insert_at > 0 and kept[insert_at].strip() == "":
        insert_at -= 1
    insert_at += 1
out = kept[:insert_at] + apx + ["---", ""] + kept[tail_idx:]
text = "\n".join(out)

# ---- 0.5 section ------------------------------------------------------------
sec05 = """### 0.5 结构化标注与机器可读索引

文档使用三类结构化标注承载元信息，全部登记于 `附录 E`，不嵌入正文叙述：

| 类型 | 作用 | 使用场景 |
| :--- | :--- | :--- |
| `MODULE_BOUNDARY` | 声明模块的名称、依赖与被依赖 | 卷与附录集合的边界，用于安全地增删模块 |
| `RATIONALE` | 记录备选方案、取舍与可逆路径 | 关键决策节点，供后续重构时还原设计意图 |
| `EXTENSION_POINT` | 声明标识、上下文与预期用法 | 官方扩展接入点，是后续增补内容的约定入口 |

标注以原始 HTML 注释语法逐行保存在 `附录 E.3` 的代码围栏内，因此具备两项性质：其一，任何渲染管线（含禁用原始 HTML 的管线）都不会将其显示为正文；其二，脚本可按行解析，无需依赖渲染器行为。若标注直接置于章节内，禁用原始 HTML 的渲染环境会将其逐字显示，破坏正文可读性，故统一收口到附录。

"""

anchor_toc = "  - [0.4 阅读顺序建议](#04-阅读顺序建议)\n"
assert anchor_toc in text
text = text.replace(anchor_toc, anchor_toc + "  - [0.5 结构化标注与机器可读索引](#05-结构化标注与机器可读索引)\n", 1)

# place 0.5 immediately before the '---' that precedes the TOC heading
toc_pos = text.index("## 目录")
before = text.rindex("---", 0, toc_pos)
text = text[:before] + sec05 + text[before:]

# ---- TOC: appendix E --------------------------------------------------------
toc_d = "- [附录 D 已知限制清单](#附录-d-已知限制清单)\n"
assert toc_d in text
text = text.replace(toc_d, toc_d + "- [附录 E 结构化标注索引](#附录-e-结构化标注索引)\n", 1)

# ---- update protocol item 7 -------------------------------------------------
proto6 = ("6. **一致性校验**：本卷站点集合与 `卷 6` 各 Stage 的条目集合、`卷 7` 的故障用例集合必须保持双向可追溯；"
          "新增站点时须同时登记其对应的故障用例与验收标签。\n")
assert proto6 in text
text = text.replace(proto6, proto6 +
    "7. **标注同步**：三类结构化标注的权威副本位于 `附录 E`。新增或删除卷、附录与扩展接入点时，"
    "须在同一次提交内更新 `附录 E` 的归属索引与逐行原文。\n", 1)

# ---- glossary rows ----------------------------------------------------------
gloss_last = "| 在途槽位 | 已提交但未收敛至终态的订单所占据的注册表资源 |\n"
assert gloss_last in text
text = text.replace(gloss_last, gloss_last +
    "| 模块边界标注 | 声明模块名称、依赖与被依赖的结构化元信息，用于判断模块可否安全增删 |\n"
    "| 设计依据标注 | 记录关键决策的备选方案、取舍与可逆路径的结构化元信息，用于重构时还原设计意图 |\n"
    "| 扩展接入点标注 | 声明扩展位置标识、上下文与预期用法的结构化元信息，是后续增补内容前的约定入口 |\n", 1)

# ---- changelog bullets ------------------------------------------------------
cl = "- 新增扩展接入点、设计依据与模块边界三类结构化注释块，作为后续增删的官方接入位置。\n"
assert cl in text
text = text.replace(cl,
    "- 新增扩展接入点、设计依据与模块边界三类结构化标注，作为后续增删的官方接入位置。\n"
    "- 三类结构化标注由章节内就地注释改为 `附录 E` 集中登记，正文不再出现原始 HTML 注释；"
    "标注在代码围栏内逐行保留原始语法，兼顾渲染安全与脚本可解析性。\n"
    "- 新增 `附录 E 结构化标注索引` 与 `0.5 结构化标注与机器可读索引`，同步更新目录、术语表与站点图更新协议。\n", 1)

# ---- appendix D row ---------------------------------------------------------
l19 = "| L-19 | 实盘授权未授予 | 真实订单不可提交 | Owner 本人的显式动作，且须通过全部适用门禁 |\n"
assert l19 in text
text = text.replace(l19, l19 +
    "| L-20 | 禁用原始 HTML 的渲染环境中，章节内原始 HTML 注释会被逐字显示 | 就地标注会破坏正文可读性 | 标注集中于 `附录 E` 的代码围栏内，不依赖渲染器行为 |\n", 1)

P.write_text(text, encoding="utf-8")
print("\nwritten. new line count:", len(text.split("\n")))
