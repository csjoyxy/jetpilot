# 批次1 子任务拆解（v2 · 并入互审员核验）

生成日期：2026-11-22。范围：H1 / H2 / M1 / BadZipFile。目标文件 `src/jetpilotd.c`、`scripts/verify-artifacts.py`。本文档是工程师实现依据，审核组与互审员据此验收。行号以 `src/jetpilotd.c` 当前 main 分支为准。

---

## T1 · H1 并发原子性（pulse 完成标志 + spawn/join 竞争）

### 现状与根因

跨线程共享状态（worker 线程 ↔ 主循环）：

```c
2479: static volatile int   g_pulse_in_progress = 0;      // 非原子
2480: static volatile pid_t g_pulse_current_pid = 0;      // 非原子
2481: static char           g_pulse_current_bid[128];     // 非 volatile/原子，只写不读（见下）
2483: static _Atomic int    g_pulse_thread_running = 0;   // 原子
2484: static int            g_pulse_thread_has_handle = 0;// 仅主线程读写，安全
```

worker 收尾（`pulse_worker_thread` 3626–3630，`pulse_ctrl_worker_thread` 3767–3770）：

```c
g_pulse_in_progress  = 0;   // (a) 先清
g_pulse_current_pid  = 0;   // (b)
g_pulse_current_bid[0]=0;   // (c)
g_pulse_thread_running = 0; // (d) 最后清，但无 release 语义
```

spawn/join 守卫（3993–3997、4033–4036，同型）：

```c
if (g_pulse_thread_has_handle && !g_pulse_thread_running) {
    pthread_join(g_pulse_thread, NULL);
    g_pulse_thread_has_handle = 0;
}
g_pulse_thread_running = 1;          // ← 无条件置 1
pthread_create(&g_pulse_thread, ...); // ← 无条件覆写句柄
```

竞争窗口：worker 在 (a) 之后、(d) 之前，处于「逻辑上已无 pulse（in_progress==0）但线程仍存活（running==1）」。此时主循环 `while (!g_pulse_in_progress && ...)`（3964）判定可 spawn，进 3993 时 `has_handle && !running` 为假 → **跳过 join**，随后 `running=1` + `pthread_create` **覆写旧线程句柄**（旧线程永不 join，joinable 资源泄漏）；旧 worker 到达 (d) 写 `running=0`，**清掉主线程刚为新线程置的 running=1**，导致下一轮 3993 对仍在运行的新线程误判 `!running` 并 join → 阻塞单线程主循环或二次 spawn。

### 修复方案

1. 类型改为原子：`g_pulse_in_progress` → `_Atomic int`，`g_pulse_current_pid` → `_Atomic pid_t`。`g_pulse_current_bid` **保持原样**（全文件只写不读，无撕裂读，风险为零——互审员核验结论，改造范围确认记此一笔）。
2. worker 收尾（3626–3630、3767–3770）：保持「先清逻辑态、最后清 running」顺序，`g_pulse_thread_running` 的置 0 改为 `atomic_store_explicit(&g_pulse_thread_running, 0, memory_order_release)`，作为 worker 完成的**唯一权威信号**。
3. 所有 spawn/join 守卫（3993–3997、4033–4036，以及同型的 3794–3797「回收已完成 worker」）改为：
   - `atomic_load_explicit(&g_pulse_thread_running, memory_order_acquire)` 判活；
   - 若 `has_handle && running`（旧 worker 仍在收尾）→ **本轮不 spawn**（清掉 in_progress/current_pid/current_bid 后 break，下轮重试），**绝不 pthread_create**；
   - 若 `has_handle && !running` → join + 清 has_handle；
   - spawn 前置 `atomic_store_explicit(&g_pulse_thread_running, 1, memory_order_release)` 后再 pthread_create。
4. 读点 3929 / 4789 / 5390（`g_pulse_in_progress && g_pulse_current_pid == main_pid`）改为 `atomic_load_explicit(..., acquire)`；逻辑上无假阳性（worker 先清 in_progress 再清 current_pid），此处主要消除 UB 并统一语义。
5. 关停路径 5911–5919 改用 `atomic_load` 读取 running。

### 验收

- TSan 下对 `g_pulse_in_progress` / `g_pulse_current_pid` / `g_pulse_thread_running` 零数据竞争告警。
- 不变量成立：`running==0` 时其它状态必已清；spawn 路径永不与未 join 的旧 joinable 线程并存。
- `g_pulse_current_bid` 记录为「只写不读」出改造范围。

---

## T2 · H2 规则数组撕裂读

### 现状

主线程 memcpy 整块规则数组：

```c
1892: memcpy(g_restart_rules, parsed_rules, sizeof(restart_rule_t) * nparsed);  // restart_load_config
2194: memcpy(g_pulse_rules,   parsed_rules, sizeof(pulse_rule_t)   * nparsed);  // pulse_load_config
5228: memcpy(g_bgkill_rules,  parsed_rules, sizeof(bgkill_rule_t)  * nparsed);  // bgkill_load_config
```

worker 线程经 `pulse_revalidate_conditions`（2402，worker 在 3140 调用）读 `g_pulse_rules`（2407/2408）与 `g_restart_rules`（2451/2452）。并发 memcpy + 无锁读 → 撕裂读（读到半更新的规则，阈值/开关瞬时错乱）。

### 修复方案

- 新增 `static pthread_rwlock_t s_rule_lock = PTHREAD_RWLOCK_INITIALIZER;`
- 三处 memcpy（1892 / 2194 / 5228）在 `write lock` 内完成；worker 可达的读点（`pulse_revalidate_conditions` 内对 `g_pulse_rules`/`g_restart_rules` 的访问）在 `read lock` 内完成。
- 备选（更无锁）：双缓冲 + 原子指针交换，但改动面大、需把全部 `g_*_rules[i]` 改成经指针访问；**默认选 rwlock**，改动局部、代价可接受（规则加载频率低）。

### 验收

- TSan 对规则数组零撕裂告警。
- worker 运行期间改配置（触发 memcpy）不产生脏读；读到的规则要么全旧、要么全新。

---

## T3 · M1 原子写统一（O_NOFOLLOW + fsync）

### 现状

六处状态文件写点，`rename` 前均无 `fsync`，崩溃/掉电可能「先落 rename 后落数据块」，恢复出空/截断状态文件（`pulse_startup_recovery` 丢恢复记录 → App 无限期卡低内存上限；`restart` 日计数清零 → 突破 daymax）。

| # | 文件 | 写点 | O_NOFOLLOW | tmp+rename | 优先级 |
|---|---|---|---|---|---|
| 1 | status.txt | `write_status` fopen 1059 / rename 1070 | 缺 | 有 | 高 |
| 2 | mempressure_state.txt | `mempressure_write_state` fopen 1489 / rename 1496 | 缺 | 有 | 高 |
| 3 | pulse 状态 | `pulse_save_state_file` open 2588 / rename 2613 | 有 | 有 | 高 |
| 4 | restart 状态 | `restart_state_save` fopen 4327 / rename 4367 | 缺 | 有 | 高 |
| 5 | daemon.pid | `main` fopen 5612（**直接写，非 tmp+rename**） | 缺 | **无** | 高 |
| 6 | bands.txt | bands 处理 fopen 5661 / rename 5881 | 缺 | 有 | 低（瞬态缓存，丢可再生成） |

说明：互审员列出的五处即 #1–#5；#6 是额外发现的 tmp+rename 点，属瞬态缓存、损坏无害，**是否纳入统一处理由 Chief 定**（默认建议顺手统一，代价极低）。

### 修复方案

统一收口到一个提交助手，消除各点重复：

```c
/* 提交 tmp 内容到 final：flush → fsync(数据) → close → rename → fsync(目录) */
static int jp_atomic_commit(FILE *f, const char *tmp_path, const char *final_path);
```

- 内部：`fflush(f)` + `ferror(f)` 检查 → `fsync(fileno(f))`（对 open+fdopen 的点可直接 `fsync(fd)`）→ `fclose(f)` → `rename(tmp, final)` → `open(CONFIG_DIR, O_RDONLY|O_DIRECTORY)` 后 `fsync(dfd)` + `close(dfd)`，任一步失败清理 tmp 并返回 -1。
- #1/#2/#4：改用 `open(tmp, O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW, 0644)` + `fdopen`，替换现有 `fopen(tmp,"w")`，再走 `jp_atomic_commit`。
- #5 daemon.pid：**改为 tmp+rename**（写 `PID_PATH ".tmp"` 后 rename），并补 O_NOFOLLOW + fsync，避免 torn write 破坏 App 存活探测。
- #3：已有 O_NOFOLLOW，补 `fsync(fd)`（在 `fclose` 前）+ 目录 fsync 即可。
- 目录 fsync 只需对 `CONFIG_DIR` 做一次（所有目标文件都在该目录）。

### 验收

- 全部五处（+#6 若纳入）走统一提交助手；每处 `rename` 前数据已 `fsync`、`rename` 后目录已 `fsync`。
- 构造「写一半被 kill」无法得到空/截断的目标文件（旧版本文件保持完整）。
- daemon.pid 改成原子替换后，App 存活探测不受 torn write 影响。

---

## T4 · BadZipFile 优雅报错

### 现状

`scripts/verify-artifacts.py` `extract_app_dir`（369–379）直接 `zipfile.ZipFile(tipa_path)` 后 `extractall`，损坏 zip 会抛 `zipfile.BadZipFile` 未捕获 → 脚本崩溃（traceback 退出）。

### 修复方案

- 用 `try/except zipfile.BadZipFile`（及 `OSError`）包裹 370–371，捕获后打印带路径与原因的一行错误、返回明确的失败退出码（非 0），不抛 traceback。
- 其余 `ZipFile` 调用点若存在同样裸调用，一并包裹（先 grep 确认全部 `zipfile.ZipFile(` 位置）。

### 验收

- 传损坏 zip → 脚本输出清晰错误、以非 0 退出、无 traceback。
- 正常 zip 行为不变。

---

## 汇总

| 子任务 | 改动点（行） | 关键产物 | 状态 |
|---|---|---|---|
| T1 H1 | 2479/2480 类型、3626–3630、3767–3770、3794–3797、3929、3993–3997、4033–4036、4789、5390、5911–5919 | 原子完成标志 + defer-spawn 守卫 | 待实现 |
| T2 H2 | 新增 rwlock；1892/2194/5228 写锁；2402 内读点读锁 | 规则数组读写互斥 | 待实现 |
| T3 M1 | 新增 `jp_atomic_commit`；1059/1070、1489/1496、2588/2613、4327/4367、5612（+#6 待定） | 统一原子提交助手 + fsync | 待实现 |
| T4 BadZipFile | verify-artifacts.py 369–379 及其余 ZipFile 调用点 | 优雅报错 | 待实现 |

## 风险与回滚

- H1/H2 改动集中在 pulse/restart 线程同步，回滚 = 还原 jetpilotd.c 对应 hunk（git 保留原逻辑）。风险点：defer-spawn 会让 pulse 最多推迟一个主循环周期，可接受；rwlock 在规则加载频率极低下无性能影响。
- M1 的 fsync 会增加每轮写盘延迟（若 status 变化则每次写盘多两次 fsync）；与「E3 status 按需写」正交，后者落地后写盘频率下降，叠加无害。回滚 = 还原写点代码。
- daemon.pid 改 tmp+rename 属行为变更，需在真机验证 App 存活探测；失败回滚为原直接写。
