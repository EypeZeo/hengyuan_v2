// 极简 Chrome DevTools Protocol 客户端：零依赖，用 Node 内置 WebSocket 驱动无头 Chrome。
// 仅用于本目录的交互回归测试，不参与产物生成。
import { spawn } from "node:child_process";
import { existsSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const CANDIDATES = [
  "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
  "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
  "C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
];

export function findBrowser() {
  for (const p of CANDIDATES) if (existsSync(p)) return p;
  throw new Error("未找到 Chrome/Edge 可执行文件");
}

export class Browser {
  constructor(child, profile, wsUrl) {
    this.child = child;
    this.profile = profile;
    this.wsUrl = wsUrl;
    this.id = 0;
    this.pending = new Map();
    this.sessionId = null;
  }

  static async launch({ width = 1440, height = 960 } = {}) {
    const exe = findBrowser();
    const profile = mkdtempSync(join(tmpdir(), "hy-cdp-"));
    const args = [
      "--headless=new",
      "--remote-debugging-port=0",
      `--user-data-dir=${profile}`,
      "--no-first-run",
      "--no-default-browser-check",
      "--disable-extensions",
      "--disable-gpu",
      "--hide-scrollbars",
      "--allow-file-access-from-files",
      `--window-size=${width},${height}`,
      "about:blank",
    ];
    const child = spawn(exe, args, { stdio: ["ignore", "ignore", "pipe"] });
    const wsUrl = await new Promise((resolve, reject) => {
      let buf = "";
      const t = setTimeout(() => reject(new Error("等待 DevTools 端口超时")), 30000);
      child.stderr.on("data", (d) => {
        buf += d.toString();
        const m = buf.match(/ws:\/\/[^\s]+/);
        if (m) { clearTimeout(t); resolve(m[0]); }
      });
      child.on("exit", (c) => { clearTimeout(t); reject(new Error("浏览器提前退出 " + c)); });
    });
    const b = new Browser(child, profile, wsUrl);
    await b.#connect();
    return b;
  }

  #connect() {
    return new Promise((resolve, reject) => {
      const ws = new WebSocket(this.wsUrl);
      this.ws = ws;
      ws.onmessage = (ev) => {
        const msg = JSON.parse(ev.data);
        if (msg.id && this.pending.has(msg.id)) {
          const { resolve: res, reject: rej } = this.pending.get(msg.id);
          this.pending.delete(msg.id);
          msg.error ? rej(new Error(JSON.stringify(msg.error))) : res(msg.result);
        } else if (msg.method === "Page.loadEventFired") {
          this._loaded?.();
        }
      };
      ws.onerror = (e) => reject(new Error("WS 错误 " + e.message));
      ws.onopen = async () => {
        const { targetInfos } = await this.send("Target.getTargets");
        const page = targetInfos.find((t) => t.type === "page");
        const r = await this.send("Target.attachToTarget", { targetId: page.targetId, flatten: true });
        this.sessionId = r.sessionId;
        await this.send("Page.enable", {}, this.sessionId);
        await this.send("Runtime.enable", {}, this.sessionId);
        resolve();
      };
    });
  }

  send(method, params = {}, sessionId = null) {
    const id = ++this.id;
    const payload = { id, method, params };
    if (sessionId) payload.sessionId = sessionId;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      this.ws.send(JSON.stringify(payload));
    });
  }

  /** 导航并等待 load 事件落定。 */
  async goto(url) {
    const loaded = new Promise((res) => { this._loaded = res; });
    await this.send("Page.navigate", { url }, this.sessionId);
    await Promise.race([loaded, new Promise((r) => setTimeout(r, 15000))]);
    this._loaded = null;
  }

  /** 在页面里求值，返回 JSON 可序列化结果。`expression` 只能是调用方写死的代码文本。 */
  async eval(expression) {
    const r = await this.send("Runtime.evaluate", {
      expression: `(() => { ${expression} })()`,
      returnByValue: true,
      awaitPromise: true,
    }, this.sessionId);
    return this.#unwrap(r);
  }

  /**
   * 同 eval，但数据不拼进代码：`args`（JSON 可序列化）作为参数交给页面里的函数，函数体 `body`
   * 里以 `args` 引用。凡是要把页面或数据里读出来的值带进求值，都用这个，不要做字符串拼接。
   */
  async evalWith(args, body) {
    const g = await this.send("Runtime.evaluate", { expression: "globalThis", returnByValue: false }, this.sessionId);
    const r = await this.send("Runtime.callFunctionOn", {
      objectId: g.result.objectId,
      functionDeclaration: `function (args) { ${body} }`,
      arguments: [{ value: args }],
      returnByValue: true,
      awaitPromise: true,
    }, this.sessionId);
    return this.#unwrap(r);
  }

  #unwrap(r) {
    if (r.exceptionDetails) {
      throw new Error("页面求值异常：" + (r.exceptionDetails.exception?.description || r.exceptionDetails.text));
    }
    return r.result.value;
  }

  /** 派发真实鼠标事件（CDP 层面，会触发 hover/click 全链路）。 */
  async mouse(type, x, y, button = "none") {
    await this.send("Input.dispatchMouseEvent", {
      type, x, y, button, clickCount: type === "mousePressed" || type === "mouseReleased" ? 1 : 0,
      buttons: type === "mousePressed" ? 1 : 0,
    }, this.sessionId);
  }

  async clickAt(x, y) {
    await this.mouse("mouseMoved", x, y);
    await sleep(30);
    await this.mouse("mousePressed", x, y, "left");
    await this.mouse("mouseReleased", x, y, "left");
  }

  async screenshot(path, { fullPage = false } = {}) {
    const r = await this.send("Page.captureScreenshot",
      { format: "png", captureBeyondViewport: fullPage }, this.sessionId);
    writeFileSync(path, Buffer.from(r.data, "base64"));
  }

  async close() {
    try { this.ws.close(); } catch {}
    try { this.child.kill(); } catch {}
    await sleep(300);
    try { rmSync(this.profile, { recursive: true, force: true }); } catch {}
  }
}

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export function fileUrl(p) {
  return "file:///" + p.replace(/\\/g, "/").replace(/^\/+/, "");
}
