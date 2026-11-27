# JetPilot

![Release](https://img.shields.io/github/v/release/csjoyxy/jetpilot)
![Platform](https://img.shields.io/badge/Platform-iOS%2016.4.1%20(TrollStore)-lightgrey)

原名 JetsamPilot，v0.6.0 起改名。从旧版升级请看下方《从 JetsamPilot 升级》。

**TrollStore 应用 + root 常驻进程：周期性把受保护 App 的 Jetsam 档位抬到目标值，让系统在内存紧张时先杀别的进程。默认规则把 Reynard 保在 90 档。**

目标系统：iOS 16.4.1（档位数值自 iOS 16 起整体 ×10：后台 30，前台 100）。

---

## 目录

- 1. 工作原理
- 2. 系统架构与数据流
- 3. 安全边界
- 4. 规则配置
- 5. 超内存重启（v0.7.7）
- 6. 仓库结构
- 7. 开发者流程
- 8. 安装与升级
- 9. 发版门禁与真机验收
- 10. 版本历史
- 11. 已知限制

---

## 1. 工作原理

iOS 用 Jetsam 按内存档位（band）决定内存紧张时先杀谁：档位越低越先被杀。前台 App 是 100 档，后台一般是 30 档。系统不会主动帮你保住某个后台 App——JetPilot 做的就是**每 5–10 秒把规则里的 App 档位重新抬回目标值**，让它在内存紧张时排在被杀队列的后面。

核心语义只有三条：

- **前台永远优先**：守护进程绝不碰前台 App 的档位，档位封顶 95，永不触及前台 100。
- **后台按规则保**：`protect` 抬档位，`demote` 在 App 不在前台时降到 30（见第 4 节）。
- **内存真告急时后台都可以杀**：JetPilot 不是"永不杀"，它只做框架、不写死每个 App 的优先级，档位由用户自己在 App 里排。

## 2. 系统架构与数据流

```
App 界面 (main.m)
   │  编辑规则、启动/停止守护进程、看状态
   ▼
/var/mobile/Library/JetPilot/config.txt ──► 守护进程 jetpilotd (root)
                                            │  每 5–10 秒轮询
                                            ├─► memorystatus_control：按规则重设档位
                                            ├─► status.txt：当前档位 / 被杀统计（原子写入）
                                            └─► 日志（5 档等级，写日志页）
```

- 规则保存后 5–10 秒内生效，不用重启守护进程。
- 守护进程只读自己的配置文件和系统内存状态，**不联网、无遥测**。

## 3. 安全边界

- **只碰用户 App**：守护进程只操作 `/var/containers/Bundle/Application/` 下的用户 App，绝不动系统进程。
- **档位封顶 95**：任何规则都不能把档位抬到 100，不会跟前台抢。
- **重启有门槛**：超内存重启只在系统档位 ≤30 时触发，后台播放、通话中的 App 不会被杀（见第 5 节）。
- **配置原子写入**：`status.txt` 原子写入，App 连续保存不会把规则阈值或冷却还原为旧值（v0.7.3 修复）。
- **发版流程**：先审后推——改动先过评审、推 review 分支跑 CI、绿了再终审、真机验证通过才合 main 打 tag（见第 9 节）。

## 4. 规则配置

App 内「保存规则」写入 `/var/mobile/Library/JetPilot/config.txt`，常驻进程 5–10 秒内生效：

```
# protect <路径片段 或 bid:Bundle ID> <档位，最高 95>
# demote  <路径片段>   （不在前台时降到 30）

protect Reynard.app/ 90
protect bid:<目标 App 的 Bundle ID> 90
```

Bundle ID 可在 Filza 里打开该 App 的 Info.plist，看 CFBundleIdentifier。

其它配置（App 内两页独立配置）：

- **进程优先级**：保护 / 降级规则，降级目标档位可选 0–30。
- **内存释放**：按规则冷却与底线退避。
- **内存压力触发**（v0.7.0）：前台 App 内存压力触发，由 `mempressure.txt` 独立配置；另有只读内存限额诊断（前台 / 后台 / 后台 60 秒三读数）。

## 5. 超内存重启（v0.7.7 新增第三个 Tab）

App 在后台驻留超时且内存占用超过阈值时，自动终止并按设定拉起。

- 默认阈值 800 MB、后台 10 分钟、每天最多 3 次、自动拉起默认开。
- 只在系统档位 ≤30 时触发，后台播放、通话不会被杀。
- 升级后请在 App 里停止并重新启动守护进程，否则新功能不生效。

> 已知限制：重启只杀系统档位 ≤30 的 App；微信、支付宝稳态档位钉在 75，稳态下重启基本触发不了（见第 11 节）。

## 6. 仓库结构

```
src/
  app/main.m                # 巨魔 App 界面：编辑规则、启动/停止常驻进程、看状态
  app/Info.plist            # App 信息（com.lili.jetpilot）
  jetpilotd.c               # 常驻进程：每 5–10 秒按规则重设 jetsam 档位
  entitlements-app.plist    # App 权限（persona-mgmt 拉起 root 子进程）
  entitlements-daemon.plist # 守护进程权限（memorystatus / jetsam.modify-priority）
scripts/
  check-release.sh          # 发版门禁：静态检查脚本，提交前必跑
.github/workflows/
  build-jetpilot.yml        # Actions：macos-latest 编译 → 打包 .tipa → 上传产物
```

## 7. 开发者流程

1. 从 `origin/main` 开 `review/<版本>` 分支改代码，用推送脚本推到 GitHub；
2. CI 出包，真机验证通过；
3. `PATCH git/refs/heads/main`（`force: false`，只允许快进）把 main 快进到验证过的提交，再经 API 在同一提交建 tag `v<版本>`；
4. Actions 自动把 JetPilot.tipa 挂到 Release；
5. 手机上从 Release 下载 .tipa，用 TrollStore 安装，打开点「启动」。

## 8. 安装与升级

手机上从 Release 下载 .tipa，用 TrollStore 安装，打开点「启动」。

### 从 JetsamPilot 升级

1. 在 TrollStore 删除 JetsamPilot；
2. 安装 JetPilot.tipa，打开后重新添加规则并启动。

新版是新的 Bundle ID 和配置目录，不导入旧规则。

删除旧 App 不会删掉 `/var/mobile/Library/JetsamPilot/`，留着不影响新版，想清理可用 Filza 删除。

## 9. 发版门禁与真机验收

每个版本发版前必须过三道关：

1. **评审**：改动先过评审（方案 + 预审 diff），再推 review 分支；
2. **CI**：`check-release.sh` 门禁 + Actions 编译出包，全绿；
3. **真机验收**：在目标系统（iOS 16.4.1）上装机实测通过，才合 main 打 tag。

v0.7.7 真机验收通过后发版（main `cf0bde8`，tag `v0.7.7`）。

## 10. 版本历史

- **v0.8.7**（2026-11-27，预览包，非发版 / 待真机验收）：新增零轮询系统事件推送抓前后台（`APPMON`，基于 `AssertionServices` `BKSApplicationStateMonitor` 按规则 Bundle ID 名单精准订阅 + 非阻塞自唤醒管道多路复用 `wait_events_ms` + 启动 `com.apple.springboard` 探针自检）：(1) 刷微信等目标 App 期间守护进程 **0 轮询唤醒**，上滑退后台瞬间 `<100ms` 唤醒主循环立即补抬保护档位并按真实切换时刻启动 `pulse` 精确倒计时（`lag=0`，支持 `0s/3s/5s/10s` 到点即发，过渡动画档位滞留 `100` 时以 `250ms` 短检紧盯跌落、`3s` 封顶防空转）；(2) 点开 `0.3s` 即划走的快进快出场景亦 100% 闩锁 `bgkill` 的 `ever_fg=1`，且严格过滤 `.appex` 扩展进程；(3) `APPMON` 自检通过时解除「有 `pulse` 规则时慢轮锁死 ≤20s」的历史妥协，恢复 `30~240s` 自适应长退避进一步省电（自检失败自动无感回退 20s 慢轮兜底）；同步包含 v0.8.6 全部修复（XNU `75` 档冷冻室闲置态精确放行清理/重启、`JETSAM_STATE` 256 槽防颠簸、同轮改档缓存同步等）。
- **v0.8.6**（2026-11-27，预览包，非发版 / 待真机验收）：(1) 经 iOS 16.4.1 全量 121 进程实测与 XNU `kern_memorystatus` 核实，向 XNU 冷冻室闲置冻结态（`BAND_FREEZER=75`，实时 `entry.state` 同时置位 `Suspended(0x1)|Frozen(0x2)` 且无 `Assertion(0x40)|Active(0x80)`）精确放开 `bgkill` 与 `restart` 清理/重启门槛（`jp_kill_allowed_with_state` + `restart` 动手前 O(1) 实时复核），解决币安/微信/支付宝等切后台被冷冻在 75 档后长期无法定时清理或超内存重启的顽疾，而持活跃断言（`0x40`，如后台音频/通话/任务）的进程照常拦截；(2) 修复 A2 `diag_jetsam_state_note` 在 64 槽 FIFO 下因全机 >120 进程引发 100% 缓存颠簸、2 分钟耗尽全天 600 条 DIAG 额度的 P0 缺陷（扩容至 256 槽 + 陈旧轮优先回收 + 仅主应用进程采样 + 解耦共享 `g_diag_dedup`）；(3) 补齐 C3 慢轮 `RAISE`/`LOWER`/`DEMOTE`/`RETURN` 成功后同步刷新同轮共享缓存优先级（`rst_update_cached_priority`）；(4) 保留 A1 归属可信且 `cur==band` 直取 `orig`（绕过 240s TTL）、A3 收尾重算 `pulse_gate_active` 钳 ≤5s 休眠、A4 禁用规则早退、B0 分离 `delay` 起点（`last_fg_seen`）与 25s 死线、C1 `prewarm`/`band` 跳过日志合并、C2 `diag_latch`/`diag_budget` 拆分与 C3 epoch 共享进程缓存。所有配置文件格式均未变。
- **v0.8.5**（2026-11-26）：熔断粒度由「当天布尔」改为「首次 GONE 冷却 2 小时 + 冷却过期后同 bid 再次 GONE 升级到当日结束」，新增 `circuit_cooldown` / `circuit_escalated` 原因码；恢复失败熔断 `restore_tripped_today` 与两道门不变。日志 PLS_SKIP 增「首因 + 终态因」双槽与 `PLS_FINAL` 行，修复 worker `DSK_GONE` 使用实时 `diag_bg_seq` 及 `bg_seq<0` 覆写有效会话闩锁问题；`PLS_SKIP elapsed=-1` 统一渲染 `NA`；`respondsToSelector:` 的 `objc_msgSend` 返回类型 `intptr_t`→`bool`。真机日志复核追加三项核心修复：(1) 废弃误指 `GET_JETSAM_SNAPSHOT` 的错误命令号 `3`，统一改为 XNU 原生 `MEMORYSTATUS_CMD_GET_PRIORITY_LIST(1, pid>0)` 单进程定向查询并严格校验返回字节数与 `entry.pid`；(2) `pulse` 恢复 `L = ceil(fp0 / 0.85)` 黄金警告带与 `95%×L` 越界前恢复守卫（`target_mb` 仅作后台触发门槛），修正 `L = target < fp0` 落入 `limit_exceeded` 分支导致 UIKit 收不到 `PRESSURE_WARN/CRITICAL` 内存警告且引发内核 `highwater` 误杀保活 App 的问题；(3) `bgkill` 新增前台经历追踪（`ever_fg`）与系统 `dasd` 预热空壳跳过机制（从未进过前台且 `<40MB` 记单次 `prewarm_skip` 不杀不计次，用户真点开过的进程无论内存大小照常按时清理）；顺带将 `pulse_check_candidate` 熔断冷却判定前移至 `delay_s`/`PULSE_LATE` 之前并在冷却期抑制 5s 门控快轮，排除用户切回前台引发的 `OVERRIDDEN` 计入 `unresponsive`。另集中修复 4 项 P0 与多项 P1/P2：D-003 门控快轮 `pulse_round()` 重置 `s_napp_procs` 防脏缓存误判前台 App；慢轮/快轮仅在系统漂移时记 `sys_band` 并在 TTL 内续期 `band_seen_mono`（根治 `sys_band` 污染自然档位门控及 240s 后 `LOWER`/`RETURN` 失灵，含 `CLAIM` 条目）；DIAG2 全状态加叶锁 `s_diag_mutex` + 原子计数消除多线程数据竞争；修复 `RETURN` 日志 `30->30`、`ovr_t` 常量误用、跨日恢复熔断漏清零、worker 启动失败回滚 `pulsed_this_bg`、`pulse.state` 打开后 `fstat` 复核、`bgkill` 删配置写锁；App 端修复 `delay_s=0` 回读变坏行、Tab 2 轮询同步释放卡片（去 N+1 读盘）与测试模式按钮、`bgkill.txt` 支持 `bandgate=0|1` 与 `setRule` 同步、日志尾读先按原始字节裁首行防 UTF-8 乱码、Tab 3 `deadBanner` 口径统一、`AppPicker` 空态约束与配置目录权限收紧为 `0755`。
- **v0.8.4**（2026-11-25）：新增用户可见「测试模式」四档日志（`off` / `basic` / `detailed` / `full`，由 `diag.txt` 配置，配置与 stat 去重感知）；PULSE 升级 v=2 汇总与 PLS_SKIP 全 reason 码闩锁去重，新增 PLS_CFG/STATE/CIRCUIT/OVERRIDE/STAGE/SAMPLE 与 DIAG_STATS 统计；修复本次测试模式保存失败（去掉 `diag.txt` 的 root 属主要求，App 端保留 regular + `(mode & 0022)==0` 复核、daemon 端改用专用只读预检）及 main.m 编译错误。真机 8 小时高频日志验证通过：0 early_abort / 0 PULSE_LATE / 0 unresponsive，日志体积约 50KB/24h 推算。
- **v0.8.1**（2026-11-22）：P0-1 root 写路径统一安全加固——所有 root 落盘文件经 `jp_secure_precheck` / `jp_secure_fd_check` 双重校验（防符号链接 `O_NOFOLLOW` + root 属主与组/其他不可写），保留 M1 原子提交；CI 门禁 UNVERIFIED 改硬失败（移除 `--allow-unverified`，`set -euo pipefail` + grep 吞错修复）；远端批次 0–2：H1 并发原子性 / H2 规则数组撕裂读（读写锁）/ M1 全写点原子写 + fsync，省电优化（快慢双节奏扫描 / 定向查询 / 自适应退避 / status 按需写），九场景回归测试 harness 入库 `tests/`。
- **v0.8.0**（2026-11-20）：删除 E 规则；新增后台定时清理（按后台时长自动结束）；后台释放支持限额被改写后重设一次。
- **v0.7.9**（2026-11-19）：重启阈值下限降到 50MB、释放目标下限降到 100MB；释放最小间隔可设 0 分钟；目标值校验放宽。
- **v0.7.8**（2026-11-19）：新增后台释放（pulse）：App 切后台后把内存压到目标值以下；E 方案停用。
- **v0.7.7**（2026-11-12）：新增第三个 Tab「超内存重启」：后台驻留超时且内存超阈值时自动终止并拉起（默认 800 MB / 10 分钟 / 每天最多 3 次，自动拉起默认开）；只在系统档位 ≤30 时触发。
- **v0.7.6**（2026-11-12）：日志等级 5 档放日志页；当前档位可点进看全部进程；被杀统计挪到主页底部。
- **v0.7.5**（2026-11-12）：日志等级可选；RAISE 日志去重；原档位快照；阈值提示；降级卡片显示运行状态。
- **v0.7.4**（2026-11-12）：降级规则目标档位可选（0–30 档）；App 列表加规则时可选保护或降级；导出日志打包成 zip；内存压力底线取最低值。
- **v0.7.3**（2026-11-07）：修复连续保存时规则阈值或冷却被还原为旧值的问题。
- **v0.7.2**（2026-11-06）：App 支持进程优先级与内存释放两页独立配置；内存释放支持按规则冷却与底线退避。
- **v0.7.1**（2026-11-06）：只读记录目标应用内存限额诊断。
- **v0.7.0**（2026-11-06）：支持前台 App 内存压力触发，由 mempressure.txt 独立配置。
- **v0.6.0**（2026-11-05）：改名 JetPilot（Bundle ID、配置目录变更，需删除旧版重装，规则需重新添加）。
- **v0.5.6**（2026-11-05）：规则按 Bundle ID 精确匹配，避免误伤同名应用；支持规则编辑与自动迁移。
- **v0.5.5**（2026-11-04）：状态文件按需写入，更省电；守护进程未运行时顶部红条提醒，一键启动；受保护进程标出主程序或扩展；新增调试日志。
- **v0.5.4**（2026-11-04）：重启后不再误调系统给的档位；扩展进程的档位也会随规则下调；保护中状态显示更准确。
- **v0.5.3**（2026-11-04）：重启/升级后可降级旧档位；保护中三态显示；关于页加版本号与仓库链接。
- **v0.5.2**（2026-11-04）：保护档位可下调；删除规则后档位自动恢复。
- **v0.5.1**（2026-11-03）：提权失败显示错误文本；status.txt 原子写入；被杀统计放后台线程解析；删掉进程名反向匹配。
- **v0.5.0**（2026-11-03）：被杀统计；提权失败原因红字显示。
- **v0.4.0**（2026-11-03）：「保护中」可点开看名单；锁屏省电改事件驱动。
- **v0.3.0**（2026-11-03）：锁屏后降低轮询频率省电。
- **v0.2.0**（2026-11-03）：UI 重做；新增 App 图标。
- **v0.1.0**（2026-11-03）：首版。

## 11. 已知限制

- 锁屏省电逻辑（state=1=锁屏）按社区常用语义实现，未在真机逐行核实；若行为反了，看 README 版本历史回退。
- 守护进程部分权限需求按 XNU 一般规则推断，未在 16.4.1 上逐行核实；若 `memorystatus_control` 报 EPERM，看守护进程 stderr 排查。
- 定时清理与超内存重启仅放行自然档位 ≤30 或经内核 `entry.state` 实时确认为 XNU 冷冻室闲置态（75 档，`Suspended|Frozen` 且无活跃断言）的 App；持后台活跃断言（如音频播放、通话、定位、未完结后台任务）的 >30 档进程仍会被保护性跳过。
