# 启动优化（优化项 5）

## 结论先行

- **WebView2 预热：做。**`cdc_host_prewarm()` 在后端初始化阶段尽早启动
  `CreateCoreWebView2EnvironmentWithOptions`（异步），让浏览器进程拉起与
  命令注册、索引状态读取等工作重叠；`cdc_host_run()` 在建窗口前启动环境
  创建（已预热过时是空操作），环境就绪后才建控制器。
- **Vue 页面懒加载：做。**`web/src/App.vue` 里 9 个页面由静态 import 改为
  `defineAsyncComponent(() => import(...))`，首屏只加载当前页（默认 clean）。
- **资源嵌入 exe：不做。**见下。

## 预热设计（host/host.cpp）

```
main.c（GUI 入口）
  ├─ cdc_host_prewarm()          // 尽早：启动环境创建（异步），立即返回
  ├─ cdc_gui_register_commands() // 后端初始化 + 注册 45 个命令（与拉起重叠）
  └─ cdc_host_run()              // 建窗口；环境就绪后建控制器；跑消息循环
```

- 环境创建是异步的：`CreateCoreWebView2EnvironmentWithOptions` 只提交请求，
  真正的 `EnvironmentHandler::Invoke` 回调在消息循环跑起来后才触发。
- 回调到达时若窗口还没建好（预热阶段），控制器创建推迟到窗口建好后：
  `g_env_ready` 标志；`cdc_host_run()` 建窗后调共用的 `create_controller()`，
  回调里同样调它——两处共用一份逻辑，不会重复创建（`g_controller` 非空时
  直接返回）。
- `cdc_host_run()` 在建窗口**前**启动环境创建；已预热过时
  `start_environment()` 是空操作（`g_env` 非空或 `g_env_creating` 为真时
  直接返回），不会重复提交。
- COM 初始化：预热入口与 `cdc_host_run()` 内部都确保 STA 初始化
  （`CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)`）。
  `S_FALSE` 表示调用方已初始化过——宿主不接管、不 `CoUninitialize`；
  `RPC_E_CHANGED_MODE`（调用方已是 MTA）返回失败；
  只有本次由宿主初始化（`S_OK`）时，宿主才在消息循环结束后
  `CoUninitialize`（`g_com_init_by_us` 标志）。
- **不做隐藏 WebView 预热渲染进程**：控制器建好后立刻 `Navigate`，
  提前建一个隐藏 WebView 的收益约等于 0，还多占一个渲染进程。

## 页面懒加载（web/src/App.vue）

- 9 个页面（Clean/System/Frag/Privacy/Big/Dup/Wechat/Qq/Shred）改为
  `defineAsyncComponent`；AppSidebar、WindowBar、TaskDock、ToastHost、
  ModalHost、HistoryModal 等常驻组件保持静态 import。
- 实测（`npm run build`）：首屏 JS 由 120.11 kB（gzip 44.92）降为
  main chunk 86.47 kB + CleanPage 4.26 kB = 90.73 kB（gzip 36.59），
  **−29.38 kB ≈ 24%**；其余 8 个页面 chunk 合计约 25 kB，在切页时加载。
- 对照组（App.vue 改回静态 import）：69 个模块、单 JS 120.11 kB——
  确认收益来自懒加载本身。

## 资源嵌入评估：不做嵌入

把 `web/dist/` 嵌进 exe（生成 C 数组 / Windows 资源）**不做**，理由：

- dist 很小：约 **141 kB、15 个文件**；首屏实际请求只有 4 个
  （index.html、CSS、主 JS、CleanPage chunk）。
- 磁盘读是启动里最小的一环：4 个小文件，热缓存 <1ms、SSD 冷启动
  几毫秒；WebView2 环境创建是几百毫秒级。嵌进 exe 省掉的只是几个
  `CreateFile` 调用（每个几十微秒），**合计不到 0.1ms**，相对启动
  时间可以忽略。
- 代价：exe 增大约 8%（+~141 kB）；要加代码生成步骤（构建时把 dist
  转成 C 数组或 .rc）；丢掉“改前端只换 dist/”的迭代方式——现在
  前端改完重新 `npm run build` 即可，不用重新链接 exe。
- 结论：收益可忽略，代价真实。保持“exe 旁边的 dist/ 目录”加载方式。

## 未验证

- 没有真 Windows 环境：预热与懒加载只验证到交叉编译通过（见下）与
  前端构建产物对比；**真实机器上的启动时间未测量**。
- MSVC 构建、真实 WebView2 SDK 头文件未验证（交叉编译用最小桩头）。
- 启动时间的基线与改进量，需要在用户机器上实测
  （建议：冷启动计时，预热前后各测 3 次取中位）。
