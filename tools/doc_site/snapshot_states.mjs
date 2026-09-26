// 截取关键界面状态，供人工核对视觉与主题。
// 用法：node tools/doc_site/snapshot_states.mjs [输出目录]
import { Browser, sleep, fileUrl } from "./cdp.mjs";
import { join } from "node:path";
import { mkdirSync } from "node:fs";

const ROOT = new URL("../../", import.meta.url).pathname.replace(/^\//, "");
const PAGE = fileUrl(join(ROOT, "docs", "HengYuan_v2_全自动量化实盘系统实施指南与TODOLIST.html"));
const OUT = process.argv[2] || join(process.env.TEMP || ".", "hy_shots");
mkdirSync(OUT, { recursive: true });

const b = await Browser.launch({ width: 1600, height: 1000 });
async function settle() {
  await sleep(150);
  await b.eval(`return new Promise(r=>{let last=-1,n=0;(function p(){
    const y=Math.round(window.scrollY); n=(y===last)?n+1:0; last=y;
    if(n>=3)return r(1); requestAnimationFrame(p);})();});`);
}
async function shot(name) { await b.screenshot(join(OUT, name)); console.log("  " + name); }

try {
  await b.goto(PAGE);
  await sleep(600);

  for (const theme of ["dark", "light"]) {
    await b.eval(`localStorage.setItem('hy-theme','${theme}');
      document.documentElement.setAttribute('data-theme','${theme}');
      document.documentElement.style.colorScheme='${theme}';`);
    await sleep(300);

    await b.eval(`document.getElementById('metro-panel').scrollIntoView({block:'start'});`);
    await settle();
    await b.eval(`window.scrollBy(0,-90);`);
    await settle();
    await shot(`01-metro-${theme}.png`);

    // 悬浮摘要
    const p = await b.eval(`const g=document.querySelector('.st[data-station="B2"]');
      const r=g.getBoundingClientRect();return {x:Math.round(r.left+r.width/2),y:Math.round(r.top+r.height/2)};`);
    await b.mouse("mouseMoved", p.x, p.y);
    await sleep(500);
    await shot(`02-tip-${theme}.png`);

    // 检索命中高亮与筛选联动
    await b.eval(`document.getElementById('q').value='限流';
      document.getElementById('q').dispatchEvent(new Event('input',{bubbles:true}));`);
    await sleep(700);
    await shot(`03-search-${theme}.png`);
    await b.eval(`document.querySelector('[data-clear="all"]').click();`);
    await sleep(400);

    // 站点卡片区
    await b.eval(`document.getElementById('card-B2').scrollIntoView({block:'start'});`);
    await settle();
    await b.eval(`window.scrollBy(0,-110);`);
    await settle();
    await shot(`04-cards-${theme}.png`);

    // 故障注册表（长表吸顶表头）
    await b.eval(`document.querySelector('.fi-table').scrollIntoView({block:'start'});`);
    await settle();
    await shot(`05-fi-${theme}.png`);
  }

  // 顶部与顶栏
  await b.eval(`document.documentElement.setAttribute('data-theme','light');
    document.documentElement.style.colorScheme='light';window.scrollTo(0,0);`);
  await settle();
  await shot("06-top-light.png");
} finally {
  await b.close();
}
console.log("输出目录：" + OUT);
