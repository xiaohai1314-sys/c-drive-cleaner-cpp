<!-- 索引状态 + 计划任务：重复文件页与大文件页共用的两行。
  第一行：索引状态子行（新鲜度、文件数、最后同步时间）+ 立即同步 / 重建索引。
  第二行：计划任务状态 + 运行计划任务 + 启用/禁用。
  同步完成后发 synced 事件，页面随即重新扫描（与 Rust 版旧 app.js 一致）。
  busy 由页面传入（扫描进行中时禁用同步按钮）。 -->
<script setup lang="ts">
import {
  indexStatusText,
  indexSyncing,
  indexTask,
  indexTaskText,
  runIndexTask,
  setIndexTaskEnabled,
  syncIndexNow,
} from "../stores/index";

defineProps<{ busy?: boolean }>();

const emit = defineEmits<{ synced: [] }>();

async function sync(rebuild: boolean) {
  if (await syncIndexNow(rebuild)) emit("synced");
}
</script>

<template>
  <div class="smart-tools">
    <span class="muted">{{ indexStatusText }}</span>
    <button class="smart-btn" :disabled="indexSyncing || busy" @click="sync(false)">
      {{ indexSyncing ? "同步中…" : "立即同步" }}
    </button>
    <button class="smart-btn" :disabled="indexSyncing || busy" @click="sync(true)">重建索引</button>
  </div>
  <div class="smart-tools">
    <span class="muted">{{ indexTaskText }}</span>
    <button
      class="smart-btn"
      title="立即在后台跑一次计划任务（--index），顺带验证计划任务的提权是否生效"
      :disabled="!indexTask?.exists"
      @click="runIndexTask"
    >
      运行计划任务
    </button>
    <button class="smart-btn" :disabled="!indexTask?.exists" @click="setIndexTaskEnabled(!indexTask?.enabled)">
      {{ indexTask?.enabled ? "禁用" : "启用" }}
    </button>
  </div>
</template>
