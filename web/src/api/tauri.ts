/** 对后端的一层薄封装：优先走 C 宿主的 WebView2 桥，降级到 Tauri。
 *
 * 两个后端提供完全相同的 invoke(cmd, args) 签名（命令表见
 * docs/bridge-commands.md），所以各页面 / store / composable 不用改
 * 业务代码，只从这里 import invoke。
 *
 * 降级顺序：chrome.webview（C 宿主）→ __TAURI__.core.invoke（Tauri 版，
 * 保留用于对照测试）→ 抛错（浏览器预览）。
 */

import { hasBridge, invoke as bridgeInvoke, type BridgeInvokeOptions } from "./bridge";

interface TauriCore {
  invoke?: (cmd: string, args?: Record<string, unknown>) => Promise<unknown>;
  convertFileSrc?: (path: string, protocol?: string) => string;
}

interface TauriWindowHandle {
  minimize(): void;
  toggleMaximize(): void;
  close(): void;
}

interface TauriGlobal {
  core?: TauriCore;
  window?: { getCurrentWindow?: () => TauriWindowHandle };
  dialog?: { open?: (options: Record<string, unknown>) => Promise<unknown> };
}

function globalTauri(): TauriGlobal | undefined {
  return (window as unknown as { __TAURI__?: TauriGlobal }).__TAURI__;
}

export function hasTauri(): boolean {
  return Boolean(globalTauri()?.core?.invoke);
}

export function hasBackend(): boolean {
  return hasBridge() || hasTauri();
}

export async function invoke<T>(
  cmd: string,
  args?: Record<string, unknown>,
  opts?: BridgeInvokeOptions,
): Promise<T> {
  // 长命令的 timeoutMs 只对桥模式生效（Tauri invoke 本身不超时）。
  if (hasBridge()) return bridgeInvoke<T>(cmd, args, opts);
  const fn = globalTauri()?.core?.invoke;
  if (!fn) throw new Error("当前不在 Tauri 或 WebView2 环境中");
  return (await fn(cmd, args)) as T;
}

export function convertFileSrc(path: string, protocol = "asset"): string {
  const fn = globalTauri()?.core?.convertFileSrc;
  if (fn) return fn(path, protocol);
  // 桥模式：imthumb 自定义协议尚未接入宿主的文件服务（见
  // docs/frontend-bridge.md「已知缺口」），先返回 ""（缩略图暂不显示）。
  void path;
  return "";
}

function bridgeWindow(): TauriWindowHandle {
  return {
    minimize: () => void bridgeInvoke("window_minimize"),
    toggleMaximize: () => void bridgeInvoke("window_maximize"),
    close: () => void bridgeInvoke("window_close"),
  };
}

export function currentWindow(): TauriWindowHandle | undefined {
  if (hasBridge()) return bridgeWindow();
  return globalTauri()?.window?.getCurrentWindow?.();
}

export async function pickPaths(kind: "file" | "folder"): Promise<string[]> {
  // 桥模式：原生文件对话框尚未接入，暂返回 []（粉碎页选文件入口不可用），
  // 见 docs/frontend-bridge.md「已知缺口」。
  if (hasBridge()) return [];
  const open = globalTauri()?.dialog?.open;
  if (!open) return [];
  const picked = await open(
    kind === "file"
      ? { multiple: true, title: "选择要粉碎的文件" }
      : { directory: true, multiple: true, title: "选择要粉碎的文件夹" },
  );
  if (!picked) return [];
  return Array.isArray(picked) ? (picked as string[]) : [picked as string];
}
