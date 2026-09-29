// 无 JavaScript 时的降级核对：页面必须仍然完整可读，且不得出现"看起来能点但没反应"的残留。
import { Browser, sleep, fileUrl } from "./cdp.mjs";
import { join } from "node:path";
const ROOT = new URL("../../", import.meta.url).pathname.replace(/^\//, "");
const PAGE = fileUrl(join(ROOT, "docs", "HengYuan_v2_全自动量化实盘系统实施指南与TODOLIST.html"));
const b = await Browser.launch({ width: 1400, height: 900 });
let fails = 0;
const check = (n, ok, d = "") => { console.log(`  ${ok ? "OK  " : "FAIL"} ${n}${d ? "  " + d : ""}`); if (!ok) fails++; };
try {
  await b.send("Emulation.setScriptExecutionDisabled", { value: true }, b.sessionId);
  await b.goto(PAGE);
  await sleep(400);
  const r = await b.eval(`return {
    theme:document.documentElement.getAttribute('data-theme'),
    headings:document.querySelectorAll('main h1,main h2,main h3,main h4,main h5,main h6').length,
    h2:document.querySelectorAll('main h2').length,
    anchors:document.querySelectorAll('main [id]').length,
    cards:document.querySelectorAll('.card').length,
    tasks:document.querySelectorAll('.task').length,
    fiRows:document.querySelectorAll('.fi-table tbody tr').length,
    stations:document.querySelectorAll('.metro-svg .st').length,
    lanes:document.querySelectorAll('.metro-svg path.lane:not(.spur)').length,
    flows:document.querySelectorAll('.metro-svg .lane-flow').length,
    spurs:document.querySelectorAll('.metro-svg path.lane.spur').length,
    arcs:document.querySelectorAll('.metro-svg .xarc').length,
    trains:document.querySelectorAll('.metro-svg .train').length,
    diagrams:document.querySelectorAll('.diagram').length,
    math:document.querySelectorAll('math').length,
    unused:[...document.querySelectorAll('.empty')].length,
    tipVisible:getComputedStyle(document.getElementById('tip')).visibility,
    topVisible:getComputedStyle(document.getElementById('totop')).opacity,
    clearHidden:document.querySelector('[data-clear="all"]').hidden,
    hiddenCards:document.querySelectorAll('.card.hidden').length,
  };`);
  check("无 JS 时仍呈现完整正文", r.headings >= 96 && r.h2 >= 15,
    `标题=${r.headings} h2=${r.h2} 锚点=${r.anchors}`);
  check("无 JS 时站点图已静态绘制",
    r.stations === 25 && r.lanes === 5 && r.flows === 5 && r.spurs === 1 && r.arcs === 5 && r.trains === 5,
    `站点=${r.stations} 线路=${r.lanes} 光流=${r.flows} 支线=${r.spurs} 联络弧=${r.arcs} 列车=${r.trains}`);
  check("无 JS 时五张 Mermaid 图与公式已转换", r.diagrams === 4 && r.math > 0,
    `图解=${r.diagrams} 公式=${r.math}`);
  check("无 JS 时卡片与任务全部可见", r.cards === 25 && r.tasks === 47 && r.hiddenCards === 0,
    `${r.cards}/${r.tasks}/隐藏${r.hiddenCards}`);
  check("无 JS 时故障表完整", r.fiRows === 45, String(r.fiRows));
  check("无 JS 时无 JS 才创建的残留空态", r.unused === 0, String(r.unused));
  check("无 JS 时悬浮层不可见", r.tipVisible === "hidden", r.tipVisible);
  check("无 JS 时回到顶部按钮不可见", r.topVisible === "0", r.topVisible);
  check("无 JS 时清除筛选按钮保持隐藏", r.clearHidden === true);
  check("无 JS 时回退到静态深色主题", r.theme === "dark", r.theme);
} finally { await b.close(); }
console.log(fails ? `\n失败 ${fails} 项` : "\n无 JS 降级核对全部通过");
process.exit(fails ? 1 : 0);
