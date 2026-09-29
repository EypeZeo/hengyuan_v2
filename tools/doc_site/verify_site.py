# -*- coding: utf-8 -*-
"""校验生成的 HTML 视图与权威 Markdown 逐项一致，并检查结构完整性。

校验项：
  A 结构   单文档、标签配对、无未闭合围栏、无原始 HTML 注释泄漏
  B 离线   无任何外部引用（协议相对、http、https）
  C 锚点   每个站内链接都能命中同名 id
  D 同源   站点、故障用例、任务三张清单与 Markdown 逐项一致（数量 + 关键字段）
  E 图形   五个 Mermaid 图全部转为 SVG，且图内不存在残留的 Mermaid 语法
  F 脚本   数据岛可解析，属性中的换行为真实换行
"""
from __future__ import annotations

import collections
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "doc_site"))

import build_site as B  # noqa: E402

FAIL = []


def check(name: str, ok: bool, detail: str = "") -> None:
    print("%-46s %s%s" % (name, "OK" if ok else "FAIL", ("  " + detail) if detail else ""))
    if not ok:
        FAIL.append(name)


# --------------------------------------------------------------------------- #
# 可独立调用的判定函数：verify_gates.py 用它们对生成物做变异，验证每项闸门确实会失败。
# 一条从不失败的闸门等于没有闸门，因此这些判定必须与主流程共用同一份实现。
# --------------------------------------------------------------------------- #

def extract_scripts(page: str) -> list:
    return re.findall(r"<script>(.*?)</script>", page, re.S)


def css_style_block(page: str) -> str:
    a = page.find("<style>")
    return page[a + 7:page.find("</style>")] if a >= 0 else ""


def theme_vars(style: str) -> tuple:
    """返回（深色变量名集合, 浅色变量名集合）。"""
    i = style.find(':root{')
    j = style.find('[data-theme="light"]')
    if i < 0 or j < 0:
        return set(), set()
    dark = set(re.findall(r"(--[a-z0-9-]+)\s*:", style[i:j]))
    tail = style[j:]
    k = tail.find("}")
    light = set(re.findall(r"(--[a-z0-9-]+)\s*:", tail[:k if k >= 0 else len(tail)]))
    return dark, light


def undefined_css_vars(style: str) -> list:
    used = set(re.findall(r"var\((--[a-z0-9-]+)", style))
    defined = set(re.findall(r"(--[a-z0-9-]+)\s*:", style))
    return sorted(used - defined)


def js_syntax_errors(scripts: list, node: str) -> list:
    """用 node --check 对内联脚本做真语法解析。"""
    import subprocess
    import tempfile
    bad = []
    for i, src in enumerate(scripts):
        with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False,
                                         encoding="utf-8", newline="\n") as fh:
            fh.write(src)
            path = fh.name
        try:
            r = subprocess.run([node, "--check", path], capture_output=True, text=True)
            if r.returncode != 0:
                tail = (r.stderr or "").strip().splitlines()
                bad.append("脚本%d: %s" % (i, tail[-1] if tail else "解析失败"))
        finally:
            os.unlink(path)
    return bad


def theme_before_style(page: str) -> bool:
    head = page[:page.find("</head>")] if "</head>" in page else page
    a, b = head.find("hy-theme"), head.find("<style>")
    return a >= 0 and b >= 0 and a < b


def inline_svg_colors(page: str) -> bool:
    return bool(re.search(r'class="(?:lane|xarc|train)[^"]*"[^>]*stroke="#', page))


def double_percent_in_svg(page: str) -> bool:
    """SVG 长度里的双百分号曾使滤镜失效并刷出 4 条控制台错误。
    只在站点图 SVG 范围内检查，避免正文出现 %% 时误报。"""
    a = page.find('<svg id="metro-svg"')
    b = page.find("</svg>", a) if a >= 0 else -1
    return "%%" in page[a:b] if a >= 0 and b > a else False


# 快捷键行以大写字母标示字母键；同时脚本必须以大小写无关的方式匹配，
# 否则标着 P 却只有小写 p 生效——标示与实际行为不一致比小写标示更糟。
LETTER_KEYS = ("P", "T")
PLAIN_KEYS = ("/", "Esc", "?")


def hint_letter_keys(page: str) -> tuple:
    """返回 (标示的字母键列表, 是否残留小写标示)。"""
    m = re.search(r'<span id="qhint".*?</span>', page, re.S)
    if not m:
        return ((), True)
    row = m.group(0)
    found = tuple(k for k in LETTER_KEYS if "<kbd>%s</kbd>" % k in row)
    lowercase_leftover = any("<kbd>%s</kbd>" % k.lower() in row for k in LETTER_KEYS)
    return (found, lowercase_leftover)


def key_match_case_insensitive(page: str) -> bool:
    """快捷键分支必须先把 e.key 归一化，否则 Shift+P 得到 'P' 会漏判。
    只检查归一化语句存在是不够的——三个分支必须真的拿归一化后的值去比对。"""
    if "var k=e.key.toLowerCase();" not in page:
        return False
    return all(("k==='%s'" % c) in page for c in ("p", "t", "?"))


def main() -> int:
    md = io.open(B.SRC, encoding="utf-8").read()
    lines = md.split("\n")
    if not os.path.exists(B.OUT):
        print("产物不存在，请先运行 build_site.py")
        return 2
    page = io.open(B.OUT, encoding="utf-8").read()

    # ---- A 结构 ----
    check("A1 单文档骨架", page.count("<!DOCTYPE html>") == 1 and page.count("<html") == 1
          and page.count("</html>") == 1 and page.count("<body") == 1 and page.count("</body>") == 1)
    for tag in ("div", "svg", "g", "table", "tr", "td", "th", "article", "section",
                "dl", "dt", "dd", "nav", "main", "pre", "figure", "header", "details"):
        o = len(re.findall(r"<%s[\s>]" % tag, page))
        c = page.count("</%s>" % tag)
        if o != c:
            check("A2 <%s> 配对" % tag, False, "开 %d 闭 %d" % (o, c))
            break
    else:
        check("A2 标签配对（17 类）", True)
    check("A3 注释泄漏", "<!--" not in page, "发现 %d 处" % page.count("<!--"))
    check("A4 CSS 花括号平衡", page[page.find("<style>"):page.find("</style>")].count("{")
          == page[page.find("<style>"):page.find("</style>")].count("}"))

    # ---- B 离线 ----
    ext = re.findall(r'(?:src|href)\s*=\s*"(?:https?:)?//', page, re.I)
    check("B1 无外部引用", not ext, str(ext[:3]))
    check("B2 无 import/外链样式", "@import" not in page and "<link" not in page)

    # ---- C 锚点 ----
    ids = set(re.findall(r'id="([^"]+)"', page))
    refs = [r for r in re.findall(r'href="#([^"]+)"', page)]
    broken = sorted({r for r in refs if r not in ids})
    check("C1 站内链接全部命中", not broken, "失效 %d：%s" % (len(broken), broken[:4]))
    head_ids = {B.slug(h["title"]) for h in B.parse_heading_index(lines)}
    missing_head = sorted(h for h in head_ids if h not in ids and h)
    check("C2 Markdown 标题锚点齐备", not missing_head, "缺 %d：%s" % (len(missing_head), missing_head[:4]))

    # ---- D 同源 ----
    mermaid = B.parse_mermaid(B.parse_mermaid_blocks(lines)[0]["body"])
    B.LANE_ORDER = {s["id"][1:] if re.match(r"^L[A-Z]$", s["id"]) else s["id"]: s["nodes"]
                    for s in mermaid["subgraphs"]}
    stations = B.parse_stations(lines, mermaid)
    fi, _ = B.parse_fi(lines)
    tasks, _ = B.parse_tasks(lines, B.parse_heading_index(lines))
    annotations = B.parse_annotation_lines(lines)
    data = json.loads(re.search(r'<script id="site-data" type="application/json">(.*?)</script>',
                                page, re.S).group(1))

    mset = {(s["id"], s["status"], s["due"], s["anchor"]) for s in stations}
    hset = {(s["id"], s["status"], s["due"], s["anchor"]) for s in data["stations"]}
    check("D1 站点逐项一致", mset == hset,
          "Markdown %d / HTML %d / 差集 %s" % (len(mset), len(hset), sorted(mset ^ hset)[:3]))
    check("D2 站点数量 25", len(stations) == 25, str(len(stations)))
    check("D3 图内站点节点齐备",
          all(page.count('data-station="%s"' % s["id"]) >= 2 for s in stations),
          "缺失：" + str([s["id"] for s in stations if page.count('data-station="%s"' % s["id"]) < 2][:4]))
    check("D4 故障用例逐项一致",
          {(f["id"], f["gate"], f["impl"]) for f in fi}
          == {(f["id"], f["gate"], f["impl"]) for f in data["fi"]})
    check("D5 故障用例 45 条且完备", len(fi) == 45
          and sorted(int(f["id"][3:]) for f in fi) == list(range(1, 46)), str(len(fi)))
    check("D6 任务清单逐项一致",
          {(t["id"], t["priority"], t["status"]) for t in tasks}
          == {(t["id"], t["priority"], t["status"]) for t in data["tasks"]})
    check("D7 任务数量 47", len(tasks) == 47, str(len(tasks)))
    check("D8 任务卡片全部渲染", sum(page.count('class="task ') for _ in [0]) == len(tasks),
          "卡片 %d / 任务 %d" % (page.count('class="task '), len(tasks)))
    check("D9 站点卡片全部渲染", page.count('class="card c-') == len(stations),
          "卡片 %d / 站点 %d" % (page.count('class="card c-'), len(stations)))
    check("D10 标注副本一致", len(annotations) == 25 and page.count("&lt;!--") == 25,
          "Markdown %d / HTML %d" % (len(annotations), page.count("&lt;!--")))

    # 1.2 线路总览的类名与节点内联标记，必须与 1.3 明细表的状态列一致
    cls_of = {}
    for m in re.finditer(r"^    class ([A-Z0-9,]+) (\w+)$",
                         "\n".join(B.parse_mermaid_blocks(lines)[0]["body"]), flags=re.M):
        for nid in m.group(1).split(","):
            cls_of[nid] = m.group(2)
    glyph = {"done": "DONE", "wip": "WIP", "todo": "TODO",
             "blocked": "BLOCKED", "dropped": "DROPPED"}
    conflicts = []
    for s in stations:
        want = glyph.get(cls_of.get(s["id"], ""), "?")
        inline = next((n for n, rx in B.STATUS_MARKS
                       if rx.search(s["label"].split("<br/>")[-1].strip())), None)
        if want != s["status"] or (inline and inline != s["status"]):
            conflicts.append("%s(类名%s/内联%s/明细%s)" % (s["id"], want, inline, s["status"]))
    check("D11 站点图与明细表状态一致", not conflicts, "；".join(conflicts))

    # ---- E 图形 ----
    n_mermaid = len(B.parse_mermaid_blocks(lines))
    check("E1 Mermaid 块全部转 SVG", page.count('class="diagram"') == n_mermaid - 1
          and page.count('class="metro-svg"') == 1,
          "diagram %d / metro %d / 源 %d" % (page.count('class="diagram"'),
                                             page.count('class="metro-svg"'), n_mermaid))
    leaked = [k for k in ("graph LR", "graph TD", "classDef", "-.->", "```mermaid")
              if k in page]
    leaked += re.findall(r'subgraph\s+\w+\s*\["', page)
    check("E2 无 Mermaid 语法残留", not leaked, str(leaked))
    check("E3 动画原语存在", all(k in page for k in ("animateMotion", "<mpath href=", "@keyframes flow",
                                                   "@keyframes pulse", "prefers-reduced-motion")))
    check("E4 线路上色齐备", all(("lane-%s" % l) in page for l in "ABCDE"))

    # ---- F 脚本 ----
    check("F1 数据岛可解析", isinstance(data, dict) and "meta" in data)
    check("F2 无字面换行转义", "\\n" not in re.sub(r'<script>.*?</script>', "", page, flags=re.S))
    check("F3 页面无生成时间戳", not re.search(r"生成(于|时间)[：:]\s*20\d\d", page))

    # ---- G 溯源 ----
    import hashlib
    raw = io.open(B.SRC, "rb").read()
    real = hashlib.sha256(raw).hexdigest()
    claimed = re.search(r'name="source-sha256" content="([0-9a-f]+)"', page)
    check("G1 源哈希可由外部工具复核", bool(claimed) and claimed.group(1) == real,
          "页面 %s / 实测 %s" % (claimed.group(1)[:12] if claimed else "无", real[:12]))
    check("G2 页脚展示的哈希前缀一致", real[:12] in page)

    # ---- H 脚本可执行性 ----
    # H1 是本文件最重要的一项：脚本语法错误时，页面结构完全正常、A~G 全绿，
    # 而所有交互静默失效。生成物曾因为 fn.apply()) 多一个右括号，整段脚本不执行，
    # 结构校验 26 项仍全部通过。此处用 node --check 对抽取出的内联脚本做真语法解析。
    scripts = extract_scripts(page)
    check("H0 内联脚本数量", len(scripts) == 2, str(len(scripts)))
    import shutil
    node = shutil.which("node")
    if not node:
        check("H1 内联脚本语法可解析", True, "跳过：未安装 node")
    else:
        check("H1 内联脚本语法可解析（语法错误会让全部交互静默失效）",
              not js_syntax_errors(scripts, node), "解析失败")

    # ---- I 主题与样式完整性 ----
    style = css_style_block(page)
    check("I1 主题在首帧前落定（引导脚本先于样式表）", theme_before_style(page))
    check("I2 color-scheme 已声明且随主题切换",
          "color-scheme" in page[:page.find("</head>")] and page.count("color-scheme") >= 3)
    undef = undefined_css_vars(style)
    check("I3 CSS 变量无未定义引用", not undef, str(undef[:6]))
    # 浅色主题必须覆盖全部会随主题变化的变量，否则浅色下会残留深色取值
    must = {"--bg", "--fg", "--fg2", "--fg3", "--panel", "--panel2", "--line", "--line2",
            "--accent", "--done", "--wip", "--todo", "--blocked", "--dropped",
            "--arc", "--edge", "--hit", "--shadow", "--topbar", "--mark",
            "--lane-a", "--lane-b", "--lane-c", "--lane-d", "--lane-e",
            "--done-edge", "--wip-edge", "--blocked-edge"}
    dk, lt = theme_vars(style)
    check("I4 浅色主题覆盖全部主题相关变量", not (must & dk) - lt,
          "缺：" + str(sorted((must & dk) - lt))[:6])
    # 生成端不应再写死图形颜色，颜色只由 CSS 变量持有
    check("I5 站点图无写死的图形描边色", not inline_svg_colors(page), "发现内联颜色")
    check("I6 站点图 SVG 长度无双百分号", not double_percent_in_svg(page))

    # ---- J 交互挂载点 ----
    # 主脚本依赖这些元素；缺失时脚本不会报错，只会静默少一块能力
    need = {
        "顶部检索框": 'id="q"',
        "主题按钮": 'id="btn-theme"',
        "动画按钮": 'id="btn-anim"',
        "站点图根节点": 'id="metro-svg"',
        "站点图面板": 'id="metro-panel"',
        "站点点位样式钩子": 'class="held"',
        "悬浮摘要容器": 'class="tip-meta"',
        "状态提示区": 'id="status-note"',
        "阅读进度条": 'id="prog"',
        "回到顶部": 'id="totop"',
        "单击行为切换": 'data-clickmode="section"',
        "单击行为切换（卡片）": 'data-clickmode="card"',
        "结果计数（站点）": 'data-count="metro"',
        "结果计数（任务）": 'data-count="task"',
        "结果计数（用例）": 'data-count="fi"',
        "清除筛选": 'data-clear="all"',
        "分面计数钩子": 'data-n="status:DONE"',
        "快捷键说明": 'class="row sub"',
    }
    missing = [k for k, v in need.items() if v not in page]
    check("J1 交互挂载点齐备", not missing, "缺：" + str(missing))
    # 暂停必须同时覆盖 CSS 与 SMIL：只写 animation-play-state 时 SMIL 列车照跑
    check("J2 暂停同时覆盖 CSS 与 SMIL",
          "animation-play-state:paused" in style and "pauseAnimations" in page)
    check("J3 检索防抖已实现", "DEBOUNCE_MS=300" in page and "setTimeout" in page)
    check("J4 摘要不再挂在会被子元素误触发的 data-tip 上",
          page.count('data-tip="') == 0)
    check("J5 mpath 同时提供 href 与 xlink:href（兼容性）",
          'xlink:href="#run-' in page and 'xmlns:xlink' in page)
    # 快捷键行标示大写字母，脚本匹配大小写无关：二者必须同时成立
    letters, leftover = hint_letter_keys(page)
    check("J6 快捷键行以大写字母标示字母键",
          letters == LETTER_KEYS and not leftover,
          "标示 %s，残留小写=%s" % (list(letters), leftover))
    check("J7 快捷键匹配大小写无关（Shift+P 必须生效）",
          key_match_case_insensitive(page))

    print()
    if FAIL:
        print("失败 %d 项：%s" % (len(FAIL), FAIL))
        return 1
    print("全部校验通过（%d 字节）" % len(page.encode("utf-8")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
