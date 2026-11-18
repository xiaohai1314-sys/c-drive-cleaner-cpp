/**
 * C 宿主（WebView2 原生 COM 桥）的前端封装，与 Tauri `invoke` 同签名。
 *
 * 协议（见 docs/bridge-commands.md）：
 *   JS -> C : chrome.webview.postMessage({ id, cmd, args })
 *   C  -> JS: window.__cdc_onMessage({ id, ok, data | error })
 *   C  -> JS: window.__cdc_onMessage({ event, data })（推送，无 id）
 *
 * 必须直接 post 对象，不能包 JSON.stringify：宿主读的是
 * WebMessageAsJson，字符串会被它再包一层引号（postMessage('x') 读到
 * 的是 '"x"'），宿主的信封解析要求以 '{' 开头，解析失败且无回包，
 * 所有调用会永远挂起。v0.1.0 就是这么坏的（见 tests/test_bridge.mjs）。
 *
 * 调用方（各页面 / store / composable）继续从 `api/tauri` import `invoke`，
 * 那里会优先走这里，Tauri 环境则降级到 `__TAURI__.core.invoke`。
 * 所以本文件不直接被页面引用，只被 `api/tauri.ts` 引用。
 */

interface ChromeWebview {
  // 直接 post 对象（见文件头注释）：包 JSON.stringify 会被 WebMessageAsJson
  // 再包一层引号，宿主解析不到信封。
  postMessage?: (message: unknown) => void;
}

interface BridgeWindow {
  chrome?: { webview?: ChromeWebview };
  __cdc_onMessage?: (message: unknown) => void;
}

interface PendingCall {
  resolve: (value: unknown) => void;
  reject: (error: Error) => void;
  timer: number | undefined;
  cmd: string;
}

export interface BridgeInvokeOptions {
  /** 超时毫秒数；0 表示不限。默认 5 分钟。 */
  timeoutMs?: number;
}

type ReplyMessage =
  | { id: number; ok: true; data: unknown }
  | { id: number; ok: false; error?: string }
  | { event: string; data?: unknown };

const DEFAULT_TIMEOUT_MS = 5 * 60 * 1000;

let defaultTimeoutMs = DEFAULT_TIMEOUT_MS;
let nextId = 1;
const pending = new Map<number, PendingCall>();
const eventListeners = new Map<string, Set<(data: unknown) => void>>();
let handlerInstalled = false;

function bridgeWindow(): BridgeWindow {
  return window as unknown as BridgeWindow;
}

function postMessageFn(): ((message: unknown) => void) | undefined {
  return bridgeWindow().chrome?.webview?.postMessage;
}

/** 当前是否跑在 C 宿主（WebView2）里。 */
export function hasBridge(): boolean {
  return Boolean(postMessageFn());
}

/** 改默认超时（毫秒；0 表示不限）。 */
export function setDefaultTimeout(ms: number): void {
  defaultTimeoutMs = ms;
}

function isReplyMessage(value: unknown): value is ReplyMessage {
  if (typeof value !== "object" || value === null) return false;
  const obj = value as Record<string, unknown>;
  if (typeof obj.event === "string") return true;
  return typeof obj.id === "number" && typeof obj.ok === "boolean";
}

function handleMessage(raw: unknown): void {
  const message: unknown =
    typeof raw === "string" ? safeJsonParse(raw) : raw;
  if (!isReplyMessage(message)) return;

  if ("event" in message) {
    const listeners = eventListeners.get(message.event);
    if (listeners) {
      for (const cb of [...listeners]) {
        try {
          cb(message.data);
        } catch (_) {
          // 某个监听回调抛错不影响其他监听者。
        }
      }
    }
    return;
  }

  const call = pending.get(message.id);
  if (!call) {
    // 超时后才到的迟到回复：无待处理项，直接丢弃。
    return;
  }
  pending.delete(message.id);
  if (call.timer !== undefined) window.clearTimeout(call.timer);
  if (message.ok) {
    call.resolve(message.data);
  } else {
    call.reject(new Error(message.error || `命令 ${call.cmd} 失败`));
  }
}

function safeJsonParse(text: string): unknown {
  try {
    return JSON.parse(text);
  } catch (_) {
    return null;
  }
}

function ensureHandler(): void {
  if (handlerInstalled) return;
  handlerInstalled = true;
  bridgeWindow().__cdc_onMessage = handleMessage;
}

/**
 * 与 Tauri `invoke(cmd, args)` 同签名。命令名、参数名、返回值形态
 * 与 Rust 版一致（见 docs/bridge-commands.md）。
 *
 * 注意：长命令（scan_dups / scan_big / index_rebuild 等）可能跑几分钟，
 * 默认 5 分钟超时不够时调用方传更大的 timeoutMs（0 表示不限）。
 */
export async function invoke<T>(
  cmd: string,
  args?: Record<string, unknown>,
  opts?: BridgeInvokeOptions,
): Promise<T> {
  ensureHandler();
  const post = postMessageFn();
  if (!post) throw new Error("当前不在 WebView2 环境中");

  const id = nextId++;
  const timeoutMs = opts?.timeoutMs ?? defaultTimeoutMs;
  return new Promise<T>((resolve, reject) => {
    const timer =
      timeoutMs > 0
        ? window.setTimeout(() => {
            pending.delete(id);
            reject(new Error(`命令 ${cmd} 超时（${timeoutMs}ms 未返回）`));
          }, timeoutMs)
        : undefined;
    pending.set(id, {
      resolve: resolve as (value: unknown) => void,
      reject,
      timer,
      cmd,
    });
    try {
      // 直接 post 对象：WebMessageAsJson 原样返回对象 JSON；发字符串会被
      // 再包一层引号，宿主解析失败且无回包（v0.1.0 的 bug）。
      post({ id, cmd, args: args ?? {} });
    } catch (error) {
      pending.delete(id);
      if (timer !== undefined) window.clearTimeout(timer);
      reject(error instanceof Error ? error : new Error(String(error)));
    }
  });
}

/**
 * 订阅 C 侧的事件推送（`cdc_bridge_push_event`）。返回取消订阅函数。
 * 目前后端主要用轮询（scan_status / task_status）报进度，事件通道
 * 留给后续的流式进度 / 增量结果。
 */
export function onEvent(
  name: string,
  callback: (data: unknown) => void,
): () => void {
  ensureHandler();
  let set = eventListeners.get(name);
  if (!set) {
    set = new Set();
    eventListeners.set(name, set);
  }
  set.add(callback);
  return () => {
    set.delete(callback);
    if (!set.size) eventListeners.delete(name);
  };
}
