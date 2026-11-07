/*
 * jetpilotd.c — JetPilot 常驻进程
 *
 * 作用：周期性读取全系统 jetsam 档位，把「受保护」App（按 Bundle ID（`bid:`）或 bundle 路径片段匹配）
 *       的后台档位抬到目标值，让系统在内存紧张时先杀别的进程。
 *
 * 适配：iOS 16.x（档位数值自 iOS 16 起整体 ×10）。iPhone 11 / 16.4.1 主目标。
 * 前提：必须以 root 运行，且二进制带 com.apple.private.memorystatus
 *       与 com.apple.private.jetsam.modify-priority 权限（见 entitlements-daemon.plist）。
 *       这些权限要求按 XNU 一般规则推断，未在 16.4.1 上逐行核实。
 *
 * 编译（GitHub Actions macos-latest，见 .github/workflows/build-jetpilot.yml）：
 *   xcrun -sdk iphoneos clang -arch arm64 -O2 -Wall \
 *     -miphoneos-version-min=15.0 jetpilotd.c -o jetpilotd
 *   ldid -Sentitlements-daemon.plist jetpilotd
 *
 * 配置：/var/mobile/Library/JetPilot/config.txt
 *   每行一条规则，井号开头是注释。格式：
 *     protect <路径片段> <目标档位>
 *     demote  <路径片段>
 *   例：
 *     protect Reynard.app/ 90
 *     demote  SomeGame.app/
 *   停止：删除同目录下的 running.flag（App 的「停止」按钮就是做这个）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>
/* iOS SDK 没有 libproc.h，手动声明 proc_* 函数与 proc_bsdinfo */
#define PROC_PIDPATHINFO_MAXSIZE (4 * 1024)
extern int proc_pidpath(int pid, void *buffer, uint32_t buffersize);
extern int proc_name(int pid, void *buffer, uint32_t buffersize);
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <notify.h>   /* 锁屏状态：com.apple.springboard.lockstate（libSystem 自带） */
#include <sys/select.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <dispatch/dispatch.h>
#include <CoreFoundation/CoreFoundation.h>
#include <math.h>

/* kern.memorypressure_manual_trigger 相关定义
 * TEST_LOW_MEMORY_TRIGGER_ONE 来自 xnu-8796.101.5 kern_memorystatus_notify.c */
#define TEST_LOW_MEMORY_TRIGGER_ONE 1

/* rusage_info_v4 与 proc_pid_rusage 原型声明
 * struct rusage_info_v4 来自 <sys/resource.h>，iOS SDK 无 libproc.h 手动声明原型 */
#ifndef RUSAGE_INFO_V4
#define RUSAGE_INFO_V4 4
typedef void *rusage_info_t;
struct rusage_info_v4 {
    uint8_t  ri_uuid[16];
    uint64_t ri_user_time;
    uint64_t ri_system_time;
    uint64_t ri_pkg_idle_wkups;
    uint64_t ri_interrupt_wkups;
    uint64_t ri_pageins;
    uint64_t ri_wired_size;
    uint64_t ri_resident_size;
    uint64_t ri_phys_footprint;
    uint64_t ri_proc_start_abstime;
    uint64_t ri_proc_exit_abstime;
    uint64_t ri_child_user_time;
    uint64_t ri_child_system_time;
    uint64_t ri_child_pkg_idle_wkups;
    uint64_t ri_child_interrupt_wkups;
    uint64_t ri_child_pageins;
    uint64_t ri_child_elapsed_abstime;
    uint64_t ri_diskio_bytesread;
    uint64_t ri_diskio_byteswritten;
    uint64_t ri_cpu_time_qos_default;
    uint64_t ri_cpu_time_qos_maintenance;
    uint64_t ri_cpu_time_qos_background;
    uint64_t ri_cpu_time_qos_utility;
    uint64_t ri_cpu_time_qos_legacy;
    uint64_t ri_cpu_time_qos_user_initiated;
    uint64_t ri_cpu_time_qos_user_interactive;
    uint64_t ri_billed_system_time;
    uint64_t ri_serviced_system_time;
    uint64_t ri_logical_writes;
    uint64_t ri_lifetime_max_phys_footprint;
    uint64_t ri_instructions;
    uint64_t ri_cycles;
    uint64_t ri_billed_energy;
    uint64_t ri_serviced_energy;
    uint64_t ri_interval_max_phys_footprint;
    uint64_t ri_runnable_time;
};
#endif
extern int proc_pid_rusage(int pid, int flavor, rusage_info_t *buffer);

/* struct proc_bsdinfo 与 proc_pidinfo 原型声明
 * 用于读取进程启动时间 pbi_start_tvsec / pbi_start_tvusec (xnu-8796.101.5 bsd/sys/proc_info.h) */
#ifndef _SYS_PROC_INFO_H
#define PROC_PIDTBSDINFO 3
struct proc_bsdinfo {
    uint32_t  pbi_flags;
    uint32_t  pbi_status;
    uint32_t  pbi_xstatus;
    uint32_t  pbi_pid;
    uint32_t  pbi_ppid;
    uid_t     pbi_uid;
    gid_t     pbi_gid;
    uid_t     pbi_ruid;
    gid_t     pbi_rgid;
    uid_t     pbi_svuid;
    gid_t     pbi_svgid;
    uint32_t  rfu_1;
    char      pbi_comm[16];
    char      pbi_name[32];
    uint32_t  pbi_nfiles;
    uint32_t  pbi_pgid;
    uint32_t  pbi_pjobc;
    uint32_t  e_tdev;
    uint32_t  e_tpgid;
    int32_t   pbi_nice;
    uint64_t  pbi_start_tvsec;
    uint64_t  pbi_start_tvusec;
};
#endif
extern int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);

/* ---- XNU kern_memorystatus.h 相关定义（头文件不公开，这里自己声明） ---- */
#define MEMORYSTATUS_CMD_GET_PRIORITY_LIST        1
#define MEMORYSTATUS_CMD_SET_PRIORITY_PROPERTIES  2

/* iOS 16+ 档位参考值 */
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

extern int memorystatus_control(uint32_t command, int32_t pid, uint32_t flags,
                                void *buffer, size_t buffersize);

/* ---- 配置 ---- */
#define CONFIG_DIR  "/var/mobile/Library/JetPilot"
#define CONFIG_PATH CONFIG_DIR "/config.txt"
#define FLAG_PATH   CONFIG_DIR "/running.flag"
#define STATUS_PATH CONFIG_DIR "/status.txt"
#define PID_PATH    CONFIG_DIR "/daemon.pid"
#define JP_VERSION   "0.7.3"
#define LOG_PATH     CONFIG_DIR "/daemon.log"
#define LOG_OLD_PATH CONFIG_DIR "/daemon.log.1"
#define LOG_MAX_BYTES (256 * 1024)

static void jp_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void jp_log(const char *fmt, ...) {
    struct stat st;
    int existed = (stat(LOG_PATH, &st) == 0);
    if (existed && st.st_size > LOG_MAX_BYTES) {
        rename(LOG_PATH, LOG_OLD_PATH);          /* 覆盖旧的 .1 */
        existed = 0;
    }
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    if (!existed) {
        fchmod(fileno(f), 0644);
    }
    time_t t = time(NULL);
    char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));
    fprintf(f, "%s ", ts);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fclose(f);
}

#define MAX_SKIP_DEDUP 64
typedef struct {
    pid_t   pid;
    int32_t cur;
} skip_dedup_t;
static skip_dedup_t g_skip_dedup[MAX_SKIP_DEDUP];
static int g_skip_dedup_count = 0;
static int g_skip_dedup_head = 0;

static int skip_dedup_seen(pid_t pid, int32_t cur) {
    for (int i = 0; i < g_skip_dedup_count; i++) {
        if (g_skip_dedup[i].pid == pid) {
            if (g_skip_dedup[i].cur == cur) return 1;
            g_skip_dedup[i].cur = cur;
            return 0;
        }
    }
    if (g_skip_dedup_count < MAX_SKIP_DEDUP) {
        g_skip_dedup[g_skip_dedup_count].pid = pid;
        g_skip_dedup[g_skip_dedup_count].cur = cur;
        g_skip_dedup_count++;
    } else {
        g_skip_dedup[g_skip_dedup_head].pid = pid;
        g_skip_dedup[g_skip_dedup_head].cur = cur;
        g_skip_dedup_head = (g_skip_dedup_head + 1) % MAX_SKIP_DEDUP;
    }
    return 0;
}

/* 非 SKIP 路径：清掉该 pid 的去重项，去重只对连续 SKIP 生效 */
static void skip_dedup_clear(pid_t pid) {
    for (int i = 0; i < g_skip_dedup_count; i++) {
        if (g_skip_dedup[i].pid == pid) {
            g_skip_dedup_count--;
            g_skip_dedup[i] = g_skip_dedup[g_skip_dedup_count];
            return;
        }
    }
}

#define MAX_RULES 32
typedef struct {
    char    frag[256];
    int32_t target;   /* protect 用；demote 规则 target = -1 */
    int     is_bid;   /* 1 = 按 Bundle ID 匹配；0 = 按路径片段匹配 */
} rule_t;

static rule_t g_protect[MAX_RULES];  static int g_nprotect = 0;
static rule_t g_demote[MAX_RULES];   static int g_ndemote  = 0;
static int g_has_bid_rules = 0;      /* 是否存在 bid: 规则 */

#define MAX_BID_CACHE 64
typedef struct {
    char   app_dir[512];
    char   bid[128];
    time_t fail_time; /* CLOCK_MONOTONIC 读失败时间；成功为 0 */
    int    valid;
} bid_cache_entry_t;

static bid_cache_entry_t g_bid_cache[MAX_BID_CACHE];
static int g_bid_cache_head = 0;

/* 从进程路径获取主 App Bundle ID：
 * 截取第一个 .app/ 之前（含 .app）为主 App 目录，扩展归主 App。
 * 成功永久缓存；失败缓存 60 秒（CLOCK_MONOTONIC 过期重试）。无 .app/ 返回 NULL。 */
static const char *app_bid_for_path(const char *path) {
    if (!path) return NULL;
    const char *dot_app = strstr(path, ".app/");
    if (!dot_app) return NULL;

    size_t dirlen = (size_t)(dot_app + 4 - path); /* 包含 .app */
    if (dirlen >= 512) return NULL;
    char app_dir[512];
    memcpy(app_dir, path, dirlen);
    app_dir[dirlen] = '\0';

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    /* 1. 查找缓存 */
    int slot = -1;
    for (int i = 0; i < MAX_BID_CACHE; i++) {
        if (g_bid_cache[i].valid && strcmp(g_bid_cache[i].app_dir, app_dir) == 0) {
            if (g_bid_cache[i].bid[0] != '\0') {
                return g_bid_cache[i].bid; /* 成功永久有效 */
            }
            if (now.tv_sec - g_bid_cache[i].fail_time < 60) {
                return NULL; /* 失败在 60 秒内，直接返回 NULL */
            }
            /* 超过 60 秒过期，重新读取 */
            slot = i;
            break;
        }
    }

    /* 2. 读取 CFBundle */
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
                                                          (const UInt8 *)app_dir,
                                                          dirlen,
                                                          true);
    CFBundleRef bundle = url ? CFBundleCreate(kCFAllocatorDefault, url) : NULL;
    CFStringRef bidRef = bundle ? CFBundleGetIdentifier(bundle) : NULL;
    char read_bid[128] = {0};
    int success = 0;
    if (bidRef) {
        if (CFStringGetCString(bidRef, read_bid, sizeof(read_bid), kCFStringEncodingUTF8) && read_bid[0] != '\0') {
            success = 1;
        }
    }
    if (bundle) CFRelease(bundle);
    if (url) CFRelease(url);

    /* 3. 写入/更新缓存 */
    if (slot < 0) {
        slot = g_bid_cache_head;
        g_bid_cache_head = (g_bid_cache_head + 1) % MAX_BID_CACHE;
    }
    strncpy(g_bid_cache[slot].app_dir, app_dir, sizeof(g_bid_cache[slot].app_dir) - 1);
    g_bid_cache[slot].app_dir[sizeof(g_bid_cache[slot].app_dir) - 1] = '\0';
    g_bid_cache[slot].valid = 1;

    if (success) {
        strncpy(g_bid_cache[slot].bid, read_bid, sizeof(g_bid_cache[slot].bid) - 1);
        g_bid_cache[slot].bid[sizeof(g_bid_cache[slot].bid) - 1] = '\0';
        g_bid_cache[slot].fail_time = 0;
        return g_bid_cache[slot].bid;
    } else {
        g_bid_cache[slot].bid[0] = '\0';
        g_bid_cache[slot].fail_time = now.tv_sec;
        return NULL;
    }
}
static unsigned long g_reassert_count = 0;
static int g_locked_passes = 0;
/* set_priority 失败记录：供 App 在状态页红字展示（不用 stderr，常驻进程 setsid 后没人读） */
static unsigned long g_set_fail_total = 0;
static int      g_last_err_no = 0;
static char     g_last_err_text[128] = "";
static pid_t    g_last_err_pid = 0;
static int32_t  g_last_err_target = 0;
static char     g_last_err_time[64] = "";

/* 记录每个 pid 最后一次由我们设置的档位：
 * 只有当前档位等于我们上次设的值时才允许往下调，
 * 避免把系统给的前台 100 等档位误降 */
#define MAX_APPLIED 64
typedef struct { pid_t pid; int32_t band; int seen; } applied_t;
static applied_t g_applied[MAX_APPLIED];
static int g_napplied = 0;

static applied_t *applied_find(pid_t pid) {
    for (int i = 0; i < g_napplied; i++)
        if (g_applied[i].pid == pid) return &g_applied[i];
    return NULL;
}

static void applied_set(pid_t pid, int32_t band) {
    applied_t *a = applied_find(pid);
    if (!a) {
        if (g_napplied >= MAX_APPLIED) return;  /* 表满就不记，只是失去降档能力 */
        a = &g_applied[g_napplied++];
        a->pid = pid;
    }
    a->band = band;
    a->seen = 1;
}

/* 每轮结束清掉本轮没出现的 pid（进程已退出），防止 pid 复用误判 */
static void applied_prune(void) {
    int j = 0;
    for (int i = 0; i < g_napplied; i++) {
        if (g_applied[i].seen) {
            g_applied[i].seen = 0;
            g_applied[j++] = g_applied[i];
        }
    }
    g_napplied = j;
}

static void applied_remove(pid_t pid) {
    for (int i = 0; i < g_napplied; i++)
        if (g_applied[i].pid == pid) {
            g_applied[i] = g_applied[--g_napplied];
            return;
        }
}

#define MAX_OLD_TARGETS 64
typedef struct {
    pid_t   pid;
    int32_t target;
} old_target_t;
static old_target_t g_old_targets[MAX_OLD_TARGETS];
static int g_nold_targets = 0;

static int old_target_find(pid_t pid, int32_t *target_out) {
    for (int i = 0; i < g_nold_targets; i++) {
        if (g_old_targets[i].pid == pid) {
            if (target_out) *target_out = g_old_targets[i].target;
            return 1;
        }
    }
    return 0;
}

/* 启动时导入旧 status.txt 记录供首轮认领：
 * 只有比开机时间新的 status.txt 才导入，防止重启手机后误降档 */
static void load_old_targets(void) {
    struct stat st;
    if (stat(STATUS_PATH, &st) != 0) return;  /* 没有旧文件，不导入 */

    struct timeval boottime;
    size_t len = sizeof(boottime);
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (sysctl(mib, 2, &boottime, &len, NULL, 0) != 0) return;  /* 取不到，不导入 */
    if (boottime.tv_sec == 0) return;  /* 取到0，不导入，更保守 */

    /* 旧文件不比开机新 → 丢弃 */
    if (st.st_mtime <= boottime.tv_sec) return;

    FILE *f = fopen(STATUS_PATH, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "protected_app=", 14) != 0) continue;
        char *p = line + 14;
        int pid = 0, cur = 0, target = 0;
        /* 从右往左找3个|，frag 含 | 也不错位；格式: frag|pid|cur|target */
        char *b3 = strrchr(p, '|');
        char *b2 = NULL, *b1 = NULL;
        if (b3) {
            for (char *q = b3 - 1; q >= p; q--) if (*q == '|') { b2 = q; break; }
        }
        if (b2) {
            for (char *q = b2 - 1; q >= p; q--) if (*q == '|') { b1 = q; break; }
        }
        if (!b1 || !b2 || !b3) continue;
        if (sscanf(b1 + 1, "%d", &pid) != 1) continue;
        if (sscanf(b2 + 1, "%d", &cur) != 1) continue;
        if (sscanf(b3 + 1, "%d", &target) != 1) continue;
        if (target <= BAND_BACKGROUND || target > BAND_MAX_PROTECT) continue;
        if (pid > 0 && g_nold_targets < MAX_OLD_TARGETS) {
            int found = 0;
            for (int i = 0; i < g_nold_targets; i++) {
                if (g_old_targets[i].pid == (pid_t)pid) {
                    g_old_targets[i].target = target;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                g_old_targets[g_nold_targets].pid = (pid_t)pid;
                g_old_targets[g_nold_targets].target = target;
                g_nold_targets++;
            }
        }
    }
    fclose(f);
}

static int load_config(void) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return -1;
    g_nprotect = g_ndemote = 0;
    g_has_bid_rules = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        char verb[32], frag[256]; int band = 0;
        int n = sscanf(p, "%31s %255s %d", verb, frag, &band);
        if (n < 2) continue;
        int is_bid = (strncmp(frag, "bid:", 4) == 0);
        if (strcmp(verb, "protect") == 0 && g_nprotect < MAX_RULES) {
            if (n < 3 || band <= 0) band = BAND_FOREGROUND - 10; /* 默认 90 */
            if (band < BAND_BACKGROUND) band = BAND_BACKGROUND;   /* 与 App 滑杆最小值 30 一致 */
            if (band > BAND_MAX_PROTECT) band = BAND_MAX_PROTECT;
            strncpy(g_protect[g_nprotect].frag, frag, 255);
            g_protect[g_nprotect].target = band;
            g_protect[g_nprotect].is_bid = is_bid;
            if (is_bid) g_has_bid_rules = 1;
            g_nprotect++;
        } else if (strcmp(verb, "demote") == 0 && g_ndemote < MAX_RULES) {
            strncpy(g_demote[g_ndemote].frag, frag, 255);
            g_demote[g_ndemote].target = -1;
            g_demote[g_ndemote].is_bid = is_bid;
            if (is_bid) g_has_bid_rules = 1;
            g_ndemote++;
        }
    }
    fclose(f);
    return 0;
}

static int should_stop(void) {
    if (access(FLAG_PATH, F_OK) != 0) return 1;  /* 标记文件被删 */
    FILE *f = fopen(CONFIG_PATH, "r");
    if (f) {
        char first[16] = {0};
        if (fgets(first, sizeof(first), f)) {
            if (strncmp(first, "stop", 4) == 0) { fclose(f); return 1; }
        }
        fclose(f);
    }
    return 0;
}

static int set_priority(pid_t pid, int32_t band) {
    memorystatus_priority_properties_t props = { band, 0 };
    int rc = memorystatus_control(MEMORYSTATUS_CMD_SET_PRIORITY_PROPERTIES,
                                  pid, 0, &props, sizeof(props));
    if (rc != 0) {
        /* EPERM 多半是缺权限；记下来写进 status.txt，App 端红字展示 */
        g_set_fail_total++;
        g_last_err_no = errno;
        {
            /* 错误文本一并记录；管道符替换掉以免破坏字段分隔 */
            const char *es = strerror(errno);
            size_t n = 0;
            while (es[n] && n < sizeof(g_last_err_text) - 1) {
                char c = es[n];
                g_last_err_text[n] = (c == '|') ? '/' : c;
                n++;
            }
            g_last_err_text[n] = '\0';
        }
        g_last_err_pid = pid;
        g_last_err_target = band;
        time_t t = time(NULL);
        strftime(g_last_err_time, sizeof(g_last_err_time),
                 "%Y-%m-%d %H:%M:%S", localtime(&t));
        fprintf(stderr, "[JetPilot] set pid %d -> %d 失败: %s\n",
                pid, band, strerror(errno));
    }
    return rc;
}

/* 取回全系统档位表，调用方负责 free */
static memorystatus_priority_entry_t *get_list(int *count_out) {
    int size = memorystatus_control(MEMORYSTATUS_CMD_GET_PRIORITY_LIST, 0, 0, NULL, 0);
    if (size <= 0) return NULL;
    /* 多留一点，列表可能在两次调用之间变长 */
    size += sizeof(memorystatus_priority_entry_t) * 16;
    memorystatus_priority_entry_t *buf = malloc(size);
    if (!buf) return NULL;
    int ret = memorystatus_control(MEMORYSTATUS_CMD_GET_PRIORITY_LIST, 0, 0, buf, size);
    if (ret <= 0) { free(buf); return NULL; }
    *count_out = ret / sizeof(memorystatus_priority_entry_t);
    return buf;
}

static int path_matches(const char *path, const char *app_bid,
                        rule_t *rules, int n,
                        int32_t *target_out, const char **frag_out) {
    for (int i = 0; i < n; i++) {
        if (rules[i].is_bid) {
            if (app_bid && strcmp(app_bid, rules[i].frag + 4) == 0) {
                if (target_out) *target_out = rules[i].target;
                if (frag_out) *frag_out = rules[i].frag;
                return 1;
            }
        } else {
            if (strstr(path, rules[i].frag)) {
                if (target_out) *target_out = rules[i].target;
                if (frag_out) *frag_out = rules[i].frag;
                return 1;
            }
        }
    }
    return 0;
}

/* status.txt 格式（App 端解析）：
 *   原有 key=value 行保持不变；新增 protected_app 行（可多行）：
 *     bid_support=1
 *     protected_app=<路径片段>|<pid>|<当前档位>|<目标档位>
 *     protected_proc=<pid>|<标签>|<bid>
 *   旧版 App 忽略不认识的行；新版 App 读不到这些行时显示"暂无数据"。 */
#define MAX_PROTECTED_LIST 64
typedef struct {
    const char *frag;   /* 指向 g_protect 规则的 frag，本轮 scan 内有效 */
    pid_t       pid;
    int32_t     cur;
    int32_t     target;
    char        label[64];  /* "main" 或扩展名（不带 .appex） */
    char        bid[128];   /* 主 App Bundle ID，读不到为空串 */
} protected_entry_t;

/* 从可执行路径得出进程标签：主程序 "main"，扩展取 xxx.appex 的 xxx */
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

/* 日志标签辅助：单线程静态缓冲区，调用 proc_label 提取主程序或扩展名 */
static const char *proc_label_log(const char *path) {
    if (!path) return "main";
    static char buf[64];
    proc_label(path, buf, sizeof(buf));
    return buf;
}

/* status.txt 按需写：内容不变时最多每 STATUS_HEARTBEAT_SECS 秒写一次（心跳），省写盘 */
#define STATUS_HEARTBEAT_SECS 60
static char  *g_last_body = NULL;   /* 上次成功写盘的内容（不含 updated= 行） */
static size_t g_last_body_len = 0;
static time_t g_last_write = 0;     /* 上次成功写盘的 CLOCK_MONOTONIC 秒 */

static void write_status(int protected_now, int demoted_now,
                         protected_entry_t *plist, int nplist) {
    /* 1. 先在内存里拼出除 updated= 以外的全部内容 */
    char *body = NULL; size_t blen = 0;
    FILE *m = open_memstream(&body, &blen);
    if (!m) return;
    fprintf(m, "bid_support=1\n"
               "protect_rules=%d\ndemote_rules=%d\n"
               "protected_pids=%d\ndemoted_pids=%d\nreasserts_total=%lu\n",
            g_nprotect, g_ndemote, protected_now, demoted_now, g_reassert_count);
    /* 失败记录：无失败时只写总数 0，App 端隐藏红色区域 */
    fprintf(m, "set_fail_total=%lu\n", g_set_fail_total);
    if (g_set_fail_total > 0)
        fprintf(m, "last_error=%d|%s|%d|%d|%s\n",
                g_last_err_no, g_last_err_text,
                (int)g_last_err_pid, g_last_err_target, g_last_err_time);
    for (int i = 0; i < nplist; i++)
        fprintf(m, "protected_app=%s|%d|%d|%d\n",
                plist[i].frag, (int)plist[i].pid, plist[i].cur, plist[i].target);
    for (int i = 0; i < nplist; i++)
        fprintf(m, "protected_proc=%d|%s|%s\n",
                (int)plist[i].pid, plist[i].label, plist[i].bid);
    int merr = ferror(m);
    fclose(m);                         /* fclose 后 body/blen 才有效 */
    if (merr || !body) { free(body); return; }

    /* 2. 内容没变且心跳未到 → 不写盘 */
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    int same = g_last_body && blen == g_last_body_len &&
               memcmp(body, g_last_body, blen) == 0;
    if (same && now.tv_sec - g_last_write < STATUS_HEARTBEAT_SECS) { free(body); return; }

    /* 3. 写盘：先写临时文件再 rename，原子替换：App 永远读不到写一半的 status.txt */
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", STATUS_PATH);
    FILE *f = fopen(tmp, "w");
    if (!f) { free(body); return; }
    time_t t = time(NULL);
    char ts[64]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));
    fprintf(f, "updated=%s\n", ts);
    fwrite(body, 1, blen, f);
    int werr = ferror(f);  /* 必须在 fclose 之前查 */
    fclose(f);
    if (werr) { unlink(tmp); free(body); return; }  /* 写坏了就不转正，保留旧 status.txt */
    if (rename(tmp, STATUS_PATH) != 0) { unlink(tmp); free(body); return; }
    /* 4. 成功才记账；失败下轮重试 */
    free(g_last_body);
    g_last_body = body; g_last_body_len = blen;
    g_last_write = now.tv_sec;
}

static int g_locked = 0;
static int g_lock_token = -1;
static int g_lock_fd = -1;

static void lock_refresh(void) {
    uint64_t st = 0;
    if (g_lock_token >= 0 &&
        notify_get_state(g_lock_token, &st) == NOTIFY_STATUS_OK)
        g_locked = (st != 0);
}

static void lockmon_init(void) {
    if (notify_register_file_descriptor("com.apple.springboard.lockstate",
            &g_lock_fd, 0, &g_lock_token) != NOTIFY_STATUS_OK) {
        g_lock_fd = -1; g_lock_token = -1;   /* fail-open：保持常速 */
        return;
    }
    lock_refresh();
}

/* 睡 secs 秒，锁屏状态一变就提前醒。返回 1 表示状态变了 */
static int wait_lock_change(int secs) {
    if (g_lock_fd < 0) { sleep(secs); return 0; }
    fd_set rf; FD_ZERO(&rf); FD_SET(g_lock_fd, &rf);
    struct timeval tv = { secs, 0 };
    if (select(g_lock_fd + 1, &rf, NULL, NULL, &tv) > 0) {
        int tok;
        while (read(g_lock_fd, &tok, sizeof(tok)) == sizeof(tok)) {
            /* 一次可能来多条，读空即可 */
            fd_set r2; FD_ZERO(&r2); FD_SET(g_lock_fd, &r2);
            struct timeval z = {0, 0};
            if (select(g_lock_fd + 1, &r2, NULL, NULL, &z) <= 0) break;
        }
        int was = g_locked;
        lock_refresh();
        return was != g_locked;
    }
    return 0;
}

/* =========================================================================
 * 前台 App 内存压力触发模块 (v0.7.0)
 * ========================================================================= */

#define MEMP_CONFIG_PATH   CONFIG_DIR "/mempressure.txt"
#define MEMP_PENDING_PATH  CONFIG_DIR "/mempressure_pending"
#define MEMP_STATE_PATH    CONFIG_DIR "/mempressure_state.txt"
#define MEMP_TAINT_PATH    CONFIG_DIR "/mempressure_taint.txt"
#define MEMP_FATAL_PATH    CONFIG_DIR "/mempressure_fatal"
#define MEMP_FLOOR_PATH    CONFIG_DIR "/mempressure_floor.txt"
#define MEMP_FLOOR_TMP     CONFIG_DIR "/mempressure_floor.txt.tmp"

#define MEMP_MAX_RULES     32
#define MEMP_MAX_TAINT     8
#define MEMP_MAX_YIELD     32
#define MEMP_DEFAULT_COOLDOWN 600
#define MEMP_MIN_COOLDOWN     60
#define MEMP_MAX_COOLDOWN     86400
#define MEMP_MIN_THR_MB       100
#define MEMP_DEFAULT_THR_MB   600

#define MEMP_MAX_FLOOR_ENTRIES (MEMP_MAX_RULES + 16)
#define MEMP_MAX_FLOOR_SAMPLES 5

typedef enum {
    MEMP_STATE_OFF = 0,
    MEMP_STATE_ON,
    MEMP_STATE_DISABLED,
    MEMP_STATE_UNSUPPORTED,
    MEMP_STATE_FATAL
} memp_state_t;

typedef enum {
    MEMP_RESET_PENDING_NONE = 0,
    MEMP_RESET_PENDING_NORMAL,
    MEMP_RESET_PENDING_MARKER
} memp_reset_pending_t;

typedef struct {
    char    frag[256];
    int32_t thr_mb;       /* 阈值 MB (100 .. memcap) */
    int32_t cooldown;     /* 单条冷却秒 (60 .. 86400, 0 = 用全局) */
    int     is_bid;       /* 1 = bid: 开头, 0 = 路径片段 */
    time_t  last_warn_sec;/* CLOCK_MONOTONIC 秒，冷却起点 */
    time_t  last_skip_sec;/* 上次输出 skip 日志的 CLOCK_MONOTONIC 秒 (限频 60s) */
    int     ineff;        /* 连续无效次数 (freed < 30) */
    int32_t eff_cd;       /* 有效冷却 (退避后可能翻倍) */
} memp_rule_t;

static memp_rule_t g_memp_rules[MEMP_MAX_RULES];
static int g_nmemp_rules = 0;
static int g_memp_cooldown = MEMP_DEFAULT_COOLDOWN;
static int g_memp_cfg_file_absent = 1;
static int g_memp_cfg_first_load = 1;

/* 底线记录表 */
typedef struct {
    char   frag[256];
    time_t last_time;
    int    nsamp;
    double samples[MEMP_MAX_FLOOR_SAMPLES];
} memp_floor_entry_t;

static memp_floor_entry_t g_memp_floors[MEMP_MAX_FLOOR_ENTRIES];
static int g_nmemp_floors = 0;
static void memp_floor_load(void);

static uint64_t g_memp_memcap = 2048;
static int g_memp_memcap_fallback = 0;

static memp_state_t g_memp_state = MEMP_STATE_OFF;
static memp_reset_pending_t g_memp_reset_pending = MEMP_RESET_PENDING_NONE;
static int g_memp_fails = 0;
static int g_memp_last_errno = 0;
static char g_memp_fatal_reason[32] = "";
static char g_memp_unsupported_reason[32] = "";

static int g_memp_reset_retry_count = 0;
static time_t g_memp_last_reset_retry_time = 0;
static struct timespec g_memp_warn_monotonic = {0, 0};
static int g_memp_reset_overdue_logged = 0;

static int g_memp_marker_rm_fail_count = 0;
static time_t g_memp_last_marker_rm_fail_time = 0;

static int g_memp_level_first_logged = 0;

/* 观察槽（§八） */
typedef struct {
    int      valid;
    char     frag[256];
    pid_t    pid;
    uint64_t start_tvsec;
    uint64_t start_tvusec;
    time_t   warn_mono_sec;
    double   fp_before_mb;
    int      band_warn;
    int      fg_ok;
    int      nsamp;
} memp_post_slot_t;

static memp_post_slot_t g_memp_post = {0};

/* 污染表（§五-6） */
typedef struct {
    pid_t    pid;
    uint64_t tvsec;
    uint64_t tvusec;
    char     frag[256];
} memp_taint_entry_t;

static memp_taint_entry_t g_memp_taint[MEMP_MAX_TAINT];
static int g_memp_ntaint = 0;

/* 修复限频：每个 pid 上次尝试修复的时间（60 秒最多一次）与连续失败计数 */
#define MEMP_MAX_REPAIR_SEEN 16
typedef struct {
    pid_t  pid;
    time_t last_repair_sec;
    int    fail_count;
} memp_repair_seen_t;
static memp_repair_seen_t g_memp_repair_seen[MEMP_MAX_REPAIR_SEEN];
static int g_memp_nrepair_seen = 0;

/* 目标识别初次记录（§九 target） */
#define MEMP_MAX_SEEN_TARGETS 64
static pid_t g_memp_seen_targets[MEMP_MAX_SEEN_TARGETS];
static int g_memp_nseen_targets = 0;

/* rusage 失败限频：同一 pid 每 60 秒最多一行 */
#define MEMP_MAX_RUSAGE_FAIL_SEEN 16
typedef struct {
    pid_t  pid;
    time_t last_fail_sec;
} memp_rusage_fail_seen_t;
static memp_rusage_fail_seen_t g_memp_rusage_fail_seen[MEMP_MAX_RUSAGE_FAIL_SEEN];
static int g_memp_nrusage_fail_seen = 0;

/* 修复跳过限频：同一 pid 每 60 秒最多一行 */
#define MEMP_MAX_REPAIR_SKIP_SEEN 16
typedef struct {
    pid_t  pid;
    time_t last_fail_sec;
} memp_repair_skip_seen_t;
static memp_repair_skip_seen_t g_memp_repair_skip_seen[MEMP_MAX_REPAIR_SKIP_SEEN];
static int g_memp_nrepair_skip_seen = 0;

static void memp_log_repair_skip_fp_fail(pid_t pid, time_t now_sec) {
    for (int i = 0; i < g_memp_nrepair_skip_seen; i++) {
        if (g_memp_repair_skip_seen[i].pid == pid) {
            if (now_sec - g_memp_repair_skip_seen[i].last_fail_sec < 60) {
                return;
            }
            g_memp_repair_skip_seen[i].last_fail_sec = now_sec;
            jp_log("MEMP repair_skip reason=fp_fail pid=%d", (int)pid);
            return;
        }
    }
    int idx = 0;
    if (g_memp_nrepair_skip_seen < MEMP_MAX_REPAIR_SKIP_SEEN) {
        idx = g_memp_nrepair_skip_seen++;
    } else {
        idx = 0;
    }
    g_memp_repair_skip_seen[idx].pid = pid;
    g_memp_repair_skip_seen[idx].last_fail_sec = now_sec;
    jp_log("MEMP repair_skip reason=fp_fail pid=%d", (int)pid);
}

/* 让位记录项 */
typedef struct {
    pid_t    pid;
    uint64_t tvsec;
    uint64_t tvusec;
    int      is_sys; /* 是否为 B 类系统/非保护让位进程 (1=B类, 0=A类) */
} memp_yielded_entry_t;

/* 格式化 errno: 0 -> 0(-)，非0 -> <n>(<strerror>) */
static void memp_fmt_errno(int err, char *buf, size_t sz) {
    if (err == 0) {
        snprintf(buf, sz, "0(-)");
    } else {
        const char *s = strerror(err);
        snprintf(buf, sz, "%d(%s)", err, s ? s : "unknown");
    }
}

/* 获取并清洗进程名：最多 16 字节，读不到写 "?"，其中的 ','、':'、空格替换为 '_' */
static void memp_get_proc_name(pid_t pid, char *out, size_t sz) {
    if (!out || sz == 0) return;
    char raw[64] = {0};
    int ret = proc_name((int)pid, raw, sizeof(raw));
    if (ret <= 0 || raw[0] == '\0') {
        snprintf(out, sz, "?");
        return;
    }
    raw[sizeof(raw) - 1] = '\0';
    if (strlen(raw) > 16) {
        raw[16] = '\0';
    }
    for (char *p = raw; *p; p++) {
        if (*p == ',' || *p == ':' || *p == ' ') {
            *p = '_';
        }
    }
    snprintf(out, sz, "%s", raw);
}

/* 统计让位记录中 B 类进程数量 */
static int memp_count_yielded_sys(const memp_yielded_entry_t *yielded, int nyielded) {
    int cnt = 0;
    for (int i = 0; i < nyielded; i++) {
        if (yielded[i].is_sys) cnt++;
    }
    return cnt;
}

/* 设档封装：内部使用 MEMORYSTATUS_CMD_SET_PRIORITY_PROPERTIES 与 set_priority() 一致，
 * 但不更新 g_set_fail_total、g_last_err_* 等全局错误统计 */
static int mp_set_band(pid_t pid, int32_t band) {
    memorystatus_priority_properties_t props = { band, 0 };
    int rc = memorystatus_control(MEMORYSTATUS_CMD_SET_PRIORITY_PROPERTIES,
                                  pid, 0, &props, sizeof(props));
    if (rc != 0) return errno != 0 ? errno : -1;
    return 0;
}

/* 获取进程启动时间 (秒与微秒) */
static int memp_get_process_start_time(pid_t pid, uint64_t *sec_out, uint64_t *usec_out) {
    struct proc_bsdinfo bi;
    memset(&bi, 0, sizeof(bi));
    int ret = proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof(bi));
    if (ret == (int)sizeof(bi)) {
        if (sec_out) *sec_out = bi.pbi_start_tvsec;
        if (usec_out) *usec_out = bi.pbi_start_tvusec;
        return 0;
    }
    return -1;
}

/* 读取进程物理内存 footprint (字节) */
static int memp_get_phys_footprint(pid_t pid, uint64_t *fp_bytes_out) {
    struct rusage_info_v4 ri;
    memset(&ri, 0, sizeof(ri));
    int rc = proc_pid_rusage(pid, RUSAGE_INFO_V4, (rusage_info_t *)&ri);
    if (rc == 0) {
        if (fp_bytes_out) *fp_bytes_out = ri.ri_phys_footprint;
        return 0;
    }
    return errno != 0 ? errno : -1;
}

/* 检查是否为主 App 二进制 (路径以 /private/var/containers/Bundle/Application/ 或 /var/containers/Bundle/Application/ 开头且所在目录以 .app 结尾) */
static int memp_is_main_app_binary(const char *path) {
    if (!path) return 0;
    static const char k1[] = "/private/var/containers/Bundle/Application/";
    static const char k2[] = "/var/containers/Bundle/Application/";
    if (strncmp(path, k1, sizeof(k1) - 1) != 0 &&
        strncmp(path, k2, sizeof(k2) - 1) != 0) return 0;
    const char *slash = strrchr(path, '/');
    if (!slash || slash - path < 4) return 0;
    return (strncmp(slash - 4, ".app", 4) == 0);
}

/* 读取真实压力级别 */
static int memp_read_level(int *level_out, int *err_out) {
    int lvl = 0;
    size_t sz = sizeof(lvl);
    int rc = sysctlbyname("kern.memorystatus_vm_pressure_level", &lvl, &sz, NULL, 0);
    int err = (rc != 0) ? errno : 0;
    if (!g_memp_level_first_logged) {
        g_memp_level_first_logged = 1;
        char eb[128];
        memp_fmt_errno(err, eb, sizeof(eb));
        if (rc == 0) {
            jp_log("MEMP level_first val=%d errno=%s", lvl, eb);
        } else {
            jp_log("MEMP level_first val=NA errno=%s", eb);
        }
    }
    if (err_out) *err_out = err;
    if (rc == 0 && level_out) *level_out = lvl;
    return rc;
}

/* 规则匹配：按文件顺序逐条比，取第一条匹配的规则 */
static memp_rule_t *memp_match_rule(const char *path, const char *app_bid) {
    for (int i = 0; i < g_nmemp_rules; i++) {
        if (g_memp_rules[i].is_bid) {
            if (app_bid && strcmp(app_bid, g_memp_rules[i].frag + 4) == 0) {
                return &g_memp_rules[i];
            }
        } else {
            if (strstr(path, g_memp_rules[i].frag)) {
                return &g_memp_rules[i];
            }
        }
    }
    return NULL;
}

static void memp_enter_fatal(const char *reason) {
    g_memp_state = MEMP_STATE_FATAL;
    strncpy(g_memp_fatal_reason, reason, sizeof(g_memp_fatal_reason) - 1);
    g_memp_fatal_reason[sizeof(g_memp_fatal_reason) - 1] = '\0';
    FILE *f = fopen(MEMP_FATAL_PATH, "w");
    if (f) {
        fprintf(f, "reason=%s\n", reason);
        fflush(f);
        fsync(fileno(f));
        fclose(f);
    }
    jp_log("MEMP fatal reason=%s", reason);
}

static void memp_enter_unsupported(const char *reason, int err) {
    g_memp_state = MEMP_STATE_UNSUPPORTED;
    strncpy(g_memp_unsupported_reason, reason, sizeof(g_memp_unsupported_reason) - 1);
    g_memp_unsupported_reason[sizeof(g_memp_unsupported_reason) - 1] = '\0';
    g_memp_last_errno = err;
    char eb[128];
    memp_fmt_errno(err, eb, sizeof(eb));
    jp_log("MEMP unsupported reason=%s errno=%s", reason, eb);
}

static void memp_enter_reset_pending_normal(int retry_count, struct timespec warn_mono) {
    g_memp_reset_pending = MEMP_RESET_PENDING_NORMAL;
    g_memp_reset_retry_count = retry_count;
    g_memp_warn_monotonic = warn_mono;
    g_memp_last_reset_retry_time = 0;
    g_memp_reset_overdue_logged = 0;
}

static void memp_taint_save(void) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", MEMP_TAINT_PATH);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < g_memp_ntaint; i++) {
        fprintf(f, "%d %llu %llu %s\n", (int)g_memp_taint[i].pid,
                (unsigned long long)g_memp_taint[i].tvsec,
                (unsigned long long)g_memp_taint[i].tvusec,
                g_memp_taint[i].frag);
    }
    fflush(f);
    int serr = fsync(fileno(f));
    int ferr = ferror(f);
    fclose(f);
    if (ferr || serr != 0) {
        unlink(tmp);
        return;
    }
    rename(tmp, MEMP_TAINT_PATH);
}

static void memp_taint_remove(pid_t pid) {
    for (int i = 0; i < g_memp_ntaint; i++) {
        if (g_memp_taint[i].pid == pid) {
            for (int j = i; j < g_memp_ntaint - 1; j++) {
                g_memp_taint[j] = g_memp_taint[j + 1];
            }
            g_memp_ntaint--;
            memp_taint_save();
            return;
        }
    }
}

static void memp_taint_add(pid_t pid, uint64_t tvsec, uint64_t tvusec, const char *frag, const char *reason) {
    for (int i = 0; i < g_memp_ntaint; i++) {
        if (g_memp_taint[i].pid == pid && g_memp_taint[i].tvsec == tvsec && g_memp_taint[i].tvusec == tvusec) {
            return; /* 已在污染表 */
        }
    }
    if (g_memp_ntaint >= MEMP_MAX_TAINT) {
        pid_t dropped_pid = g_memp_taint[0].pid;
        jp_log("MEMP taint_drop pid=%d", (int)dropped_pid);
        for (int i = 0; i < MEMP_MAX_TAINT - 1; i++) {
            g_memp_taint[i] = g_memp_taint[i + 1];
        }
        g_memp_ntaint = MEMP_MAX_TAINT - 1;
    }
    memp_taint_entry_t *e = &g_memp_taint[g_memp_ntaint++];
    e->pid = pid;
    e->tvsec = tvsec;
    e->tvusec = tvusec;
    strncpy(e->frag, frag ? frag : "", sizeof(e->frag) - 1);
    e->frag[sizeof(e->frag) - 1] = '\0';
    jp_log("MEMP taint app=%s pid=%d reason=%s", e->frag, (int)pid, reason);
    memp_taint_save();
}

static int memp_is_tainted(pid_t pid, uint64_t tvsec, uint64_t tvusec) {
    for (int i = 0; i < g_memp_ntaint; i++) {
        if (g_memp_taint[i].pid == pid) {
            if (g_memp_taint[i].tvsec == tvsec && g_memp_taint[i].tvusec == tvusec) {
                return 1;
            }
            /* 身份不符：进程已变，自动清理 */
            memp_taint_remove(pid);
            return 0;
        }
    }
    return 0;
}

static void memp_taint_load(void) {
    g_memp_ntaint = 0;
    FILE *f = fopen(MEMP_TAINT_PATH, "r");
    if (!f) {
        if (errno != ENOENT) {
            char eb[128];
            memp_fmt_errno(errno, eb, sizeof(eb));
            jp_log("MEMP taint_load_fail line=- errno=%s", eb);
        }
        return;
    }
    char line[512];
    int line_no = 0;
    int needs_resave = 0;
    while (fgets(line, sizeof(line), f)) {
        line_no++;
        int pid = 0;
        unsigned long long tvsec = 0, tvusec = 0;
        char frag[256] = {0};
        if (sscanf(line, "%d %llu %llu %255s", &pid, &tvsec, &tvusec, frag) != 4) {
            char eb[128];
            memp_fmt_errno(0, eb, sizeof(eb));
            jp_log("MEMP taint_load_fail line=%d errno=%s", line_no, eb);
            needs_resave = 1;
            continue;
        }
        uint64_t cur_sec = 0, cur_usec = 0;
        if (memp_get_process_start_time((pid_t)pid, &cur_sec, &cur_usec) == 0 &&
            cur_sec == tvsec && cur_usec == tvusec) {
            if (g_memp_ntaint < MEMP_MAX_TAINT) {
                g_memp_taint[g_memp_ntaint].pid = (pid_t)pid;
                g_memp_taint[g_memp_ntaint].tvsec = tvsec;
                g_memp_taint[g_memp_ntaint].tvusec = tvusec;
                strncpy(g_memp_taint[g_memp_ntaint].frag, frag, sizeof(g_memp_taint[g_memp_ntaint].frag) - 1);
                g_memp_taint[g_memp_ntaint].frag[sizeof(g_memp_taint[g_memp_ntaint].frag) - 1] = '\0';
                g_memp_ntaint++;
            } else {
                needs_resave = 1;
            }
        } else {
            /* 进程已退出或重启过，丢弃 */
            needs_resave = 1;
        }
    }
    fclose(f);
    if (needs_resave) {
        memp_taint_save();
    }
}

static void mempressure_init_memcap(void) {
    uint64_t memsize = 0;
    size_t len = sizeof(memsize);
    int rc = sysctlbyname("hw.memsize", &memsize, &len, NULL, 0);
    int err = errno;
    if (rc != 0 || len != sizeof(memsize)) {
        g_memp_memcap = 2048;
        g_memp_memcap_fallback = 1;
        char eb[128];
        memp_fmt_errno(err, eb, sizeof(eb));
        jp_log("MEMP memsize_fail errno=%s", eb);
        return;
    }
    uint64_t mb = memsize / (1024ULL * 1024ULL);
    uint64_t half = mb / 2;
    if (half < 200) {
        g_memp_memcap = 2048;
        g_memp_memcap_fallback = 1;
        jp_log("MEMP memsize_small bytes=%llu", (unsigned long long)memsize);
        return;
    }
    g_memp_memcap = half;
    g_memp_memcap_fallback = 0;
}

static void memp_check_fatal_persist(void) {
    if (access(MEMP_FATAL_PATH, F_OK) != 0) return;
    FILE *f = fopen(MEMP_FATAL_PATH, "r");
    if (!f) return;
    char line[128] = {0};
    char reason[32] = "einval";
    if (fgets(line, sizeof(line), f)) {
        sscanf(line, "reason=%31s", reason);
    }
    fclose(f);
    g_memp_state = MEMP_STATE_FATAL;
    strncpy(g_memp_fatal_reason, reason, sizeof(g_memp_fatal_reason) - 1);
    g_memp_fatal_reason[sizeof(g_memp_fatal_reason) - 1] = '\0';
    jp_log("MEMP fatal_persist reason=%s", reason);
}

static void memp_check_boot_reset(void) {
    if (access(MEMP_PENDING_PATH, F_OK) != 0) return;
    FILE *f = fopen(MEMP_PENDING_PATH, "r");
    if (!f) return;
    int pid = 0;
    unsigned long long tvsec = 0, tvusec = 0;
    char frag[256] = {0};
    int n = fscanf(f, "%d %llu %llu %255s", &pid, &tvsec, &tvusec, frag);
    fclose(f);
    if (n < 4) {
        unlink(MEMP_PENDING_PATH);
        return;
    }

    int v = (TEST_LOW_MEMORY_TRIGGER_ONE << 16) | DISPATCH_MEMORYPRESSURE_NORMAL;
    int rc = sysctlbyname("kern.memorypressure_manual_trigger", NULL, NULL, &v, sizeof(int));
    int err = (rc != 0) ? errno : 0;
    char eb[128];
    memp_fmt_errno(err, eb, sizeof(eb));
    jp_log("MEMP boot_reset ret=%d errno=%s", rc, eb);

    if (rc == 0) {
        unlink(MEMP_PENDING_PATH);
        memp_taint_add((pid_t)pid, tvsec, tvusec, frag, "boot");
    } else if (err == EINVAL) {
        memp_enter_fatal("einval");
    } else if (err == ENOENT || err == EPERM) {
        unlink(MEMP_PENDING_PATH);
        memp_enter_unsupported("trigger", err);
    } else {
        memp_taint_add((pid_t)pid, tvsec, tvusec, frag, "boot");
        struct timespec mono;
        clock_gettime(CLOCK_MONOTONIC, &mono);
        memp_enter_reset_pending_normal(0, mono);
    }
}

static void mempressure_init(void) {
    mempressure_init_memcap();
    memp_check_fatal_persist();
    if (g_memp_state != MEMP_STATE_FATAL) {
        memp_taint_load();
        memp_check_boot_reset();
        memp_floor_load();
    }
}

static void mempressure_write_state(void) {
    const char *state_str = "off";
    const char *reason_str = "-";

    if (g_memp_state == MEMP_STATE_FATAL) {
        state_str = "fatal";
        reason_str = g_memp_fatal_reason[0] ? g_memp_fatal_reason : "einval";
    } else if (g_memp_state == MEMP_STATE_UNSUPPORTED) {
        state_str = "unsupported";
        reason_str = g_memp_unsupported_reason[0] ? g_memp_unsupported_reason : "trigger";
    } else if (g_memp_reset_pending == MEMP_RESET_PENDING_NORMAL) {
        state_str = "reset_pending_normal";
    } else if (g_memp_reset_pending == MEMP_RESET_PENDING_MARKER) {
        state_str = "reset_pending_marker";
    } else if (g_memp_state == MEMP_STATE_DISABLED) {
        state_str = "disabled";
    } else if (g_nmemp_rules == 0 || g_memp_cfg_file_absent) {
        state_str = "off";
    } else {
        state_str = "on";
    }

    char line[256];
    snprintf(line, sizeof(line), "state=%s fails=%d last_errno=%d reason=%s tainted=%d\n",
             state_str, g_memp_fails, g_memp_last_errno, reason_str, g_memp_ntaint);

    static char s_last_line[256] = "";
    if (strcmp(line, s_last_line) == 0) return; /* 内容无变化不写 */

    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", MEMP_STATE_PATH);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fputs(line, f);
    fflush(f);
    int werr = ferror(f);
    fclose(f);
    if (werr) { unlink(tmp); return; }
    if (rename(tmp, MEMP_STATE_PATH) == 0) {
        strncpy(s_last_line, line, sizeof(s_last_line) - 1);
        s_last_line[sizeof(s_last_line) - 1] = '\0';
    } else {
        unlink(tmp);
    }
}

static void memp_restore_yielded(memp_yielded_entry_t *yielded, int nyielded,
                                 memorystatus_priority_entry_t *latest_list, int nlatest,
                                 int *ok_out, int *moved_out, int *gone_out, int *rfail_out) {
    int ok = 0, moved = 0, gone = 0, rfail = 0;
    for (int i = 0; i < nyielded; i++) {
        uint64_t cur_sec = 0, cur_usec = 0;
        if (memp_get_process_start_time(yielded[i].pid, &cur_sec, &cur_usec) != 0 ||
            cur_sec != yielded[i].tvsec || cur_usec != yielded[i].tvusec) {
            gone++;
            continue;
        }
        int cur_band = -1;
        int found = 0;
        if (latest_list) {
            for (int j = 0; j < nlatest; j++) {
                if (latest_list[j].pid == yielded[i].pid) {
                    cur_band = latest_list[j].priority;
                    found = 1;
                    break;
                }
            }
        }
        if (!found || cur_band != 80) {
            moved++;
            continue;
        }
        int err = mp_set_band(yielded[i].pid, 90);
        if (err == 0) {
            ok++;
        } else {
            rfail++;
            char eb[128];
            memp_fmt_errno(err, eb, sizeof(eb));
            jp_log("MEMP restore_fail pid=%d errno=%s", (int)yielded[i].pid, eb);
            if (yielded[i].is_sys) {
                char pname[32];
                memp_get_proc_name(yielded[i].pid, pname, sizeof(pname));
                jp_log("MEMP yield_sys_fail pid=%d name=%s step=restore errno=%s",
                       (int)yielded[i].pid, pname, eb);
            }
        }
    }
    if (ok_out) *ok_out = ok;
    if (moved_out) *moved_out = moved;
    if (gone_out) *gone_out = gone;
    if (rfail_out) *rfail_out = rfail;
}

static void memp_format_comp(memorystatus_priority_entry_t *list, int count, pid_t target_pid, char *buf, size_t sz) {
    buf[0] = '\0';
    size_t len = 0;
    int first = 1;
    for (int i = 0; i < count; i++) {
        if (list[i].pid <= 0 || list[i].pid == target_pid) continue;
        if (list[i].priority == 90 || list[i].priority == 100) {
            uint64_t fp_bytes = 0;
            int rerr = memp_get_phys_footprint(list[i].pid, &fp_bytes);
            char pname[32];
            memp_get_proc_name(list[i].pid, pname, sizeof(pname));
            char item[64];
            if (rerr == 0) {
                snprintf(item, sizeof(item), "%s%d:%d:%.1f:%s", first ? "" : ",",
                         (int)list[i].pid, (int)list[i].priority, (double)fp_bytes / (1024.0 * 1024.0), pname);
            } else {
                snprintf(item, sizeof(item), "%s%d:%d:NA:%s", first ? "" : ",",
                         (int)list[i].pid, (int)list[i].priority, pname);
            }
            first = 0;
            size_t ilen = strlen(item);
            if (len + ilen < sz - 1) {
                strcat(buf, item);
                len += ilen;
            } else {
                break;
            }
        }
    }
    if (first) {
        snprintf(buf, sz, "-");
    }
}

static void memp_log_skip(memp_rule_t *rule, pid_t pid, double fp_mb, int thr_mb,
                          const char *reason, int yielded, int yielded_sys, int err, const char *comp_str,
                          int ok, int moved, int gone, int rfail) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (rule && rule->last_skip_sec > 0 && now.tv_sec - rule->last_skip_sec < 60) {
        return; /* 同一规则每 60 秒最多一行 */
    }
    if (rule) {
        rule->last_skip_sec = now.tv_sec;
    }

    char eb[128];
    if (strcmp(reason, "yield_fail") == 0 || strcmp(reason, "marker_fail") == 0) {
        memp_fmt_errno(err, eb, sizeof(eb));
    } else {
        snprintf(eb, sizeof(eb), "0(-)");
        ok = moved = gone = rfail = 0;
    }

    jp_log("MEMP skip app=%s pid=%d fp=%.1f thr=%d reason=%s yielded=%d yielded_sys=%d errno=%s comp=%s restored=ok:%d,moved:%d,gone:%d,rfail:%d",
           rule ? rule->frag : "-", (int)pid, fp_mb, thr_mb, reason, yielded, yielded_sys, eb,
           (comp_str && comp_str[0]) ? comp_str : "-", ok, moved, gone, rfail);
}

static int memp_rule_cfg_equal(const memp_rule_t *a, const memp_rule_t *b) {
    if (!a || !b) return 0;
    if (strcmp(a->frag, b->frag) != 0) return 0;
    if (a->thr_mb != b->thr_mb) return 0;
    if (a->cooldown != b->cooldown) return 0;
    return 1;
}

static void memp_floor_format_median(const double *samples, int nsamp, char *buf, size_t sz) {
    if (nsamp < 3 || nsamp > MEMP_MAX_FLOOR_SAMPLES) {
        snprintf(buf, sz, "NA");
        return;
    }
    double sorted[MEMP_MAX_FLOOR_SAMPLES];
    memcpy(sorted, samples, sizeof(double) * nsamp);
    for (int a = 0; a < nsamp - 1; a++) {
        for (int b = 0; b < nsamp - 1 - a; b++) {
            if (sorted[b] > sorted[b + 1]) {
                double tmp = sorted[b];
                sorted[b] = sorted[b + 1];
                sorted[b + 1] = tmp;
            }
        }
    }
    double med = 0.0;
    if (nsamp == 3) {
        med = sorted[1];
    } else if (nsamp == 4) {
        med = round(((sorted[1] + sorted[2]) / 2.0) * 10.0) / 10.0;
    } else if (nsamp == 5) {
        med = sorted[2];
    }
    snprintf(buf, sz, "%.1f", med);
}

static void memp_floor_get_median_str(const char *frag, char *buf, size_t sz) {
    if (!frag || !buf || sz == 0) return;
    snprintf(buf, sz, "NA");
    for (int i = 0; i < g_nmemp_floors; i++) {
        if (strcmp(g_memp_floors[i].frag, frag) == 0) {
            memp_floor_entry_t *fe = &g_memp_floors[i];
            memp_floor_format_median(fe->samples, fe->nsamp, buf, sz);
            return;
        }
    }
}

static void memp_floor_save(void) {
    FILE *f = fopen(MEMP_FLOOR_TMP, "w");
    if (!f) return;
    fprintf(f, "# jetpilot floor v1\n");
    for (int i = 0; i < g_nmemp_floors; i++) {
        memp_floor_entry_t *fe = &g_memp_floors[i];
        char med_str[32];
        memp_floor_format_median(fe->samples, fe->nsamp, med_str, sizeof(med_str));
        char s_buf[256] = "";
        char *sp = s_buf;
        size_t rem = sizeof(s_buf);
        for (int s = 0; s < fe->nsamp; s++) {
            int n = snprintf(sp, rem, "%s%.1f", (s > 0 ? "," : ""), fe->samples[s]);
            if (n > 0 && (size_t)n < rem) {
                sp += n;
                rem -= (size_t)n;
            }
        }
        fprintf(f, "%s t=%ld n=%d med=%s s=%s\n",
                fe->frag, (long)fe->last_time, fe->nsamp, med_str, s_buf);
    }
    fflush(f);
    int werr = ferror(f);
    fclose(f);
    if (werr) {
        unlink(MEMP_FLOOR_TMP);
        return;
    }
    if (rename(MEMP_FLOOR_TMP, MEMP_FLOOR_PATH) != 0) {
        char eb[128];
        memp_fmt_errno(errno, eb, sizeof(eb));
        jp_log("MEMP floor_save_fail errno=%s", eb);
        unlink(MEMP_FLOOR_TMP);
    }
}

static void memp_floor_prune(void) {
    int non_rule_count = 0;
    for (int i = 0; i < g_nmemp_floors; i++) {
        int is_rule = 0;
        for (int r = 0; r < g_nmemp_rules; r++) {
            if (strcmp(g_memp_floors[i].frag, g_memp_rules[r].frag) == 0) {
                is_rule = 1;
                break;
            }
        }
        if (!is_rule) non_rule_count++;
    }

    int modified = 0;
    while (non_rule_count > 16) {
        int oldest_idx = -1;
        time_t oldest_time = 0;
        for (int i = 0; i < g_nmemp_floors; i++) {
            int is_rule = 0;
            for (int r = 0; r < g_nmemp_rules; r++) {
                if (strcmp(g_memp_floors[i].frag, g_memp_rules[r].frag) == 0) {
                    is_rule = 1;
                    break;
                }
            }
            if (!is_rule) {
                if (oldest_idx == -1 || g_memp_floors[i].last_time < oldest_time) {
                    oldest_idx = i;
                    oldest_time = g_memp_floors[i].last_time;
                }
            }
        }
        if (oldest_idx >= 0) {
            for (int i = oldest_idx; i < g_nmemp_floors - 1; i++) {
                g_memp_floors[i] = g_memp_floors[i + 1];
            }
            g_nmemp_floors--;
            non_rule_count--;
            modified = 1;
        } else {
            break;
        }
    }
    if (modified) {
        memp_floor_save();
    }
}

static void memp_floor_add_sample(const char *frag, double fp_post30) {
    if (!frag || frag[0] == '\0') return;
    memp_floor_entry_t *entry = NULL;
    for (int i = 0; i < g_nmemp_floors; i++) {
        if (strcmp(g_memp_floors[i].frag, frag) == 0) {
            entry = &g_memp_floors[i];
            break;
        }
    }
    if (!entry) {
        if (g_nmemp_floors >= MEMP_MAX_FLOOR_ENTRIES) {
            memp_floor_prune();
        }
        if (g_nmemp_floors < MEMP_MAX_FLOOR_ENTRIES) {
            entry = &g_memp_floors[g_nmemp_floors++];
            memset(entry, 0, sizeof(*entry));
            strncpy(entry->frag, frag, sizeof(entry->frag) - 1);
        } else {
            return;
        }
    }
    if (entry->nsamp < MEMP_MAX_FLOOR_SAMPLES) {
        entry->samples[entry->nsamp++] = fp_post30;
    } else {
        for (int i = 0; i < MEMP_MAX_FLOOR_SAMPLES - 1; i++) {
            entry->samples[i] = entry->samples[i + 1];
        }
        entry->samples[MEMP_MAX_FLOOR_SAMPLES - 1] = fp_post30;
    }
    entry->last_time = time(NULL);
    memp_floor_save();
}

static void memp_floor_load(void) {
    FILE *f = fopen(MEMP_FLOOR_PATH, "r");
    if (!f) return;
    char line[512];
    int line_no = 0;
    int is_v1 = 0;
    int had_bad = 0;
    g_nmemp_floors = 0;
    while (fgets(line, sizeof(line), f)) {
        line_no++;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\r' || *p == '\n' || *p == '\0') continue;
        if (line_no == 1) {
            if (strncmp(p, "# jetpilot floor v1", 19) == 0) {
                is_v1 = 1;
            } else {
                jp_log("MEMP floor_reset reason=version");
                fclose(f);
                return;
            }
            continue;
        }
        if (*p == '#') continue;
        if (!is_v1) continue;

        char frag[256] = {0};
        long t_val = 0;
        int n_val = 0;
        char s_val[256] = {0};
        char line_copy[512];
        strncpy(line_copy, p, sizeof(line_copy) - 1);
        line_copy[sizeof(line_copy) - 1] = '\0';

        char *saveptr = NULL;
        char *tok = strtok_r(line_copy, " \t\r\n", &saveptr);
        if (!tok) continue;
        strncpy(frag, tok, sizeof(frag) - 1);
        while ((tok = strtok_r(NULL, " \t\r\n", &saveptr)) != NULL) {
            if (strncmp(tok, "t=", 2) == 0) {
                t_val = strtol(tok + 2, NULL, 10);
            } else if (strncmp(tok, "n=", 2) == 0) {
                n_val = (int)strtol(tok + 2, NULL, 10);
            } else if (strncmp(tok, "s=", 2) == 0) {
                strncpy(s_val, tok + 2, sizeof(s_val) - 1);
            }
        }
        if (frag[0] == '\0' || n_val <= 0 || n_val > MEMP_MAX_FLOOR_SAMPLES || s_val[0] == '\0') {
            had_bad = 1;
            continue;
        }

        double parsed_samples[MEMP_MAX_FLOOR_SAMPLES];
        int n_parsed_samples = 0;
        char *sp = s_val;
        char *subptr = NULL;
        char *stok = strtok_r(sp, ",", &subptr);
        while (stok && n_parsed_samples < MEMP_MAX_FLOOR_SAMPLES) {
            char *endp = NULL;
            double v = strtod(stok, &endp);
            if (!endp || endp == stok) break;
            parsed_samples[n_parsed_samples++] = v;
            stok = strtok_r(NULL, ",", &subptr);
        }

        /* 核对 n= 与 s= 里实际样本数是否一致，以 s= 为准；不一致时把这一行当坏行跳过 */
        if (n_parsed_samples != n_val) {
            had_bad = 1;
            continue;
        }

        if (g_nmemp_floors < MEMP_MAX_FLOOR_ENTRIES) {
            memp_floor_entry_t *fe = &g_memp_floors[g_nmemp_floors++];
            memset(fe, 0, sizeof(*fe));
            strncpy(fe->frag, frag, sizeof(fe->frag) - 1);
            fe->last_time = (time_t)t_val;
            fe->nsamp = n_parsed_samples;
            memcpy(fe->samples, parsed_samples, sizeof(double) * n_parsed_samples);
        }
    }
    fclose(f);
    if (had_bad && g_nmemp_floors == 0) {
        jp_log("MEMP floor_reset reason=corrupt");
    }
}

static void memp_backoff_on_sample(const char *frag, double freed_mb) {
    memp_rule_t *rule = NULL;
    for (int i = 0; i < g_nmemp_rules; i++) {
        if (strcmp(g_memp_rules[i].frag, frag) == 0) {
            rule = &g_memp_rules[i];
            break;
        }
    }
    if (!rule) return;

    int32_t base_cd = rule->cooldown > 0 ? rule->cooldown : g_memp_cooldown;
    if (rule->eff_cd <= 0) rule->eff_cd = base_cd;

    if (freed_mb < 30.0) {
        rule->ineff++;
        if (rule->ineff >= 3) {
            int32_t max_limit = 1800 > base_cd ? 1800 : base_cd;
            int32_t new_cd = rule->eff_cd * 2;
            if (new_cd > max_limit) new_cd = max_limit;
            rule->eff_cd = new_cd;
            rule->ineff = 0;
            char med_str[32] = "NA";
            memp_floor_get_median_str(rule->frag, med_str, sizeof(med_str));
            jp_log("MEMP backoff app=%s cd=%d floor=%s", rule->frag, (int)rule->eff_cd, med_str);
        }
    } else {
        if (rule->eff_cd != base_cd) {
            jp_log("MEMP backoff_reset app=%s cd=%d", rule->frag, (int)base_cd);
        }
        rule->eff_cd = base_cd;
        rule->ineff = 0;
    }
}

static void memp_post_sample(memorystatus_priority_entry_t *list, int count) {
    if (!g_memp_post.valid) return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    /* 每轮采样 */
    if (g_locked) {
        g_memp_post.fg_ok = 0;
    }
    uint64_t cur_sec = 0, cur_usec = 0;
    if (memp_get_process_start_time(g_memp_post.pid, &cur_sec, &cur_usec) != 0 ||
        cur_sec != g_memp_post.start_tvsec || cur_usec != g_memp_post.start_tvusec) {
        g_memp_post.fg_ok = 0;
    } else {
        int found = 0;
        int cur_prio = -1;
        if (list && count > 0) {
            for (int i = 0; i < count; i++) {
                if (list[i].pid == g_memp_post.pid) {
                    found = 1;
                    cur_prio = list[i].priority;
                    break;
                }
            }
        }
        if (!found || cur_prio != 100) {
            g_memp_post.fg_ok = 0;
        }
    }
    g_memp_post.nsamp++;

    if (now.tv_sec - g_memp_post.warn_mono_sec < 30) return;

    long dt = (long)(now.tv_sec - g_memp_post.warn_mono_sec);
    char fp_post30_buf[64] = "gone";
    char freed_buf[64] = "NA";
    int is_num = 0;
    double fp_post30_val = 0.0;
    double freed_mb = 0.0;

    cur_sec = 0; cur_usec = 0;
    if (memp_get_process_start_time(g_memp_post.pid, &cur_sec, &cur_usec) == 0 &&
        cur_sec == g_memp_post.start_tvsec && cur_usec == g_memp_post.start_tvusec) {
        uint64_t fp_bytes = 0;
        int rerr = memp_get_phys_footprint(g_memp_post.pid, &fp_bytes);
        if (rerr == 0) {
            fp_post30_val = (double)fp_bytes / (1024.0 * 1024.0);
            snprintf(fp_post30_buf, sizeof(fp_post30_buf), "%.1f", fp_post30_val);
            freed_mb = g_memp_post.fp_before_mb - fp_post30_val;
            snprintf(freed_buf, sizeof(freed_buf), "%.1f", freed_mb);
            is_num = 1;
        } else {
            snprintf(fp_post30_buf, sizeof(fp_post30_buf), "NA:%d", rerr);
        }
    } else {
        g_memp_post.fg_ok = 0;
    }

    jp_log("MEMP post app=%s pid=%d fp_before=%.1f fp_post30=%s dt=%ld fg_ok=%d nsamp=%d freed=%s",
           g_memp_post.frag, (int)g_memp_post.pid, g_memp_post.fp_before_mb,
           fp_post30_buf, dt, g_memp_post.fg_ok, g_memp_post.nsamp, freed_buf);

    if (g_memp_post.fg_ok == 1 && is_num) {
        memp_floor_add_sample(g_memp_post.frag, fp_post30_val);
        memp_backoff_on_sample(g_memp_post.frag, freed_mb);
    }

    g_memp_post.valid = 0;
}

/* 登记 post 观察槽（§八）
 * cur_band: 目标触发时的档位。当前门控要求 cur == 100（见 mempressure_check 候选进程选取），
 * 传入选中档位以便放宽门控时 band_warn 仍保持准确 */
static void memp_post_register(const char *frag, pid_t pid, uint64_t tvsec, uint64_t tvusec, time_t warn_sec, double fp_before_mb, int cur_band) {
    if (g_memp_post.valid) {
        long dt = (long)(warn_sec - g_memp_post.warn_mono_sec);
        jp_log("MEMP post app=%s pid=%d fp_before=%.1f fp_post30=dropped dt=%ld fg_ok=0 nsamp=%d freed=NA",
               g_memp_post.frag, (int)g_memp_post.pid, g_memp_post.fp_before_mb, dt, g_memp_post.nsamp);
    }
    g_memp_post.valid = 1;
    strncpy(g_memp_post.frag, frag ? frag : "", sizeof(g_memp_post.frag) - 1);
    g_memp_post.frag[sizeof(g_memp_post.frag) - 1] = '\0';
    g_memp_post.pid = pid;
    g_memp_post.start_tvsec = tvsec;
    g_memp_post.start_tvusec = tvusec;
    g_memp_post.warn_mono_sec = warn_sec;
    g_memp_post.fp_before_mb = fp_before_mb;
    g_memp_post.band_warn = cur_band;
    g_memp_post.fg_ok = 1;
    g_memp_post.nsamp = 0;
}

static int mempressure_load_config(void) {
    FILE *f = fopen(MEMP_CONFIG_PATH, "r");
    int file_absent = (f == NULL);
    int new_cooldown = MEMP_DEFAULT_COOLDOWN;
    memp_rule_t parsed_rules[MEMP_MAX_RULES];
    int nparsed = 0;

    if (f) {
        char line[512];
        int line_no = 0;
        while (fgets(line, sizeof(line), f)) {
            line_no++;
            size_t llen = strlen(line);
            int oversized = 0;
            if (llen == sizeof(line) - 1 && line[llen - 1] != '\n' && !feof(f)) {
                oversized = 1;
                int c;
                while ((c = fgetc(f)) != EOF && c != '\n') {}
            }

            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '#' || *p == '\r' || *p == '\n' || *p == '\0') continue;

            char raw_text[512];
            strncpy(raw_text, p, sizeof(raw_text) - 1);
            raw_text[sizeof(raw_text) - 1] = '\0';
            char *nl = strchr(raw_text, '\n');
            if (nl) *nl = '\0';
            char *cr = strchr(raw_text, '\r');
            if (cr) *cr = '\0';

            if (oversized) {
                jp_log("MEMP cfg_bad line=%d text=%s", line_no, raw_text);
                continue;
            }

            char line_copy[512];
            strncpy(line_copy, raw_text, sizeof(line_copy) - 1);
            line_copy[sizeof(line_copy) - 1] = '\0';

            char *tokens[10];
            int ntokens = 0;
            char *saveptr = NULL;
            char *tok = strtok_r(line_copy, " \t", &saveptr);
            while (tok) {
                if (ntokens < 9) {
                    tokens[ntokens] = tok;
                }
                ntokens++;
                tok = strtok_r(NULL, " \t", &saveptr);
            }

            if (ntokens <= 0) continue;
            if (ntokens > 8) {
                jp_log("MEMP cfg_bad line=%d text=%s", line_no, raw_text);
                continue;
            }

            if (strcmp(tokens[0], "cooldown") == 0) {
                if (ntokens != 2) {
                    jp_log("MEMP cfg_bad line=%d text=%s", line_no, raw_text);
                    continue;
                }
                char *endp = NULL;
                long cd = strtol(tokens[1], &endp, 10);
                if (!endp || *endp != '\0' || cd < MEMP_MIN_COOLDOWN || cd > MEMP_MAX_COOLDOWN) {
                    jp_log("MEMP cfg_bad line=%d text=%s", line_no, raw_text);
                    continue;
                }
                new_cooldown = (int)cd;
            } else {
                /* 规则行 */
                if (nparsed >= MEMP_MAX_RULES) {
                    jp_log("MEMP cfg_bad line=%d text=%s", line_no, raw_text);
                    continue;
                }

                int duplicate = 0;
                for (int i = 0; i < nparsed; i++) {
                    if (strcmp(parsed_rules[i].frag, tokens[0]) == 0) {
                        duplicate = 1;
                        break;
                    }
                }
                if (duplicate) {
                    jp_log("MEMP cfg_bad line=%d text=%s", line_no, raw_text);
                    continue;
                }

                int thr = MEMP_DEFAULT_THR_MB;
                int rule_cd = 0;
                int has_cd = 0;
                int bad = 0;
                int idx = 1;

                if (idx < ntokens && strchr(tokens[idx], '=') == NULL) {
                    char *endp = NULL;
                    long val = strtol(tokens[idx], &endp, 10);
                    if (!endp || *endp != '\0' || val < MEMP_MIN_THR_MB || (uint64_t)val > g_memp_memcap) {
                        bad = 1;
                    } else {
                        thr = (int)val;
                    }
                    idx++;
                }

                if (bad) {
                    jp_log("MEMP cfg_bad line=%d text=%s", line_no, raw_text);
                    continue;
                }

                for (; idx < ntokens; idx++) {
                    char *eq = strchr(tokens[idx], '=');
                    if (!eq || eq == tokens[idx]) {
                        bad = 1;
                        break;
                    }
                    *eq = '\0';
                    char *key = tokens[idx];
                    char *val = eq + 1;
                    if (strcmp(key, "cooldown") != 0) {
                        bad = 1;
                        break;
                    }
                    if (has_cd) {
                        bad = 1;
                        break;
                    }
                    char *endp = NULL;
                    long cd = strtol(val, &endp, 10);
                    if (!endp || *endp != '\0' || cd < MEMP_MIN_COOLDOWN || cd > MEMP_MAX_COOLDOWN) {
                        bad = 1;
                        break;
                    }
                    rule_cd = (int)cd;
                    has_cd = 1;
                }

                if (bad) {
                    jp_log("MEMP cfg_bad line=%d text=%s", line_no, raw_text);
                    continue;
                }

                memp_rule_t *r = &parsed_rules[nparsed++];
                strncpy(r->frag, tokens[0], sizeof(r->frag) - 1);
                r->frag[sizeof(r->frag) - 1] = '\0';
                r->thr_mb = thr;
                r->cooldown = rule_cd;
                r->is_bid = (strncmp(tokens[0], "bid:", 4) == 0);
                r->last_warn_sec = 0;
                r->last_skip_sec = 0;
                r->ineff = 0;
                r->eff_cd = (rule_cd > 0 ? rule_cd : new_cooldown);
            }
        }
        fclose(f);
    }

    /* 检查配置是否有变 */
    int changed = 0;
    if (g_memp_cfg_first_load) {
        changed = 1;
        g_memp_cfg_first_load = 0;
    } else if (file_absent != g_memp_cfg_file_absent) {
        changed = 1;
    } else if (new_cooldown != g_memp_cooldown) {
        changed = 1;
    } else if (nparsed != g_nmemp_rules) {
        changed = 1;
    } else {
        for (int i = 0; i < nparsed; i++) {
            if (!memp_rule_cfg_equal(&parsed_rules[i], &g_memp_rules[i])) {
                changed = 1;
                break;
            }
        }
    }

    if (changed) {
        int rule_cd_count = 0;
        for (int i = 0; i < nparsed; i++) {
            if (parsed_rules[i].cooldown > 0) rule_cd_count++;
        }

        char cap_str[64];
        if (g_memp_memcap_fallback) {
            snprintf(cap_str, sizeof(cap_str), "2048(fallback)");
        } else {
            snprintf(cap_str, sizeof(cap_str), "%llu", (unsigned long long)g_memp_memcap);
        }
        jp_log("MEMP cfg rules=%d cooldown=%d file=%s memcap=%s rule_cd=%d",
               nparsed, new_cooldown, file_absent ? "absent" : "ok", cap_str, rule_cd_count);

        for (int i = 0; i < nparsed; i++) {
            char cd_str[32];
            if (parsed_rules[i].cooldown > 0) {
                snprintf(cd_str, sizeof(cd_str), "%d", (int)parsed_rules[i].cooldown);
            } else {
                snprintf(cd_str, sizeof(cd_str), "global");
            }
            jp_log("MEMP cfg_rule i=%d app=%s thr=%d cd=%s",
                   i, parsed_rules[i].frag, (int)parsed_rules[i].thr_mb, cd_str);
        }

        /* 继承相同片段的冷却与退避状态 */
        for (int i = 0; i < nparsed; i++) {
            int32_t base_cd = parsed_rules[i].cooldown > 0 ? parsed_rules[i].cooldown : new_cooldown;
            parsed_rules[i].ineff = 0;
            parsed_rules[i].eff_cd = base_cd;
            for (int j = 0; j < g_nmemp_rules; j++) {
                if (strcmp(parsed_rules[i].frag, g_memp_rules[j].frag) == 0) {
                    parsed_rules[i].last_warn_sec = g_memp_rules[j].last_warn_sec;
                    parsed_rules[i].last_skip_sec = g_memp_rules[j].last_skip_sec;
                    int same_cfg = (parsed_rules[i].thr_mb == g_memp_rules[j].thr_mb &&
                                    parsed_rules[i].cooldown == g_memp_rules[j].cooldown &&
                                    (parsed_rules[i].cooldown > 0 || new_cooldown == g_memp_cooldown));
                    if (same_cfg) {
                        parsed_rules[i].ineff = g_memp_rules[j].ineff;
                        parsed_rules[i].eff_cd = g_memp_rules[j].eff_cd > 0 ? g_memp_rules[j].eff_cd : base_cd;
                    }
                    break;
                }
            }
        }
        memcpy(g_memp_rules, parsed_rules, sizeof(memp_rule_t) * nparsed);
        g_nmemp_rules = nparsed;
        g_memp_cooldown = new_cooldown;
        g_memp_cfg_file_absent = file_absent;
        memp_floor_prune();
    }

    return file_absent ? -1 : 0;
}

static void mempressure_round(memorystatus_priority_entry_t *list, int count) {
    /* 1. 每一轮开头检查观察槽与采样（§八、§三-A） */
    memp_post_sample(list, count);

    /* 2. 重新加载配置（§四） */
    mempressure_load_config();

    /* 3. 检查 fatal 状态 */
    if (g_memp_state == MEMP_STATE_FATAL) return;

    /* 4. §五-1a 第一件事：复位处理 (reset_pending) */
    if (g_memp_reset_pending == MEMP_RESET_PENDING_MARKER) {
        if (unlink(MEMP_PENDING_PATH) == 0 || errno == ENOENT) {
            g_memp_reset_pending = MEMP_RESET_PENDING_NONE;
            g_memp_marker_rm_fail_count = 0;
        } else {
            int err = errno;
            g_memp_marker_rm_fail_count++;
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            if (g_memp_last_marker_rm_fail_time == 0 || now.tv_sec - g_memp_last_marker_rm_fail_time >= 60) {
                g_memp_last_marker_rm_fail_time = now.tv_sec;
                char eb[128];
                memp_fmt_errno(err, eb, sizeof(eb));
                jp_log("MEMP marker_rm_fail n=%d errno=%s", g_memp_marker_rm_fail_count, eb);
            }
        }
        return;
    }

    if (g_memp_reset_pending == MEMP_RESET_PENDING_NORMAL) {
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        long secs_since_warn = (long)(now.tv_sec - g_memp_warn_monotonic.tv_sec);
        if (secs_since_warn >= 600 && !g_memp_reset_overdue_logged) {
            g_memp_reset_overdue_logged = 1;
            jp_log("MEMP reset_overdue secs=%ld", secs_since_warn);
        }
        g_memp_reset_retry_count++;
        int v = (TEST_LOW_MEMORY_TRIGGER_ONE << 16) | DISPATCH_MEMORYPRESSURE_NORMAL;
        int rc = sysctlbyname("kern.memorypressure_manual_trigger", NULL, NULL, &v, sizeof(int));
        int err = (rc != 0) ? errno : 0;
        if (rc == 0) {
            if (unlink(MEMP_PENDING_PATH) == 0 || errno == ENOENT) {
                g_memp_reset_pending = MEMP_RESET_PENDING_NONE;
                g_memp_reset_retry_count = 0;
            } else {
                g_memp_reset_pending = MEMP_RESET_PENDING_MARKER;
                g_memp_marker_rm_fail_count = 1;
                g_memp_last_marker_rm_fail_time = now.tv_sec;
                char eb[128];
                memp_fmt_errno(errno, eb, sizeof(eb));
                jp_log("MEMP marker_rm_fail n=1 errno=%s", eb);
            }
        } else if (err == EINVAL) {
            memp_enter_fatal("einval");
        } else if (err == ENOENT || err == EPERM) {
            memp_enter_fatal("unreachable");
        } else {
            if (g_memp_last_reset_retry_time == 0 || now.tv_sec - g_memp_last_reset_retry_time >= 60) {
                g_memp_last_reset_retry_time = now.tv_sec;
                char eb[128];
                memp_fmt_errno(err, eb, sizeof(eb));
                jp_log("MEMP reset_retry n=%d ret=%d errno=%s", g_memp_reset_retry_count, rc, eb);
            }
        }
        return;
    }

    /* 检查停用与配置有效性 */
    if (g_memp_state == MEMP_STATE_UNSUPPORTED || g_memp_state == MEMP_STATE_DISABLED) return;
    if (g_nmemp_rules == 0 || g_memp_cfg_file_absent) return;
    if (g_locked) return;
    if (!list || count <= 0) return;

    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);

    /* 5. §五-1a 第二件事：修复（§五-6） */
    pid_t repair_pid = 0;
    uint64_t repair_max_fp = 0;
    memp_rule_t *repair_rule = NULL;
    int repair_fp_fail_skipped = 0;

    for (int i = 0; i < count; i++) {
        pid_t pid = list[i].pid;
        int32_t cur = list[i].priority;
        if (pid <= 0 || cur != 100) continue;

        char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
        if (proc_pidpath(pid, path, sizeof(path)) <= 0) continue;
        if (!memp_is_main_app_binary(path)) continue;

        const char *app_bid = app_bid_for_path(path);
        memp_rule_t *rule = memp_match_rule(path, app_bid);
        if (!rule) continue;

        uint64_t sec = 0, usec = 0;
        if (memp_get_process_start_time(pid, &sec, &usec) != 0) continue;
        if (!memp_is_tainted(pid, sec, usec)) continue;

        /* 修复限频：同一进程每 60 秒最多一次 */
        int recent_repair = 0;
        for (int r = 0; r < g_memp_nrepair_seen; r++) {
            if (g_memp_repair_seen[r].pid == pid) {
                if (now_ts.tv_sec - g_memp_repair_seen[r].last_repair_sec < 60) {
                    recent_repair = 1;
                }
                break;
            }
        }
        if (recent_repair) continue;

        uint64_t fp = 0;
        if (memp_get_phys_footprint(pid, &fp) != 0) {
            memp_log_repair_skip_fp_fail(pid, now_ts.tv_sec);
            repair_fp_fail_skipped = 1;
            continue;
        }
        if (repair_pid == 0 || fp > repair_max_fp) {
            repair_pid = pid;
            repair_max_fp = fp;
            repair_rule = rule;
        }
    }

    if (repair_fp_fail_skipped) {
        return;
    }

    if (repair_pid > 0) {
        /* 更新该 pid 的修复限频计时与条目指针 */
        memp_repair_seen_t *seen_entry = NULL;
        for (int r = 0; r < g_memp_nrepair_seen; r++) {
            if (g_memp_repair_seen[r].pid == repair_pid) {
                seen_entry = &g_memp_repair_seen[r];
                seen_entry->last_repair_sec = now_ts.tv_sec;
                break;
            }
        }
        if (!seen_entry) {
            if (g_memp_nrepair_seen < MEMP_MAX_REPAIR_SEEN) {
                seen_entry = &g_memp_repair_seen[g_memp_nrepair_seen++];
            } else {
                seen_entry = &g_memp_repair_seen[0];
            }
            seen_entry->pid = repair_pid;
            seen_entry->last_repair_sec = now_ts.tv_sec;
            seen_entry->fail_count = 0;
        }

        /* 检查真实级别 */
        int lvl = 0, lerr = 0;
        int lrc = memp_read_level(&lvl, &lerr);
        if (lrc != 0) {
            if (lerr == ENOENT || lerr == EPERM) {
                memp_enter_unsupported("level", lerr);
            }
            return;
        }
        if (lvl != 1) return; /* 非 Normal，跳过修复 */

        /* 修复第 1 步：取表确认 T 仍是 100 且无其他 100 档进程 footprint >= T */
        int rcount1 = 0;
        memorystatus_priority_entry_t *rlist1 = get_list(&rcount1);
        int t_is_100 = 0;
        int comp_has_larger = 0;
        if (rlist1) {
            for (int i = 0; i < rcount1; i++) {
                if (rlist1[i].pid == repair_pid && rlist1[i].priority == 100) {
                    t_is_100 = 1;
                    break;
                }
            }
            if (t_is_100) {
                for (int i = 0; i < rcount1; i++) {
                    if (rlist1[i].pid <= 0 || rlist1[i].pid == repair_pid) continue;
                    if (rlist1[i].priority == 100) {
                        uint64_t cfp = 0;
                        if (memp_get_phys_footprint(rlist1[i].pid, &cfp) != 0 || cfp >= repair_max_fp) {
                            comp_has_larger = 1;
                            break;
                        }
                    }
                }
            }
        }

        char comp_buf[256] = "-";
        if (rlist1) {
            memp_format_comp(rlist1, rcount1, repair_pid, comp_buf, sizeof(comp_buf));
        }

        if (!t_is_100 || comp_has_larger) {
            jp_log("MEMP repair app=%s pid=%d band_before=100 band_after=%d comp=%s ret=0 errno=0(-) result=skip",
                   repair_rule ? repair_rule->frag : "-", (int)repair_pid, t_is_100 ? 100 : 0, comp_buf);
            if (rlist1) free(rlist1);
            return;
        }
        if (rlist1) free(rlist1);

        /* 修复第 2 步：写一次 NORMAL */
        int v = (TEST_LOW_MEMORY_TRIGGER_ONE << 16) | DISPATCH_MEMORYPRESSURE_NORMAL;
        int rc = sysctlbyname("kern.memorypressure_manual_trigger", NULL, NULL, &v, sizeof(int));
        int err = (rc != 0) ? errno : 0;
        if (err == EINVAL) {
            memp_enter_fatal("einval");
            return;
        }
        if (err == ENOENT || err == EPERM) {
            const char *reason = (err == ENOENT) ? "repair_enoent" : "repair_eperm";
            memp_enter_unsupported(reason, err);
            return;
        }

        /* 修复第 3 步：立即复核 */
        int rcount2 = 0;
        memorystatus_priority_entry_t *rlist2 = get_list(&rcount2);
        int after_is_100 = 0;
        int after_comp_larger = 0;
        char band_after_str[32] = "gone";
        if (rlist2) {
            for (int i = 0; i < rcount2; i++) {
                if (rlist2[i].pid == repair_pid) {
                    snprintf(band_after_str, sizeof(band_after_str), "%d", (int)rlist2[i].priority);
                    if (rlist2[i].priority == 100) after_is_100 = 1;
                    break;
                }
            }
            if (after_is_100) {
                for (int i = 0; i < rcount2; i++) {
                    if (rlist2[i].pid <= 0 || rlist2[i].pid == repair_pid) continue;
                    if (rlist2[i].priority == 100) {
                        uint64_t cfp = 0;
                        if (memp_get_phys_footprint(rlist2[i].pid, &cfp) != 0 || cfp >= repair_max_fp) {
                            after_comp_larger = 1;
                            break;
                        }
                    }
                }
            }
        }

        char comp_buf2[256] = "-";
        if (rlist2) {
            memp_format_comp(rlist2, rcount2, repair_pid, comp_buf2, sizeof(comp_buf2));
            free(rlist2);
        }

        char eb[128];
        memp_fmt_errno(err, eb, sizeof(eb));

        if (rc == 0 && after_is_100 && !after_comp_larger) {
            if (seen_entry) seen_entry->fail_count = 0;
            memp_taint_remove(repair_pid);
            jp_log("MEMP repair app=%s pid=%d band_before=100 band_after=%s comp=%s ret=%d errno=%s result=ok",
                   repair_rule ? repair_rule->frag : "-", (int)repair_pid, band_after_str, comp_buf2, rc, eb);
        } else {
            int n = seen_entry ? ++seen_entry->fail_count : 1;
            jp_log("MEMP repair app=%s pid=%d band_before=100 band_after=%s comp=%s ret=%d errno=%s result=retry n=%d",
                   repair_rule ? repair_rule->frag : "-", (int)repair_pid, band_after_str, comp_buf2, rc, eb, n);
        }
        return; /* 修复已执行 sysctl，本轮结束 */
    }

    /* 6. §五-1a 第三件事：触发（§五-1） */
    pid_t target_pid = 0;
    uint64_t target_sec = 0, target_usec = 0;
    uint64_t target_fp_bytes = 0;
    memp_rule_t *target_rule = NULL;

    for (int i = 0; i < count; i++) {
        pid_t pid = list[i].pid;
        int32_t cur = list[i].priority;
        if (pid <= 0 || cur != 100) continue;

        char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
        if (proc_pidpath(pid, path, sizeof(path)) <= 0) continue;

        /* 一次性诊断：记录首个档位 100 且路径包含 /containers/Bundle/Application/ 的进程主二进制判定结果 */
        static int fg_first_logged = 0;
        if (!fg_first_logged && cur == 100 && strstr(path, "/containers/Bundle/Application/") != NULL) {
            fg_first_logged = 1;
            jp_log("MEMP fg_first pid=%d main=%d path=%s", (int)pid, memp_is_main_app_binary(path), path);
        }

        if (!memp_is_main_app_binary(path)) continue;

        const char *app_bid = app_bid_for_path(path);
        memp_rule_t *rule = memp_match_rule(path, app_bid);
        if (!rule) continue;

        uint64_t fp_bytes = 0;
        int rerr = memp_get_phys_footprint(pid, &fp_bytes);
        if (rerr != 0) {
            if (rerr != ESRCH) {
                int recent_fail = 0;
                for (int fidx = 0; fidx < g_memp_nrusage_fail_seen; fidx++) {
                    if (g_memp_rusage_fail_seen[fidx].pid == pid) {
                        if (now_ts.tv_sec - g_memp_rusage_fail_seen[fidx].last_fail_sec < 60) {
                            recent_fail = 1;
                        }
                        break;
                    }
                }
                if (!recent_fail) {
                    int fidx = 0;
                    for (; fidx < g_memp_nrusage_fail_seen; fidx++) {
                        if (g_memp_rusage_fail_seen[fidx].pid == pid) break;
                    }
                    if (fidx >= g_memp_nrusage_fail_seen) {
                        if (g_memp_nrusage_fail_seen < MEMP_MAX_RUSAGE_FAIL_SEEN) {
                            fidx = g_memp_nrusage_fail_seen++;
                        } else {
                            fidx = 0;
                        }
                    }
                    g_memp_rusage_fail_seen[fidx].pid = pid;
                    g_memp_rusage_fail_seen[fidx].last_fail_sec = now_ts.tv_sec;
                    char eb[128];
                    memp_fmt_errno(rerr, eb, sizeof(eb));
                    jp_log("MEMP rusage_fail pid=%d errno=%s", (int)pid, eb);
                }
            }
            continue;
        }

        uint64_t thr_bytes = (uint64_t)rule->thr_mb * 1024ULL * 1024ULL;
        if (fp_bytes <= thr_bytes) continue;

        /* 初次认作目标日志（§九 target） */
        int seen_target = 0;
        for (int s = 0; s < g_memp_nseen_targets; s++) {
            if (g_memp_seen_targets[s] == pid) {
                seen_target = 1;
                break;
            }
        }
        if (!seen_target) {
            if (g_memp_nseen_targets < MEMP_MAX_SEEN_TARGETS) {
                g_memp_seen_targets[g_memp_nseen_targets++] = pid;
            } else {
                g_memp_seen_targets[0] = pid;
            }
            jp_log("MEMP target app=%s pid=%d rule=%s path=%s",
                   rule->frag, (int)pid, rule->frag, path);
        }

        uint64_t sec = 0, usec = 0;
        if (memp_get_process_start_time(pid, &sec, &usec) != 0) continue;

        /* 条件 4：目标不在污染表里 */
        if (memp_is_tainted(pid, sec, usec)) continue;

        /* 条件 5：该规则有效冷却已过 */
        int32_t eff_cd = rule->eff_cd > 0 ? rule->eff_cd : (rule->cooldown > 0 ? rule->cooldown : g_memp_cooldown);
        if (rule->last_warn_sec > 0 && now_ts.tv_sec - rule->last_warn_sec < eff_cd) continue;

        /* 同一轮有多个目标超过阈值时，取 footprint 最大的一个 */
        if (target_pid == 0 || fp_bytes > target_fp_bytes) {
            target_pid = pid;
            target_sec = sec;
            target_usec = usec;
            target_fp_bytes = fp_bytes;
            target_rule = rule;
        }
    }

    if (target_pid == 0) return;

    double fp_before_mb = (double)target_fp_bytes / (1024.0 * 1024.0);

    /* 条件 6：读真实压力级别 */
    int lvl = 0, lerr = 0;
    int lrc = memp_read_level(&lvl, &lerr);
    if (lrc != 0) {
        if (lerr == ENOENT || lerr == EPERM) {
            memp_enter_unsupported("level", lerr);
        } else {
            memp_log_skip(target_rule, target_pid, fp_before_mb, target_rule->thr_mb,
                          "level_unread", 0, 0, 0, "-", 0, 0, 0, 0);
        }
        return;
    }
    if (lvl != 1) {
        memp_log_skip(target_rule, target_pid, fp_before_mb, target_rule->thr_mb,
                      "real_pressure", 0, 0, 0, "-", 0, 0, 0, 0);
        return;
    }

    /* 条件 7 & §五-2 步骤 2：让位（§三-3 / 勘误 3） */
    memp_yielded_entry_t yield_candidates[MEMP_MAX_YIELD + 1];
    int k = 0;
    pid_t self_pid = getpid();
    for (int i = 0; i < count; i++) {
        pid_t pid = list[i].pid;
        int32_t cur = list[i].priority;
        if (pid <= 0 || pid == target_pid || cur != 90) continue;

        char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
        int have_path = (proc_pidpath(pid, path, sizeof(path)) > 0);

        /* 判定是否为 A 类（受保护且目标档位为 90 的用户 App） */
        int is_a = 0;
        if (have_path && strstr(path, "/var/containers/Bundle/Application/")) {
            const char *app_bid = app_bid_for_path(path);
            int32_t prot_target = 0;
            const char *frag = NULL;
            if (path_matches(path, app_bid, g_protect, g_nprotect, &prot_target, &frag)) {
                if (prot_target == 90) {
                    is_a = 1;
                }
            }
        }

        int is_sys = 0;
        if (is_a) {
            is_sys = 0;
        } else if (pid != self_pid) {
            /* B 类：非 T、非 A 类、非 daemon 自己的其他所有 90 档进程 */
            is_sys = 1;
        } else {
            /* daemon 自身排除在 B 类之外 */
            continue;
        }

        if (k < MEMP_MAX_YIELD) {
            yield_candidates[k].pid = pid;
            yield_candidates[k].is_sys = is_sys;
            yield_candidates[k].tvsec = 0;
            yield_candidates[k].tvusec = 0;
            k++;
        } else {
            k++; /* 溢出标记 */
        }
    }

    if (k > MEMP_MAX_YIELD) {
        memp_log_skip(target_rule, target_pid, fp_before_mb, target_rule->thr_mb,
                      "yield_full", 0, 0, 0, "-", 0, 0, 0, 0);
        return;
    }

    /* 逐个读启动时间 */
    for (int i = 0; i < k; i++) {
        if (memp_get_process_start_time(yield_candidates[i].pid,
                                        &yield_candidates[i].tvsec,
                                        &yield_candidates[i].tvusec) != 0) {
            memp_log_skip(target_rule, target_pid, fp_before_mb, target_rule->thr_mb,
                          "yield_nostart", 0, 0, 0, "-", 0, 0, 0, 0);
            return;
        }
    }

    /* 逐个设为 80 */
    memp_yielded_entry_t yielded[MEMP_MAX_YIELD];
    int nyielded = 0;
    int yield_failed = 0;
    int yield_fail_err = 0;
    for (int i = 0; i < k; i++) {
        int err = mp_set_band(yield_candidates[i].pid, 80);
        if (err == 0) {
            yielded[nyielded++] = yield_candidates[i];
        } else {
            yield_failed = 1;
            yield_fail_err = err;
            if (yield_candidates[i].is_sys) {
                char pname[32];
                memp_get_proc_name(yield_candidates[i].pid, pname, sizeof(pname));
                char eb[128];
                memp_fmt_errno(err, eb, sizeof(eb));
                jp_log("MEMP yield_sys_fail pid=%d name=%s step=down errno=%s",
                       (int)yield_candidates[i].pid, pname, eb);
            }
            break;
        }
    }

    if (yield_failed) {
        int r_ok = 0, r_moved = 0, r_gone = 0, r_rfail = 0;
        memp_restore_yielded(yielded, nyielded, list, count, &r_ok, &r_moved, &r_gone, &r_rfail);
        int nyielded_sys = memp_count_yielded_sys(yielded, nyielded);
        memp_log_skip(target_rule, target_pid, fp_before_mb, target_rule->thr_mb,
                      "yield_fail", nyielded, nyielded_sys, yield_fail_err, "-", r_ok, r_moved, r_gone, r_rfail);
        return;
    }

    /* 条件 7 & §五-2 步骤 3：门控（§三-4） */
    struct timespec gate_start;
    clock_gettime(CLOCK_MONOTONIC, &gate_start);
    int gate_count = 0;
    memorystatus_priority_entry_t *gate_list = get_list(&gate_count);

    const char *gate_reason = NULL;
    int fg_others = 0;
    if (!gate_list) {
        gate_reason = "comp_unread";
    } else {
        int t_found_in_100 = 0;
        for (int i = 0; i < gate_count; i++) {
            if (gate_list[i].pid == target_pid) {
                if (gate_list[i].priority == 100) t_found_in_100 = 1;
                break;
            }
        }
        if (!t_found_in_100) {
            gate_reason = "fg_lost";
        } else {
            for (int i = 0; i < gate_count; i++) {
                pid_t cpid = gate_list[i].pid;
                int32_t cband = gate_list[i].priority;
                if (cpid <= 0 || cpid == target_pid) continue;

                if (cband == 90) {
                    gate_reason = "comp90";
                    break;
                }
                if (cband == 100) {
                    fg_others++;
                    uint64_t cfp = 0;
                    if (memp_get_phys_footprint(cpid, &cfp) != 0) {
                        gate_reason = "comp_unread";
                        break;
                    }
                    if (cfp >= target_fp_bytes) {
                        gate_reason = "comp100";
                        break;
                    }
                }
            }
        }
    }

    if (gate_reason != NULL) {
        int r_ok = 0, r_moved = 0, r_gone = 0, r_rfail = 0;
        memp_restore_yielded(yielded, nyielded, gate_list, gate_count, &r_ok, &r_moved, &r_gone, &r_rfail);
        char comp_buf[256] = "-";
        if (gate_list) memp_format_comp(gate_list, gate_count, target_pid, comp_buf, sizeof(comp_buf));
        int nyielded_sys = memp_count_yielded_sys(yielded, nyielded);
        memp_log_skip(target_rule, target_pid, fp_before_mb, target_rule->thr_mb,
                      gate_reason, nyielded, nyielded_sys, 0, comp_buf, r_ok, r_moved, r_gone, r_rfail);
        if (gate_list) free(gate_list);
        return;
    }

    /* 门控通过！执行触发序列 (§五-2 步骤 4–14) */
    /* 步骤 4：写崩溃标记 */
    char marker_line[512];
    snprintf(marker_line, sizeof(marker_line), "%d %llu %llu %s\n",
             (int)target_pid, (unsigned long long)target_sec, (unsigned long long)target_usec, target_rule->frag);
    FILE *mf = fopen(MEMP_PENDING_PATH, "w");
    int marker_err = 0;
    if (!mf) {
        marker_err = errno != 0 ? errno : -1;
    } else {
        fputs(marker_line, mf);
        fflush(mf);
        if (ferror(mf) || fsync(fileno(mf)) != 0) marker_err = errno != 0 ? errno : -1;
        fclose(mf);
    }

    if (marker_err != 0) {
        unlink(MEMP_PENDING_PATH);
        int r_ok = 0, r_moved = 0, r_gone = 0, r_rfail = 0;
        memp_restore_yielded(yielded, nyielded, gate_list, gate_count, &r_ok, &r_moved, &r_gone, &r_rfail);
        char comp_buf[256] = "-";
        if (gate_list) memp_format_comp(gate_list, gate_count, target_pid, comp_buf, sizeof(comp_buf));
        int nyielded_sys = memp_count_yielded_sys(yielded, nyielded);
        memp_log_skip(target_rule, target_pid, fp_before_mb, target_rule->thr_mb,
                      "marker_fail", nyielded, nyielded_sys, marker_err, comp_buf, r_ok, r_moved, r_gone, r_rfail);
        if (gate_list) free(gate_list);
        return;
    }

    /* 步骤 5：写 WARN */
    int wv = (TEST_LOW_MEMORY_TRIGGER_ONE << 16) | DISPATCH_MEMORYPRESSURE_WARN;
    int warn_rc = sysctlbyname("kern.memorypressure_manual_trigger", NULL, NULL, &wv, sizeof(int));
    int warn_err = (warn_rc != 0) ? errno : 0;
    struct timespec warn_mono;
    clock_gettime(CLOCK_MONOTONIC, &warn_mono);

    /* 步骤 6：立即复核快照，得到 recheck */
    int recheck_count = 0;
    memorystatus_priority_entry_t *recheck_list = get_list(&recheck_count);
    long gate_ms = (warn_mono.tv_sec - gate_start.tv_sec) * 1000 +
                   (warn_mono.tv_nsec - gate_start.tv_nsec) / 1000000;
    if (gate_ms < 0) gate_ms = 0;

    int recheck_same = 1;
    if (!recheck_list || !gate_list) {
        recheck_same = 0;
    } else {
        /* 比对 90/100 pid 集合 */
        int g90_100[128], ng = 0;
        int r90_100[128], nr = 0;
        for (int i = 0; i < gate_count && ng < 128; i++) {
            if (gate_list[i].priority == 90 || gate_list[i].priority == 100) {
                g90_100[ng++] = gate_list[i].pid;
            }
        }
        for (int i = 0; i < recheck_count && nr < 128; i++) {
            if (recheck_list[i].priority == 90 || recheck_list[i].priority == 100) {
                r90_100[nr++] = recheck_list[i].pid;
            }
        }
        if (ng != nr) {
            recheck_same = 0;
        } else {
            for (int i = 0; i < ng; i++) {
                int found = 0;
                for (int j = 0; j < nr; j++) {
                    if (g90_100[i] == r90_100[j]) { found = 1; break; }
                }
                if (!found) { recheck_same = 0; break; }
            }
        }
    }
    const char *recheck_str = recheck_same ? "same" : "changed";

    int nyielded_sys = memp_count_yielded_sys(yielded, nyielded);
    char web[128];
    memp_fmt_errno(warn_err, web, sizeof(web));
    jp_log("MEMP warn app=%s pid=%d fp_before=%.1f thr=%d yielded=%d yielded_sys=%d fg_others=%d gate_ms=%ld ret=%d errno=%s recheck=%s",
           target_rule->frag, (int)target_pid, fp_before_mb, target_rule->thr_mb,
           nyielded, nyielded_sys, fg_others, gate_ms, warn_rc, web, recheck_str);

    if (gate_list) free(gate_list);
    if (recheck_list) free(recheck_list);

    /* 步骤 7：等待 5 秒 */
    int early_stop = 0;
    for (int s = 0; s < 5; s++) {
        sleep(1);
        if (should_stop()) {
            early_stop = 1;
            break;
        }
    }

    /* 步骤 8：读 fp_after5 */
    char fp_after5_buf[64] = "gone";
    uint64_t cur_tsec = 0, cur_tusec = 0;
    if (memp_get_process_start_time(target_pid, &cur_tsec, &cur_tusec) == 0 &&
        cur_tsec == target_sec && cur_tusec == target_usec) {
        uint64_t afp = 0;
        int rerr = memp_get_phys_footprint(target_pid, &afp);
        if (rerr == 0) {
            snprintf(fp_after5_buf, sizeof(fp_after5_buf), "%.1f", (double)afp / (1024.0 * 1024.0));
        } else {
            snprintf(fp_after5_buf, sizeof(fp_after5_buf), "NA:%d", rerr);
        }
    }

    /* 步骤 9：取档位表得到 band_now，紧接着写 NORMAL */
    int step9_count = 0;
    memorystatus_priority_entry_t *step9_list = get_list(&step9_count);
    int band_now = -1;
    int found_in_step9 = 0;
    if (step9_list) {
        for (int i = 0; i < step9_count; i++) {
            if (step9_list[i].pid == target_pid) {
                band_now = step9_list[i].priority;
                found_in_step9 = 1;
                break;
            }
        }
    }

    char band_now_buf[32] = "gone";
    if (found_in_step9) {
        snprintf(band_now_buf, sizeof(band_now_buf), "%d", band_now);
    }
    int stuck = (found_in_step9 && band_now == 100) ? 0 : 1;

    int nv = (TEST_LOW_MEMORY_TRIGGER_ONE << 16) | DISPATCH_MEMORYPRESSURE_NORMAL;
    int normal_rc = sysctlbyname("kern.memorypressure_manual_trigger", NULL, NULL, &nv, sizeof(int));
    int normal_err = (normal_rc != 0) ? errno : 0;

    /* 步骤 10：还原让位 */
    int r_ok = 0, r_moved = 0, r_gone = 0, r_rfail = 0;
    memp_restore_yielded(yielded, nyielded, step9_list, step9_count, &r_ok, &r_moved, &r_gone, &r_rfail);

    char neb[128];
    memp_fmt_errno(normal_err, neb, sizeof(neb));
    jp_log("MEMP normal app=%s pid=%d fp_after5=%s band_now=%s stuck=%d early_stop=%d ret=%d errno=%s yielded=%d yielded_sys=%d restored=ok:%d,moved:%d,gone:%d,rfail:%d",
           target_rule->frag, (int)target_pid, fp_after5_buf, band_now_buf, stuck, early_stop,
           normal_rc, neb, nyielded, nyielded_sys, r_ok, r_moved, r_gone, r_rfail);

    if (step9_list) free(step9_list);

    /* 步骤 11：错误分类与污染记入 */
    if (stuck == 1) {
        memp_taint_add(target_pid, target_sec, target_usec, target_rule->frag, "stuck");
    }
    if (normal_rc == -1) {
        memp_taint_add(target_pid, target_sec, target_usec, target_rule->frag, "normal_fail");
    }
    if (strcmp(recheck_str, "changed") == 0) {
        memp_taint_add(target_pid, target_sec, target_usec, target_rule->frag, "recheck");
    }

    if (normal_rc == 0) {
        if (unlink(MEMP_PENDING_PATH) != 0 && errno != ENOENT) {
            g_memp_reset_pending = MEMP_RESET_PENDING_MARKER;
            g_memp_marker_rm_fail_count = 1;
            g_memp_last_marker_rm_fail_time = 0;
            char meb[128];
            memp_fmt_errno(errno, meb, sizeof(meb));
            jp_log("MEMP marker_rm_fail n=1 errno=%s", meb);
        }
        g_memp_fails = 0;
    } else {
        g_memp_last_errno = normal_err;
        if (normal_err == EINVAL) {
            memp_enter_fatal("einval");
        } else if (warn_rc == 0 || warn_err == EINVAL) {
            /* WARN 进了测试态 */
            if (normal_err == ENOENT || normal_err == EPERM) {
                memp_enter_fatal("unreachable");
            } else {
                memp_enter_reset_pending_normal(1, warn_mono);
                char reb[128];
                memp_fmt_errno(normal_err, reb, sizeof(reb));
                jp_log("MEMP reset_retry n=1 ret=%d errno=%s", normal_rc, reb);
            }
        } else {
            /* WARN 没进测试态 */
            unlink(MEMP_PENDING_PATH);
            if (warn_err == ENOENT || warn_err == EPERM) {
                memp_enter_unsupported("trigger", warn_err);
            }
        }

        if (warn_rc != 0 || normal_rc != 0) {
            g_memp_fails++;
            if (g_memp_fails >= 3 && g_memp_state != MEMP_STATE_UNSUPPORTED && g_memp_state != MEMP_STATE_FATAL) {
                g_memp_state = MEMP_STATE_DISABLED;
                char deb[128];
                memp_fmt_errno(g_memp_last_errno, deb, sizeof(deb));
                jp_log("MEMP disabled fails=3 last_errno=%s", deb);
            }
        }
    }

    /* 步骤 12：登记 post 观察槽（§八）——当前门控确保 target_pid 处于 100 档（见 2275 行） */
    memp_post_register(target_rule->frag, target_pid, target_sec, target_usec, warn_mono.tv_sec, fp_before_mb, 100);

    /* 步骤 13：记录冷却起点 */
    target_rule->last_warn_sec = warn_mono.tv_sec;
}

int main(void) {
    if (geteuid() != 0) {
        fprintf(stderr, "[JetPilot] 必须以 root 运行\n");
        return 1;
    }
    /* 防止重复启动：已有实例在跑就直接退出 */
    FILE *pf = fopen(PID_PATH, "r");
    if (pf) {
        int old = 0;
        if (fscanf(pf, "%d", &old) == 1 && old > 0 && kill(old, 0) == 0) {
            fclose(pf);
            fprintf(stderr, "[JetPilot] 已在运行 (pid %d)\n", old);
            return 0;
        }
        fclose(pf);
    }
    pf = fopen(PID_PATH, "w");
    if (pf) { fprintf(pf, "%d\n", getpid()); fclose(pf); }

    setsid();  /* 脱离拉起它的 App 会话 */
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    lockmon_init();  /* 锁屏监听（失败则保持常速轮询，不影响保护） */
    load_old_targets();  /* 启动时导入旧 status.txt 记录供首轮认领 */
    jp_log("%-6s v%s pid=%d old_records=%d locked=%d",
           "START", JP_VERSION, (int)getpid(), g_nold_targets, g_locked);
    mempressure_init();  /* 前台 App 内存压力触发初始化 (v0.7.0) */

    while (!should_stop()) {
        if (load_config() != 0) { sleep(5); continue; }

        int count = 0;
        memorystatus_priority_entry_t *list = get_list(&count);
        int protected_now = 0, demoted_now = 0;
        protected_entry_t plist[MAX_PROTECTED_LIST];
        int nplist = 0;

        if (list) {
            for (int i = 0; i < count; i++) {
                pid_t pid = list[i].pid;
                int32_t cur = list[i].priority;
                if (pid <= 0) continue;

                char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
                if (proc_pidpath(pid, path, sizeof(path)) <= 0) continue;
                /* 只碰用户 App，绝不动系统进程 */
                if (!strstr(path, "/var/containers/Bundle/Application/")) continue;

                int32_t target = 0;
                const char *frag = NULL;
                const char *demote_frag = NULL;
                int skip_this_round = 0;   /* 本轮是否走了 SKIP：非 SKIP 路径要清去重项 */
                applied_t *prev = applied_find(pid);
                if (prev) prev->seen = 1;
                /* 首轮认领：上个实例记录过、且当前档位仍等于它设的值 */
                if (!prev && g_nold_targets > 0 && cur < BAND_FOREGROUND) {
                    int32_t ot = 0;
                    if (old_target_find(pid, &ot) && cur == ot) {
                        applied_set(pid, cur);
                        prev = applied_find(pid);   /* 表满时为 NULL，安全 */
                        jp_log("%-6s pid=%d band=%d", "CLAIM", (int)pid, (int)cur);
                    }
                }
                const char *app_bid = NULL;
                if (g_has_bid_rules) {
                    app_bid = app_bid_for_path(path);
                }
                if (path_matches(path, app_bid, g_protect, g_nprotect, &target, &frag)) {
                    protected_now++;
                    if (!app_bid) {
                        app_bid = app_bid_for_path(path);
                    }
                    if (nplist < MAX_PROTECTED_LIST) {
                        protected_entry_t *e = &plist[nplist++];
                        e->frag = frag; e->pid = pid; e->cur = cur; e->target = target;
                        proc_label(path, e->label, sizeof(e->label));
                        if (app_bid) {
                            strncpy(e->bid, app_bid, sizeof(e->bid) - 1);
                            e->bid[sizeof(e->bid) - 1] = '\0';
                        } else {
                            e->bid[0] = '\0';
                        }
                    }
                    if (cur < target) {
                        /* 低于目标：抬高 */
                        int rc = set_priority(pid, target);
                        if (rc == 0) {
                            g_reassert_count++;
                            applied_set(pid, target);
                            jp_log("%-6s pid=%d %s %s %d->%d ok",
                                   "RAISE", (int)pid, proc_label_log(path), frag, (int)cur, (int)target);
                        } else {
                            /* errno 已被 set_priority 失败分支的日志调用污染，用 g_last_err_no */
                            jp_log("%-6s pid=%d %s %s %d->%d FAIL errno=%d %s",
                                   "RAISE", (int)pid, proc_label_log(path), frag, (int)cur, (int)target,
                                   g_last_err_no, strerror(g_last_err_no));
                        }
                    } else if (cur > target && cur < BAND_FOREGROUND) {
                        if (prev && prev->band == cur) {
                            /* 高于目标且是我们（或旧实例）设的：跟着降 */
                            int rc = set_priority(pid, target);
                            if (rc == 0) {
                                g_reassert_count++;
                                applied_set(pid, target);
                                jp_log("%-6s pid=%d %s %s %d->%d ok",
                                       "LOWER", (int)pid, proc_label_log(path), frag, (int)cur, (int)target);
                            } else {
                                /* errno 已被 set_priority 失败分支的日志调用污染，用 g_last_err_no */
                                jp_log("%-6s pid=%d %s %s %d->%d FAIL errno=%d %s",
                                       "LOWER", (int)pid, proc_label_log(path), frag, (int)cur, (int)target,
                                       g_last_err_no, strerror(g_last_err_no));
                            }
                        } else {
                            /* 本轮走了 SKIP：置位，去重表留待连续 SKIP 判断 */
                            skip_this_round = 1;
                            if (!skip_dedup_seen(pid, cur)) {
                                char ours_str[32];
                                if (prev) snprintf(ours_str, sizeof(ours_str), "%d", (int)prev->band);
                                else snprintf(ours_str, sizeof(ours_str), "none");
                                jp_log("%-6s pid=%d %s %s cur=%d target=%d ours=%s",
                                       "SKIP", (int)pid, proc_label_log(path), frag, (int)cur, (int)target, ours_str);
                            }
                        }
                    }
                } else if (path_matches(path, app_bid, g_demote, g_ndemote, NULL, &demote_frag)) {
                    /* 只在非前台时降，且只降到 BACKGROUND，避免误杀正在用的 App */
                    if (cur < BAND_FOREGROUND && cur > BAND_BACKGROUND) {
                        demoted_now++;
                        int rc = set_priority(pid, BAND_BACKGROUND);
                        if (rc == 0) {
                            jp_log("%-6s pid=%d %s %s %d->%d ok",
                                   "DEMOTE", (int)pid, proc_label_log(path), demote_frag, (int)cur, BAND_BACKGROUND);
                        } else {
                            /* errno 已被 set_priority 失败分支的日志调用污染，用 g_last_err_no */
                            jp_log("%-6s pid=%d %s %s %d->%d FAIL errno=%d %s",
                                   "DEMOTE", (int)pid, proc_label_log(path), demote_frag, (int)cur, BAND_BACKGROUND,
                                   g_last_err_no, strerror(g_last_err_no));
                        }
                    }
                } else if (prev && prev->band == cur && cur > BAND_BACKGROUND &&
                           cur < BAND_FOREGROUND) {
                    /* 规则被删：把我们抬过的档位还回普通后台 */
                    int rc = set_priority(pid, BAND_BACKGROUND);
                    if (rc == 0) {
                        applied_remove(pid);   /* 还回后不再是我们的档位 */
                        prev = NULL;           /* 指针已失效 */
                        jp_log("%-6s pid=%d %s %d->%d ok",
                               "RETURN", (int)pid, proc_label_log(path), (int)cur, BAND_BACKGROUND);
                    } else {
                        /* errno 已被 set_priority 失败分支的日志调用污染，用 g_last_err_no */
                        jp_log("%-6s pid=%d %s %d->%d FAIL errno=%d %s",
                               "RETURN", (int)pid, proc_label_log(path), (int)cur, BAND_BACKGROUND,
                               g_last_err_no, strerror(g_last_err_no));
                    }
                }
                /* 本轮没走 SKIP：清掉该 pid 的去重项，去重只对连续 SKIP 生效 */
                if (!skip_this_round) skip_dedup_clear(pid);
            }
            mempressure_round(list, count);
            free(list);
            applied_prune();
            g_nold_targets = 0;  /* 首轮结束，清空旧记录 */
        } else {
            mempressure_round(NULL, 0);
        }

        write_status(protected_now, demoted_now, plist, nplist);
        mempressure_write_state();

        if (g_locked) {
            /* 刚锁屏的前两轮用 10 秒间隔，盖住系统降档的窗口；之后 60 秒 */
            int secs = (g_locked_passes < 2) ? 10 : 60;
            g_locked_passes++;
            wait_lock_change(secs);       /* 解锁会立刻唤醒 */
        } else {
            g_locked_passes = 0;
            /* 亮屏 5~10 秒；一锁屏立刻醒来补一轮抬档 */
            wait_lock_change(5 + rand() % 6);
        }
        /* 注意：锁屏时最长 60 秒才发现 running.flag 被删（停止变慢），
         * 但点「停止」时人在用手机、是亮屏状态，实际不影响。 */
    }

    jp_log("STOP");
    unlink(PID_PATH);
    fprintf(stderr, "[JetPilot] 收到停止信号，退出\n");
    return 0;
}
