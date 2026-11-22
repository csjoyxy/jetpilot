# JetPilot 守护进程省电/效率优化设计

生成日期：2026-11-22。范围：E1 快慢双节奏 / E2 自适应退避 / E3 status 按需写 / E4 memorystatus_control 定向查询核实。目标文件 `src/jetpilotd.c`（行号以批次1 合入后的 main 分支为准）。**先审后动代码。**

---

## 0. 现状与热路径

主循环 `while (!should_stop())`（`:5748`）每轮做一次「全表 + 逐进程」扫描：

- `get_list()`（`:923`）：`memorystatus_control(GET_PRIORITY_LIST)` 取**全系统进程档位表**（数百条，二次调用：先探测 size 再取）。
- 逐进程 `proc_pidpath(pid, path)`（扫描循环内 `:5811`）：每条进程一次 syscall，再 `strstr(path, "/var/containers/Bundle/Application/")` 过滤用户 App。
- 规则匹配 + `set_priority` + 收集 plist/dlist → `mempressure_round`（`:5991`）→ `applied_prune`（`:5993`）→ `write_status`（`:6012`）→ 锁屏/亮屏休眠（`:6017-6025`）。

热路径成本 = 一次全表 syscall + **数百次 proc_pidpath**。亮屏 5~10 秒一轮、锁屏 10~60 秒一轮，反复重扫「几乎没变化」的进程表。省电目标：空转时把扫描频率与扫描范围都降下来，有事时快速响应。

---

## 1. E4 · memorystatus_control 定向查询可行性核实

### 结论：支持按 pid 定向查档位

`memorystatus_control` 除 `GET_PRIORITY_LIST`（命令 1，`:177`）外，**支持按 pid 定向查询**：`MEMORYSTATUS_CMD_GET_PRIORITY_PROPERTIES`（命令 **3**，XNU `osfmk/kern/kern_memorystatus.h`，未公开、代码里尚未声明）。

```c
/* 需在文件头部补声明（现有 177-200 只声明了 1/2/8/9） */
#define MEMORYSTATUS_CMD_GET_PRIORITY_PROPERTIES 3

typedef struct memorystatus_priority_properties {   /* 现有 194-197 已声明同名结构 */
    int32_t  priority;
    uint64_t user_data;
} memorystatus_priority_properties_t;

/* 用法：返回 0 成功，props.priority 即当前档位；失败 -1 + errno（进程退出 ESRCH） */
int rc = memorystatus_control(MEMORYSTATUS_CMD_GET_PRIORITY_PROPERTIES,
                              pid, 0, &props, sizeof(props));
```

### 佐证

- 同一头文件的兄弟命令当前代码已在用：`SET_PRIORITY_PROPERTIES`（2，`:178`/`:892`）、`GET_PRIORITY_LIST`（1，`:924`/`:930`）、`SET/GET_MEMLIMIT_PROPERTIES`（8/9，`:2562`/`:2572` 区）。命令 3 夹在 2 与 4 之间，语义与 8/9 同构（带 pid、返回 0 成功）。
- 命令号与结构在 iOS 9–18 稳定；守护进程以 root（persona uid 0）运行，可查任意 pid。

### 风险与兜底

- **风险**：私有未公开 API，不同 iOS 版本行为可能有差异；必须在目标机（iOS 16.4.1）运行时验证 `GET_PRIORITY_PROPERTIES` 的返回码与 `props.priority` 是否等于 `GET_PRIORITY_LIST` 里同 pid 的 `priority`。
- **兜底方案（三级降级）**：
  1. 快轮仍调 `get_list()`，但**只遍历已知 pid**（按 pid 建索引命中，跳过其余），省掉的是 proc_pidpath，不是全表 syscall；
  2. 再退：快轮不重扫、只延长休眠，重断言交给慢轮（正确性不丢、只牺牲响应度）；
  3. 全退：整体回退到当前「每轮全表扫」的现状，改动最小。

---

## 2. E1 · 快慢双节奏扫描

把主循环 `:5797-5993` 的「全表 + proc_pidpath」拆成两种轮次：

| | 快轮（多数轮） | 慢轮（周期轮 + 事件触发） |
|---|---|---|
| 取档位 | 逐已知 pid `GET_PRIORITY_PROPERTIES`（O(已知数)） | `get_list()` 全表 |
| 路径解析 | **不调 proc_pidpath**，用缓存 | 逐进程 proc_pidpath 发现新 App / 清理退出 |
| 重断言 | 已知 pid 档位漂移→抬/降回 target | 完整匹配 + RAISE/LOWER/DEMOTE/RETURN |
| mempressure_round | 跳过 | 跑（restart/pulse/bgkill 需要全表） |
| write_status | 不写 | 写（见 E3） |

### 2.1 慢轮判定（何时走慢轮）

以下任一满足即本轮走慢轮：

1. **首轮**（`g_slow_due_mono == 0`）；
2. `now >= g_slow_due_mono`（周期到期，见 E2 自适应）；
3. **锁屏状态变化**（`wait_lock_change` 返回 `was != g_locked`，即解锁/锁屏瞬间）——锁屏触发系统降档窗口、解锁需立刻补一轮抬档；
4. **config 变化**（`config.txt` 的 ino/size/mtime 与上次不同，复用 `jp_file_stat_changed` 加一个 `s_config_stamp`）；
5. **bands.req 到达**（`:5767` `handle_bands_req=1`，用户在看档位页）。

### 2.2 新增状态：已知 pid 集合（与 applied 表的关系）

**方案：扩展 `applied_t` 兼任「已知受保护 pid 集合」，不另建表。** 理由：快轮要重断言的 pid，恰好就是「我们设过档位」的 pid，即现有 applied 表；而「已匹配规则但正好在目标档、没改过档」的少数 App 由慢轮兜底发现（见 2.4 边界）。

```c
/* 现有 applied_t（:607-615）扩展 */
typedef struct {
    pid_t   pid;
    int32_t band;                 /* 我们最后设的档位 */
    int     seen;
    int32_t orig;
    int     orig_known;
    int32_t sys_band;
    int     sys_band_known;
    /* ---- E1 新增：快轮身份缓存 ---- */
    int     has_identity;         /* 1 = 下面四项有效（慢轮填充） */
    char    path[PROC_PIDPATHINFO_MAXSIZE];  /* 可执行路径 */
    int32_t target;               /* protect: 目标档；demote: demote 目标档 */
    int     kind;                 /* 0=protect，1=demote */
    char    label[64];            /* proc_label（日志/status 用） */
    char    bid[128];             /* 主 App Bundle ID（status 用） */
} applied_t;
```

内存：`MAX_APPLIED=64`，每条新增约 4.4KB（path 4K + label + bid），静态约 280KB BSS。守护进程可接受；若要更省，path 可改堆分配（首轮 strdup、`applied_remove` 时 free），设计上推荐堆分配，实现在意静态占用即可二选一。

与现有函数的关系（不改现有语义，只加字段）：

- `applied_set_ex`（`:629`）与 `applied_set`（`:644`）：仅在**慢轮**里调用时额外填充 `has_identity/path/target/kind/label/bid`（在已定位 path、匹配出 target 之后）。**新建条目时填、已有条目时保留**（快轮重断言不重填身份）。
- `applied_find`（`:619`）：不变，快轮遍历 `g_applied[0..g_napplied)` 直接索引，不调它。
- `applied_prune`（`:663`）：不变，快慢轮**都**在结尾调；快轮对 `GET_PRIORITY_PROPERTIES` 成功者置 `seen=1`，ESRCH 者不置 → 被剪除（含既有 `act_dedup_clear`「(end)」冲刷语义）。
- `applied_remove`（`:676`）：不变（含堆分配 path 的 free，若选堆分配）。

### 2.3 快轮流程（新函数 `fast_round()`）

```c
static void fast_round(void) {
    for (int i = 0; i < g_napplied; i++) {
        applied_t *a = &g_applied[i];
        if (!a->has_identity) continue;              /* 无身份缓存，交给慢轮 */
        memorystatus_priority_properties_t p;
        if (memorystatus_control(GET_PRIORITY_PROPERTIES, a->pid, 0, &p, sizeof(p)) != 0) {
            /* 进程已退出：不置 seen，由 applied_prune 剪除 */
            continue;
        }
        a->seen = 1;
        if (p.priority != a->sys_band) { a->sys_band = p.priority; a->sys_band_known = 1; }
        if (p.priority == a->band) continue;         /* 没漂移，不动 */
        /* 档位被系统改了 → 重断言回 target */
        if (set_priority(a->pid, a->target) == 0) {
            g_reassert_count++;
            applied_set_ex(a->pid, a->target, p.priority);  /* 只更新 band，保留身份缓存 */
            act_log_success(a->kind == 0 ? ACT_KIND_RAISE : ACT_KIND_DEMOTE,
                            a->pid, a->path, /*frag 需补*/ NULL, p.priority, a->target);
        }
    }
    applied_prune();
}
```

注意：`act_log_success` 的 frag 参数要一并缓存（上述 applied_t 里补一个 `char frag[256]` 或指向规则 frag 的索引），否则快轮日志缺 frag。**改动点清单里把 frag 也列入身份缓存。**

### 2.4 边界情况

1. **App 快速启动/退出**：快轮不发现新 App（那是慢轮的活），新 App 的抬档延迟 ≤ 慢轮周期（E2 封顶 240s）。但解锁/锁屏/config 变化都会强制慢轮，实际「用户正在操作」时几乎立刻慢轮发现，冷启动守护进程的首轮也是慢轮。退出则由快轮的 ESRCH 剪除，不依赖慢轮。
2. **表满（MAX_APPLIED=64）**：`applied_set_ex` 表满时不记（现有 `:632` 语义），快轮就漏掉这些 pid 的重断言——与现状一致（现状表满也只是失去降档能力），无新增风险。可接受，不改。
3. **pid 复用**：快轮用缓存 path 重断言，极端情况下 pid 被复用、`GET_PRIORITY_PROPERTIES` 查到的是新进程，可能把新进程误设到 target。缓解：慢轮周期内会重新 proc_pidpath 校验并修正；可选加固——缓存进程启动时间（`proc_pidinfo(PROC_PIDTBSDINFO)` 的 `pbi_start_tvsec`）在快轮比对，不一致则视为退出交给慢轮。设计默认**不做**，留作可选加固项。
4. **无身份缓存的条目**（has_identity=0，如旧实例认领 `CLAIM` 路径 `:5815` 附近新建的条目）：快轮 `continue` 跳过，交给慢轮填充身份。

---

## 3. E2 · 自适应退避

### 3.1 空转轮判定（每轮结束统计）

本轮为空转，当且仅当同时满足：

- 本轮无 RAISE / LOWER / DEMOTE / RETURN 动作（`g_reassert_count` 未增、无 `act_log_success`）；
- config 无变化（`s_config_stamp` 未变）；
- **慢轮**时：无新受保护进程（`protected_now` 与上轮相同，且无新 applied 条目）——此判定只有慢轮能做；
- 无 bands.req（`handle_bands_req == 0`）。

快轮只做「无漂移重断言 + 无 config 变化 + 无 bands.req」这半组判定；「无新进程」由慢轮兜底。

### 3.2 翻倍 / 回落策略

```c
#define BACKOFF_BASE_SECS  5    /* 亮屏基础间隔（对齐现状 5~10s） */
#define BACKOFF_MAX_SECS  60    /* 亮屏最大间隔（与锁屏 60s 对齐） */
#define SLOW_BASE_SECS    30    /* 慢轮基础周期 */
#define SLOW_MAX_SECS    240    /* 慢轮最大周期 */

if (idle) {
    g_idle_rounds++;
    g_interval   = min(BACKOFF_BASE_SECS << g_idle_rounds, BACKOFF_MAX_SECS);   /* 5→10→20→40→60 */
    g_slow_period = min(SLOW_BASE_SECS   << g_idle_rounds, SLOW_MAX_SECS);      /* 30→60→120→240 */
} else {
    g_idle_rounds = 0;
    g_interval   = BACKOFF_BASE_SECS;
    g_slow_period = SLOW_BASE_SECS;
}
g_slow_due_mono = now_mono + g_slow_period;   /* 慢轮周期到期时间 */
```

### 3.3 结合锁屏/亮屏休眠逻辑（`:6017-6025` 改造）

```c
if (g_locked) {
    /* 保留现有语义：刚锁屏前两轮 10s 盖住系统降档窗口，之后 60s */
    int secs = (g_locked_passes < 2) ? 10 : 60;
    g_locked_passes++;
    wait_lock_change(secs);            /* 解锁立刻唤醒 */
} else {
    g_locked_passes = 0;
    wait_lock_change(g_interval);      /* 亮屏用退避后的间隔，5~60s */
}
```

要点：锁屏时的 10/60s 是「盖降档窗口」的独立理由，**不参与**退避（退避只作用在亮屏 `g_interval` 与慢轮 `g_slow_period`）。解锁瞬间 `wait_lock_change` 返回 → 判定「锁屏状态变化」→ 强制慢轮补抬档，正好衔接。

---

## 4. E3 · status 按需写

### 4.1 现状（`:1048-1098`）

`write_status` 拼 body → 与 `g_last_body` 比对，`same && now - g_last_write < STATUS_HEARTBEAT_SECS(60)` 则跳过（`:1098`）→ 否则 `jp_atomic_commit` 写盘。即「内容不变也最多 60s 写一次心跳」。

### 4.2 改造

- **删除 60s 心跳**，改为「**内容变化才写**」：`if (same) { free(body); return; }`。
- **bands.req 时写**：`handle_bands_req` 本轮到达 → 即使内容没变也写（用户正看档位页，强制刷新 status 的 `updated=` 与 band 数据）。
- 实现：`write_status` 增加 `int force` 形参，慢轮调用时传 `handle_bands_req`；`if (same && !force) skip`。
- `STATUS_HEARTBEAT_SECS` 宏（`:996`）随之删除（或改为无引用的注释说明）。
- 快轮**不调用** write_status（快轮不重建 plist/dlist，避免用空列表写出错误 body；见 2.1 表）。status 只在慢轮（周期或事件触发）更新。

### 4.3 App 端 3s 轮询与 mtime 判断的配合评估

- App 每 3 秒 `NSTimer` 触发 `refreshStatus`（main.m `:5855`/`:8843`/`:10594`）→ `loadCurrentStatus` 每次**整文件重读重解析**（`stringWithContentsOfFile`，无 mtime 短路）。
- App 存活判定 `daemonState`（main.m `:2131`）用的是：flag 文件存在 + status.txt mtime 存在且 **>= 开机时间**（`:2141`）+ `kill(pid,0)` 对 pid 文件（`:2146`）。**不依赖 mtime 新鲜度**，只要求「开机后写过至少一次」。
- 结论：**删心跳安全**。守护进程存活期间 status.txt mtime 停在最后一次写，App 仍靠 `kill(pid,0)` 判活，不会误报「已停止」；守护进程崩溃后 `kill(pid,0)` 失败 → 正确判死。
- 副作用（可接受）：空转期间 `updated=` 时间戳停住，App 顶栏「更新于」显示旧时间；一旦有变化或 bands.req 即刷新。
- 补充：守护进程首轮 body 必然与 `g_last_body(NULL)` 不同 → 必写一次，保证开机后 status.txt 存在，满足「>= 开机时间」判据。

---

## 5. 改动点清单

| # | 位置 | 改动 |
|---|---|---|
| 1 | 头部 `:177-200` | 补 `#define MEMORYSTATUS_CMD_GET_PRIORITY_PROPERTIES 3` |
| 2 | `applied_t`（`:607`） | 加 `has_identity/path/target/kind/label/bid/frag` 身份缓存字段 |
| 3 | `applied_set_ex`/`applied_set` | 慢轮路径填充身份缓存；`applied_remove` 释放（若堆分配 path） |
| 4 | 新增 `fast_round()` | 快轮：逐已知 pid `GET_PRIORITY_PROPERTIES` 重断言 + `applied_prune` |
| 5 | 主循环 `:5797-5993` | 抽成 `slow_round()`；加慢轮判定（首轮/到期/锁屏变化/config 变化/bands.req） |
| 6 | `mempressure_round`（`:5546`） | 只在慢轮调用（快轮跳过） |
| 7 | 新增全局 | `g_slow_due_mono/g_slow_period/g_idle_rounds/g_interval/s_config_stamp` |
| 8 | 休眠段 `:6017-6025` | 亮屏改 `wait_lock_change(g_interval)`；锁屏不变 |
| 9 | `write_status`（`:1048`） | 加 `force` 形参；`same && !force` 才跳过；删 `STATUS_HEARTBEAT_SECS` |
| 10 | 主循环尾部 | 快轮不调 `write_status`；慢轮调且传 `force=handle_bands_req` |

## 6. 新增状态变量汇总

| 变量 | 类型/初值 | 作用 |
|---|---|---|
| `g_slow_due_mono` | `time_t` 0 | 下次慢轮到期（CLOCK_MONOTONIC） |
| `g_slow_period` | `int` 30 | 当前慢轮周期（30→240） |
| `g_idle_rounds` | `int` 0 | 连续空转轮数（翻倍指数） |
| `g_interval` | `int` 5 | 亮屏休眠间隔（5→60） |
| `s_config_stamp` | `jp_file_stamp_t` | config.txt 变更戳（复用 `jp_file_stat_changed`） |
| `applied_t.*` 新增 6 字段 | — | 快轮身份缓存（path/target/kind/label/bid/frag/has_identity） |

## 7. 风险与回滚

1. **私有 API 不确定性**（E4）：`GET_PRIORITY_PROPERTIES` 必须先真机验证；三级兜底（见 §1）保证任何一级失败都能安全降级，最坏回退现状。
2. **快轮重断言窗口**：pid 复用极端场景可能误设，慢轮周期内自愈；可选加固（缓存启动时间）留待观察。
3. **status 新鲜度下降**：空转时 `updated=` 停更（App 判活不受影响，§4.3 已论证）；若产品要求「心跳可见」，可保留一个很长的兜底心跳（如 300s）作为折中。
4. **mempressure 采样变粗**：restart/pulse/bgkill 只在慢轮跑，采样粒度从 5~10s 变 30~240s；但三者状态机全部基于 CLOCK_MONOTONIC 时间差（非轮次计数），仅粒度变粗、不产生计时错误；锁屏/解锁/config 变化会强制慢轮，关键时机仍即时。
5. **lockmon fail-open 与 §2.1 前提的偏差**（互审员抽查记录）：`lockmon_init` 注册通知失败时保持常速轮询（fail-open），此时「锁屏状态变化触发慢轮」不生效——锁屏降档后的抬档延迟由快轮 cmd3 漂移检测兜底，最坏一个亮屏退避间隔（≤60s）。行为可接受，特此记录。
6. **回滚方式**：改动集中在主循环与 `applied_t`/`write_status`，git 按 hunk 还原即回现状；建议合入时拆成独立提交（E1/E2/E3/E4 各一 commit），可单项回滚。
