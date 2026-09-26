// 交互回归闸门：用真实浏览器、真实鼠标事件与真实计时钟，逐条验证交互行为。
// 这是 verify_site.py 的结构校验无法覆盖的一层——页面脚本一旦抛异常或行为退化，
// 结构校验仍会全绿，只有这里会失败。
//
// 用法：node tools/doc_site/verify_interaction.mjs
import { Browser, sleep, fileUrl } from "./cdp.mjs";
import { join } from "node:path";

const ROOT = new URL("../../", import.meta.url).pathname.replace(/^\//, "");
const PAGE = fileUrl(join(ROOT, "docs", "HengYuan_v2_全自动量化实盘系统实施指南与TODOLIST.html"));

let pass = 0;
const fails = [];
function check(name, ok, detail = "") {
  if (ok) { pass++; console.log(`  OK   ${name}${detail ? "  " + detail : ""}`); }
  else { fails.push(name); console.log(`  FAIL ${name}${detail ? "  " + detail : ""}`); }
}
function section(t) { console.log(`\n${t}`); }

const b = await Browser.launch({ width: 1500, height: 950 });
const errors = [];
b.ws.addEventListener("message", (ev) => {
  const m = JSON.parse(ev.data);
  if (m.method === "Runtime.exceptionThrown") {
    errors.push("EXC " + (m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text));
  }
  if (m.method === "Log.entryAdded" && m.params.entry.level === "error") {
    errors.push("LOG " + m.params.entry.text);
  }
});
await b.send("Log.enable", {}, b.sessionId);

try {
  await b.goto(PAGE);
  await sleep(600);

  // ---------- 1 脚本执行 ----------
  section("[1] 脚本执行与运行时健康");
  check("主脚本无未捕获异常（曾因 fn.apply()) 多一个右括号整体不执行）", errors.length === 0,
    errors.slice(0, 3).join(" | "));
  check("交互已初始化（筛选计数已写入）",
    (await b.eval(`return (document.querySelector('[data-count="metro"]').textContent||'').length>0`)));
  check("控制台无渲染错误（曾因 SVG 滤镜百分比长度写成双百分号而报警）",
    !errors.some(e => /Expected length|filter/.test(e)), errors.filter(e => /filter/.test(e))[0] || "");

  // ---------- 2 悬浮摘要 ----------
  section("[2] 悬浮摘要：内容、防抖、无闪烁");
  // 页面启用了 scroll-behavior:smooth。跨章节的长距离平滑滚动可达数秒，
  // 必须等 scrollY 真正稳定后再取坐标，否则 rect 是滚动中途的旧值，鼠标会点到空处。
  async function settle() {
    await b.eval(`
      window.__stable=0;
      return new Promise(res=>{
        let last=-1,tick=0;
        (function poll(){
          const y=Math.round(window.scrollY);
          window.__stable = (y===last)?window.__stable+1:0;
          last=y;
          if(window.__stable>=3||++tick>240)return res(window.__stable);
          requestAnimationFrame(poll);
        })();
      });`);
  }
  async function centerOn(sel) {
    await b.eval(`document.querySelector('${sel}').scrollIntoView({block:'center'});`);
    await sleep(120);
    await settle();
    return b.eval(`
      const r=document.querySelector('${sel}').getBoundingClientRect();
      return {x:Math.round(r.left+r.width/2),y:Math.round(r.top+r.height/2)};`);
  }
  const pos = await centerOn('.st[data-station="B2"]');
  await b.mouse("mouseMoved", pos.x, pos.y);
  await sleep(60);
  const tipEarly = await b.eval(`return document.getElementById('tip').classList.contains('on')`);
  await sleep(300);
  const tipInfo = await b.eval(`
    const t=document.getElementById('tip');
    return {on:t.classList.contains('on'),
            id:t.querySelector('.tip-id').textContent,
            badge:t.querySelector('.tip-badge').textContent,
            name:t.querySelector('.tip-name').textContent,
            rows:t.querySelectorAll('.tip-meta>div').length,
            hint:t.querySelector('.tip-hint').textContent};`);
  check("悬停延迟后才显示（非瞬间闪现）", tipEarly === false);
  check("摘要已显示且带站点标识", tipInfo.on && tipInfo.id === "B2", `${tipInfo.id} ${tipInfo.badge}`);
  check("摘要含简要概括与结构化字段", tipInfo.name.length > 2 && tipInfo.rows >= 6,
    `${tipInfo.name.slice(0, 18)}… / ${tipInfo.rows} 行`);
  check("摘要提示单击行为", /单击/.test(tipInfo.hint), tipInfo.hint);

  // 在同一站点内部移动：曾经会先隐藏再显示造成闪烁
  const inner = await b.eval(`
    const g=document.querySelector('.st[data-station="B2"]');
    const dot=g.querySelector('.dot'), code=g.querySelector('.code');
    const dr=dot.getBoundingClientRect(), cr=code.getBoundingClientRect();
    return {ax:Math.round(dr.left+dr.width/2),ay:Math.round(dr.top+dr.height/2),
            bx:Math.round(cr.left+cr.width/2),by:Math.round(cr.top+cr.height/2)};`);
  const tipEl = await b.eval(`
    const g=document.querySelector('.st[data-station="B2"]');
    const dot=g.querySelector('.dot');
    let hidden=false;
    const t=document.getElementById('tip');
    const mo=new MutationObserver(()=>{if(!t.classList.contains('on'))hidden=true;});
    mo.observe(t,{attributes:true,attributeFilter:['class']});
    dot.dispatchEvent(new MouseEvent('mouseout',{bubbles:true,relatedTarget:g.querySelector('.code')}));
    return {hidden};`);
  check("同一站点内部移动不隐藏摘要（消除闪烁）", tipEl.hidden === false);

  const gone = await b.eval(`
    document.querySelector('.lane-A').dispatchEvent(new MouseEvent('mouseover',{bubbles:true,relatedTarget:null}));
    return !document.getElementById('tip').classList.contains('on');`);
  check("离开站点后摘要收起", gone === true);

  // ---------- 3 单击跳转 ----------
  section("[3] 单击站点跳转到对应文字节点");
  const jump = await b.eval(`
    const g=document.querySelector('.st[data-station="B2"]');
    const a=g.getAttribute('data-anchor');
    const row=[...document.querySelectorAll('tbody tr')].find(r=>r.textContent.trim().startsWith('B2'));
    const inTable=(row&&row.querySelector('a'))?row.querySelector('a').getAttribute('href').slice(1):null;
    const data=JSON.parse(document.getElementById('site-data').textContent);
    const inIsland=(data.stations.find(s=>s.id==='B2')||{}).anchor;
    return {anchor:a,sectionExists:!!document.getElementById(a),
            inTable:inTable,inIsland:inIsland,title:(document.getElementById(a)||{}).textContent};`);
  check("站点节点的章节锚点与 1.3 明细表、数据岛三处一致",
    jump.sectionExists && jump.anchor === jump.inTable && jump.anchor === jump.inIsland,
    `${jump.anchor} / 表 ${jump.inTable} / 岛 ${jump.inIsland}`);
  check("锚点确实指向对应章节（而不仅是任一节点）",
    jump.anchor.startsWith("61-stage-1a") && /Stage 1A/.test(jump.title || ""), jump.title);

  const cardClick = await centerOn('.st[data-station="B2"]');
  await b.clickAt(cardClick.x, cardClick.y);
  await sleep(1400);
  const landed = await b.eval(`
    const h=document.getElementById('23-已合入主干的执行底座');
    const t=document.getElementById(decodeURIComponent(location.hash).slice(1));
    const r=t.getBoundingClientRect();
    const bar=document.querySelector('.topbar').getBoundingClientRect().height;
    return {top:Math.round(r.top),bar:Math.round(bar),
            hash:decodeURIComponent(location.hash),targetId:t.id,
            flashed:t.classList.contains('head-hit')};`);
  check("跳转落在对应章节而非站点卡片", landed.targetId === "61-stage-1a-现货最小安全垂直闭环",
    landed.targetId);
  check("跳转后目标标题位于视口内", landed.top > 0 && landed.top < 300, `top=${landed.top}`);
  check("目标标题未被吸顶栏遮挡（scroll-margin-top 生效）", landed.top >= landed.bar - 6,
    `top=${landed.top} bar=${landed.bar}`);
  check("地址栏锚点已同步，可复制分享", landed.hash === "#61-stage-1a-现货最小安全垂直闭环",
    landed.hash);

  // 定位卡片模式
  await b.eval(`document.querySelector('[data-clickmode="card"]').click();`);
  await sleep(200);
  const cp = await centerOn('.st[data-station="B3"]');
  await b.clickAt(cp.x, cp.y);
  await sleep(1400);
  const cardLanded = await b.eval(`
    const c=document.getElementById('card-B3');
    const r=c.getBoundingClientRect();
    return {visible:!c.classList.contains('hidden'),
            inView:r.top>-50&&r.bottom<window.innerHeight+50,
            hit:c.classList.contains('hit')};`);
  check("切换为“定位卡片”后单击落在卡片上", cardLanded.visible && cardLanded.inView,
    JSON.stringify(cardLanded));
  await b.eval(`document.querySelector('[data-clickmode="section"]').click();`);

  // ---------- 4 暂停动画 ----------
  section("[4] 暂停动画：CSS 与 SMIL 必须一起停");
  await b.eval(`document.getElementById('metro-panel').scrollIntoView({block:'center'});`);
  await sleep(700);
  const running = await b.eval(`
    const svg=document.getElementById('metro-svg');
    const t=svg.querySelector('.train');
    const a=t.getBoundingClientRect().left;
    return new Promise(res=>setTimeout(()=>res({
      moved:Math.abs(t.getBoundingClientRect().left-a),
      paused:svg.animationsPaused?svg.animationsPaused():null,
      css:getComputedStyle(svg.querySelector('.lane-flow')).animationPlayState}),1000));`);
  check("默认状态下列车在动", running.moved > 0.5 || running.css === "running",
    `位移=${running.moved.toFixed(2)} css=${running.css}`);
  check("默认状态 SMIL 未暂停", running.paused === false, String(running.paused));

  await b.eval(`document.getElementById('btn-anim').click();`);
  await sleep(300);
  const stopped = await b.eval(`
    const svg=document.getElementById('metro-svg');
    const t=svg.querySelector('.train');
    const a=t.getBoundingClientRect().left;
    return new Promise(res=>setTimeout(()=>res({
      moved:Math.abs(t.getBoundingClientRect().left-a),
      paused:svg.animationsPaused?svg.animationsPaused():null,
      css:getComputedStyle(svg.querySelector('.lane-flow')).animationPlayState,
      label:document.getElementById('btn-anim').textContent,
      pressed:document.getElementById('btn-anim').getAttribute('aria-pressed')}),900));`);
  check("暂停后 SMIL 列车真正停住（此前 animation-play-state 管不到 SMIL）",
    stopped.moved < 0.4, `位移=${stopped.moved.toFixed(2)}`);
  check("暂停后 SMIL 时间轴自报已暂停", stopped.paused === true, String(stopped.paused));
  check("暂停后 CSS 动画也停", stopped.css === "paused", stopped.css);
  check("按钮文案与 aria 状态同步", stopped.label === "播放动画" && stopped.pressed === "true",
    `${stopped.label}/${stopped.pressed}`);

  // 重启页面后仍保持暂停
  await b.goto(PAGE);
  await sleep(500);
  await b.eval(`document.getElementById('metro-panel').scrollIntoView({block:'center'});`);
  await sleep(600);
  const persisted = await b.eval(`
    const svg=document.getElementById('metro-svg');
    return {paused:svg.animationsPaused?svg.animationsPaused():null,
            cls:document.body.className,
            label:document.getElementById('btn-anim').textContent};`);
  check("重新加载后暂停状态被记住（此前不持久化）",
    persisted.paused === true && persisted.label === "播放动画", JSON.stringify(persisted));

  // ---------- 5 进行中站点的短停顿 ----------
  section("[5] 单击进行中站点：短暂停顿后自动恢复");
  await b.eval(`document.getElementById('btn-anim').click();`);   // 先恢复动画
  await sleep(500);
  const wipStation = await b.eval(`
    return document.querySelector('.st[data-status="wip"]')?.getAttribute('data-station') || null;`);
  const wipSelector = wipStation ? `.st[data-station="${wipStation}"]` : null;
  const wipSelectorLiteral = JSON.stringify(wipSelector);
  const wipPos = wipSelector ? await centerOn(wipSelector) : {x: 0, y: 0};
  const wipStatus = await b.eval(`
    const node=document.querySelector(${wipSelectorLiteral});
    return node?.getAttribute('data-status') || null;`);
  check("被测试站点确为进行中", wipStatus === "wip", wipStatus);
  if (!wipSelector) {
    check("存在可用于停顿回归的进行中站点", false, "没有 data-status=\"wip\" 的站点");
  }
  await b.clickAt(wipPos.x, wipPos.y);
  await sleep(200);
  const held = await b.eval(`
    const svg=document.getElementById('metro-svg');
    const t=svg.querySelector('.train');
    const a=t.getBoundingClientRect().left;
    return new Promise(res=>setTimeout(()=>res({
      paused:svg.animationsPaused?svg.animationsPaused():null,
      moved:Math.abs(t.getBoundingClientRect().left-a),
      held:document.querySelector(${wipSelectorLiteral})?.classList.contains('held') || false,
      offscreen:!document.getElementById('metro-panel').getBoundingClientRect().bottom>0,
      note:document.getElementById('status-note').textContent}),900));`);
  check("单击进行中站点后动画停顿", held.paused === true, String(held.paused));
  check("停顿时站点出现锁定环（此前 CSS 有样式但脚本从未加类）", held.held === true);
  const ring = await b.eval(`
    const c=document.querySelector(${wipSelectorLiteral} + ' .held');
    const pulse=document.querySelector(${wipSelectorLiteral} + ' .halo');
    if (!c || !pulse) return {stroke:"none",width:0,opacity:0,halo:"block"};
    const s=getComputedStyle(c);
    return {stroke:s.stroke,width:parseFloat(s.strokeWidth),opacity:parseFloat(s.opacity),
            halo:getComputedStyle(pulse).display};`);
  check("锁定环样式确实生效（不只是加了类名）",
    ring.stroke !== "none" && ring.width >= 2 && ring.opacity > 0.5,
    `stroke=${ring.stroke} 宽=${ring.width} 透明=${ring.opacity}`);
  check("停顿时脉冲环让位给锁定环", ring.halo === "none", ring.halo);
  check("停顿时给出文字反馈", /停顿|暂停/.test(held.note), held.note);
  // 该次单击同时会跳走，站点图离开视口；规则是回到站点图才收尾，因此先返回
  await sleep(2600);
  await b.eval(`document.getElementById('metro-panel').scrollIntoView({block:'center'});`);
  await sleep(900);
  const resumed = await b.eval(`
    const svg=document.getElementById('metro-svg');
    const t=svg.querySelector('.train');
    const a=t.getBoundingClientRect().left;
    return new Promise(res=>setTimeout(()=>res({
       paused:svg.animationsPaused?svg.animationsPaused():null,
       moved:Math.abs(t.getBoundingClientRect().left-a),
       held:document.querySelector(${wipSelectorLiteral})?.classList.contains('held') || false}),1200));`);
  check("回到站点图后停顿收尾、动画恢复（离开视口期间不会静默过期）",
    resumed.paused === false && resumed.moved > 0.5, `位移=${resumed.moved.toFixed(2)}`);
  check("恢复后锁定环撤下", resumed.held === false);

  // ---------- 6 检索防抖与筛选 ----------
  section("[6] 检索防抖与筛选反馈");
  await b.eval(`window.scrollTo(0,0);document.getElementById('q').value='';`);
  await b.eval(`document.querySelector('[data-clear="all"]').click();`);
  await sleep(200);
  const debounce = await b.eval(`
    const q=document.getElementById('q');
    const vis=()=>document.querySelectorAll('.card:not(.hidden)').length;
    const before=vis();
    q.value='限流';q.dispatchEvent(new Event('input',{bubbles:true}));
    const t0=performance.now();
    const immediate=vis();
    return new Promise(res=>setTimeout(()=>{
      const at150=vis();
      setTimeout(()=>res({before,immediate,at150,at400:vis(),ms:Math.round(performance.now()-t0)}),260);
    },150));`);
  check("输入后未立即筛选（防抖生效）",
    debounce.immediate === debounce.before && debounce.at150 === debounce.before,
    `即时=${debounce.immediate} 150ms=${debounce.at150} 起=${debounce.before}`);
  check("约 0.3 秒后筛选生效", debounce.at400 < debounce.before && debounce.at400 > 0,
    `400ms 后可见 ${debounce.at400}/${debounce.before}`);
  const facets = await b.eval(`
    return {count:document.querySelector('[data-count="metro"]').textContent,
            nDone:document.querySelector('[data-n="status:DONE"]').textContent,
            nTodo:document.querySelector('[data-n="status:TODO"]').textContent,
            clearVisible:!document.querySelector('[data-clear="all"]').hidden};`);
  check("结果计数可见", /显示/.test(facets.count), facets.count);
  check("筛选片显示分面数量", facets.nDone !== "" && facets.nTodo !== "",
    `DONE=${facets.nDone} TODO=${facets.nTodo}`);
  check("存在筛选时出现清除按钮", facets.clearVisible === true);

  const mapSync = await b.eval(`
    return {dim:document.querySelectorAll('.st.dim').length,
            lit:document.querySelectorAll('.st:not(.dim)').length,
            marks:document.querySelectorAll('mark').length,
            bars:document.querySelectorAll('.gantt .bar.dim').length};`);
  check("检索同时收敛线路图（此前图完全不受检索影响）",
    mapSync.dim > 0 && mapSync.lit > 0, `淡化=${mapSync.dim} 命中=${mapSync.lit}`);
  check("检索同时收敛时间轴", mapSync.bars > 0, String(mapSync.bars));

  const nomatch = await b.eval(`
    const q=document.getElementById('q');
    q.value='zzz-不存在-zzz';q.dispatchEvent(new Event('input',{bubbles:true}));
    return new Promise(res=>setTimeout(()=>res({
      empty:[...document.querySelectorAll('.empty')].filter(e=>!e.hidden).length,
      cards:document.querySelectorAll('.card:not(.hidden)').length}),420));`);
  check("无结果时给出空状态提示", nomatch.empty >= 1 && nomatch.cards === 0,
    `空状态=${nomatch.empty} 卡片=${nomatch.cards}`);

  const cleared = await b.eval(`
    document.querySelector('[data-clear="all"]').click();
    return new Promise(res=>setTimeout(()=>res({
      q:document.getElementById('q').value,
      cards:document.querySelectorAll('.card:not(.hidden)').length,
      marks:document.querySelectorAll('mark').length}),250));`);
  check("一键清除恢复全部结果", cleared.cards === 24 && cleared.q === "", JSON.stringify(cleared));
  check("清除后命中高亮一并撤下", cleared.marks === 0, String(cleared.marks));

  // ---------- 7 浅色主题 ----------
  section("[7] 浅色主题");
  const theme = await b.eval(`
    const before=document.documentElement.getAttribute('data-theme');
    document.getElementById('btn-theme').click();
    const cs=getComputedStyle(document.body);
    const v=n=>cs.getPropertyValue(n).trim();
    const probe=(sel,p)=>{const e=document.querySelector(sel);return e?getComputedStyle(e)[p]:null;};
    return {before,after:document.documentElement.getAttribute('data-theme'),
      colorScheme:getComputedStyle(document.documentElement).colorScheme,
      schemeStyle:document.documentElement.style.colorScheme,
      arc:probe('.xarc','stroke'),
      edge:probe('.diagram .edge','stroke'),
      todo:v('--todo'),hit:v('--hit'),shadow:v('--shadow'),
      laneA:probe('.lane-A','stroke'),
      dotFill:probe('.st-todo .dot','fill'),
      barFill:probe('.gantt .bar','fill')};`);
  const isLight = theme.after === "light";
  if (!isLight) { await b.eval(`document.getElementById('btn-theme').click();`); }
  const lt = await b.eval(`
    const cs=getComputedStyle(document.body);
    const v=n=>cs.getPropertyValue(n).trim();
    const probe=(sel,p)=>{const e=document.querySelector(sel);return e?getComputedStyle(e)[p]:null;};
    return {theme:document.documentElement.getAttribute('data-theme'),
      colorScheme:getComputedStyle(document.documentElement).colorScheme,
      arc:probe('.xarc','stroke'),edge:probe('.diagram .edge','stroke'),
      todo:v('--todo'),hit:v('--hit'),shadow:v('--shadow'),
      laneA:probe('.lane-A','stroke'),dotFill:probe('.st-todo .dot','fill')};`);
  check("主题切换按钮生效", lt.theme === "light", lt.theme);
  check("color-scheme 随之切换（滚动条/表单控件跟随）", lt.colorScheme === "light", lt.colorScheme);
  check("装饰色改为主题变量驱动（此前 #7c8899 / #5b6675 写死在浅色下仍用深色值）",
    lt.arc !== "rgb(124, 136, 153)" && lt.edge !== "rgb(91, 102, 117)", `${lt.arc} / ${lt.edge}`);
  check("空心站点在浅色底上可见（dot 填充不再等于页面背景）",
    lt.dotFill !== "rgb(246, 248, 250)", lt.dotFill);
  check("线路颜色在浅色主题下加深", lt.laneA !== "rgb(77, 163, 255)", lt.laneA);
  const contrast = await b.eval(`
    function lum(c){const m=c.match(/\\d+/g).map(Number).map(v=>{v/=255;return v<=0.03928?v/12.92:Math.pow((v+0.055)/1.055,2.4)});
      return 0.2126*m[0]+0.7152*m[1]+0.0722*m[2];}
    function ratio(a,b){const l1=lum(a),l2=lum(b);return (Math.max(l1,l2)+0.05)/(Math.min(l1,l2)+0.05);}
    const bg=getComputedStyle(document.body).backgroundColor;
    const t=getComputedStyle(document.querySelector('.count')).color;
    const fg3=getComputedStyle(document.querySelector('.maphint')).color;
    return {count:Math.round(ratio(t,bg)*100)/100,maphint:Math.round(ratio(fg3,bg)*100)/100,bg};`);
  check("浅色下次要文字对比度达标（≥3:1）",
    contrast.count >= 3 && contrast.maphint >= 3, JSON.stringify(contrast));

  await b.goto(PAGE);
  await sleep(500);
  const persistedTheme = await b.eval(`return document.documentElement.getAttribute('data-theme');`);
  check("主题偏好跨刷新保持", persistedTheme === "light", persistedTheme);

  // ---------- 8 键盘与可访问性 ----------
  section("[8] 键盘与可访问性");
  await b.eval(`document.getElementById('metro-panel').scrollIntoView({block:'center'});`);
  await sleep(800);
  const keys = await b.eval(`
    const svg=document.getElementById('metro-svg');
    const paused=()=>svg.animationsPaused?svg.animationsPaused():document.body.classList.contains('anim-off');
    const fire=k=>document.dispatchEvent(new KeyboardEvent('keydown',{key:k,bubbles:true}));
    const before=document.documentElement.getAttribute('data-theme');
    fire('t');
    const afterTheme=document.documentElement.getAttribute('data-theme');
    const animBefore=paused();
    fire('p');
    return {before,afterTheme,animBefore,animAfter:paused(),
      pressed:document.getElementById('btn-anim').getAttribute('aria-pressed'),
      hint:document.querySelector('.topbar .row.sub').classList.contains('on'),
      offscreen:document.body.classList.contains('anim-off')};`);
  check("t 键切换主题", keys.before !== keys.afterTheme, `${keys.before}→${keys.afterTheme}`);
  check("p 键切换动画", keys.animBefore !== keys.animAfter,
    `${keys.animBefore}→${keys.animAfter}（aria-pressed=${keys.pressed}, 视图内=${!keys.offscreen}）`);
  check("快捷键说明行默认展开", keys.hint === true);

  /* 快捷键行标示的是大写字母（P/T），所以必须验证大写形态真的生效：
     仅比对 e.key==='p' 时，Shift+P 与大小写锁定都会得到 'P' 而漏判。 */
  const upper = await b.eval(`
    const svg=document.getElementById('metro-svg');
    const paused=()=>svg.animationsPaused?svg.animationsPaused():document.body.classList.contains('anim-off');
    const fire=(k,shift)=>document.dispatchEvent(new KeyboardEvent('keydown',{key:k,shiftKey:!!shift,bubbles:true}));
    const themeBefore=document.documentElement.getAttribute('data-theme');
    const animBefore=paused();
    fire('T',true);
    const themeAfter=document.documentElement.getAttribute('data-theme');
    fire('P',true);
    const animAfter=paused();
    fire('p');                                   // 复位：小写仍须生效
    fire('t',true);
    return {themeBefore,themeAfter,animBefore,animAfter,restored:paused()};`);
  check("Shift+T（大写）切换主题", upper.themeBefore !== upper.themeAfter,
    `${upper.themeBefore}→${upper.themeAfter}`);
  check("Shift+P（大写）切换动画", upper.animBefore !== upper.animAfter,
    `${upper.animBefore}→${upper.animAfter}`);
  check("大小写两种形态均可切换动画", upper.animAfter !== upper.restored,
    `大写后=${upper.animAfter}，小写复位后=${upper.restored}`);

  const ks = await b.eval(`
    return {anim:document.getElementById('btn-anim').getAttribute('aria-keyshortcuts'),
      theme:document.getElementById('btn-theme').getAttribute('aria-keyshortcuts'),
      search:document.getElementById('q').getAttribute('aria-keyshortcuts'),
      hint:document.getElementById('qhint').innerHTML.replace(/\\s+/g,' ')};`);
  check("按钮声明 P/T 快捷键", ks.anim === "P" && ks.theme === "T", `${ks.anim}/${ks.theme}`);
  check("检索框声明 / 快捷键", ks.search === "/", String(ks.search));
  check("快捷键行以大写字母标示",
    ks.hint.includes("<kbd>P</kbd> 暂停动画") && ks.hint.includes("<kbd>T</kbd> 切换主题"),
    ks.hint.slice(0, 130));
  check("快捷键行不再残留小写 p/t 标示",
    !ks.hint.includes("<kbd>p</kbd>") && !ks.hint.includes("<kbd>t</kbd>"));
  const a11y = await b.eval(`
    return {cardFocusable:!!document.querySelector('.card[tabindex="0"]'),
      cardRole:document.querySelector('.card').getAttribute('role'),
      stAria:(document.querySelector('.st').getAttribute('aria-label')||'').length>8,
      liveRegions:document.querySelectorAll('[aria-live]').length,
      barFocusable:!!document.querySelector('.gantt .bar[tabindex="0"]'),
      progress:!!document.getElementById('prog'),
      totop:!!document.getElementById('totop')};`);
  check("卡片可聚焦且声明角色", a11y.cardFocusable && a11y.cardRole === "button");
  check("站点具备完整 aria 标签", a11y.stAria === true);
  check("时间轴条形可键盘聚焦", a11y.barFocusable === true);
  check("存在 aria-live 播报区", a11y.liveRegions >= 3, String(a11y.liveRegions));
  check("具备阅读进度与回到顶部", a11y.progress && a11y.totop);

  // ---------- 9 长表表头吸附 ----------
  section("[9] 长表在容器内滚动时表头保持可见");
  await b.eval(`document.querySelector('.tw.tall').scrollIntoView({block:'center'});`);
  await sleep(900);
  const sticky0 = await b.eval(`
    const boxes=[...document.querySelectorAll('.tw.tall')];
    return {boxes:boxes.length,
            rows:boxes.reduce((n,b)=>n+b.querySelectorAll('tbody tr').length,0),
            scrollable:document.querySelector('.tw.tall').scrollHeight
                       >document.querySelector('.tw.tall').clientHeight};`);
  check("三张故障用例表均启用容器内滚动，合计 41 行",
    sticky0.boxes === 3 && sticky0.rows === 41 && sticky0.scrollable, JSON.stringify(sticky0));
  const sticky1 = await b.eval(`
    const box=document.querySelector('.tw.tall');
    box.scrollTop=900;
    return new Promise(res=>requestAnimationFrame(()=>{
      const th=box.querySelector('thead th');
      const r=[...box.querySelectorAll('tbody tr')]
        .filter(x=>x.getBoundingClientRect().top>box.getBoundingClientRect().top);
      res({offset:Math.round(th.getBoundingClientRect().top-box.getBoundingClientRect().top),
           firstRow:r.length?r[0].cells[0].textContent.trim():null});}));`);
  check("容器内滚动后表头仍吸附在顶部（长表不丢列名）",
    Math.abs(sticky1.offset) < 4, `偏移 ${sticky1.offset}px，首行 ${sticky1.firstRow}`);

  // ---------- 10 页面加载后仍无异常 ----------
  section("[10] 全流程结束后运行时状态");
  check("整个交互流程未产生未捕获异常", errors.length === 0, errors.slice(0, 3).join(" | "));
} finally {
  await b.close();
}

console.log(`\n通过 ${pass} 项，失败 ${fails.length} 项`);
if (fails.length) { console.log("失败清单：" + fails.join("；")); process.exit(1); }
console.log("交互回归全部通过");
