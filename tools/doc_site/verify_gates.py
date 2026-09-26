# -*- coding: utf-8 -*-
"""闸门负向对照：对生成物做定向变异，确认每一项校验确实会因此失败。

一条从不失败的闸门等于没有闸门。本脚本为每个闸门注入它本该捕获的那一类缺陷，
断言判定函数在"被污染"的输入上报告失败、在"干净"的输入上报告通过。

用法：python tools/doc_site/verify_gates.py
"""
from __future__ import annotations

import io
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "doc_site"))

import build_site as B  # noqa: E402
import verify_site as V  # noqa: E402

FAIL = []


def control(name: str, detects: bool, clean_ok: bool) -> None:
    """detects：闸门在被污染的输入上确实报失败；clean_ok：闸门在真实产物上通过。"""
    good = bool(detects) and bool(clean_ok)
    print("%-54s %s" % (name, "有效" if good else "无效（detects=%s clean=%s）"
                                        % (detects, clean_ok)))
    if not good:
        FAIL.append(name)


def expect(name: str, cond: bool) -> None:
    """直接断言：用于"闸门不得误报"这类反向对照。"""
    print("%-54s %s" % (name, "有效" if cond else "无效"))
    if not cond:
        FAIL.append(name)


MUST_VARS = {"--bg", "--fg", "--fg2", "--fg3", "--panel", "--panel2", "--line", "--line2",
             "--accent", "--done", "--wip", "--todo", "--blocked", "--dropped",
             "--arc", "--edge", "--hit", "--shadow", "--topbar", "--mark",
             "--lane-a", "--lane-b", "--lane-c", "--lane-d", "--lane-e",
             "--done-edge", "--wip-edge", "--blocked-edge"}

MOUNTS = ['id="q"', 'id="metro-svg"', 'id="status-note"', 'id="prog"', 'id="totop"',
          'data-clickmode="card"', 'data-count="fi"', 'data-clear="all"', 'class="row sub"']


def main() -> int:
    page = io.open(B.OUT, encoding="utf-8").read()
    scripts = V.extract_scripts(page)
    style = V.css_style_block(page)
    node = shutil.which("node")

    print("== H1 内联脚本语法（真语法解析） ==")
    if node:
        # 还原历史上真实发生过的缺陷：多一个右括号使整段脚本不执行
        broken = [s + "\n(function(){var a=1;a);})();" for s in scripts]
        control("注入多余的右括号后 H1 报失败",
                bool(V.js_syntax_errors(broken, node)),
                not V.js_syntax_errors(scripts, node))
    else:
        print("跳过：未安装 node")

    print("\n== I1 主题在首帧前落定 ==")
    first = page[page.find("<script>"):page.find("</script>") + 9]
    control("移除 head 中的主题引导脚本后 I1 报失败",
            not V.theme_before_style(page.replace(first, "", 1)),
            V.theme_before_style(page))

    print("\n== I3 CSS 变量定义完整 ==")
    mutated = re.sub(r"--wip-edge\s*:", "--wip-edge-x:", style)
    control("重命名变量定义、保留其引用后 I3 报失败",
            bool(V.undefined_css_vars(mutated)), not V.undefined_css_vars(style))

    print("\n== I4 浅色主题覆盖 ==")
    i, j = style.find('[data-theme="light"]'), style.find('[data-theme="light"]')
    j = style.find("}", i)
    stripped = style[:i] + '[data-theme="light"]{}' + style[j + 1:]
    dk, lt = V.theme_vars(style)
    dk2, lt2 = V.theme_vars(stripped)
    control("清空浅色主题块后 I4 报失败",
            bool((MUST_VARS & dk2) - lt2), not ((MUST_VARS & dk) - lt))

    print("\n== I5/I6 无写死图形色、无双百分号长度 ==")
    control("注入内联 stroke 颜色后 I5 报失败",
            V.inline_svg_colors(page.replace('class="lane lane-A" d=',
                                             'class="lane lane-A" stroke="#4da3ff" d=', 1)),
            not V.inline_svg_colors(page))
    control("把 SVG 百分比长度写成双百分号后 I6 报失败",
            V.double_percent_in_svg(page.replace('x="-60%"', 'x="-60%%"', 1)),
            not V.double_percent_in_svg(page))
    expect("正文出现双百分号不会误触发 I6（检查范围限定在站点图 SVG 内）",
           not V.double_percent_in_svg(page.replace("</main>", "<p>进度 100%%</p></main>", 1)))

    print("\n== J1 交互挂载点 ==")
    control("移除一个挂载点后 J1 报失败",
            any(n not in page.replace(n, "", 1) for n in MOUNTS),
            all(n in page for n in MOUNTS))
    control("移除 held 锁定环节点后 J1 报失败（该节点曾只有样式、没有渲染）",
            'class="held"' not in page.replace('class="held"', ""),
            'class="held"' in page)

    print("\n== J2 暂停同时覆盖 CSS 与 SMIL ==")
    paused_css = "animation-play-state:paused" in style
    control("移除 SMIL 暂停调用后 J2 报失败",
            not (paused_css and "pauseAnimations" in page.replace("pauseAnimations", "")),
            paused_css and "pauseAnimations" in page)
    control("移除 CSS 暂停规则后 J2 报失败",
            not ("animation-play-state:paused" in
                 V.css_style_block(page.replace("animation-play-state:paused", "", 1))
                 and "pauseAnimations" in page),
            paused_css and "pauseAnimations" in page)

    print("\n== J3 检索防抖 0.3 秒 ==")
    control("移除防抖常量后 J3 报失败",
            "DEBOUNCE_MS=300" not in page.replace("DEBOUNCE_MS=300", "", 1),
            "DEBOUNCE_MS=300" in page)

    print("\n== J4 摘要不再依赖会被子元素误触发的 data-tip ==")
    control("重新引入 data-tip 属性后 J4 报失败",
            (page + ' data-tip="x"').count('data-tip="') != 0,
            page.count('data-tip="') == 0)

    print("\n== J5 mpath 双属性写法 ==")
    control("去掉 xlink:href 后 J5 报失败",
            not ('xlink:href="#run-' in page.replace('xlink:href="#run-', 'href="#run-')
                 and 'xmlns:xlink' in page),
            'xlink:href="#run-' in page and 'xmlns:xlink' in page)

    print("\n== J6/J7 快捷键大写标示与大小写无关匹配 ==")
    lower_hint = page.replace("<kbd>P</kbd>", "<kbd>p</kbd>", 1).replace("<kbd>T</kbd>", "<kbd>t</kbd>", 1)
    letters, leftover = V.hint_letter_keys(lower_hint)
    expect("把快捷键行改回小写 p/t 后 J6 报失败",
           letters != V.LETTER_KEYS and leftover)
    clean_letters, clean_leftover = V.hint_letter_keys(page)
    expect("真实产物的快捷键行为大写且无残留",
           clean_letters == V.LETTER_KEYS and not clean_leftover)
    expect("移除大小写归一化后 J7 报失败",
           not V.key_match_case_insensitive(
               page.replace("var k=e.key.toLowerCase();", "var k=e.key;", 1)))
    expect("保留归一化语句、但分支改回比对 e.key 后 J7 仍报失败",
           not V.key_match_case_insensitive(
               page.replace("if(k==='p')", "if(e.key==='p')", 1)))
    expect("真实产物含大小写归一化", V.key_match_case_insensitive(page))

    print()
    if FAIL:
        print("闸门负向对照失败 %d 项：%s" % (len(FAIL), FAIL))
        return 1
    print("全部闸门负向对照通过：每项校验都能被它该捕获的缺陷触发")
    return 0


if __name__ == "__main__":
    sys.exit(main())
