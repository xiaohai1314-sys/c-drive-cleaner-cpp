<script setup lang="ts">
import {
  computed,
  defineAsyncComponent,
  onMounted,
  watchEffect,
  type Component,
} from "vue";
import AppSidebar from "./components/AppSidebar.vue";
import WindowBar from "./components/WindowBar.vue";
import TaskDock from "./components/TaskDock.vue";
import ToastHost from "./components/ToastHost.vue";
import ModalHost from "./components/ModalHost.vue";
import HistoryModal from "./components/HistoryModal.vue";
// 9 个页面按需加载：首屏只加载当前页（默认 clean），切页时再加载对应
// chunk。AppSidebar/WindowBar 等常驻组件保持静态 import。
const CleanPage = defineAsyncComponent(() => import("./pages/CleanPage.vue"));
const SystemPage = defineAsyncComponent(
  () => import("./pages/SystemPage.vue"),
);
const FragPage = defineAsyncComponent(() => import("./pages/FragPage.vue"));
const PrivacyPage = defineAsyncComponent(
  () => import("./pages/PrivacyPage.vue"),
);
const BigPage = defineAsyncComponent(() => import("./pages/BigPage.vue"));
const DupPage = defineAsyncComponent(() => import("./pages/DupPage.vue"));
const WechatPage = defineAsyncComponent(
  () => import("./pages/WechatPage.vue"),
);
const QqPage = defineAsyncComponent(() => import("./pages/QqPage.vue"));
const ShredPage = defineAsyncComponent(
  () => import("./pages/ShredPage.vue"),
);
import { currentPage, loadDisk, loadDrives, setNavSize, type PageKey } from "./stores/app";
import { bigRows } from "./stores/big";
import { categories, selected } from "./stores/clean";
import { dupGroups } from "./stores/dup";
import { fmtSize } from "./lib/format";
import { startTaskPolling } from "./stores/tasks";

const pages: Record<PageKey, Component> = {
  clean: CleanPage,
  system: SystemPage,
  frag: FragPage,
  privacy: PrivacyPage,
  big: BigPage,
  dup: DupPage,
  wechat: WechatPage,
  qq: QqPage,
  shred: ShredPage,
};

const pageComponent = computed(() => pages[currentPage.value]);

watchEffect(() => {
  const cleanTotal = categories.value
    .filter((cat) => selected.has(cat.key))
    .reduce((sum, cat) => sum + Math.max(cat.size, 0), 0);
  setNavSize("clean", cleanTotal > 0 ? `[${fmtSize(cleanTotal)}]` : "");

  const bigTotal = bigRows.value.reduce((sum, row) => sum + row.size, 0);
  setNavSize("big", bigTotal > 0 ? `[${fmtSize(bigTotal)}]` : "");

  const dupTotal = dupGroups.value.reduce(
    (sum, group) => sum + group.size * Math.max(group.files.length - 1, 0),
    0,
  );
  setNavSize("dup", dupTotal > 0 ? `[${fmtSize(dupTotal)}]` : "");
});

onMounted(() => {
  void loadDisk();
  void loadDrives();
  startTaskPolling();
});
</script>

<template>
  <div class="app">
    <AppSidebar />
    <main class="main">
      <WindowBar />
      <section id="page" class="page">
        <component :is="pageComponent" :key="currentPage" />
      </section>
    </main>
    <TaskDock />
    <ToastHost />
    <ModalHost />
    <HistoryModal />
  </div>
</template>
