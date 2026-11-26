/* E1/E4 行为 harness：applied_* / proc_label / fast_round 系列 / 恢复函数逐字节提取自
 * src/jetpilotd.c，私有 API 用可编程 mock 替换。覆盖：applied 语义回归、快轮漂移矩阵、
 * E4 三级兜底链（PID1→LISTSCAN→SLEEP→LEGACY）、P2 恢复路径、P2 seen 残留清零（部分进度降级）、
 * 以及 XNU GET_PRIORITY_LIST(1, pid>0) 返回字节数与 entry.pid 强校验（防旧 cmd=3 空快照假 0 档）。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <time.h>

#define JP_ERR   1
#define JP_WARN  2
#define JP_INFO  3
#define JP_DEBUG 4
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
#define MEMORYSTATUS_CMD_GET_PRIORITY_LIST        1
#define MEMORYSTATUS_CMD_SET_PRIORITY_PROPERTIES  2
typedef struct {
    int initialized; int file_absent; ino_t ino; off_t size;
    struct timespec mtimespec; int open_failed;
} jp_file_stamp_t;
static unsigned long g_reassert_count = 0;

#define BAND_IDLE            0
#define BAND_BACKGROUND      30  /* 自然后台上界 / kill 允许上界 */
#define BAND_PROTECT_DEFAULT 90  /* 保护规则默认目标 (= BAND_FOREGROUND - 10) */
#define BAND_MAX_PROTECT     95  /* 保护封顶，永不触及前台档与系统抬升档 */
#define BAND_FOREGROUND      100 /* 前台：保护环绝不触碰 */
#define BAND_SYSTEM_ELEVATED 120 /* 系统抬升档（原字面量 120） */

_Static_assert(BAND_MAX_PROTECT < BAND_FOREGROUND &&
               BAND_PROTECT_DEFAULT == BAND_FOREGROUND - 10,
               "band constants drift: protect/candidate/foreground mismatch");

/* T3-IMP 3.3: applied 快照新鲜度上限（= SLOW_MAX_SECS） */
#define JP_SNAPSHOT_TTL_SECS 240

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

static int mock_cmd3_mode;            /* 0=查表 1=EINVAL 2=ESRCH 3=模拟旧cmd3空返回0且不写缓冲区 */
static pid_t mock_cmd3_fail_pid;      /* 非 0：该 pid 的单 pid 查询返回 EINVAL（部分进度测试） */
static pid_t mock_prio_pid[16]; static int32_t mock_prio_val[16]; static int mock_nprio;
static int mock_probe_rc;             /* 启动探测返回值：0 ok / -1 fail */

int memorystatus_control(uint32_t command, int32_t pid, uint32_t flags, void *buffer, size_t size) {
    (void)flags;
    if (command == MEMORYSTATUS_CMD_GET_PRIORITY_LIST && pid > 0) {
        if (size != sizeof(memorystatus_priority_entry_t) || !buffer) {
            errno = EINVAL;
            return -1;
        }
        if (mock_cmd3_mode == 3) {
            /* 模拟旧 GET_JETSAM_SNAPSHOT(cmd=3) 无快照时返回 0 字节且不触碰 buffer */
            return 0;
        }
        if (pid == getpid()) {   /* 启动探测（自身 pid）走 mock_probe_rc */
            if (mock_probe_rc != 0) { errno = ENOSYS; return -1; }
            memorystatus_priority_entry_t *pe = (memorystatus_priority_entry_t *)buffer;
            memset(pe, 0, sizeof(*pe));
            pe->pid = pid;
            pe->priority = 20;
            return (int)sizeof(*pe);
        }
        if (mock_cmd3_fail_pid != 0 && pid == mock_cmd3_fail_pid) { errno = EINVAL; return -1; }
        if (mock_cmd3_mode == 1) { errno = EINVAL; return -1; }
        if (mock_cmd3_mode == 2) { errno = ESRCH; return -1; }
        for (int i = 0; i < mock_nprio; i++) {
            if (mock_prio_pid[i] == pid) {
                memorystatus_priority_entry_t *pe = (memorystatus_priority_entry_t *)buffer;
                memset(pe, 0, sizeof(*pe));
                pe->pid = pid;
                pe->priority = mock_prio_val[i];
                return (int)sizeof(*pe);
            }
        }
        errno = ESRCH;
        return -1;
    }
    errno = ENOSYS;
    return -1;
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
    /* ---- T3-IMP 3.3: 快照新鲜度（CLOCK_MONOTONIC 秒，纯内存/不落盘） ---- */
    time_t  band_seen_mono;
    time_t  orig_seen_mono;
    time_t  sys_band_seen_mono;
    int     claim_origin;
    /* ---- E1 新增：快轮身份缓存（慢轮填充/刷新，快轮只读不改） ---- */
    int     has_identity;
    char    path[PROC_PIDPATHINFO_MAXSIZE];
    int32_t target;
    int     kind;
    char    label[64];
    char    bid[128];
    char    frag[256];
} applied_t;
static applied_t g_applied[MAX_APPLIED];
static int g_napplied = 0;
static unsigned long g_applied_creates = 0;

static applied_t *applied_find(pid_t pid) {
    for (int i = 0; i < g_napplied; i++)
        if (g_applied[i].pid == pid) return &g_applied[i];
    return NULL;
}

static time_t applied_mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static int jp_snapshot_fresh(time_t seen_mono, time_t now_mono) {
    return seen_mono > 0 && now_mono >= seen_mono &&
           (now_mono - seen_mono) <= JP_SNAPSHOT_TTL_SECS;
}

/* 定向现读当前有效档位（XNU memorystatus_cmd_get_priority_list 单 pid 分支，O(1)）；
 * 严格校验返回字节数 == sizeof(entry) 且 entry.pid == pid，杜绝未写入缓冲区假阳性；
 * 失败返回 -1 并保留内核原始 errno（进程退出为 ESRCH，协议不符置 EPROTO）。 */
static int jp_read_current_band(pid_t pid, int32_t *band_out) {
    if (pid <= 0 || !band_out) {
        errno = EINVAL;
        return -1;
    }
    memorystatus_priority_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    int ret = memorystatus_control(MEMORYSTATUS_CMD_GET_PRIORITY_LIST,
                                   pid, 0, &entry, sizeof(entry));
    if (ret < 0) return -1;
    if ((size_t)ret != sizeof(entry) || entry.pid != pid) {
        errno = EPROTO;
        return -1;
    }
    *band_out = entry.priority;
    return 0;
}

static int jp_is_foreground(int32_t band) {
    return band >= BAND_FOREGROUND;
}
static int jp_is_candidate(int32_t band) {
    return band >= BAND_IDLE && band <= BAND_MAX_PROTECT;
}
static int jp_kill_allowed(int32_t natural_band) {
    return natural_band <= BAND_BACKGROUND;
}
static int32_t jp_natural_band(pid_t pid, int32_t cur_now) {
    time_t now = applied_mono_now();
    applied_t *a = applied_find(pid);
    if (a && a->sys_band_known && jp_snapshot_fresh(a->sys_band_seen_mono, now))
        return a->sys_band;
    if (a && a->orig_known && jp_snapshot_fresh(a->orig_seen_mono, now))
        return a->orig;
    int32_t live = cur_now;
    if (jp_read_current_band(pid, &live) == 0) return live;
    return cur_now;
}

static void applied_note_sys_band(applied_t *a, int32_t cur) {
    if (!a) return;
    a->sys_band = cur;
    a->sys_band_known = 1;
    a->sys_band_seen_mono = applied_mono_now();
}

static void applied_observe_band(applied_t *a, int32_t cur) {
    if (!a) return;
    if (cur != a->band) {
        applied_note_sys_band(a, cur);
    } else {
        time_t now_m = applied_mono_now();
        if (jp_snapshot_fresh(a->band_seen_mono, now_m)) {
            a->band_seen_mono = now_m;
        }
    }
}

static int jp_attribution_trusted(const applied_t *a, time_t now_mono) {
    if (!a) return 0;
    if (!a->has_identity && !a->claim_origin) return 0;
    return jp_snapshot_fresh(a->band_seen_mono, now_mono);
}

static void applied_set_ex(pid_t pid, int32_t band, int32_t cur_before) {
    applied_t *a = applied_find(pid);
    time_t now = applied_mono_now();
    if (!a) {
        if (g_napplied >= MAX_APPLIED) return;
        a = &g_applied[g_napplied++];
        a->pid = pid;
        a->orig = cur_before;
        a->orig_known = 1;
        a->orig_seen_mono = now;
        a->sys_band = 0;
        a->sys_band_known = 0;
        a->sys_band_seen_mono = now;
        a->claim_origin = 0;
        a->has_identity = 0;
        g_applied_creates++;
    }
    a->band = band;
    a->band_seen_mono = now;
    a->seen = 1;
}

static void applied_set(pid_t pid, int32_t band) {
    applied_t *a = applied_find(pid);
    time_t now = applied_mono_now();
    if (!a) {
        if (g_napplied >= MAX_APPLIED) return;
        a = &g_applied[g_napplied++];
        a->pid = pid;
        a->orig = 0;
        a->orig_known = 0;
        a->orig_seen_mono = now;
        a->sys_band = 0;
        a->sys_band_known = 0;
        a->sys_band_seen_mono = now;
        a->claim_origin = 0;
        a->has_identity = 0;
        g_applied_creates++;
    }
    a->band = band;
    a->band_seen_mono = now;
    a->seen = 1;
}

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

#define BACKOFF_BASE_SECS 5
#define BACKOFF_MAX_SECS  60
#define SLOW_BASE_SECS    30
#define SLOW_MAX_SECS     JP_SNAPSHOT_TTL_SECS

#define FAST_MODE_PID1     0
#define FAST_MODE_CMD3     FAST_MODE_PID1
#define FAST_MODE_LISTSCAN 1
#define FAST_MODE_SLEEP    2
#define FAST_MODE_LEGACY   3

static int          g_fast_mode       = FAST_MODE_PID1;
static int          g_fast_mode_env_forced = 0;
static time_t       g_slow_due_mono   = 0;
static int          g_slow_period     = SLOW_BASE_SECS;
static int          g_idle_rounds     = 0;
static int          g_interval        = BACKOFF_BASE_SECS;
static int          g_last_protected_now = -1;
static int          g_slow_list_fail_streak = 0;
static int          g_fast_pulse_list_fail_streak = 0;
static jp_file_stamp_t s_config_stamp = {0};

static time_t mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static void fast_scan_probe(void) {
    const char *legacy = getenv("JP_FAST_SCAN");
    if (legacy && strcmp(legacy, "legacy") == 0) {
        g_fast_mode = FAST_MODE_LEGACY;
        g_fast_mode_env_forced = 1;
        JPLOG(JP_WARN, "%-6s env JP_FAST_SCAN=legacy -> full-scan every round", "E4");
        return;
    }
    if (getenv("JP_E4_PROBE_FAIL")) {
        g_fast_mode = FAST_MODE_LISTSCAN;
        JPLOG(JP_WARN, "%-6s env JP_E4_PROBE_FAIL -> fast=list-scan fallback", "E4");
        return;
    }
    int32_t self_prio = -1;
    if (jp_read_current_band(getpid(), &self_prio) == 0) {
        g_fast_mode = FAST_MODE_PID1;
        JPLOG(JP_INFO, "%-6s probe ok pid1 self_prio=%d -> fast=pid1",
               "E4", (int)self_prio);
    } else {
        g_fast_mode = FAST_MODE_LISTSCAN;
        JPLOG(JP_WARN, "%-6s probe fail errno=%d -> fast=list-scan fallback", "E4", errno);
    }
}

static void applied_fill_identity(applied_t *a, const char *path, const char *frag,
                                  int32_t target, int kind, const char *bid) {
    if (!a) return;
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

static int fast_reassert_one(applied_t *a, int32_t cur) {
    if (jp_is_foreground(cur)) {
        if (cur != a->band) applied_note_sys_band(a, cur);
        return 0;
    }
    int do_reassert = (a->kind == 0) ? (cur < a->target) : (cur > a->target);
    if (!do_reassert) {
        applied_observe_band(a, cur);
        return 0;
    }
    if (set_priority(a->pid, a->target) == 0) {
        g_reassert_count++;
        applied_note_sys_band(a, cur);
        applied_set_ex(a->pid, a->target, cur);
        act_log_success((a->kind == 0) ? ACT_KIND_RAISE : ACT_KIND_DEMOTE,
                        a->pid, a->path, a->frag, cur, a->target);
        return 1;
    }
    return 0;
}

static int fast_round_pid1(void) {
    int actions = 0;
    for (int i = 0; i < g_napplied; i++) {
        applied_t *a = &g_applied[i];
        int32_t cur = -1;
        if (jp_read_current_band(a->pid, &cur) != 0) {
            if (errno == ESRCH) continue;
            for (int k = 0; k < g_napplied; k++) g_applied[k].seen = 0;
            JPLOG(JP_WARN, "%-6s pid1_fail pid=%d errno=%d -> fast=list-scan fallback",
                   "E4", (int)a->pid, errno);
            g_fast_mode = FAST_MODE_LISTSCAN;
            return -1;
        }
        a->seen = 1;
        if (!a->has_identity) {
            applied_observe_band(a, cur);
            continue;
        }
        actions += fast_reassert_one(a, cur);
    }
    applied_prune();
    return actions;
}

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
        for (int j = 0; j < count; j++) {
            if (list[j].pid == a->pid) { cur = list[j].priority; break; }
        }
        if (cur < 0) continue;
        a->seen = 1;
        if (!a->has_identity) {
            applied_observe_band(a, cur);
            continue;
        }
        actions += fast_reassert_one(a, cur);
    }
    free(list);
    applied_prune();
    return actions;
}

static int fast_round(void) {
    switch (g_fast_mode) {
    case FAST_MODE_PID1:     return fast_round_pid1();
    case FAST_MODE_LISTSCAN: return fast_round_listscan();
    case FAST_MODE_SLEEP:
    case FAST_MODE_LEGACY:
    default:                 return 0;
    }
}

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
    g_fast_mode = FAST_MODE_PID1; g_fast_mode_env_forced = 0; g_reassert_count = 0;
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

    /* ===== B. fast_round_pid1 正常路径 ===== */
    reset_all();
    applied_set_ex(100, 90, 40); applied_fill_identity(applied_find(100), "/x/Foo.app/Foo", "com.foo", 90, 0, "b1");
    applied_set_ex(200, 30, 80); applied_fill_identity(applied_find(200), "/x/Bar.app/Bar", "com.bar", 30, 1, "b2");
    applied_set_ex(300, 90, 95); applied_fill_identity(applied_find(300), "/x/Baz.app/Baz", "com.baz", 90, 0, "b3");
    applied_set_ex(400, 90, 40); applied_fill_identity(applied_find(400), "/x/Quit.app/Quit", "com.q", 90, 0, "b4");
    applied_prune();
    mock_nprio = 3;
    mock_prio_pid[0] = 100; mock_prio_val[0] = 40;
    mock_prio_pid[1] = 200; mock_prio_val[1] = 80;
    mock_prio_pid[2] = 300; mock_prio_val[2] = 95;
    mock_cmd3_mode = 0;
    int actions = fast_round();
    assert(g_fast_mode == FAST_MODE_PID1);
    assert(actions == 2);
    assert(mock_nset == 2 && mock_set_pid[0] == 100 && mock_set_band[0] == 90);
    assert(mock_set_pid[1] == 200 && mock_set_band[1] == 30);
    assert(g_nacts == 2 && g_acts[0].kind == ACT_KIND_RAISE && strcmp(g_acts[0].frag, "com.foo") == 0 && g_acts[0].target == 90);
    assert(g_acts[1].kind == ACT_KIND_DEMOTE && strcmp(g_acts[1].frag, "com.bar") == 0 && g_acts[1].target == 30);
    assert(g_applied[0].band == 90 && g_applied[1].band == 30);
    assert(g_applied[0].has_identity == 1);
    assert(g_applied[0].sys_band == 40 && g_applied[0].sys_band_known == 1);
    assert(g_applied[1].sys_band == 80 && g_applied[1].sys_band_known == 1);
    int found300 = 0;
    for (int i = 0; i < g_napplied; i++) if (g_applied[i].pid == 300) { found300 = 1; assert(g_applied[i].sys_band == 95 && g_applied[i].sys_band_known == 1); }
    assert(found300);
    for (int i = 0; i < g_napplied; i++) assert(g_applied[i].pid != 400);
    printf("[B] fast pid1 OK (raise/demote reassert, sys_band-before-action, ESRCH prune, identity kept)\n");

    /* ===== B2. P3-1：无身份条目（如 CLAIM）快轮只读观察存活、不重断言 ===== */
    reset_all();
    applied_set(500, 30);
    applied_prune();
    mock_nprio = 1; mock_prio_pid[0] = 500; mock_prio_val[0] = 5;
    actions = fast_round();
    assert(actions == 0 && mock_nset == 0);
    assert(g_napplied == 1 && g_applied[0].pid == 500 && g_applied[0].sys_band == 5 && g_applied[0].sys_band_known == 1);
    printf("[B2] identity-less entry OK (alive-confirmed, observed sys_band, no reassert)\n");

    /* ===== C. fast_round_pid1 立即异常（第 0 条就失败）→ 降级、不 prune ===== */
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
    printf("[C] fast pid1 immediate-error fallback OK (mode->LISTSCAN, no prune, seen clean)\n");

    /* ===== C2. P2：部分进度后异常降级——seen 残留清零，下一轮死条目被剪除 ===== */
    reset_all();
    applied_set_ex(100, 90, 40); applied_fill_identity(applied_find(100), "/x/Foo.app/Foo", "com.foo", 90, 0, "b1");
    applied_set_ex(200, 90, 40); applied_fill_identity(applied_find(200), "/x/Bar.app/Bar", "com.bar", 30, 0, "b2");
    applied_prune();
    mock_nprio = 2; mock_prio_pid[0] = 100; mock_prio_val[0] = 40;
    mock_prio_pid[1] = 200; mock_prio_val[1] = 40;
    mock_cmd3_fail_pid = 200;
    actions = fast_round();
    assert(actions == -1 && g_fast_mode == FAST_MODE_LISTSCAN);
    assert(g_napplied == 2);
    for (int i = 0; i < g_napplied; i++) assert(g_applied[i].seen == 0);
    assert(mock_nset == 1 && mock_set_pid[0] == 100);
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
    assert(g_fast_mode == FAST_MODE_PID1);
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
    g_fast_mode = FAST_MODE_PID1;
    fast_mode_recover_on_slow_success();
    assert(g_fast_mode == FAST_MODE_PID1);
    g_fast_mode = FAST_MODE_LISTSCAN;
    applied_set_ex(100, 90, 40); applied_fill_identity(applied_find(100), "/x/Foo.app/Foo", "com.foo", 90, 0, "b1");
    applied_prune();
    mock_nlist = 1; mock_list[0].pid = 100; mock_list[0].priority = 40;
    actions = fast_round();
    assert(g_fast_mode == FAST_MODE_LISTSCAN && actions == 1);
    assert(mock_nset == 1 && mock_set_pid[0] == 100 && mock_set_band[0] == 90);
    printf("[G] P2 recovery OK (sleep/failed-legacy -> listscan, env-forced excluded, fast works again)\n");

    /* ===== H. T3-IMP 3.3: 快照 TTL / 现读兜底 / 归属可信条件 ===== */
    reset_all();
    time_t t0 = applied_mono_now();
    assert(jp_snapshot_fresh(t0, t0) && jp_snapshot_fresh(t0, t0 + JP_SNAPSHOT_TTL_SECS));
    assert(!jp_snapshot_fresh(t0, t0 + JP_SNAPSHOT_TTL_SECS + 1) &&
           !jp_snapshot_fresh(0, t0));
    assert(jp_kill_allowed(BAND_BACKGROUND));
    assert(!jp_kill_allowed(BAND_BACKGROUND + 1));
    assert(!jp_kill_allowed(BAND_PROTECT_DEFAULT));
    applied_set_ex(100, 90, 40);
    applied_t *ttl_a = applied_find(100);
    assert(ttl_a->orig_seen_mono > 0 && ttl_a->band_seen_mono > 0 &&
           ttl_a->sys_band_seen_mono > 0);
    ttl_a->sys_band = 75; ttl_a->sys_band_known = 1;
    assert(jp_natural_band(100, 20) == 75);
    assert(!jp_kill_allowed(jp_natural_band(100, 20)));
    ttl_a->sys_band_seen_mono = 0; ttl_a->orig_seen_mono = 0;
    mock_nprio = 1; mock_prio_pid[0] = 100; mock_prio_val[0] = 20;
    assert(jp_natural_band(100, 30) == 20);
    assert(jp_kill_allowed(jp_natural_band(100, 30)));
    mock_nprio = 0;
    assert(jp_natural_band(100, 30) == 30);
    assert(!jp_attribution_trusted(ttl_a, t0));
    ttl_a->claim_origin = 1;
    assert(jp_attribution_trusted(ttl_a, t0));
    assert(!jp_attribution_trusted(ttl_a, t0 + JP_SNAPSHOT_TTL_SECS + 1));
    printf("[H] snapshot TTL / live-read fallback / attribution trust OK\n");

    /* ===== I. 真机 P0 回归：未写入缓冲区的 0 返回值必须被拒绝，75 档未保护 App 不得被误判为 0 档 ===== */
    reset_all();
    mock_cmd3_mode = 3; /* 模拟旧 GET_JETSAM_SNAPSHOT(3) 返回 0 且不写 buffer */
    int32_t dummy_band = 99;
    assert(jp_read_current_band(10003, &dummy_band) == -1 && errno == EPROTO && dummy_band == 99);
    fast_scan_probe();
    assert(g_fast_mode == FAST_MODE_LISTSCAN); /* 探测必须拒绝假 0 并降级 LISTSCAN */
    /* 未受保护的 75 档支付宝 (applied_find==NULL)：现读被拒后必须回退 cur_now=75，严禁返回 0 绕过 bandgate */
    assert(jp_natural_band(10003, 75) == 75);
    assert(!jp_kill_allowed(jp_natural_band(10003, 75)));
    /* 正常单 pid 查询下：未保护 75 档进程现读返回真实 75，同样拒绝清理 */
    mock_cmd3_mode = 0;
    mock_nprio = 1; mock_prio_pid[0] = 10003; mock_prio_val[0] = 75;
    assert(jp_natural_band(10003, 75) == 75);
    assert(!jp_kill_allowed(jp_natural_band(10003, 75)));
    printf("[I] XNU single-pid byte-count/pid guard & 75-band bandgate regression OK\n");

    printf("ALL E1/E4 HARNESS TESTS OK\n");
    return 0;
}
