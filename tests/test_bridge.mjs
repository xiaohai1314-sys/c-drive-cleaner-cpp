// tests/test_bridge.mjs — 前端桥接层（web/src/api/bridge.ts）的协议测试。
//
// 不跑浏览器：用 tsc（vue-tsc 的依赖，npm ci 后在 node_modules 里）把
// 真实的 bridge.ts 转译成 ESM，在 Node 里加载；用 mock 的
// window.chrome.webview.postMessage 扮演 C 宿主。
//
// 覆盖：
//   1. 基本往返：invoke 返回 data；postMessage 的 payload 是 {id, cmd, args}。
//   2. id 匹配：两个并发 invoke，乱序回复，各自拿到自己的值。
//   3. 错误回传：{ok:false, error} → invoke reject，错误信息原样透出。
//   4. 超时：宿主不回包 → 按 timeoutMs reject；超时后才到的迟到回复被丢弃。
//   5. 事件推送：{event, data} → onEvent 回调收到；取消订阅后不再收到。
//   6. 非 bridge 环境：没有 chrome.webview 时 invoke 抛错。
//   7. 回归 v0.1.0：post 的必须是对象。mock 按真实 WebView2 的
//      WebMessageAsJson 语义（JSON.stringify( posted 值 )）喂给宿主；
//      前端若包 JSON.stringify 发字符串，宿主拿到带引号的字面量，
//      信封解析失败且无回包 —— 真机上全部调用永远挂起。
//
// 运行：node tests/test_bridge.mjs（需先在 web/ 里跑过 npm ci）。
// 退出码 0 = 全部通过。

import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, "..");
const BRIDGE_TS = path.join(REPO, "web", "src", "api", "bridge.ts");
const TSC = path.join(REPO, "web", "node_modules", "typescript", "bin", "tsc");

let passed = 0;
function ok(name) {
  passed += 1;
  console.log(`ok - ${name}`);
}

// --- 转译真实的 bridge.ts -------------------------------------------------
const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), "cdc-bridge-test-"));
const bundle = path.join(tmpDir, "bridge.mjs");
execFileSync(
  TSC,
  [
    BRIDGE_TS,
    "--target",
    "es2022",
    "--module",
    "nodenext",
    "--moduleResolution",
    "nodenext",
    "--outDir",
    tmpDir,
  ],
  { stdio: "pipe" },
);
fs.renameSync(path.join(tmpDir, "bridge.js"), bundle);

// --- mock window.chrome.webview（扮演 C 宿主）------------------------------
const sent = []; // 宿主收到的 postMessage（已解析）
const posted = []; // 前端 post 的原始值（回归项 7：必须是对象，不能是字符串）
let hostReply = null; // 测试设置的回复策略：(msg) => void
const host = {
  postMessage(value) {
    posted.push(value);
    // 模拟真实 WebView2：宿主读到的 WebMessageAsJson =
    // JSON.stringify( posted 值 )。前端若发字符串，宿主拿到的是带引号的
    // JSON 字符串字面量，信封解析（要求以 '{' 开头）失败且无回包 ——
    // v0.1.0 在真机上就是这么坏的；旧 mock 用 JSON.parse(text) 直接
    // 还原了字符串，把 bug 藏住了。
    const asJson = JSON.stringify(value);
    if (!asJson.startsWith("{")) {
      throw new Error("宿主收到的不是对象 JSON：前端必须直接 post 对象");
    }
    const msg = JSON.parse(asJson);
    sent.push(msg);
    if (hostReply) hostReply(msg);
  },
};

globalThis.window = {
  setTimeout: (...args) => setTimeout(...args),
  clearTimeout: (...args) => clearTimeout(...args),
  chrome: { webview: host },
};

const bridge = await import(pathToFileURL(bundle).href);
const { invoke, onEvent, hasBridge, setDefaultTimeout } = bridge;

function reply(id, data) {
  globalThis.window.__cdc_onMessage({ id, ok: true, data });
}
function replyError(id, error) {
  globalThis.window.__cdc_onMessage({ id, ok: false, error });
}

// --- 1. 基本往返 + payload 形态 -------------------------------------------
assert.equal(hasBridge(), true);
hostReply = (msg) => reply(msg.id, { volumes: [] });
{
  const data = await invoke("index_status");
  assert.deepEqual(data, { volumes: [] });
  assert.equal(sent.length, 1);
  assert.equal(typeof sent[0].id, "number");
  assert.equal(sent[0].cmd, "index_status");
  assert.deepEqual(sent[0].args, {});
  // 回归 v0.1.0：post 的必须是对象。发字符串时 WebMessageAsJson 会再包
  // 一层引号，宿主解析不到信封，所有调用永远挂起。
  assert.equal(typeof posted[0], "object");
  assert.notEqual(posted[0], null);
  ok("基本往返：invoke 返回 data，payload 为 {id, cmd, args}");
}

// --- 2. id 匹配：并发两个，乱序回复 ----------------------------------------
// 两个 post 是同步发生的；等第二个到了，先回后到的（id 3），再回先到的（id 2）。
hostReply = (msg) => {
  if (sent.length < 2) return;
  reply(msg.id, `value-${msg.id}`);
  reply(sent[0].id, `value-${sent[0].id}`);
};
{
  sent.length = 0;
  const [a, b] = await Promise.all([invoke("scan_big"), invoke("index_status")]);
  assert.equal(a, "value-2");
  assert.equal(b, "value-3");
  ok("id 匹配：并发 invoke 乱序回复，各自拿到自己的值");
}

// --- 3. 错误回传 ------------------------------------------------------------
hostReply = (msg) => replyError(msg.id, "需要管理员权限。");
{
  await assert.rejects(invoke("index_sync_now"), /需要管理员权限。/);
  ok("错误回传：{ok:false, error} → invoke reject，错误信息原样透出");
}

// --- 4. 超时 + 迟到回复丢弃 --------------------------------------------------
hostReply = null; // 宿主不回包。
setDefaultTimeout(50);
{
  await assert.rejects(invoke("scan_dups", { fast: true }), /超时/);
  // 超时后才到的回复：不应抛错、不应影响后续调用。
  reply(sent[sent.length - 1].id, { late: true });
  hostReply = (msg) => reply(msg.id, "after-timeout");
  const data = await invoke("index_status");
  assert.equal(data, "after-timeout");
  ok("超时：不回包按 timeoutMs reject；迟到回复被丢弃，后续调用正常");
}
setDefaultTimeout(5 * 60 * 1000);

// --- 5. 事件推送 --------------------------------------------------------------
hostReply = (msg) => reply(msg.id, null);
{
  const seen = [];
  const off = onEvent("scan_progress", (data) => seen.push(data));
  globalThis.window.__cdc_onMessage({
    event: "scan_progress",
    data: { done: 3, total: 10 },
  });
  globalThis.window.__cdc_onMessage({ event: "other", data: 1 });
  assert.deepEqual(seen, [{ done: 3, total: 10 }]);
  off();
  globalThis.window.__cdc_onMessage({
    event: "scan_progress",
    data: { done: 9, total: 10 },
  });
  assert.deepEqual(seen, [{ done: 3, total: 10 }]);
  ok("事件推送：onEvent 收到 {event, data}；取消订阅后不再收到");
}

// --- 6. 非 bridge 环境 ---------------------------------------------------------
{
  delete globalThis.window.chrome;
  assert.equal(hasBridge(), false);
  await assert.rejects(invoke("index_status"), /不在 WebView2 环境中/);
  ok("非 bridge 环境：没有 chrome.webview 时 invoke 抛错");
}

fs.rmSync(tmpDir, { recursive: true, force: true });
console.log(`\nbridge: ${passed}/6 通过`);
