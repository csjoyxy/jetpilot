# JetWeb (`webhub/`)

面向 iOS 15.0+（主目标 iPhone 11 / iOS 16.4.1，TrollStore 安装）的**多合一常驻 WebApp 容器**，解决 Safari「添加到主屏幕」WebApp 切后台极易被 Jetsam 回收导致白屏重刷的问题。

## 核心特性

1. **多站点单容器常驻**：支持在本机自由添加/编辑/排序最多 8 个常用 WebApp（配置仅保存在 `/var/mobile/Library/JetWeb/config.json`，不进入代码仓库），标签间 **0ms 秒切不白屏**。
2. **外壳 + `WebContent` 成对提档保活**：内置瞬时 root 助手 `jetwebctl`（毫秒级执行即退，零常驻后台耗电），通过 `WKWebView` 获取当前各标签精确的 `com.apple.WebKit.WebContent` 进程 PID，与主进程一并抬升至目标保护档位（默认 `85`，上限 `95`，绝不触碰前台 `100` 档）。
3. **4GB 内存安全阀**：单标签渲染进程物理内存超过设定上限（默认 `450 MB`）时自动停止保活并退回 `30` 档，避免异常网页撑爆整机内存。
4. **全屏沉浸模式**：支持一键收起底部切换栏为可拖动迷你悬浮胶囊（点击展开、长按快捷切站），兼顾 100% 全屏视野与快速切换。
5. **与 JetPilot 联动**：安装后也可在 JetPilot 主页将 `JetWeb`（`com.lili.jetweb`）加入保护规则，同时同步写出 `/var/mobile/Library/JetPilot/webhub_pids.txt` 供后续版本守护进程联动巡检。
