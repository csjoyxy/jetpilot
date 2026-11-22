# JetPilot 审查与修复路线图

生成日期：2026-11-22。来源：开发组摸底 + 审核组全量审查 + 互审员独立核验。

## 摸底结论摘要

JetPilot = TrollStore 应用 + root 常驻守护进程（Objective-C，iOS 16.4.1），每 5–10 秒把规则内 App 的 Jetsam 档位抬回目标值。两进程以文件为契约（`/var/mobile/Library/JetPilot/`），无 IPC socket。守护进程单线程主循环 + 一个 pulse worker 线程；四大配置域（主 config / restart / pulse / bgkill）；flock 单实例锁；tmp+rename 原子写。

## 问题清单

### A. 代码正确性 / 安全（代码审核员 + 工程师 + 互审员核验）

| 编号 | 严重度 | 问题 | 状态 |
|---|---|---|---|
| H1 | 高 | pulse 完成标志（`in_progress`/`current_pid`）volatile 非原子，跨线程数据竞争；spawn/join 守卫也在竞争窗口内（`current_bid` 经核验只写不读、无实际风险） | ✅ 已修（批次1） |
| H2 | 高 | 规则数组主线程 memcpy、worker 无锁读 → 撕裂读 | ✅ 已修（批次1，s_rule_lock 读写锁） |
| M1 | 中 | 多处原子写 `fopen("w")` 缺 O_NOFOLLOW（root 写可被 mobile 劫持）+ **全文件无 fsync**（崩溃可能恢复出空/截断状态文件） | ✅ 已修（批次1，jp_atomic_commit 六处写点收口） |
| M2 | 中 | `set_priority`/memlimit 依赖 errno 不可靠、未区分 EPERM | 待修 |
| M3 | 中 | `get_list`「+16 条」启发式可能静默截断进程列表 | 待修 |
| L1 | 低 | `g_locked` 等跨线程标志非原子 | 待排 |
| L2 | 低 | dedup 环形缓冲 clear 后 head 失效 | 待排 |
| L3 | 低 | 启动早期日志在 loglevel 生效前输出 | 待排 |
| L4 | 低 | 单实例锁依赖 lock 文件不被删 + 重复实例返回 0 | 待排 |

工程师补充的易错点：`load_config` `strncpy(...,255)` 不补 `\0`；规则超限静默截断；`rst_fbs_launch` dlopen 不 dlclose；App 侧 pid 复用误判；status.txt `|` 分隔符未过滤。

### B. 门禁 / 测试覆盖（测试审核员 + 测试工程师）

| 编号 | 问题 | 状态 |
|---|---|---|
| P0-1 | 门禁软失败（`--allow-unverified`） | ✅ 已修 |
| P0-2 | shell 无 `-e`/`pipefail` + grep 吞错 | ✅ 已修 |
| F1 | JPLOG 计数 `grep \|\| echo 0` 双「0」bug | ✅ 已修（含在 P0-2） |
| G1 | CI 无并发/内存安全测试（无 ASan/TSan/UBSan）——最高杠杆 | 待做 |
| G2 | tag 只验签名、不验真机验收 | 待做 |
| G3 | verify-artifacts 跳过 slot -1（Info.plist 可被篡改） | 待做 |
| G4 | 无 minOS/platform/platform-application/get-task-allow 断言 | 待做 |
| F2 | 版本 sed 正则多处匹配会 FAIL | 待做 |
| F3 | 禁串/entitlement 子串 grep 误报/漏报 | 待做 |
| F4 | 硬编码 v0.7.7 参考包大小 | 待做 |
| F5 | codesign --verify 只 WARN 不 FAIL | 待做 |
| — | verify-artifacts.py 对损坏 zip 未捕获 `BadZipFile` 崩溃 | ✅ 已修（批次1） |

### C. 省电 / 效率优化

| 编号 | 优化项 | 状态 |
|---|---|---|
| E1 | 快慢双节奏扫描（快轮只查已知受保护 pid，省掉每轮上百次 proc_pidpath） | ✅ 已实现（批次2，待验收） |
| E2 | 自适应退避（空转才慢、有事即快） | ✅ 已实现（批次2，待验收） |
| E3 | status 按需写（变化才写 + bands.req 时写） | ✅ 已实现（批次2，待验收） |
| E4 | 核实 memorystatus_control 是否支持按 pid 定向查档位 | ✅ 已核实（命令 3 支持；须真机验证，三级兜底） |

## 执行批次

1. ✅ 批次0：P0-1 / P0-2 门禁修复（工程师，已完成并验证）
2. ✅ 批次1：H1/H2（含 spawn 路径）+ M1（六处写点含 fsync）+ BadZipFile 优雅报错 —— 已完成；审查链：代码审核员揪出双解锁 bug 与 1 P2 + 5 P3，互审员独立抽查补 bands.txt 漏点，架构师复核关闭
3. 🔄 批次2：省电优化 E1–E4 —— 实现完成，全链通过：设计批准（docs/optimization-design.md）→ 工程师实现（src/jetpilotd.c +310/-18，三个提交单元 E4+E1 / E2 / E3）→ 架构师复核揪出 SLEEP/LEGACY 永久休眠缺口（已修）与 CLAIM kind=0 偏差（已采纳修正）→ 代码审核通过（1 P2 seen 残留已修 + 3 P3）→ 互审员独立抽查通过（harness 入库流程缺口已补齐）→ 回归 harness 九场景入库 tests/（可复现）。待 Chief 验收后关闭
4. 📋 批次3：CI 测试台（G1）+ 脚本加固 F2–F5
5. 📋 后续：M2/M3、L1–L4、工程师补充的易错点

## 发版门禁（沿用项目既有流程）

先审后推 → `check-release.sh` 门禁 + Actions 编译出包 → 真机验收 → 合 main 打 tag。团队本地无 Mac 编译环境和真机，CI 出包与真机验收由用户侧完成。
