/**
 * 常驻索引的状态 store（与 web/src/stores/dup.ts 等同一模式：模块级 ref）。
 *
 * 行为与 Rust 版旧 app.js（Phase 3/4，commit 2ecf6ff）一致：
 * indexStatusText / indexTaskText 的文案、同步后刷新状态、重建需二次确认、
 * 同步/重建后由页面重新扫描（页面监听 IndexBar 的 synced 事件）。
 * 两个页面（DupPage / BigPage）共用这一份状态。
 */
import { computed, ref } from "vue";
import { invoke } from "../api/tauri";
import type { IndexState, IndexTaskState } from "../api/types";
import { showConfirm } from "../composables/useModal";
import { toast } from "../composables/useToast";
import { fmtTime } from "../lib/format";

export const indexStatus = ref<IndexState | null>(null);
export const indexTask = ref<IndexTaskState | null>(null);
export const indexSyncing = ref(false);

const INDEX_STATE_NAMES: Record<string, string> = {
  fresh: "已同步",
  behind: "待同步",
  stale: "已过期",
  missing: "未建立",
  unavailable: "不可用",
};

/** 索引状态子行：新鲜度 + 文件数 + 最后同步时间。 */
export const indexStatusText = computed(() => {
  const st = indexStatus.value;
  if (!st || !st.volumes || !st.volumes.length) return "索引：未知";
  const total = st.volumes.reduce((sum, v) => sum + v.files, 0);
  const last = st.volumes.reduce((max, v) => Math.max(max, v.updated_at), 0);
  const lastText = last ? `，最后同步 ${fmtTime(last)}` : "";
  const bad = st.volumes.filter((v) => v.state !== "fresh");
  if (!bad.length) return `索引已同步（共 ${total.toLocaleString()} 个文件${lastText}）`;
  const parts = bad.map(
    (v) => `${v.drive.replace(/\\+$/, "")}${INDEX_STATE_NAMES[v.state] || v.state}`,
  );
  return `索引：${parts.join("、")}（共 ${total.toLocaleString()} 个文件${lastText}）`;
});

/** 计划任务状态子行。 */
export const indexTaskText = computed(() => {
  const t = indexTask.value;
  if (!t) return "计划任务：未知";
  if (!t.exists) return "计划任务：未注册";
  return t.enabled
    ? "计划任务：每日 03:00 自动同步（已启用）"
    : "计划任务：每日 03:00 自动同步（已禁用）";
});

export async function refreshIndexStatus(): Promise<void> {
  try {
    indexStatus.value = await invoke<IndexState>("index_status");
  } catch (_) {
    indexStatus.value = null;
  }
}

export async function refreshIndexTask(): Promise<void> {
  try {
    indexTask.value = await invoke<IndexTaskState>("index_task_status");
  } catch (_) {
    indexTask.value = null;
  }
}

/**
 * 立即同步（rebuild=false）或重建索引（rebuild=true）。
 * 返回 true 表示同步完成（页面应随即重新扫描）；false 表示未执行或失败。
 * 重建是耗时操作，先二次确认；同步期间 indexSyncing 为 true，按钮禁用。
 */
export async function syncIndexNow(rebuild: boolean): Promise<boolean> {
  if (indexSyncing.value) return false;
  if (
    rebuild &&
    !(await showConfirm(
      "确认重建索引",
      [
        "将清掉全部索引并全量重建，大盘可能需要几分钟。",
        "重建期间重复/大文件页会回落实时扫描。",
      ],
      "开始重建",
    ))
  ) {
    return false;
  }
  indexSyncing.value = true;
  try {
    const summary = await invoke<string>(
      rebuild ? "index_rebuild" : "index_sync_now",
    );
    toast(summary);
  } catch (error) {
    toast("索引同步失败：" + error);
    indexSyncing.value = false;
    return false;
  }
  indexSyncing.value = false;
  await refreshIndexStatus();
  return true;
}

/** 手动跑一次计划任务（后台执行，顺带验证 S4U 提权；跑完看索引状态与日志）。 */
export async function runIndexTask(): Promise<void> {
  try {
    toast(await invoke<string>("index_task_run"));
  } catch (error) {
    toast("运行计划任务失败：" + error);
  }
  await refreshIndexTask();
  await refreshIndexStatus();
}

export async function setIndexTaskEnabled(enabled: boolean): Promise<void> {
  try {
    toast(await invoke<string>("index_task_set_enabled", { enabled }));
  } catch (error) {
    toast("修改计划任务失败：" + error);
  }
  await refreshIndexTask();
}
