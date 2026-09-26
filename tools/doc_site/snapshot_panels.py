# -*- coding: utf-8 -*-
"""抓取页面中的代表性区块，生成独立调试页，供无头浏览器截图检查。"""
import io, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_site as B

page = io.open(B.OUT, encoding='utf-8').read()
css = page[page.find('<style>') + 7:page.find('</style>')]
TMP = os.environ.get('TEMP', '.')


def balanced_div(start):
    depth = 0
    for tok in re.finditer(r'<div\b|</div>', page[start:]):
        depth += 1 if tok.group(0) == '<div' else -1
        if depth == 0:
            return page[start:start + tok.end()]
    return ""


def write(name, body):
    doc = ('<!DOCTYPE html><html lang="zh-CN" data-theme="dark"><head><meta charset="utf-8">'
           '<style>' + css + '</style></head><body class="anim-off">'
           '<div class="shell"><main id="main">' + body + '</main></div></body></html>')
    io.open(os.path.join(TMP, name), 'w', encoding='utf-8', newline='\n').write(doc)
    print('%-30s %6d bytes' % (name, len(doc.encode('utf-8'))))


def panel_by_title(title):
    i = page.find('<div class="ptitle">' + title)
    if i < 0:
        return None
    return balanced_div(page.rfind('<div class="panel"', 0, i))


def section(start_id, stop_ids):
    a = page.find('id="%s"' % start_id)
    if a < 0:
        return None
    a = page.rfind('<h', 0, a)
    ends = [page.find('id="%s"' % s) for s in stop_ids]
    ends = [e for e in ends if e > a]
    b = min(ends) if ends else a + 30000
    return page[a:page.rfind('<h', 0, b)]


write('hy_dbg_metro-panel.html', panel_by_title('技术路线站点图'))
write('hy_dbg_cards.html', panel_by_title('站点卡片'))
write('hy_dbg_timeline.html', panel_by_title('预期完成时间轴'))
write('hy_dbg_dep.html', panel_by_title('可开工性判定'))
write('hy_dbg_tasks.html', section('60-stage-0-安全债务清零',
                                   ['61-stage-1a-现货最小安全垂直闭环']))
write('hy_dbg_math.html', section('31-收益来源风险溢价与小本金生存三铁律',
                                  ['32-仓库真实资产映射', '4-卷-4-审计缺陷处置矩阵']))
write('hy_dbg_fi.html', section('72-pre_d3-门禁用例', ['73-pre_owner_live-门禁用例']))
write('hy_dbg_appendix.html', section('附录-d-已知限制清单', ['附录-e-结构化标注索引']))
