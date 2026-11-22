/* E1/E4 行为 harness：applied_* / proc_label / fast_round 系列 / 恢复函数逐字节提取自
 * src/jetpilotd.c，私有 API 用可编程 mock 替换。覆盖：applied 语义回归、快轮漂移矩阵、
 * E4 三级兜底链（CMD3→LISTSCAN→SLEEP→LEGACY）、P2 恢复路径、P2 seen 残留清零（部分进度降级）。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <time.h>

#define JP_LOG_ERR 1
#define JP_LOG_WARN 2
#define JP_LOG_INFO 3
#define JP_LOG_DEBUG 4
#define JPLOG(lvl, fmt, ...) do { printf("[LOG] " fmt "\n", ##__VA_ARGS__); } while (0)
#define ACT_KIND_RAISE  1
#define ACT_KIND_DEMOTE 2
static int g_dedup_clears[16]; static int g_ndedup_clears;
static void act_dedup_clear(pid_t pid) { if (g_ndedup_clears < 16) g_dedup_clears[g_ndedup_clears++] = (int)pid; }
static void act_dedup_flush_all(void) {}

typedef struct { int kind; int pid; char frag[256]; int cur; int target; } act_rec_t;
static act_rec_t g_acts[16]; static int g_nacts;
static void act_log_success(int kind, pid_t pid, const char *path, const char *frag,
                            int32_t cur, int32_t target) {
    (void)path;
    if (g_nacts < 16) {
        g_acts[g_nacts].kind = kind; g_acts[g_nacts].pid = (int)pid;
        snprintf(g_acts[g_nacts].frag, sizeof(g_acts[g_nacts].frag), "%s", frag ? frag : "");
        g_acts[g_nacts].cur = cur; g_acts[g_nacts].target = target;
        g_nacts++;
    }
}

#define PROC_PIDPATHINFO_MAXSIZE (4 * 1024)
#define MEMORYSTATUS_CMD_GET_PRIORITY_PROPERTIES 3
typedef struct {
    int initialized; int file_absent; ino_t ino; off_t size;
    struct timespec mtimespec; int open_failed;
} jp_file_stamp_t;
static unsigned long g_reassert_count = 0;

#define BAND_IDLE        0
#define BAND_BACKGROUND  30
#define BAND_FOREGROUND  100
#define BAND_MAX_PROTECT 95   /* 封顶，永不触及前台 100 与系统档 */

typedef struct memorystatus_priority_entry {
    pid_t    pid;
    int32_t  priority;
    uint64_t user_data;
    int32_t  limit;        /* MB */
    uint32_t state;
} memorystatus_priority_entry_t;

typedef struct memorystatus_priority_properties {
    int32_t  priority;
    uint64_t user_data;
} memorystatus_priority_properties_t;

static int mock_cmd3_mode;            /* 0=查表 1=EINVAL 2=ESRCH */
static pid_t mock_cmd3_fail_pid;      /* 非 0：该 pid 的 cmd3 返回 EINVAL（部分进度测试） */
static pid_t mock_prio_pid[16]; static int32_t mock_prio_val[16]; static int mock_nprio;
static int mock_probe_rc;             /* 启动探测返回值：0 ok / -1 fail */

int memorystatus_control(uint32_t command, int32_t pid, uint32_t flags, void *buffer, size_t size) {
    (void)flags; (void)size;
    if (command == 3) {
        if (pid == getpid()) {   /* 启动探测（自身 pid）走 mock_probe_rc */
            if (mock_probe_rc != 0) { errno = ENOSYS; return -1; }
            memorystatus_priority_properties_t *pp = (memorystatus_priority_properties_t *)buffer;
            pp->priority = 20; pp->user_data = 0;
            return 0;
        }
        if (mock_cmd3_fail_pid != 0 && pid == mock_cmd3_fail_pid) { errno = EINVAL; return -1; }
        if (mock_cmd3_mode == 1) { errno = EINVAL; return -1; }
        if (mock_cmd3_mode == 2) { errno = ESRCH; return -1; }
        for (int i = 0; i < mock_nprio; i++) {
            if (mock_prio_pid[i] == pid) {
                memorystatus_priority_properties_t *p = (memorystatus_priority_properties_t *)buffer;
                p->priority = mock_prio_val[i]; p->user_data = 0;
                return 0;
            }
        }
        errno = ESRCH; return -1;
    }
    return mock_probe_rc;
}

static int mock_set_ok = 1;
static pid_t mock_set_pid[16]; static int32_t mock_set_band[16]; static int mock_nset;
static int set_priority(pid_t pid, int32_t band) {
    if (!mock_set_ok) { errno = EPERM; return -1; }
    if (mock_nset < 16) { mock_set_pid[mock_nset] = pid; mock_set_band[mock_nset] = band; mock_nset++; }
    return 0;
}

static memorystatus_priority_entry_t mock_list[16]; static int mock_nlist; static int mock_list_fail;
static memorystatus_priority_entry_t *get_list(int *count_out) {
    if (mock_list_fail) return NULL;
    memorystatus_priority_entry_t *copy = malloc(sizeof(mock_list));
    memcpy(copy, mock_list, sizeof(mock_list));
    *count_out = mock_nlist;
    return copy;
}

static void proc_label(const char *path, char *out, size_t outsz) {
    const char *end = NULL;
    for (const char *p = strstr(path, ".appex/"); p; p = strstr(p + 1, ".appex/"))
        end = p;                                   /* 最后一个 .appex/ */
    if (!end) { snprintf(out, outsz, "main"); return; }
    const char *start = end;
    while (start > path && start[-1] != '/') start--;
    int n = (int)(end - start);
    if (n <= 0) { snprintf(out, outsz, "main"); return; }
    snprintf(out, outsz, "%.*s", n, start);        /* 超长自动截断 */
}

#define MAX_APPLIED 64
typedef struct {
    pid_t   pid;
    int32_t band;
    int     seen;
    int32_t orig;
    int     orig_known;
    int32_t sys_band;
    int     sys_band_known;
    /* ---- E1 新增：快轮身份缓存（慢轮填充/刷新，快轮只读不改） ----
     * 静态约 64 × 4.4KB ≈ 280KB BSS，守护进程可接受（设计 §2.2 二选一取静态） */
    int     has_identity;   /* 1 = 以下身份字段有效（慢轮建条目时填） */
    char    path[PROC_PIDPATHINFO_MAXSIZE]; /* 可执行路径（快轮重断言/日志用，免 proc_pidpath） */
    int32_t target;         /* protect/demote 的目标档（快轮重断言目标） */
    int     kind;           /* 0=protect(RAISE)，1=demote(DEMOTE) */
    char    label[64];      /* proc_label 结果（日志/status 用） */
    char    bid[128];       /* 主 App Bundle ID（status 用） */
    char    frag[256];      /* 匹配的规则片段（act_log_success 去重/日志用） */
} applied_t;
static applied_t g_applied[MAX_APPLIED];
static int g_napplied = 0;
/* E2: 新建 applied 条目计数（空转判定用：慢轮无新条目才算空转） */
static unsigned long g_applied_creates = 0;

static applied_t *applied_find(pid_t pid) {
    for (int i = 0; i < g_napplied; i++)
        if (g_applied[i].pid == pid) return &g_applied[i];
    return NULL;
}

/* applied_set_ex:
 * 语义说明：orig 是我们第一次改这个 pid 之前那一刻的档位，不代表「如果没有我们，它现在会是多少」。
 * 只有新建条目时才写 orig = cur_before, orig_known = 1；已有条目只更新 band。
 */
static void applied_set_ex(pid_t pid, int32_t band, int32_t cur_before) {
    applied_t *a = applied_find(pid);
    if (!a) {
        if (g_napplied >= MAX_APPLIED) return;  /* 表满就不记，只是失去降档能力 */
        a = &g_applied[g_napplied++];
        a->pid = pid;
        a->orig = cur_before;
        a->orig_known = 1;
        a->sys_band = 0;
        a->sys_band_known = 0;
        a->has_identity = 0;   /* E1: 身份缓存由慢轮调用点随后填充 */
        g_applied_creates++;   /* E2: 空转判定（仅计数，不改既有语义） */
    }
    a->band = band;
    a->seen = 1;
}

static void applied_set(pid_t pid, int32_t band) {
    applied_t *a = applied_find(pid);
    if (!a) {
        if (g_napplied >= MAX_APPLIED) return;
        a = &g_applied[g_napplied++];
        a->pid = pid;
        a->orig = 0;
        a->orig_known = 0;
        a->sys_band = 0;
        a->sys_band_known = 0;
        a->has_identity = 0;   /* E1: 同上 */
        g_applied_creates++;   /* E2: 同上 */
    }
    a->band = band;
    a->seen = 1;
}

/* 每轮结束清掉本轮没出现的 pid（进程已退出），防止 pid 复用误判。
 * ×N (end) 触发语义：沿用现有语义，applied_prune 每轮清理未出现 pid，
 * 若进程短暂一轮未进列表（如由于偶发枚举或过滤），也会提前触发 act_dedup_clear 冲刷输出 "(end)" 汇总。
 */
static void applied_prune(void) {
    int j = 0;
    for (int i = 0; i < g_napplied; i++) {
        if (g_applied[i].seen) {
            g_applied[i].seen = 0;
            g_applied[j++] = g_applied[i];
        } else {
            act_dedup_clear(g_applied[i].pid);
        }
    }
    g_napplied = j;
}

static void applied_remove(pid_t pid) {
    for (int i = 0; i < g_napplied; i++) {
        if (g_applied[i].pid == pid) {
            g_applied[i] = g_applied[--g_napplied];
            act_dedup_clear(pid);
            return;
        }
    }
    act_dedup_clear(pid);
}

/* =========================================================================
 * E1 快慢双节奏 / E2 自适应退避 / E4 定向查询兜底 (docs/optimization-design.md)
 * ========================================================================= */
/* E2: 退避常量集中定义（亮屏 5→60s；慢轮周期 30→240s；锁屏 10/60s 独立、不参与退避） */
#define BACKOFF_BASE_SECS 5
#define BACKOFF_MAX_SECS  60
#define SLOW_BASE_SECS    30
#define SLOW_MAX_SECS     240

/* E4: 快轮模式（三级兜底链）
 *   CMD3     命令 3 按 pid 定向查档位（正常路径，私有 API，不得硬依赖）
 *   LISTSCAN get_list() 全表取回后只遍历已知 pid（省 proc_pidpath，不省全表 syscall）
 *   SLEEP    快轮纯休眠，重断言交给慢轮（正确性不丢、只牺牲响应度）
 *   LEGACY   最坏回退现状：每轮全表扫（慢轮连续 3 次取不到全表时进入） */
#define FAST_MODE_CMD3     0
#define FAST_MODE_LISTSCAN 1
#define FAST_MODE_SLEEP    2
#define FAST_MODE_LEGACY   3

static int          g_fast_mode       = FAST_MODE_CMD3;
static int          g_fast_mode_env_forced = 0; /* JP_FAST_SCAN=legacy 用户显式回退，排除在自动恢复外 */
static time_t       g_slow_due_mono   = 0;              /* 下次慢轮到期（CLOCK_MONOTONIC 秒），0=首轮 */
static int          g_slow_period     = SLOW_BASE_SECS; /* 当前慢轮周期（30→240，E2 退避） */
static int          g_idle_rounds     = 0;              /* 连续空转轮数（翻倍指数） */
static int          g_interval        = BACKOFF_BASE_SECS; /* 亮屏休眠间隔（5→60，E2 退避） */
static int          g_last_protected_now = -1;          /* 上个慢轮 protected_now 基准（空转判定） */
static int          g_slow_list_fail_streak = 0;        /* 慢轮 get_list 连续失败计数（LEGACY 触发） */
static jp_file_stamp_t s_config_stamp = {0};            /* config.txt 变更戳（E1 慢轮判定） */

static time_t mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

/* E4: 启动探测 GET_PRIORITY_PROPERTIES(3) 可用性，决定快轮初始模式。
 * 运行时注入开关（真机验证/降级演练用）：
 *   JP_E4_PROBE_FAIL=1  模拟命令 3 不可用 → 快轮降级 LISTSCAN（验证 0→1）
 *   JP_FAST_SCAN=legacy 直接回退现状（验证最坏回退，每轮全表扫） */
static void fast_scan_probe(void) {
    const char *legacy = getenv("JP_FAST_SCAN");
    if (legacy && strcmp(legacy, "legacy") == 0) {
        g_fast_mode = FAST_MODE_LEGACY;
        g_fast_mode_env_forced = 1;   /* P2: 用户显式回退，不参与自动恢复 */
        JPLOG(JP_WARN, "%-6s env JP_FAST_SCAN=legacy -> full-scan every round", "E4");
        return;
    }
    if (getenv("JP_E4_PROBE_FAIL")) {
        g_fast_mode = FAST_MODE_LISTSCAN;
        JPLOG(JP_WARN, "%-6s env JP_E4_PROBE_FAIL -> fast=list-scan fallback", "E4");
        return;
    }
    memorystatus_priority_properties_t props;
    memset(&props, 0, sizeof(props));
    int rc = memorystatus_control(MEMORYSTATUS_CMD_GET_PRIORITY_PROPERTIES,
                                  getpid(), 0, &props, sizeof(props));
    if (rc == 0) {
        g_fast_mode = FAST_MODE_CMD3;
        JPLOG(JP_INFO, "%-6s probe ok cmd3 self_prio=%d -> fast=cmd3",
               "E4", (int)props.priority);
    } else {
        g_fast_mode = FAST_MODE_LISTSCAN;
        JPLOG(JP_WARN, "%-6s probe fail errno=%d -> fast=list-scan fallback", "E4", errno);
    }
}

/* E1: 慢轮填充/刷新 applied 条目身份缓存（快轮只读，不调本函数）。
 * 慢轮每次匹配都刷新：pid 复用导致 path 变化时由此自愈（设计 §2.4.3）。 */
static void applied_fill_identity(applied_t *a, const char *path, const char *frag,
                                  int32_t target, int kind, const char *bid) {
    if (!a) return;   /* 表满时 applied_find 为 NULL，安全 */
    a->has_identity = 1;
    strncpy(a->path, path ? path : "", sizeof(a->path) - 1);
    a->path[sizeof(a->path) - 1] = '\0';
    a->target = target;
    a->kind = kind;
    proc_label(a->path, a->label, sizeof(a->label));
    strncpy(a->bid, bid ? bid : "", sizeof(a->bid) - 1);
    a->bid[sizeof(a->bid) - 1] = '\0';
    strncpy(a->frag, frag ? frag : "", sizeof(a->frag) - 1);
    a->frag[sizeof(a->frag) - 1] = '\0';
}

/* E1: 快轮单条重断言。漂移响应矩阵与慢轮严格一致（不得比慢轮更激进）：
 *   protect: 仅 cur < target 时重新抬档（RAISE）；系统抬到 target 之上 = 慢轮 SKIP 语义，不动
 *   demote:  仅非前台且 cur > target 时压回（DEMOTE）；系统压到 target 之下不反向抬高
 *   前台档位（>=100）一律不碰，交慢轮。
 * 返回 1 表示发生了一次重断言（E2 空转判定用）。 */
static int fast_reassert_one(applied_t *a, int32_t cur) {
    if (cur >= BAND_FOREGROUND) {
        if (cur != a->band) { a->sys_band = cur; a->sys_band_known = 1; }
        return 0;
    }
    int do_reassert = (a->kind == 0) ? (cur < a->target) : (cur > a->target);
    if (!do_reassert) {
        /* 不重断言：仅记录系统档位漂移观察（与慢轮 prev->sys_band 语义一致） */
        if (cur != a->band) { a->sys_band = cur; a->sys_band_known = 1; }
        return 0;
    }
    if (set_priority(a->pid, a->target) == 0) {
        g_reassert_count++;
        a->sys_band = cur; a->sys_band_known = 1;   /* P3-1: 动作前记漂移观察，与慢轮语义对齐 */
        applied_set_ex(a->pid, a->target, cur);  /* 已有条目只更新 band，身份缓存保留 */
        act_log_success((a->kind == 0) ? ACT_KIND_RAISE : ACT_KIND_DEMOTE,
                        a->pid, a->path, a->frag, cur, a->target);
        return 1;
    }
    return 0;
}

/* E4/E1 快轮 · 模式 0：命令 3 逐已知 pid 定向查档位（O(已知数)，不取全表）。
 * 返回本轮重断言次数；-1 表示中途异常降级（本轮未 prune，防误剪存活条目）。 */
static int fast_round_cmd3(void) {
    int actions = 0;
    for (int i = 0; i < g_napplied; i++) {
        applied_t *a = &g_applied[i];
        memorystatus_priority_properties_t p;
        memset(&p, 0, sizeof(p));
        if (memorystatus_control(MEMORYSTATUS_CMD_GET_PRIORITY_PROPERTIES,
                                 a->pid, 0, &p, sizeof(p)) != 0) {
            if (errno == ESRCH) continue;   /* 进程退出：不置 seen，applied_prune 剪除 */
            /* E4 运行中异常失败（非 ESRCH）：整体降级到列表扫描模式。
             * P2（代码审核）：return 前清掉本轮已置的 seen 残留——本轮部分条目已
             * 成功置 seen=1，带着陈旧 seen 进下一轮 LISTSCAN 时，轮间退出的 pid
             * 会被 prune 多保留一轮（listscan 对不在全表的条目不清 seen）。
             * 清零后下一轮 LISTSCAN 从全表重新推导存活状态，死条目当轮剪除。
             * 本轮不 prune：防误剪存活进程（重断言动作不回滚，仅丢弃存活观察）。 */
            for (int k = 0; k < g_napplied; k++) g_applied[k].seen = 0;
            JPLOG(JP_WARN, "%-6s cmd3_fail pid=%d errno=%d -> fast=list-scan fallback",
                   "E4", (int)a->pid, errno);
            g_fast_mode = FAST_MODE_LISTSCAN;
            return -1;
        }
        a->seen = 1;
        if (!a->has_identity) continue;     /* 无身份缓存：只确认存活，重断言交慢轮 */
        actions += fast_reassert_one(a, p.priority);
    }
    applied_prune();
    return actions;
}

/* E4/E1 快轮 · 模式 1：get_list() 全表取回后只遍历已知 pid（省 proc_pidpath）。
 * get_list 失败 → 降级模式 2（快轮纯休眠）。返回重断言次数。 */
static int fast_round_listscan(void) {
    int count = 0;
    memorystatus_priority_entry_t *list = get_list(&count);
    if (!list) {
        JPLOG(JP_WARN, "%-6s list_fail -> fast=sleep-only (reassert by slow round)", "E4");
        g_fast_mode = FAST_MODE_SLEEP;
        return 0;
    }
    int actions = 0;
    for (int i = 0; i < g_napplied; i++) {
        applied_t *a = &g_applied[i];
        int32_t cur = -1;
        for (int j = 0; j < count; j++) {   /* 只找已知 pid，其余条目跳过 */
            if (list[j].pid == a->pid) { cur = list[j].priority; break; }
        }
        if (cur < 0) continue;              /* 不在全表：已退出，不置 seen → prune 剪除 */
        a->seen = 1;
        if (!a->has_identity) continue;
        actions += fast_reassert_one(a, cur);
    }
    free(list);
    applied_prune();
    return actions;
}

/* E1 快轮分发：按 g_fast_mode 选择实现；SLEEP 模式纯休眠（无动作、不 prune，
 * 退出条目交慢轮剪除）。 */
static int fast_round(void) {
    switch (g_fast_mode) {
    case FAST_MODE_CMD3:     return fast_round_cmd3();
    case FAST_MODE_LISTSCAN: return fast_round_listscan();
    case FAST_MODE_SLEEP:
    case FAST_MODE_LEGACY:
    default:                 return 0;
    }
}

/* E4/P2 兜底恢复：慢轮 get_list 成功时回升快轮模式。
 * SLEEP / 失败触发的 LEGACY 都可能是瞬时失败（get_list 是两次 syscall，内存压力下
 * 恰是快轮最该工作的时候），必须可恢复，不得一次失败永久休眠。
 * 恢复目标 LISTSCAN（不依赖私有 API；重回 CMD3 由下次重启的 fast_scan_probe 探测）。
 * JP_FAST_SCAN=legacy 为用户显式回退（g_fast_mode_env_forced），排除在自动恢复外。 */
static void fast_mode_recover_on_slow_success(void) {
    if (g_fast_mode_env_forced) return;
    if (g_fast_mode == FAST_MODE_SLEEP || g_fast_mode == FAST_MODE_LEGACY) {
        g_fast_mode = FAST_MODE_LISTSCAN;
        JPLOG(JP_INFO, "%-6s get_list_recovered -> fast=list-scan", "E4");
    }
}

static void reset_all(void) {
    g_napplied = 0; g_applied_creates = 0; g_nacts = 0; mock_nset = 0;
    mock_cmd3_mode = 0; mock_cmd3_fail_pid = 0; mock_nprio = 0; mock_probe_rc = 0;
    mock_list_fail = 0; mock_nlist = 0; mock_set_ok = 1;
    g_fast_mode = FAST_MODE_CMD3; g_fast_mode_env_forced = 0; g_reassert_count = 0;
    g_dedup_clears[0] = 0; g_ndedup_clears = 0;
    memset(mock_list, 0, sizeof(mock_list));
}

int main(void) {
    /* ===== A. applied 语义回归 ===== */
    reset_all();
    applied_set_ex(100, 90, 40);
    assert(g_napplied == 1 && g_applied[0].band == 90 && g_applied[0].orig == 40 && g_applied[0].orig_known == 1);
    assert(g_applied[0].has_identity == 0 && g_applied_creates == 1);
    applied_set_ex(100, 85, 90);
    assert(g_napplied == 1 && g_applied[0].band == 85 && g_applied_creates == 1);
    applied_set(200, 30);
    assert(g_applied_creates == 2 && g_applied[1].orig_known == 0);
    applied_t *pa = applied_find(100);
    applied_fill_identity(pa, "/var/containers/Bundle/Application/Foo.app/Foo", "com.foo", 90, 0, "com.foo.bid");
    assert(pa->has_identity == 1 && pa->kind == 0 && pa->target == 90);
    assert(strcmp(pa->frag, "com.foo") == 0 && strcmp(pa->bid, "com.foo.bid") == 0);
    assert(strcmp(pa->label, "main") == 0);
    g_applied[0].seen = 1; g_applied[1].seen = 1;
    applied_prune();
    assert(g_napplied == 2 && g_ndedup_clears == 0);
    g_applied[0].seen = 1;
    applied_prune();
    assert(g_napplied == 1 && g_applied[0].pid == 100 && g_dedup_clears[0] == 200);
    applied_remove(100);
    assert(g_napplied == 0);
    printf("[A] applied semantics OK (set/set_ex/find/prune/remove/creates)\n");

    /* ===== B. fast_round_cmd3 正常路径 ===== */
    reset_all();
    applied_set_ex(100, 90, 40); applied_fill_identity(applied_find(100), "/x/Foo.app/Foo", "com.foo", 90, 0, "b1");
    applied_set_ex(200, 30, 80); applied_fill_identity(applied_find(200), "/x/Bar.app/Bar", "com.bar", 30, 1, "b2");
    applied_set_ex(300, 90, 95); applied_fill_identity(applied_find(300), "/x/Baz.app/Baz", "com.baz", 90, 0, "b3");
    applied_set_ex(400, 90, 40); applied_fill_identity(applied_find(400), "/x/Quit.app/Quit", "com.q", 90, 0, "b4");
    applied_prune();   /* 模拟创建轮收尾（prune 重置 seen，对齐生产时序） */
    mock_nprio = 3;
    mock_prio_pid[0] = 100; mock_prio_val[0] = 40;
    mock_prio_pid[1] = 200; mock_prio_val[1] = 80;
    mock_prio_pid[2] = 300; mock_prio_val[2] = 95;
    mock_cmd3_mode = 0;
    int actions = fast_round();
    assert(g_fast_mode == FAST_MODE_CMD3);
    assert(actions == 2);
    assert(mock_nset == 2 && mock_set_pid[0] == 100 && mock_set_band[0] == 90);
    assert(mock_set_pid[1] == 200 && mock_set_band[1] == 30);
    assert(g_nacts == 2 && g_acts[0].kind == ACT_KIND_RAISE && strcmp(g_acts[0].frag, "com.foo") == 0 && g_acts[0].target == 90);
    assert(g_acts[1].kind == ACT_KIND_DEMOTE && strcmp(g_acts[1].frag, "com.bar") == 0 && g_acts[1].target == 30);
    assert(g_applied[0].band == 90 && g_applied[1].band == 30);
    assert(g_applied[0].has_identity == 1);
    /* P3-1：重断言动作前记录漂移观察（sys_band=动作前观察值） */
    assert(g_applied[0].sys_band == 40 && g_applied[0].sys_band_known == 1);
    assert(g_applied[1].sys_band == 80 && g_applied[1].sys_band_known == 1);
    int found300 = 0;
    for (int i = 0; i < g_napplied; i++) if (g_applied[i].pid == 300) { found300 = 1; assert(g_applied[i].sys_band == 95 && g_applied[i].sys_band_known == 1); }
    assert(found300);
    for (int i = 0; i < g_napplied; i++) assert(g_applied[i].pid != 400);
    printf("[B] fast cmd3 OK (raise/demote reassert, sys_band-before-action, ESRCH prune, identity kept)\n");

    /* ===== B2. P3-1：无身份条目（如 CLAIM）快轮只确认存活、不重断言 ===== */
    reset_all();
    applied_set(500, 30);
    applied_prune();
    mock_nprio = 1; mock_prio_pid[0] = 500; mock_prio_val[0] = 5;
    actions = fast_round();
    assert(actions == 0 && mock_nset == 0);
    assert(g_napplied == 1 && g_applied[0].pid == 500);
    printf("[B2] identity-less entry OK (alive-confirmed, no reassert — P3-1 CLAIM semantics)\n");

    /* ===== C. fast_round_cmd3 立即异常（第 0 条就失败）→ 降级、不 prune ===== */
    reset_all();
    applied_set_ex(100, 90, 40); applied_fill_identity(applied_find(100), "/x/Foo.app/Foo", "com.foo", 90, 0, "b1");
    applied_set_ex(200, 90, 40);
    applied_prune();
    mock_nprio = 2; mock_prio_pid[0] = 100; mock_prio_val[0] = 40;
    mock_prio_pid[1] = 200; mock_prio_val[1] = 90;
    mock_cmd3_mode = 1;
    actions = fast_round();
    assert(actions == -1 && g_fast_mode == FAST_MODE_LISTSCAN);
    assert(g_napplied == 2 && mock_nset == 0);
    for (int i = 0; i < g_napplied; i++) assert(g_applied[i].seen == 0);
    printf("[C] fast cmd3 immediate-error fallback OK (mode->LISTSCAN, no prune, seen clean)\n");

    /* ===== C2. P2：部分进度后异常降级——seen 残留清零，下一轮死条目被剪除 ===== */
    reset_all();
    applied_set_ex(100, 90, 40); applied_fill_identity(applied_find(100), "/x/Foo.app/Foo", "com.foo", 90, 0, "b1");
    applied_set_ex(200, 90, 40); applied_fill_identity(applied_find(200), "/x/Bar.app/Bar", "com.bar", 90, 0, "b2");
    applied_prune();
    mock_nprio = 2; mock_prio_pid[0] = 100; mock_prio_val[0] = 40;
    mock_prio_pid[1] = 200; mock_prio_val[1] = 40;
    mock_cmd3_fail_pid = 200;              /* entry0 成功（seen=1+重断言）、entry1 EINVAL：部分进度 */
    actions = fast_round();
    assert(actions == -1 && g_fast_mode == FAST_MODE_LISTSCAN);
    assert(g_napplied == 2);               /* 本轮不 prune（防误剪存活条目） */
    for (int i = 0; i < g_napplied; i++) assert(g_applied[i].seen == 0);  /* P2：seen 残留清零 */
    assert(mock_nset == 1 && mock_set_pid[0] == 100);  /* entry0 的重断言动作已发生（不回滚） */
    /* 轮间 pid200 退出：下一轮 LISTSCAN 全表中无 200 → 当轮剪除（修复前会多保留一轮） */
    mock_cmd3_fail_pid = 0;
    mock_nlist = 1; mock_list[0].pid = 100; mock_list[0].priority = 40;
    actions = fast_round();
    assert(actions == 1 && mock_nset == 2 && mock_set_pid[1] == 100);
    assert(g_napplied == 1 && g_applied[0].pid == 100);
    printf("[C2] partial-progress downgrade OK (seen cleared, dead entry pruned next round)\n");

    /* ===== D. fast_round_listscan：get_list 失败 → SLEEP；正常 → 只遍历已知 pid ===== */
    reset_all();
    g_fast_mode = FAST_MODE_LISTSCAN;
    applied_set_ex(100, 90, 40); applied_fill_identity(applied_find(100), "/x/Foo.app/Foo", "com.foo", 90, 0, "b1");
    mock_list_fail = 1;
    actions = fast_round();
    assert(actions == 0 && g_fast_mode == FAST_MODE_SLEEP);
    reset_all();
    g_fast_mode = FAST_MODE_LISTSCAN;
    applied_set_ex(100, 90, 40); applied_fill_identity(applied_find(100), "/x/Foo.app/Foo", "com.foo", 90, 0, "b1");
    applied_set_ex(300, 90, 90); applied_fill_identity(applied_find(300), "/x/Baz.app/Baz", "com.baz", 90, 0, "b3");
    applied_prune();
    mock_nlist = 2;
    mock_list[0].pid = 100; mock_list[0].priority = 40;
    mock_list[1].pid = 999; mock_list[1].priority = 50;
    actions = fast_round();
    assert(actions == 1 && mock_nset == 1 && mock_set_pid[0] == 100 && mock_set_band[0] == 90);
    assert(g_napplied == 1 && g_applied[0].pid == 100);
    printf("[D] fast listscan OK (known-pid only, list-fail->SLEEP)\n");

    /* ===== E. fast_scan_probe 启动探测与注入 ===== */
    reset_all();
    setenv("JP_E4_PROBE_FAIL", "1", 1);
    fast_scan_probe();
    assert(g_fast_mode == FAST_MODE_LISTSCAN && g_fast_mode_env_forced == 0);
    unsetenv("JP_E4_PROBE_FAIL");
    reset_all(); mock_probe_rc = 0;
    fast_scan_probe();
    assert(g_fast_mode == FAST_MODE_CMD3);
    reset_all(); mock_probe_rc = -1; errno = ENOSYS;
    fast_scan_probe();
    assert(g_fast_mode == FAST_MODE_LISTSCAN);
    reset_all();
    setenv("JP_FAST_SCAN", "legacy", 1);
    fast_scan_probe();
    assert(g_fast_mode == FAST_MODE_LEGACY && g_fast_mode_env_forced == 1);
    unsetenv("JP_FAST_SCAN");
    printf("[E] fast_scan_probe OK (probe fail 0->1, env-forced legacy flagged)\n");

    /* ===== F. SLEEP/LEGACY 分发为纯休眠 ===== */
    reset_all(); g_fast_mode = FAST_MODE_SLEEP;
    assert(fast_round() == 0 && mock_nset == 0);
    g_fast_mode = FAST_MODE_LEGACY;
    assert(fast_round() == 0 && mock_nset == 0);
    printf("[F] sleep/legacy dispatch OK\n");

    /* ===== G. P2 恢复路径：慢轮 get_list 成功 → SLEEP/失败 LEGACY 回升 LISTSCAN ===== */
    reset_all();
    g_fast_mode = FAST_MODE_SLEEP;
    fast_mode_recover_on_slow_success();
    assert(g_fast_mode == FAST_MODE_LISTSCAN);
    g_fast_mode = FAST_MODE_LEGACY; g_fast_mode_env_forced = 0;
    fast_mode_recover_on_slow_success();
    assert(g_fast_mode == FAST_MODE_LISTSCAN);
    g_fast_mode = FAST_MODE_LEGACY; g_fast_mode_env_forced = 1;
    fast_mode_recover_on_slow_success();
    assert(g_fast_mode == FAST_MODE_LEGACY);
    g_fast_mode_env_forced = 0;
    g_fast_mode = FAST_MODE_CMD3;
    fast_mode_recover_on_slow_success();
    assert(g_fast_mode == FAST_MODE_CMD3);
    g_fast_mode = FAST_MODE_LISTSCAN;
    applied_set_ex(100, 90, 40); applied_fill_identity(applied_find(100), "/x/Foo.app/Foo", "com.foo", 90, 0, "b1");
    applied_prune();
    mock_nlist = 1; mock_list[0].pid = 100; mock_list[0].priority = 40;
    actions = fast_round();
    assert(g_fast_mode == FAST_MODE_LISTSCAN && actions == 1);
    assert(mock_nset == 1 && mock_set_pid[0] == 100 && mock_set_band[0] == 90);
    printf("[G] P2 recovery OK (sleep/failed-legacy -> listscan, env-forced excluded, fast works again)\n");

    printf("ALL E1/E4 HARNESS TESTS OK\n");
    return 0;
}
