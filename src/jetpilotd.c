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
#if !defined(__APPLE__) && !defined(st_mtimespec)
#define st_mtimespec st_mtim
#endif
#include <signal.h>
#include <pthread.h>
#include <notify.h>   /* 锁屏状态：com.apple.springboard.lockstate（libSystem 自带） */
#include <sys/select.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <dispatch/dispatch.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
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
#ifndef PROC_PIDUNIQIDENTIFIERINFO
#define PROC_PIDUNIQIDENTIFIERINFO 17
struct proc_uniqidentifierinfo {
    uint8_t   p_uuid[16];
    uint64_t  p_uniqueid;
    uint64_t  p_puniqueid;
    int32_t   p_idversion;
    uint32_t  p_reserve2;
    uint64_t  p_reserve3;
    uint64_t  p_reserve4;
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
#ifndef CONFIG_DIR
#define CONFIG_DIR  "/var/mobile/Library/JetPilot"
#endif
#define CONFIG_PATH CONFIG_DIR "/config.txt"
#define FLAG_PATH   CONFIG_DIR "/running.flag"
#define STATUS_PATH CONFIG_DIR "/status.txt"
#define PID_PATH    CONFIG_DIR "/daemon.pid"
#define BANDS_REQ_PATH   CONFIG_DIR "/bands.req"
#define BANDS_TAKEN_PATH CONFIG_DIR "/bands.req.taken"
#define BANDS_PATH       CONFIG_DIR "/bands.txt"
#define BANDS_TMP_PATH   CONFIG_DIR "/bands.txt.tmp"
#define JP_VERSION   "0.7.8"
#ifndef JP_COMMIT
#define JP_COMMIT    "dev"
#endif
#define LOG_PATH     CONFIG_DIR "/daemon.log"
#define LOG_OLD_PATH CONFIG_DIR "/daemon.log.1"
#define LOG_MAX_BYTES (256 * 1024)

enum {
    JP_LOG_OFF   = 0,
    JP_LOG_ERR   = 1,
    JP_LOG_WARN  = 2,
    JP_LOG_INFO  = 3,
    JP_LOG_DEBUG = 4,
};
#define JP_ERR   JP_LOG_ERR
#define JP_WARN  JP_LOG_WARN
#define JP_INFO  JP_LOG_INFO
#define JP_DEBUG JP_LOG_DEBUG

static int g_loglevel = JP_LOG_INFO;
static const char *loglevel_to_str(int lvl) {
    switch (lvl) {
        case JP_LOG_OFF:   return "off";
        case JP_LOG_ERR:   return "error";
        case JP_LOG_WARN:  return "warning";
        case JP_LOG_INFO:  return "info";
        case JP_LOG_DEBUG: return "debug";
        default:           return "unknown";
    }
}

static void jp_log_write(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define JPLOG(lvl, fmt, ...) do { \
    if (g_loglevel != JP_LOG_OFF && (lvl) <= g_loglevel) { \
        jp_log_write((fmt), ##__VA_ARGS__); \
    } \
} while (0)
static const char *proc_label_log(const char *path);  /* 前置声明：act_log_success 在其定义前调用 */
static pthread_mutex_t s_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static void jp_log_write(const char *fmt, ...) {
    pthread_mutex_lock(&s_log_mutex);
    struct stat st;
    int existed = (stat(LOG_PATH, &st) == 0);
    if (existed && st.st_size > LOG_MAX_BYTES) {
        rename(LOG_PATH, LOG_OLD_PATH);          /* 覆盖旧的 .1 */
        existed = 0;
    }
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) {
        pthread_mutex_unlock(&s_log_mutex);
        return;
    }
    if (!existed) {
        fchmod(fileno(f), 0644);
    }
    time_t t = time(NULL);
    struct tm tm_val;
    localtime_r(&t, &tm_val);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_val);
    fprintf(f, "%s ", ts);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fclose(f);
    pthread_mutex_unlock(&s_log_mutex);
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

#define ACT_KIND_RAISE  1
#define ACT_KIND_DEMOTE 2

#define MAX_ACT_DEDUP 64
/* 动作日志去重结构体
 * 统一管理 RAISE 和 DEMOTE 动作日志去重，共用 g_act_dedup 数组。
 * 比较规则按 kind 严格区分，禁止混用：
 * - ACT_KIND_RAISE:  去重 key 为 pid + target（不含 cur，因应用前后台切换时 cur 在 0/40 之间波动，
 *   若带 cur 无法去重；期间见到的 cur 记录在 cur_min..cur_max 并在 10min 汇总或 (end) 时输出）。
 * - ACT_KIND_DEMOTE: 去重 key 为 pid + cur + target（现行口径严格匹配起始档位 cur 与目标档位 target，
 *   任意一个变化均视为新动作并冲刷旧聚合）。
 */
typedef struct {
    int     kind;              /* ACT_KIND_RAISE or ACT_KIND_DEMOTE */
    pid_t   pid;
    int32_t target;
    int32_t cur;               /* For DEMOTE: matched cur; For RAISE: last seen cur */
    int32_t cur_min;           /* For RAISE: min cur seen */
    int32_t cur_max;           /* For RAISE: max cur seen */
    char    label[64];
    char    frag[256];
    time_t  last_log_time;
    int     suppressed_count;
} act_dedup_t;

static act_dedup_t g_act_dedup[MAX_ACT_DEDUP];
static int g_act_dedup_count = 0;
static int g_act_dedup_head = 0;

static void act_dedup_clear(pid_t pid) {
    for (int i = 0; i < g_act_dedup_count; i++) {
        if (g_act_dedup[i].pid == pid) {
            if (g_act_dedup[i].suppressed_count > 0) {
                if (g_act_dedup[i].kind == ACT_KIND_RAISE) {
                    JPLOG(JP_INFO, "%-6s pid=%d %s %s ->%d ×%d (end) cur=%d..%d",
                           "RAISE", (int)g_act_dedup[i].pid, g_act_dedup[i].label, g_act_dedup[i].frag,
                           (int)g_act_dedup[i].target, g_act_dedup[i].suppressed_count,
                           (int)g_act_dedup[i].cur_min, (int)g_act_dedup[i].cur_max);
                } else {
                    JPLOG(JP_INFO, "%-6s pid=%d %s %s %d->%d ×%d (end)",
                           "DEMOTE", (int)g_act_dedup[i].pid, g_act_dedup[i].label, g_act_dedup[i].frag,
                           (int)g_act_dedup[i].cur, (int)g_act_dedup[i].target, g_act_dedup[i].suppressed_count);
                }
            }
            g_act_dedup_count--;
            g_act_dedup[i] = g_act_dedup[g_act_dedup_count];
            i--;
        }
    }
}

static void act_dedup_flush_all(void) {
    for (int i = 0; i < g_act_dedup_count; i++) {
        if (g_act_dedup[i].suppressed_count > 0) {
            if (g_act_dedup[i].kind == ACT_KIND_RAISE) {
                JPLOG(JP_INFO, "%-6s pid=%d %s %s ->%d ×%d (end) cur=%d..%d",
                       "RAISE", (int)g_act_dedup[i].pid, g_act_dedup[i].label, g_act_dedup[i].frag,
                       (int)g_act_dedup[i].target, g_act_dedup[i].suppressed_count,
                       (int)g_act_dedup[i].cur_min, (int)g_act_dedup[i].cur_max);
            } else {
                JPLOG(JP_INFO, "%-6s pid=%d %s %s %d->%d ×%d (end)",
                       "DEMOTE", (int)g_act_dedup[i].pid, g_act_dedup[i].label, g_act_dedup[i].frag,
                       (int)g_act_dedup[i].cur, (int)g_act_dedup[i].target, g_act_dedup[i].suppressed_count);
            }
            g_act_dedup[i].suppressed_count = 0;
        }
    }
    g_act_dedup_count = 0;
}

static void act_log_success(int kind, pid_t pid, const char *path, const char *frag, int32_t cur, int32_t target) {
    time_t now = time(NULL);
    for (int i = 0; i < g_act_dedup_count; i++) {
        act_dedup_t *entry = &g_act_dedup[i];
        if (entry->kind == kind && entry->pid == pid) {
            if (kind == ACT_KIND_RAISE) {
                if (entry->target == target) {
                    entry->suppressed_count++;
                    if (cur < entry->cur_min) entry->cur_min = cur;
                    if (cur > entry->cur_max) entry->cur_max = cur;
                    entry->cur = cur;
                    if (now - entry->last_log_time >= 600) {
                        JPLOG(JP_INFO, "%-6s pid=%d %s %s ->%d ×%d (10min) cur=%d..%d",
                               "RAISE", (int)pid, entry->label, entry->frag, (int)target,
                               entry->suppressed_count, (int)entry->cur_min, (int)entry->cur_max);
                        entry->last_log_time = now;
                        entry->suppressed_count = 0;
                        entry->cur_min = cur;
                        entry->cur_max = cur;
                    }
                    return;
                }
                if (entry->suppressed_count > 0) {
                    JPLOG(JP_INFO, "%-6s pid=%d %s %s ->%d ×%d (end) cur=%d..%d",
                           "RAISE", (int)pid, entry->label, entry->frag, (int)entry->target,
                           entry->suppressed_count, (int)entry->cur_min, (int)entry->cur_max);
                }
                entry->target = target;
                entry->cur = cur;
                entry->cur_min = cur;
                entry->cur_max = cur;
                strncpy(entry->frag, frag ? frag : "", sizeof(entry->frag) - 1);
                entry->frag[sizeof(entry->frag) - 1] = '\0';
                strncpy(entry->label, proc_label_log(path), sizeof(entry->label) - 1);
                entry->label[sizeof(entry->label) - 1] = '\0';
                entry->last_log_time = now;
                entry->suppressed_count = 0;
                JPLOG(JP_INFO, "%-6s pid=%d %s %s %d->%d ok",
                       "RAISE", (int)pid, entry->label, entry->frag, (int)cur, (int)target);
                return;
            } else {
                if (entry->cur == cur && entry->target == target) {
                    entry->suppressed_count++;
                    if (now - entry->last_log_time >= 600) {
                        JPLOG(JP_INFO, "%-6s pid=%d %s %s %d->%d ×%d (10min)",
                               "DEMOTE", (int)pid, entry->label, entry->frag, (int)cur, (int)target,
                               entry->suppressed_count);
                        entry->last_log_time = now;
                        entry->suppressed_count = 0;
                    }
                    return;
                }
                if (entry->suppressed_count > 0) {
                    JPLOG(JP_INFO, "%-6s pid=%d %s %s %d->%d ×%d (end)",
                           "DEMOTE", (int)pid, entry->label, entry->frag, (int)entry->cur, (int)entry->target,
                           entry->suppressed_count);
                }
                entry->cur = cur;
                entry->target = target;
                strncpy(entry->frag, frag ? frag : "", sizeof(entry->frag) - 1);
                entry->frag[sizeof(entry->frag) - 1] = '\0';
                strncpy(entry->label, proc_label_log(path), sizeof(entry->label) - 1);
                entry->label[sizeof(entry->label) - 1] = '\0';
                entry->last_log_time = now;
                entry->suppressed_count = 0;
                JPLOG(JP_INFO, "%-6s pid=%d %s %s %d->%d ok",
                       "DEMOTE", (int)pid, entry->label, entry->frag, (int)cur, (int)target);
                return;
            }
        }
    }
    act_dedup_t *entry = NULL;
    if (g_act_dedup_count < MAX_ACT_DEDUP) {
        entry = &g_act_dedup[g_act_dedup_count++];
    } else {
        entry = &g_act_dedup[g_act_dedup_head];
        if (entry->suppressed_count > 0) {
            if (entry->kind == ACT_KIND_RAISE) {
                JPLOG(JP_INFO, "%-6s pid=%d %s %s ->%d ×%d (end) cur=%d..%d",
                       "RAISE", (int)entry->pid, entry->label, entry->frag, (int)entry->target,
                       entry->suppressed_count, (int)entry->cur_min, (int)entry->cur_max);
            } else {
                JPLOG(JP_INFO, "%-6s pid=%d %s %s %d->%d ×%d (end)",
                       "DEMOTE", (int)entry->pid, entry->label, entry->frag, (int)entry->cur, (int)entry->target,
                       entry->suppressed_count);
            }
        }
        g_act_dedup_head = (g_act_dedup_head + 1) % MAX_ACT_DEDUP;
    }
    entry->kind = kind;
    entry->pid = pid;
    entry->cur = cur;
    entry->cur_min = cur;
    entry->cur_max = cur;
    entry->target = target;
    strncpy(entry->frag, frag ? frag : "", sizeof(entry->frag) - 1);
    entry->frag[sizeof(entry->frag) - 1] = '\0';
    strncpy(entry->label, proc_label_log(path), sizeof(entry->label) - 1);
    entry->label[sizeof(entry->label) - 1] = '\0';
    entry->last_log_time = now;
    entry->suppressed_count = 0;

    const char *act_name = (kind == ACT_KIND_RAISE) ? "RAISE" : "DEMOTE";
    JPLOG(JP_INFO, "%-6s pid=%d %s %s %d->%d ok",
           act_name, (int)pid, entry->label, entry->frag, (int)cur, (int)target);
}

#define MAX_RULES 32
typedef struct {
    char    frag[256];
    int32_t target;   /* 目标档位：protect 为 30–95，demote 为 0–30 */
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
typedef struct {
    pid_t   pid;
    int32_t band;
    int     seen;
    int32_t orig;
    int     orig_known;
    int32_t sys_band;
    int     sys_band_known;
} applied_t;
static applied_t g_applied[MAX_APPLIED];
static int g_napplied = 0;

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

#define MAX_OLD_TARGETS 64
typedef struct {
    pid_t   pid;
    int32_t target;
    int32_t orig;
    int     orig_known;
} old_target_t;
static old_target_t g_old_targets[MAX_OLD_TARGETS];
static int g_nold_targets = 0;

static int old_target_find(pid_t pid, int32_t *target_out, int32_t *orig_out, int *orig_known_out) {
    for (int i = 0; i < g_nold_targets; i++) {
        if (g_old_targets[i].pid == pid) {
            if (target_out) *target_out = g_old_targets[i].target;
            if (orig_out) *orig_out = g_old_targets[i].orig;
            if (orig_known_out) *orig_known_out = g_old_targets[i].orig_known;
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
        if (strncmp(line, "applied_orig=", 13) == 0) {
            char *p = line + 13;
            int opid = 0, oorig = 0, oband = 0;
            if (sscanf(p, "%d|%d|%d", &opid, &oorig, &oband) == 3 && opid > 0) {
                int found = 0;
                for (int i = 0; i < g_nold_targets; i++) {
                    if (g_old_targets[i].pid == (pid_t)opid) {
                        g_old_targets[i].orig = oorig;
                        g_old_targets[i].orig_known = 1;
                        g_old_targets[i].target = oband;
                        found = 1;
                        break;
                    }
                }
                if (!found && g_nold_targets < MAX_OLD_TARGETS) {
                    g_old_targets[g_nold_targets].pid = (pid_t)opid;
                    g_old_targets[g_nold_targets].target = oband;
                    g_old_targets[g_nold_targets].orig = oorig;
                    g_old_targets[g_nold_targets].orig_known = 1;
                    g_nold_targets++;
                }
            }
            continue;
        }
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
        if (pid > 0) {
            int found = 0;
            for (int i = 0; i < g_nold_targets; i++) {
                if (g_old_targets[i].pid == (pid_t)pid) {
                    g_old_targets[i].target = target;
                    found = 1;
                    break;
                }
            }
            if (!found && g_nold_targets < MAX_OLD_TARGETS) {
                g_old_targets[g_nold_targets].pid = (pid_t)pid;
                g_old_targets[g_nold_targets].target = target;
                g_old_targets[g_nold_targets].orig = 0;
                g_old_targets[g_nold_targets].orig_known = 0;
                g_nold_targets++;
            }
        }
    }
    fclose(f);
}

static int parse_loglevel_str(const char *frag) {
    if (!frag) return JP_LOG_INFO;
    if (strcasecmp(frag, "off") == 0) return JP_LOG_OFF;
    if (strcasecmp(frag, "error") == 0 || strcasecmp(frag, "err") == 0) return JP_LOG_ERR;
    if (strcasecmp(frag, "warning") == 0 || strcasecmp(frag, "warn") == 0 || strcmp(frag, "1") == 0) return JP_LOG_WARN;
    if (strcasecmp(frag, "info") == 0 || strcmp(frag, "2") == 0) return JP_LOG_INFO;
    if (strcasecmp(frag, "debug") == 0 || strcmp(frag, "3") == 0) return JP_LOG_DEBUG;
    return JP_LOG_INFO;
}

static void init_loglevel(void) {
    g_loglevel = JP_LOG_INFO;
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        char verb[32], frag[256];
        int n = sscanf(p, "%31s %255s", verb, frag);
        if (n >= 2 && strcmp(verb, "loglevel") == 0) {
            g_loglevel = parse_loglevel_str(frag);
        }
    }
    fclose(f);
}

static int load_config(void) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return -1;
    g_nprotect = g_ndemote = 0;
    g_has_bid_rules = 0;
    int parsed_loglevel = JP_LOG_INFO;
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
            if (n < 3) band = BAND_BACKGROUND;                    /* 默认 30 */
            if (band < BAND_IDLE) band = BAND_IDLE;               /* 夹到 0–30 */
            if (band > BAND_BACKGROUND) band = BAND_BACKGROUND;
            strncpy(g_demote[g_ndemote].frag, frag, 255);
            g_demote[g_ndemote].target = band;
            g_demote[g_ndemote].is_bid = is_bid;
            if (is_bid) g_has_bid_rules = 1;
            g_ndemote++;
        } else if (strcmp(verb, "loglevel") == 0) {
            parsed_loglevel = parse_loglevel_str(frag);
        }
    }
    fclose(f);
    if (parsed_loglevel != g_loglevel) {
        int old_lvl = g_loglevel;
        g_loglevel = parsed_loglevel;
        if (old_lvl != JP_LOG_OFF && g_loglevel != JP_LOG_OFF) {
            JPLOG(JP_INFO, "LOGLVL %s->%s", loglevel_to_str(old_lvl), loglevel_to_str(g_loglevel));
        }
    }
    return 0;
}

static volatile sig_atomic_t g_got_sigterm = 0;
static volatile sig_atomic_t g_pulse_stop_requested = 0;

static void sigterm_handler(int sig) {
    (void)sig;
    g_got_sigterm = 1;
    g_pulse_stop_requested = 1;
}

static int should_stop(void) {
    if (g_got_sigterm) return 1;
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
        struct tm tm_val;
        localtime_r(&t, &tm_val);
        strftime(g_last_err_time, sizeof(g_last_err_time),
                 "%Y-%m-%d %H:%M:%S", &tm_val);
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
 *     restart_support=1
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
static int    g_restart_cfg_bad_count = 0; /* restart.txt 无效行数 (v0.7.8 C1-N6) */
static void pulse_write_status(FILE *m);

static void write_status(int protected_now, int demoted_pids,
                         protected_entry_t *plist, int nplist,
                         protected_entry_t *dlist, int ndlist) {
    /* 1. 先在内存里拼出除 updated= 以外的全部内容 */
    char *body = NULL; size_t blen = 0;
    FILE *m = open_memstream(&body, &blen);
    if (!m) return;
    fprintf(m, "bid_support=1\n"
               "restart_support=1\n"
               "version=" JP_VERSION "\n"
               "commit=" JP_COMMIT "\n"
               "cfg_bad=%d\n"
               "protect_rules=%d\ndemote_rules=%d\n"
               "protected_pids=%d\ndemoted_pids=%d\nreasserts_total=%lu\n",
            g_restart_cfg_bad_count,
            g_nprotect, g_ndemote, protected_now, demoted_pids, g_reassert_count);
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
    for (int i = 0; i < ndlist; i++)
        fprintf(m, "demoted_app=%s|%d|%d|%d\n",
                dlist[i].frag, (int)dlist[i].pid, dlist[i].cur, dlist[i].target);
    for (int i = 0; i < ndlist; i++)
        fprintf(m, "demoted_proc=%d|%s|%s\n",
                (int)dlist[i].pid, dlist[i].label, dlist[i].bid);
    for (int i = 0; i < g_napplied; i++) {
        if (g_applied[i].orig_known) {
            fprintf(m, "applied_orig=%d|%d|%d\n", (int)g_applied[i].pid, (int)g_applied[i].orig, (int)g_applied[i].band);
        }
    }
    pulse_write_status(m);
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
    struct tm tm_val;
    localtime_r(&t, &tm_val);
    char ts[64]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_val);
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
    if (g_got_sigterm) return 0;
    if (g_lock_fd < 0) {
        for (int i = 0; i < secs && !g_got_sigterm; i++) {
            sleep(1);
        }
        return 0;
    }
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

/* =========================================================================
 * 超内存自动重启规则模块 (v0.7.7 C1)
 * ========================================================================= */
#define RESTART_CONFIG_PATH CONFIG_DIR "/restart.txt"
#define RESTART_MAX_RULES   32
#define RESTART_MIN_THR_MB  200
#define RESTART_MIN_BG_SEC  60
#define RESTART_MAX_BG_SEC  7200
#define RESTART_MIN_MAX     1
#define RESTART_MAX_MAX     10

typedef struct {
    char    bid[128];
    int32_t thr_mb;
    int32_t bg_sec;
    int32_t max_count;
    int32_t relaunch;
} restart_rule_t;

static restart_rule_t g_restart_rules[RESTART_MAX_RULES];
static int g_nrestart_rules = 0;
static int g_restart_cfg_file_absent = 1;
static int g_restart_cfg_first_load = 1;

/* =========================================================================
 * 超内存自动重启：状态持久化与状态机定义 (v0.7.7 C2)
 * ========================================================================= */
#define RESTART_STATE_PATH     CONFIG_DIR "/restart_state.txt"
#define RESTART_STATE_TMP_PATH CONFIG_DIR "/restart_state.tmp"

#define RESTART_STATE_MAX_ENTRIES 512
#define RESTART_STATE_MAX_LAST    64

typedef struct {
    char bid[128];
    char date[16]; /* YYYY-MM-DD */
    int  count;
} rst_daily_entry_t;

typedef struct {
    char   bid[128];
    char   boot_uuid[64];
    time_t mono_sec;
} rst_last_entry_t;

static rst_daily_entry_t g_rst_daily[RESTART_STATE_MAX_ENTRIES];
static int g_rst_ndaily = 0;
static rst_last_entry_t g_rst_last[RESTART_STATE_MAX_LAST];
static int g_rst_nlast = 0;
static time_t g_rst_state_mtime = 0;

typedef enum {
    RST_STATE_IDLE = 0,
    RST_STATE_WAIT_PID,
    RST_STATE_WAIT_SETTLE
} rst_state_t;

typedef struct {
    rst_state_t     state;
    char            bid[128];
    pid_t           old_pid;
    pid_t           new_pid;
    int32_t         relaunch;
    struct timespec t_launch;
} rst_sm_t;

static rst_sm_t g_rst_sm;

typedef struct {
    char     bid[128];
    pid_t    tracked_pid;
    int      in_bg;
    time_t   bg_start_mono;
    time_t   sample_1_mono;
    uint64_t sample_1_fp_mb;
    int      sample_count;
    time_t   last_skip_mono;
    char     last_skip_reason[16];
} rst_rule_state_t;

static rst_rule_state_t g_rst_rule_states[RESTART_MAX_RULES];
static int g_nrst_rule_states = 0;

static void rst_clear_rule_state(rst_rule_state_t *st) {
    if (!st) return;
    st->tracked_pid = 0;
    st->in_bg = 0;
    st->bg_start_mono = 0;
    st->sample_1_mono = 0;
    st->sample_1_fp_mb = 0;
    st->sample_count = 0;
    st->last_skip_mono = 0;
    st->last_skip_reason[0] = '\0';
}

/* =========================================================================
 * 后台内存释放规则模块 (v0.7.8 C2-0)
 * ========================================================================= */
#define PULSE_CONFIG_PATH     CONFIG_DIR "/pulse.txt"
#define PULSE_STATE_PATH      CONFIG_DIR "/pulse.state"
#define PULSE_STATE_TMP_PATH  CONFIG_DIR "/pulse.state.tmp"

#define PULSE_MAX_RULES       32
#define PULSE_MIN_TARGET_MB   300
#define PULSE_MAX_TARGET_MB   1500
#define PULSE_MIN_N_MIN       5
#define PULSE_MAX_N_MIN       60
#define PULSE_MIN_DAYMAX      1
#define PULSE_MAX_DAYMAX      100
#define PULSE_MIN_DELAY_MIN   1
#define PULSE_MAX_DELAY_MIN   60
#define PULSE_GLOBAL_DAYMAX   100
#define PULSE_DEFAULT_WECHAT_TARGET 550

typedef struct {
    char    bid[128];
    int     enabled;        /* 1: on, 0: off */
    int32_t target_mb;      /* 300..1500 */
    int32_t n_min;          /* 5..60 */
    int32_t daymax;         /* 1..100 */
    int32_t delay_min;      /* 1..60 */
} pulse_rule_t;

#ifndef MEMORYSTATUS_CMD_SET_MEMLIMIT_PROPERTIES
#define MEMORYSTATUS_CMD_SET_MEMLIMIT_PROPERTIES  7
#endif
#ifndef MEMORYSTATUS_CMD_GET_MEMLIMIT_PROPERTIES
#define MEMORYSTATUS_CMD_GET_MEMLIMIT_PROPERTIES  8
#endif
#ifndef MEMORYSTATUS_MEMLIMIT_ATTR_FATAL
#define MEMORYSTATUS_MEMLIMIT_ATTR_FATAL          0x1
#endif

typedef struct memorystatus_memlimit_properties {
    int32_t  memlimit_active;        /* 前台限额 MB */
    uint32_t memlimit_active_attr;   /* 前台属性 (0x1=FATAL) */
    int32_t  memlimit_inactive;      /* 后台限额 MB */
    uint32_t memlimit_inactive_attr; /* 后台属性 (0x1=FATAL) */
} memorystatus_memlimit_properties_t;

_Static_assert(sizeof(memorystatus_memlimit_properties_t) == 16,
               "memorystatus_memlimit_properties_t layout mismatch");

typedef struct {
    pid_t    pid;
    uint64_t uniqueid;
    uint64_t start_sec;
    uint64_t start_usec;
    char     path[PROC_PIDPATHINFO_MAXSIZE];
} proc_identity_t;

typedef struct {
    pid_t    pid;
    char     bid[128];
    uint64_t start_sec;
    uint64_t start_usec;
    uint64_t uniqueid;
    char     path[PROC_PIDPATHINFO_MAXSIZE];
    int32_t  orig_active;
    uint32_t orig_active_attr;
    int32_t  orig_inactive;
    uint32_t orig_inactive_attr;
    int      suspect;
} pulse_saved_state_t;

/* 配置文件状态变更检查通用结构体 (供 restart.txt 与 pulse.txt 共同复用) */
typedef struct {
    int             initialized;   /* 0: 未初始化; 1: 已记录状态 */
    int             file_absent;   /* 上次检查时文件是否不存在 (1: 不存在; 0: 存在) */
    ino_t           ino;           /* 上次文件的 st_ino */
    off_t           size;          /* 上次文件的 st_size */
    struct timespec mtimespec;     /* 上次文件的 st_mtimespec (含纳秒 tv_nsec) */
    int             open_failed;   /* 是否处于打开失败状态 (用于只打一次 Warn) */
} jp_file_stamp_t;

static pulse_rule_t g_pulse_rules[PULSE_MAX_RULES];
static int g_npulse_rules = 0;
static int g_pulse_cfg_file_absent = 1;
static int g_pulse_cfg_first_load = 1;
static int g_pulse_cfg_bad_count = 0;
static jp_file_stamp_t s_pulse_cfg_stamp = {0};

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
            JPLOG(JP_DEBUG, "MEMP repair_skip reason=fp_fail pid=%d", (int)pid);
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
    JPLOG(JP_DEBUG, "MEMP repair_skip reason=fp_fail pid=%d", (int)pid);
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
            JPLOG(JP_DEBUG, "MEMP level_first val=%d errno=%s", lvl, eb);
        } else {
            JPLOG(JP_DEBUG, "MEMP level_first val=NA errno=%s", eb);
        }
    }
    if (err_out) *err_out = err;
    if (rc == 0 && level_out) *level_out = lvl;
    return rc;
}

/* 单条 memp 规则匹配判定 (供 memp_match_rule 和 rst_c4_floor_reset 共同复用) */
static int memp_rule_matches(const memp_rule_t *rule, const char *path, const char *app_bid) {
    if (!rule) return 0;
    if (rule->is_bid) {
        if (app_bid && strcmp(app_bid, rule->frag + 4) == 0) {
            return 1;
        }
    } else {
        if (path && path[0] != '\0' && strstr(path, rule->frag)) {
            return 1;
        }
    }
    return 0;
}

/* 规则匹配：按文件顺序逐条比，取第一条匹配的规则 */
static memp_rule_t *memp_match_rule(const char *path, const char *app_bid) {
    for (int i = 0; i < g_nmemp_rules; i++) {
        if (memp_rule_matches(&g_memp_rules[i], path, app_bid)) {
            return &g_memp_rules[i];
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
    JPLOG(JP_ERR, "MEMP fatal reason=%s", reason);
}

static void memp_enter_unsupported(const char *reason, int err) {
    g_memp_state = MEMP_STATE_UNSUPPORTED;
    strncpy(g_memp_unsupported_reason, reason, sizeof(g_memp_unsupported_reason) - 1);
    g_memp_unsupported_reason[sizeof(g_memp_unsupported_reason) - 1] = '\0';
    g_memp_last_errno = err;
    char eb[128];
    memp_fmt_errno(err, eb, sizeof(eb));
    JPLOG(JP_WARN, "MEMP unsupported reason=%s errno=%s", reason, eb);
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
        JPLOG(JP_DEBUG, "MEMP taint_drop pid=%d", (int)dropped_pid);
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
    JPLOG(JP_INFO, "MEMP taint app=%s pid=%d reason=%s", e->frag, (int)pid, reason);
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
            JPLOG(JP_ERR, "MEMP taint_load_fail line=- errno=%s", eb);
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
            JPLOG(JP_ERR, "MEMP taint_load_fail line=%d errno=%s", line_no, eb);
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
        JPLOG(JP_ERR, "MEMP memsize_fail errno=%s", eb);
        return;
    }
    uint64_t mb = memsize / (1024ULL * 1024ULL);
    uint64_t half = mb / 2;
    if (half < 200) {
        g_memp_memcap = 2048;
        g_memp_memcap_fallback = 1;
        JPLOG(JP_WARN, "MEMP memsize_small bytes=%llu", (unsigned long long)memsize);
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
    JPLOG(JP_ERR, "MEMP fatal_persist reason=%s", reason);
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
    JPLOG(JP_INFO, "MEMP boot_reset ret=%d errno=%s", rc, eb);

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
            JPLOG(JP_ERR, "MEMP restore_fail pid=%d errno=%s", (int)yielded[i].pid, eb);
            if (yielded[i].is_sys) {
                char pname[32];
                memp_get_proc_name(yielded[i].pid, pname, sizeof(pname));
                JPLOG(JP_ERR, "MEMP yield_sys_fail pid=%d name=%s step=restore errno=%s",
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

    JPLOG(JP_DEBUG, "MEMP skip app=%s pid=%d fp=%.1f thr=%d reason=%s yielded=%d yielded_sys=%d errno=%s comp=%s restored=ok:%d,moved:%d,gone:%d,rfail:%d",
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
    /* 函数名保留 memp_floor_format_median、文件格式保留 med= 以保持向后兼容。
     * 当前实现含义已调整为「次小值」底线统计（增加单次极低异常防护）：
     * 保持 nsamp < 3 判 NA；对 >=3 个样本，取次小值 sorted[1]（剔除单次异常低样本如重启/重建），
     * 避免瞬态极低值长期钉死底线导致 backoff 失真。当正常低样本持续到来或异常样本滑出窗口后底线恢复。
     * 注意：当有效样本数恰好为 3 时，sorted[1] 就是中位数，计算结果与原中位数逻辑完全一致。
     */
    double min_val = sorted[1];
    snprintf(buf, sz, "%.1f", min_val);
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
        /* 兼容说明：key 保持 med= 不变，实际存储的是次小值底线 */
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
        JPLOG(JP_ERR, "MEMP floor_save_fail errno=%s", eb);
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
                JPLOG(JP_INFO, "MEMP floor_reset reason=version");
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
        JPLOG(JP_INFO, "MEMP floor_reset reason=corrupt");
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
            JPLOG(JP_INFO, "MEMP backoff app=%s cd=%d floor=%s", rule->frag, (int)rule->eff_cd, med_str);
        }
    } else {
        if (rule->eff_cd != base_cd) {
            JPLOG(JP_INFO, "MEMP backoff_reset app=%s cd=%d", rule->frag, (int)base_cd);
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

    JPLOG(JP_INFO, "MEMP post app=%s pid=%d fp_before=%.1f fp_post30=%s dt=%ld fg_ok=%d nsamp=%d freed=%s",
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
        JPLOG(JP_INFO, "MEMP post app=%s pid=%d fp_before=%.1f fp_post30=dropped dt=%ld fg_ok=0 nsamp=%d freed=NA",
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
                JPLOG(JP_ERR, "MEMP cfg_bad line=%d text=%s", line_no, raw_text);
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
                JPLOG(JP_ERR, "MEMP cfg_bad line=%d text=%s", line_no, raw_text);
                continue;
            }

            if (strcmp(tokens[0], "cooldown") == 0) {
                if (ntokens != 2) {
                    JPLOG(JP_ERR, "MEMP cfg_bad line=%d text=%s", line_no, raw_text);
                    continue;
                }
                char *endp = NULL;
                long cd = strtol(tokens[1], &endp, 10);
                if (!endp || *endp != '\0' || cd < MEMP_MIN_COOLDOWN || cd > MEMP_MAX_COOLDOWN) {
                    JPLOG(JP_ERR, "MEMP cfg_bad line=%d text=%s", line_no, raw_text);
                    continue;
                }
                new_cooldown = (int)cd;
            } else {
                /* 规则行 */
                if (nparsed >= MEMP_MAX_RULES) {
                    JPLOG(JP_ERR, "MEMP cfg_bad line=%d text=%s", line_no, raw_text);
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
                    JPLOG(JP_ERR, "MEMP cfg_bad line=%d text=%s", line_no, raw_text);
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
                    JPLOG(JP_ERR, "MEMP cfg_bad line=%d text=%s", line_no, raw_text);
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
                    JPLOG(JP_ERR, "MEMP cfg_bad line=%d text=%s", line_no, raw_text);
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
        JPLOG(JP_INFO, "MEMP cfg rules=%d cooldown=%d file=%s memcap=%s rule_cd=%d",
               nparsed, new_cooldown, file_absent ? "absent" : "ok", cap_str, rule_cd_count);

        for (int i = 0; i < nparsed; i++) {
            char cd_str[32];
            if (parsed_rules[i].cooldown > 0) {
                snprintf(cd_str, sizeof(cd_str), "%d", (int)parsed_rules[i].cooldown);
            } else {
                snprintf(cd_str, sizeof(cd_str), "global");
            }
            JPLOG(JP_DEBUG, "MEMP cfg_rule i=%d app=%s thr=%d cd=%s",
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

static int restart_is_blocked_bid(const char *bid) {
    if (!bid || bid[0] == '\0') return 1;
    if (strcasecmp(bid, "SpringBoard") == 0 ||
        strcasecmp(bid, "com.apple.springboard") == 0) return 1;
    if (strcasecmp(bid, "backboardd") == 0 ||
        strcasecmp(bid, "com.apple.backboardd") == 0) return 1;
    if (strcasecmp(bid, "launchd") == 0) return 1;
    if (strcmp(bid, "com.lili.jetpilot") == 0) return 1;
    /* 路径片段或无效字符：bid 不得包含 '/' */
    if (strchr(bid, '/') != NULL) return 1;
    return 0;
}

static int restart_rule_cfg_equal(const restart_rule_t *a, const restart_rule_t *b) {
    if (strcmp(a->bid, b->bid) != 0) return 0;
    if (a->thr_mb != b->thr_mb) return 0;
    if (a->bg_sec != b->bg_sec) return 0;
    if (a->max_count != b->max_count) return 0;
    if (a->relaunch != b->relaunch) return 0;
    return 1;
}

/* =========================================================================
 * 配置文件状态变更检查与打开通用函数 (供 restart.txt 与卡 2 的 pulse.txt 共同复用)
 * 注: jp_file_stamp_t 结构体定义已前置于文件上方
 * ========================================================================= */

/* 检查指定路径文件的内容/元数据是否发生变更。
 * 判定依据：st_ino + st_size + st_mtimespec (纳秒)。
 * App 采用 atomic rename 保存配置，每次都会更换 inode；手改或覆盖时纳秒 mtime 也会变化。
 * 卡 2 的 pulse.txt 加载应直接复用此函数与 jp_file_stamp_t。
 *
 * 参数：
 *   path: 文件绝对路径
 *   stamp: 状态跟踪结构体指针（调用者维护独立静态或堆变量）
 *   out_file_absent: (可选) 输出当前文件是否存在 (1: 不存在; 0: 存在)
 *
 * 返回值：
 *   1: 文件发生变化（首次检查、文件存在性变更、或 ino/size/mtimespec 变更）
 *   0: 文件无变化
 */
static int jp_file_stat_changed(const char *path, jp_file_stamp_t *stamp, int *out_file_absent) {
    if (!path || !stamp) return 0;

    struct stat st;
    int stat_rc = stat(path, &st);
    int cur_absent = (stat_rc != 0);
    if (out_file_absent) {
        *out_file_absent = cur_absent;
    }

    if (!stamp->initialized) {
        stamp->initialized = 1;
        stamp->file_absent = cur_absent;
        if (!cur_absent) {
            stamp->ino = st.st_ino;
            stamp->size = st.st_size;
            stamp->mtimespec = st.st_mtimespec;
        } else {
            stamp->ino = 0;
            stamp->size = -1;
            memset(&stamp->mtimespec, 0, sizeof(stamp->mtimespec));
        }
        return 1;
    }

    if (cur_absent != stamp->file_absent) {
        stamp->file_absent = cur_absent;
        if (!cur_absent) {
            stamp->ino = st.st_ino;
            stamp->size = st.st_size;
            stamp->mtimespec = st.st_mtimespec;
        } else {
            stamp->ino = 0;
            stamp->size = -1;
            memset(&stamp->mtimespec, 0, sizeof(stamp->mtimespec));
        }
        return 1;
    }

    if (!cur_absent) {
        if (st.st_ino != stamp->ino ||
            st.st_size != stamp->size ||
            st.st_mtimespec.tv_sec != stamp->mtimespec.tv_sec ||
            st.st_mtimespec.tv_nsec != stamp->mtimespec.tv_nsec) {
            stamp->ino = st.st_ino;
            stamp->size = st.st_size;
            stamp->mtimespec = st.st_mtimespec;
            return 1;
        }
    }

    return 0;
}

/*
 * 检查配置文件变更并尝试打开文件。
 * 供 restart.txt 与卡 2 的 pulse.txt 共同复用。
 *
 * 参数：
 *   path: 文件绝对路径
 *   stamp: 状态跟踪结构体指针
 *   log_tag: 日志模块标签（如 "RST"、"PLS"）
 *   out_file_absent: (输出) 文件是否存在 (1: 不存在; 0: 存在)
 *   out_file: (输出) 成功打开时的 FILE* 指针；未打开或打开失败时置 NULL
 *
 * 返回值：
 *   0: 文件无变化，无需重新载入
 *   1: 文件发生变化且状态已处理（若 out_file != NULL 则成功打开；若 out_file == NULL 且 *out_file_absent == 1 则为文件缺失）
 *  -1: 文件存在但打开失败（EMFILE/EINTR 等），stamp->initialized 已置 0，下一轮将重试，已打 Warn（单次）
 */
static int jp_config_open(const char *path, jp_file_stamp_t *stamp, const char *log_tag,
                          int *out_file_absent, FILE **out_file) {
    if (out_file) *out_file = NULL;
    int absent = 0;
    int changed = jp_file_stat_changed(path, stamp, &absent);
    if (out_file_absent) *out_file_absent = absent;

    if (!changed) {
        return 0;
    }

    if (absent) {
        stamp->open_failed = 0;
        return 1;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        int err = errno;
        stamp->initialized = 0;
        if (!stamp->open_failed) {
            stamp->open_failed = 1;
            JPLOG(JP_WARN, "%-6s cfg_open_fail errno=%d", log_tag ? log_tag : "CFG", err);
        }
        return -1;
    }

    stamp->open_failed = 0;
    if (out_file) {
        *out_file = f;
    } else {
        fclose(f);
    }
    return 1;
}

static jp_file_stamp_t s_restart_cfg_stamp = {0};

static int restart_load_config(void) {
    int file_absent = 0;
    FILE *f = NULL;
    int rc = jp_config_open(RESTART_CONFIG_PATH, &s_restart_cfg_stamp, "RST", &file_absent, &f);

    if (rc == 0) {
        return g_restart_cfg_file_absent ? -1 : 0;
    }
    if (rc < 0) {
        return g_restart_cfg_file_absent ? -1 : 0;
    }

    restart_rule_t parsed_rules[RESTART_MAX_RULES];
    int nparsed = 0;
    int bad_line_count = 0;

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
                bad_line_count++;
                JPLOG(JP_ERR, "%-6s cfg_bad line=%d text=%s", "RST", line_no, raw_text);
                continue;
            }

            char line_copy[512];
            strncpy(line_copy, raw_text, sizeof(line_copy) - 1);
            line_copy[sizeof(line_copy) - 1] = '\0';

            char *tokens[16];
            int ntokens = 0;
            char *saveptr = NULL;
            char *tok = strtok_r(line_copy, " \t", &saveptr);
            while (tok) {
                if (ntokens < 15) {
                    tokens[ntokens] = tok;
                }
                ntokens++;
                tok = strtok_r(NULL, " \t", &saveptr);
            }

            if (ntokens != 5) {
                bad_line_count++;
                JPLOG(JP_ERR, "%-6s cfg_bad line=%d text=%s", "RST", line_no, raw_text);
                continue;
            }

            if (strncmp(tokens[0], "bid:", 4) != 0) {
                bad_line_count++;
                JPLOG(JP_ERR, "%-6s cfg_bad line=%d text=%s", "RST", line_no, raw_text);
                continue;
            }

            const char *bid = tokens[0] + 4;
            if (bid[0] == '\0' || strlen(bid) >= sizeof(parsed_rules[0].bid) || restart_is_blocked_bid(bid)) {
                bad_line_count++;
                JPLOG(JP_ERR, "%-6s cfg_bad line=%d text=%s", "RST", line_no, raw_text);
                continue;
            }

            int duplicate = 0;
            for (int i = 0; i < nparsed; i++) {
                if (strcmp(parsed_rules[i].bid, bid) == 0) {
                    duplicate = 1;
                    break;
                }
            }
            if (duplicate) {
                bad_line_count++;
                JPLOG(JP_ERR, "%-6s cfg_bad line=%d text=%s", "RST", line_no, raw_text);
                continue;
            }

            int has_thr = 0, has_bg = 0, has_max = 0, has_relaunch = 0;
            int32_t val_thr = 0, val_bg = 0, val_max = 0, val_relaunch = 0;
            int bad = 0;

            for (int k = 1; k < 5; k++) {
                char *eq = strchr(tokens[k], '=');
                if (!eq || eq == tokens[k] || *(eq + 1) == '\0') {
                    bad = 1;
                    break;
                }
                *eq = '\0';
                char *key = tokens[k];
                char *val_str = eq + 1;

                char *endp = NULL;
                errno = 0;
                long v = strtol(val_str, &endp, 10);
                if (errno != 0 || !endp || *endp != '\0') {
                    bad = 1;
                    break;
                }

                if (strcmp(key, "thr") == 0) {
                    if (has_thr) { bad = 1; break; }
                    if (v < RESTART_MIN_THR_MB || (uint64_t)v > g_memp_memcap) { bad = 1; break; }
                    val_thr = (int32_t)v;
                    has_thr = 1;
                } else if (strcmp(key, "bg") == 0) {
                    if (has_bg) { bad = 1; break; }
                    if (v < RESTART_MIN_BG_SEC || v > RESTART_MAX_BG_SEC) { bad = 1; break; }
                    val_bg = (int32_t)v;
                    has_bg = 1;
                } else if (strcmp(key, "max") == 0) {
                    if (has_max) { bad = 1; break; }
                    if (v < RESTART_MIN_MAX || v > RESTART_MAX_MAX) { bad = 1; break; }
                    val_max = (int32_t)v;
                    has_max = 1;
                } else if (strcmp(key, "relaunch") == 0) {
                    if (has_relaunch) { bad = 1; break; }
                    if (v != 0 && v != 1) { bad = 1; break; }
                    val_relaunch = (int32_t)v;
                    has_relaunch = 1;
                } else {
                    bad = 1;
                    break;
                }
            }

            if (bad || !has_thr || !has_bg || !has_max || !has_relaunch) {
                bad_line_count++;
                JPLOG(JP_ERR, "%-6s cfg_bad line=%d text=%s", "RST", line_no, raw_text);
                continue;
            }

            if (nparsed >= RESTART_MAX_RULES) {
                bad_line_count++;
                JPLOG(JP_ERR, "%-6s cfg_bad line=%d text=%s", "RST", line_no, raw_text);
                continue;
            }

            restart_rule_t *r = &parsed_rules[nparsed++];
            strncpy(r->bid, bid, sizeof(r->bid) - 1);
            r->bid[sizeof(r->bid) - 1] = '\0';
            r->thr_mb = val_thr;
            r->bg_sec = val_bg;
            r->max_count = val_max;
            r->relaunch = val_relaunch;
        }
        fclose(f);
    }

    g_restart_cfg_bad_count = bad_line_count;

    int changed = 0;
    if (g_restart_cfg_first_load) {
        changed = 1;
        g_restart_cfg_first_load = 0;
    } else if (file_absent != g_restart_cfg_file_absent) {
        changed = 1;
    } else if (nparsed != g_nrestart_rules) {
        changed = 1;
    } else {
        for (int i = 0; i < nparsed; i++) {
            if (!restart_rule_cfg_equal(&parsed_rules[i], &g_restart_rules[i])) {
                changed = 1;
                break;
            }
        }
    }

    if (changed) {
        JPLOG(JP_INFO, "%-6s cfg rules=%d file=%s",
              "RST", nparsed, file_absent ? "absent" : "ok");
        for (int i = 0; i < nparsed; i++) {
            JPLOG(JP_DEBUG, "%-6s cfg_rule i=%d bid=%s thr=%d bg=%d max=%d relaunch=%d",
                  "RST", i, parsed_rules[i].bid, (int)parsed_rules[i].thr_mb,
                  (int)parsed_rules[i].bg_sec, (int)parsed_rules[i].max_count,
                  (int)parsed_rules[i].relaunch);
        }
        memcpy(g_restart_rules, parsed_rules, sizeof(restart_rule_t) * nparsed);
        g_nrestart_rules = nparsed;
        g_restart_cfg_file_absent = file_absent;

        /* 压实 g_rst_rule_states 槽位，删除规则后槽位复用且计数递减 (C1-M5 / N4) */
        int new_nrst_rule_states = 0;
        for (int i = 0; i < g_nrst_rule_states; i++) {
            int found = 0;
            for (int j = 0; j < g_nrestart_rules; j++) {
                if (strcmp(g_rst_rule_states[i].bid, g_restart_rules[j].bid) == 0) {
                    found = 1;
                    break;
                }
            }
            if (found) {
                if (new_nrst_rule_states != i) {
                    g_rst_rule_states[new_nrst_rule_states] = g_rst_rule_states[i];
                }
                new_nrst_rule_states++;
            }
        }
        for (int i = new_nrst_rule_states; i < g_nrst_rule_states; i++) {
            memset(&g_rst_rule_states[i], 0, sizeof(rst_rule_state_t));
        }
        g_nrst_rule_states = new_nrst_rule_states;
    }

    return file_absent ? -1 : 0;
}

/* 前置声明：供 pulse 模块复用 restart 模块的字符串截断拷贝函数 */
static inline void rst_str_copy(char *dst, size_t sz, const char *src);

/* =========================================================================
 * 后台内存释放：配置加载与适用范围校验 (v0.7.8 C2-0)
 * ========================================================================= */
static int pulse_rule_cfg_equal(const pulse_rule_t *a, const pulse_rule_t *b) {
    if (strcmp(a->bid, b->bid) != 0) return 0;
    if (a->enabled != b->enabled) return 0;
    if (a->target_mb != b->target_mb) return 0;
    if (a->n_min != b->n_min) return 0;
    if (a->daymax != b->daymax) return 0;
    if (a->delay_min != b->delay_min) return 0;
    return 1;
}

static int pulse_is_supported_app(pid_t pid, const char *path, const char *bid) {
    if (pid <= 0 || !path || !bid || bid[0] == '\0') return 0;
    /* 1. 可执行文件必须位于 /var/containers/Bundle/Application/ 或 /private/var/containers/Bundle/Application/ 下 */
    static const char kPrefix1[] = "/private/var/containers/Bundle/Application/";
    static const char kPrefix2[] = "/var/containers/Bundle/Application/";
    if (strncmp(path, kPrefix1, sizeof(kPrefix1) - 1) != 0 &&
        strncmp(path, kPrefix2, sizeof(kPrefix2) - 1) != 0) {
        return 0;
    }
    /* 2. 不支持扩展 (路径含 /PlugIns/) */
    if (strstr(path, "/PlugIns/") != NULL) {
        return 0;
    }
    /* 3. 不支持 Safari WebContent */
    if (strstr(path, "WebContent") != NULL) {
        return 0;
    }
    /* 4. 必须为主 App 二进制 (.app/<exe>) */
    if (!memp_is_main_app_binary(path)) {
        return 0;
    }
    /* 5. 不允许黑名单系统/守护进程 */
    if (restart_is_blocked_bid(bid)) {
        return 0;
    }
    return 1;
}

static pulse_rule_t *pulse_find_rule(const char *bid) {
    if (!bid || bid[0] == '\0') return NULL;
    for (int i = 0; i < g_npulse_rules; i++) {
        if (strcmp(g_pulse_rules[i].bid, bid) == 0) {
            return &g_pulse_rules[i];
        }
    }
    return NULL;
}

static int pulse_load_config(void) {
    int file_absent = 0;
    FILE *f = NULL;
    int rc = jp_config_open(PULSE_CONFIG_PATH, &s_pulse_cfg_stamp, "PLS", &file_absent, &f);

    if (rc == 0) {
        return g_pulse_cfg_file_absent ? -1 : 0;
    }
    if (rc < 0) {
        return g_pulse_cfg_file_absent ? -1 : 0;
    }

    pulse_rule_t parsed_rules[PULSE_MAX_RULES];
    int nparsed = 0;
    int bad_line_count = 0;

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
                bad_line_count++;
                JPLOG(JP_ERR, "PULSE cfg_bad line=%d text=%s", line_no, raw_text);
                continue;
            }

            char line_copy[512];
            strncpy(line_copy, raw_text, sizeof(line_copy) - 1);
            line_copy[sizeof(line_copy) - 1] = '\0';

            char *tokens[16];
            int ntokens = 0;
            char *saveptr = NULL;
            char *tok = strtok_r(line_copy, " \t", &saveptr);
            while (tok) {
                if (ntokens < 15) {
                    tokens[ntokens] = tok;
                }
                ntokens++;
                tok = strtok_r(NULL, " \t", &saveptr);
            }

            /* 格式: bid:<bid> on|off target=<MB> n=<分钟> daymax=<次> delay=<分钟> (6 个 token) */
            if (ntokens != 6) {
                bad_line_count++;
                JPLOG(JP_ERR, "PULSE cfg_bad line=%d text=%s", line_no, raw_text);
                continue;
            }

            if (strncmp(tokens[0], "bid:", 4) != 0) {
                bad_line_count++;
                JPLOG(JP_ERR, "PULSE cfg_bad line=%d text=%s", line_no, raw_text);
                continue;
            }

            const char *bid = tokens[0] + 4;
            if (bid[0] == '\0' || strlen(bid) >= sizeof(parsed_rules[0].bid) || restart_is_blocked_bid(bid)) {
                bad_line_count++;
                JPLOG(JP_ERR, "PULSE cfg_bad line=%d text=%s", line_no, raw_text);
                continue;
            }

            int enabled = -1;
            if (strcmp(tokens[1], "on") == 0) {
                enabled = 1;
            } else if (strcmp(tokens[1], "off") == 0) {
                enabled = 0;
            } else {
                bad_line_count++;
                JPLOG(JP_ERR, "PULSE cfg_bad line=%d text=%s", line_no, raw_text);
                continue;
            }

            int duplicate = 0;
            for (int i = 0; i < nparsed; i++) {
                if (strcmp(parsed_rules[i].bid, bid) == 0) {
                    duplicate = 1;
                    break;
                }
            }
            if (duplicate) {
                bad_line_count++;
                JPLOG(JP_ERR, "PULSE cfg_bad line=%d text=%s", line_no, raw_text);
                continue;
            }

            int has_target = 0, has_n = 0, has_daymax = 0, has_delay = 0;
            int32_t val_target = 0, val_n = 0, val_daymax = 0, val_delay = 0;
            int bad = 0;

            for (int k = 2; k < 6; k++) {
                char *eq = strchr(tokens[k], '=');
                if (!eq || eq == tokens[k] || *(eq + 1) == '\0') {
                    bad = 1;
                    break;
                }
                *eq = '\0';
                char *key = tokens[k];
                char *val_str = eq + 1;

                char *endp = NULL;
                errno = 0;
                long v = strtol(val_str, &endp, 10);
                if (errno != 0 || !endp || *endp != '\0') {
                    bad = 1;
                    break;
                }

                if (strcmp(key, "target") == 0) {
                    if (has_target) { bad = 1; break; }
                    if (v < PULSE_MIN_TARGET_MB || v > PULSE_MAX_TARGET_MB) { bad = 1; break; }
                    val_target = (int32_t)v;
                    has_target = 1;
                } else if (strcmp(key, "n") == 0) {
                    if (has_n) { bad = 1; break; }
                    if (v < PULSE_MIN_N_MIN || v > PULSE_MAX_N_MIN) { bad = 1; break; }
                    val_n = (int32_t)v;
                    has_n = 1;
                } else if (strcmp(key, "daymax") == 0) {
                    if (has_daymax) { bad = 1; break; }
                    if (v < PULSE_MIN_DAYMAX || v > PULSE_MAX_DAYMAX) { bad = 1; break; }
                    val_daymax = (int32_t)v;
                    has_daymax = 1;
                } else if (strcmp(key, "delay") == 0) {
                    if (has_delay) { bad = 1; break; }
                    if (v < PULSE_MIN_DELAY_MIN || v > PULSE_MAX_DELAY_MIN) { bad = 1; break; }
                    val_delay = (int32_t)v;
                    has_delay = 1;
                } else {
                    bad = 1;
                    break;
                }
            }

            if (bad || !has_target || !has_n || !has_daymax || !has_delay) {
                bad_line_count++;
                JPLOG(JP_ERR, "PULSE cfg_bad line=%d text=%s", line_no, raw_text);
                continue;
            }

            if (nparsed >= PULSE_MAX_RULES) {
                bad_line_count++;
                JPLOG(JP_ERR, "PULSE cfg_bad line=%d text=%s", line_no, raw_text);
                continue;
            }

            pulse_rule_t *pr = &parsed_rules[nparsed++];
            strncpy(pr->bid, bid, sizeof(pr->bid) - 1);
            pr->bid[sizeof(pr->bid) - 1] = '\0';
            pr->enabled = enabled;
            pr->target_mb = val_target;
            pr->n_min = val_n;
            pr->daymax = val_daymax;
            pr->delay_min = val_delay;
        }
        fclose(f);
    }

    g_pulse_cfg_bad_count = bad_line_count;

    int changed = 0;
    if (g_pulse_cfg_first_load) {
        g_pulse_cfg_first_load = 0;
        changed = 1;
    } else if (file_absent != g_pulse_cfg_file_absent) {
        changed = 1;
    } else if (nparsed != g_npulse_rules) {
        changed = 1;
    } else {
        for (int i = 0; i < nparsed; i++) {
            if (!pulse_rule_cfg_equal(&parsed_rules[i], &g_pulse_rules[i])) {
                changed = 1;
                break;
            }
        }
    }

    if (changed) {
        JPLOG(JP_INFO, "%-6s cfg rules=%d file=%s",
              "PLS", nparsed, file_absent ? "absent" : "ok");
        for (int i = 0; i < nparsed; i++) {
            JPLOG(JP_DEBUG, "%-6s cfg_rule i=%d bid=%s on=%d target=%d n=%d daymax=%d delay=%d",
                  "PLS", i, parsed_rules[i].bid, parsed_rules[i].enabled,
                  (int)parsed_rules[i].target_mb, (int)parsed_rules[i].n_min,
                  (int)parsed_rules[i].daymax, (int)parsed_rules[i].delay_min);
        }
        memcpy(g_pulse_rules, parsed_rules, sizeof(pulse_rule_t) * nparsed);
        g_npulse_rules = nparsed;
        g_pulse_cfg_file_absent = file_absent;
    }

    return file_absent ? -1 : 0;
}

/* =========================================================================
 * 后台内存释放：档位判定、状态跟踪与触发条件校验 (v0.7.8 C2-1)
 * ========================================================================= */
typedef struct {
    char    bid[128];
    pid_t   tracked_pid;
    int     in_bg;              /* 1: 当前处于 0..95 档 */
    time_t  bg_start_mono;      /* 进入 0..95 档的单调时间 (秒) */
    time_t  last_pulse_mono;    /* 上次完成 pulse 的单调时间 (秒) */
    time_t  last_ctrl_start_mono; /* 上次启动对照组采样的单调时间 (秒) */
    int     pulsed_this_bg;     /* 1: 本次进入后台期间已触发过 pulse (每次进入后台最多 1 次) */
    int     ctrl_this_bg;       /* 1: 本次进入后台期间已采过对照组 (每段后台最多 1 次，与 pulse 二选一) (M5 / Q2) */
    int     day_count;          /* 当天已触发 pulse 次数 */
    int     ctrl_day_count;     /* 当天已采对照组次数 (每天上限 10) (M5 / Q2) */
    char    day_date[16];       /* YYYY-MM-DD 本地日期 */
    int     tripped_today;      /* 1: 当天已熔断 */
    int     unresponsive;       /* 1: 标记为对释放无响应 */
    int     total_pulses;       /* 累计 pulse 次数 */
    int     no_drop_pulses;     /* 降幅 <= 5MB 的 pulse 次数 */
} pulse_app_state_t;

#define PULSE_CTRL_APP_DAYMAX    10
#define PULSE_CTRL_GLOBAL_DAYMAX 30
static int g_pulse_ctrl_global_today_count = 0;

static pthread_mutex_t s_pulse_mutex = PTHREAD_MUTEX_INITIALIZER;

static pulse_app_state_t g_pulse_app_states[PULSE_MAX_RULES];
static int g_npulse_app_states = 0;
static int g_pulse_global_today_count = 0;
static char g_pulse_global_today_date[16] = {0};
static int g_pulse_corrupt_state = 0;
static char g_pulse_corrupt_bid[128] = {0};

static void pulse_write_status(FILE *m) {
    if (!m) return;
    pthread_mutex_lock(&s_pulse_mutex);
    fprintf(m, "pulse_support=1\n");
    fprintf(m, "pulse_today=%d\n", g_pulse_global_today_count);
    fprintf(m, "pulse_cfg_bad=%d\n", g_pulse_cfg_bad_count);
    fprintf(m, "pulse_corrupt=%d\n", g_pulse_corrupt_state);
    if (g_pulse_corrupt_state && g_pulse_corrupt_bid[0]) {
        fprintf(m, "pulse_corrupt_bid=%s\n", g_pulse_corrupt_bid);
    }
    for (int i = 0; i < g_npulse_app_states; i++) {
        pulse_app_state_t *st = &g_pulse_app_states[i];
        fprintf(m, "pulse_app=%s|%d|%d|%d\n",
                st->bid, st->day_count, st->tripped_today, st->unresponsive);
    }
    pthread_mutex_unlock(&s_pulse_mutex);
}

static void pulse_get_local_date_str(char *buf, size_t sz) {
    time_t t = time(NULL);
    struct tm tm_val;
    localtime_r(&t, &tm_val);
    strftime(buf, sz, "%Y-%m-%d", &tm_val);
}

static void pulse_check_date_reset(const char *today_str) {
    pthread_mutex_lock(&s_pulse_mutex);
    if (g_pulse_global_today_date[0] == '\0' || strcmp(g_pulse_global_today_date, today_str) != 0) {
        snprintf(g_pulse_global_today_date, sizeof(g_pulse_global_today_date), "%s", today_str);
        g_pulse_global_today_count = 0;
        g_pulse_ctrl_global_today_count = 0;
        g_pulse_corrupt_state = 0;
        g_pulse_corrupt_bid[0] = '\0';
        for (int i = 0; i < g_npulse_app_states; i++) {
            g_pulse_app_states[i].day_count = 0;
            g_pulse_app_states[i].ctrl_day_count = 0;
            g_pulse_app_states[i].tripped_today = 0;
            snprintf(g_pulse_app_states[i].day_date, sizeof(g_pulse_app_states[i].day_date), "%s", today_str);
        }
    }
    pthread_mutex_unlock(&s_pulse_mutex);
}

static pulse_app_state_t *pulse_get_app_state(const char *bid, const char *today_str) {
    if (!bid || bid[0] == '\0') return NULL;
    for (int i = 0; i < g_npulse_app_states; i++) {
        if (strcmp(g_pulse_app_states[i].bid, bid) == 0) {
            if (strcmp(g_pulse_app_states[i].day_date, today_str) != 0) {
                g_pulse_app_states[i].day_count = 0;
                g_pulse_app_states[i].ctrl_day_count = 0;
                g_pulse_app_states[i].tripped_today = 0;
                snprintf(g_pulse_app_states[i].day_date, sizeof(g_pulse_app_states[i].day_date), "%s", today_str);
            }
            return &g_pulse_app_states[i];
        }
    }
    if (g_npulse_app_states < PULSE_MAX_RULES) {
        pulse_app_state_t *st = &g_pulse_app_states[g_npulse_app_states++];
        memset(st, 0, sizeof(*st));
        rst_str_copy(st->bid, sizeof(st->bid), bid);
        snprintf(st->day_date, sizeof(st->day_date), "%s", today_str);
        return st;
    }
    return NULL;
}

static int pulse_get_footprint_mb(pid_t pid, uint64_t *fp_mb_out) {
    uint64_t fp_bytes = 0;
    int rc = memp_get_phys_footprint(pid, &fp_bytes);
    if (rc == 0) {
        if (fp_mb_out) *fp_mb_out = fp_bytes / (1024 * 1024);
        return 0;
    }
    return rc;
}

/*
 * 评估 App 是否满足 pulse 的 7 项触发条件 (C2-1)。
 * 返回 1 表示满足触发条件；返回 0 表示不满足。
 * 输出参数:
 *   out_fp: 读取到的当前物理内存占用 (MB)
 *   out_skip_ctrl: 1 表示满足前置条件但因频控 (n / daymax / 全局上限) 跳过，可供对照组采样
 */
static int pulse_check_candidate(const pulse_rule_t *rule, pulse_app_state_t *st,
                                 pid_t pid, int32_t cur, const char *path,
                                 time_t now_mono, uint64_t *out_fp, int *out_skip_ctrl) {
    if (out_skip_ctrl) *out_skip_ctrl = 0;
    if (!rule || !st || pid <= 0) return 0;

    /* 条件 1: pulse.txt 里该 App 为 on，且为受支持的主进程 */
    if (!rule->enabled) return 0;
    if (!pulse_is_supported_app(pid, path, rule->bid)) return 0;

    /* 当前档位检查：必须在 0..95 档；其他值一律不处理 (M3 / C2-1) */
    if (cur < 0 || cur > 95) return 0;

    /* 条件 2: 同一个 pid 连续在 0..95 档满 delay 分钟 */
    if (!st->in_bg || st->tracked_pid != pid || st->bg_start_mono <= 0) return 0;
    time_t elapsed_sec = now_mono - st->bg_start_mono;
    time_t delay_sec = (time_t)rule->delay_min * 60;
    if (elapsed_sec < delay_sec) return 0;

    /* 每次进入后台最多做 1 次 pulse (与对照组二选一) (M5 / Q2) */
    if (st->pulsed_this_bg || st->ctrl_this_bg) return 0;

    /* 条件 3: 当前 fp > target */
    uint64_t fp_mb = 0;
    if (pulse_get_footprint_mb(pid, &fp_mb) != 0) return 0;
    if (out_fp) *out_fp = fp_mb;
    if (fp_mb <= (uint64_t)rule->target_mb) return 0;

    /* 条件 6: 有重启规则时，fp 不能超过重启阈值 (超过则交由 0.7.7 重启规则处理) */
    for (int r = 0; r < g_nrestart_rules; r++) {
        if (strcmp(g_restart_rules[r].bid, rule->bid) == 0) {
            if (fp_mb > (uint64_t)g_restart_rules[r].thr_mb) {
                return 0; /* 超过重启阈值，跳过 pulse */
            }
            break;
        }
    }

    /* 条件 7: 该 App 今天没有被熔断 */
    if (st->tripped_today) return 0;

    /* 检查是否命中频控 (条件 4 和 5)：命中频控者可作为对照组采样来源 (M5 / Q2) */
    /* 条件 4: 距该 App 上一次 pulse 至少 n 分钟 (按 App 计，换 pid 也不清零；Hark-A 顺带裁定) */
    int cooldown_hit = 0;
    if (st->last_pulse_mono > 0 && (now_mono - st->last_pulse_mono) < (time_t)rule->n_min * 60) {
        cooldown_hit = 1;
    }
    int daymax_hit = (st->day_count >= rule->daymax);
    int global_hit = (g_pulse_global_today_count >= PULSE_GLOBAL_DAYMAX);

    if (cooldown_hit || daymax_hit) {
        /* 满 delay 那一刻，因 n 或 daymax 跳过才记对照组；全局上限不记；每段后台最多 1 个 (Q2)
         * 仅标记候选，不在此处置 ctrl_this_bg；只有真正启动采样时才标记 (C2R2-N1) */
        if (!global_hit && !st->pulsed_this_bg && !st->ctrl_this_bg &&
            st->ctrl_day_count < PULSE_CTRL_APP_DAYMAX &&
            g_pulse_ctrl_global_today_count < PULSE_CTRL_GLOBAL_DAYMAX) {
            if (out_skip_ctrl) *out_skip_ctrl = 1;
        }
        return 0;
    }
    if (global_hit) {
        return 0;
    }

    return 1;
}

/* 出队与线程启动时的前置条件复核 (M2)
 * 检查：规则是否还是 on、fp 是否还大于 target、fp 是否超过重启阈值、
 * 是否已熔断、是否超过 daymax 或全局上限。任一不满足返回 0 */
static int pulse_revalidate_conditions(pid_t pid, const char *bid, uint64_t *out_fp) {
    if (pid <= 0 || !bid || bid[0] == '\0') return 0;

    const pulse_rule_t *rule = NULL;
    for (int i = 0; i < g_npulse_rules; i++) {
        if (strcmp(g_pulse_rules[i].bid, bid) == 0) {
            rule = &g_pulse_rules[i];
            break;
        }
    }
    if (!rule || !rule->enabled) return 0;

    char today_str[16] = {0};
    pulse_get_local_date_str(today_str, sizeof(today_str));

    pthread_mutex_lock(&s_pulse_mutex);
    pulse_app_state_t *st = pulse_get_app_state(bid, today_str);
    int invalid = (!st || st->tripped_today || st->day_count >= rule->daymax ||
                   g_pulse_global_today_count >= PULSE_GLOBAL_DAYMAX);
    pthread_mutex_unlock(&s_pulse_mutex);
    if (invalid) return 0;

    uint64_t cur_fp = 0;
    if (pulse_get_footprint_mb(pid, &cur_fp) != 0) return 0;
    if (cur_fp <= (uint64_t)rule->target_mb) return 0;

    for (int r = 0; r < g_nrestart_rules; r++) {
        if (strcmp(g_restart_rules[r].bid, bid) == 0) {
            if (cur_fp > (uint64_t)g_restart_rules[r].thr_mb) {
                return 0;
            }
            break;
        }
    }

    if (out_fp) *out_fp = cur_fp;
    return 1;
}

/* =========================================================================
 * 后台内存释放：排队、并发控制与异步执行线程 (v0.7.8 C2-2)
 * ========================================================================= */
typedef struct {
    char     bid[128];
    pid_t    pid;
    uint64_t fp_mb;
    time_t   enqueued_mono;
} pulse_queue_entry_t;

#define PULSE_MAX_QUEUE 32
static pulse_queue_entry_t g_pulse_queue[PULSE_MAX_QUEUE];
static int g_npulse_queue = 0;

static volatile int   g_pulse_in_progress = 0;
static volatile pid_t g_pulse_current_pid = 0;
static char           g_pulse_current_bid[128] = {0};
static pthread_t      g_pulse_thread;
static int            g_pulse_thread_running = 0;
static int            g_pulse_thread_has_handle = 0;

static void pulse_queue_remove_at(int idx) {
    if (idx < 0 || idx >= g_npulse_queue) return;
    for (int i = idx; i < g_npulse_queue - 1; i++) {
        g_pulse_queue[i] = g_pulse_queue[i + 1];
    }
    g_npulse_queue--;
}

static void pulse_sort_queue(void) {
    for (int i = 0; i < g_npulse_queue - 1; i++) {
        for (int j = i + 1; j < g_npulse_queue; j++) {
            if (g_pulse_queue[j].fp_mb > g_pulse_queue[i].fp_mb) {
                pulse_queue_entry_t tmp = g_pulse_queue[i];
                g_pulse_queue[i] = g_pulse_queue[j];
                g_pulse_queue[j] = tmp;
            }
        }
    }
}

/* =========================================================================
 * 后台内存释放：Pulse 写入、回读校验与统一 Restore 实现 (v0.7.8 C2-3)
 * ========================================================================= */
static inline uint64_t calc_hard_guard_limit(uint64_t fp_mb) {
    return (fp_mb * 11 + 9) / 10;   /* ceil(fp * 1.1) */
}

static inline uint64_t calc_pulse_limit(uint64_t fp_mb) {
    return (fp_mb * 100 + 84) / 85; /* ceil(fp / 0.85) */
}

static inline uint64_t calc_restore_floor(uint64_t fp_mb) {
    return (fp_mb * 12 + 9) / 10;   /* ceil(fp * 1.2) */
}

static int pulse_get_proc_identity(pid_t pid, proc_identity_t *ident) {
    if (!ident || pid <= 0) return -1;
    memset(ident, 0, sizeof(*ident));
    ident->pid = pid;

    char path[PROC_PIDPATHINFO_MAXSIZE];
    memset(path, 0, sizeof(path));
    if (proc_pidpath(pid, path, sizeof(path)) <= 0) {
        return -1;
    }
    snprintf(ident->path, sizeof(ident->path), "%s", path);

    struct proc_bsdinfo bsdinfo;
    memset(&bsdinfo, 0, sizeof(bsdinfo));
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bsdinfo, sizeof(bsdinfo)) != sizeof(bsdinfo)) {
        return -1;
    }
    ident->start_sec = bsdinfo.pbi_start_tvsec;
    ident->start_usec = bsdinfo.pbi_start_tvusec;

    struct proc_uniqidentifierinfo uniqinfo;
    memset(&uniqinfo, 0, sizeof(uniqinfo));
    if (proc_pidinfo(pid, PROC_PIDUNIQIDENTIFIERINFO, 0, &uniqinfo, sizeof(uniqinfo)) == sizeof(uniqinfo)) {
        ident->uniqueid = uniqinfo.p_uniqueid;
    }
    return 0;
}

static int pulse_compare_proc_identity(const proc_identity_t *a, const proc_identity_t *b) {
    if (!a || !b) return 0;
    if (a->pid != b->pid) return 0;
    if (a->uniqueid != 0 && b->uniqueid != 0 && a->uniqueid != b->uniqueid) return 0;
    if (a->start_sec != 0 && b->start_sec != 0 &&
        (a->start_sec != b->start_sec || a->start_usec != b->start_usec)) return 0;
    if (strcmp(a->path, b->path) != 0) return 0;
    return 1;
}

static int pulse_get_memlimit_props(pid_t pid, memorystatus_memlimit_properties_t *props_out) {
    if (!props_out) return EINVAL;
    memset(props_out, 0, sizeof(*props_out));
    int rc = memorystatus_control(MEMORYSTATUS_CMD_GET_MEMLIMIT_PROPERTIES, pid, 0,
                                  props_out, sizeof(*props_out));
    if (rc != 0) {
        return errno != 0 ? errno : -1;
    }
    return 0;
}

static int pulse_set_memlimit_props(pid_t pid, const memorystatus_memlimit_properties_t *props) {
    if (!props) return EINVAL;
    int rc = memorystatus_control(MEMORYSTATUS_CMD_SET_MEMLIMIT_PROPERTIES, pid, 0,
                                  (void *)props, sizeof(*props));
    if (rc != 0) {
        return errno != 0 ? errno : -1;
    }
    return 0;
}

static int pulse_save_state_file(const pulse_saved_state_t *st) {
    if (!st || st->pid <= 0) return -1;

    struct stat dir_st;
    if (lstat(CONFIG_DIR, &dir_st) != 0 || S_ISLNK(dir_st.st_mode) || !S_ISDIR(dir_st.st_mode)) {
        return -1;
    }

    int fd = open(PULSE_STATE_TMP_PATH, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    fchmod(fd, 0600);

    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(PULSE_STATE_TMP_PATH);
        return -1;
    }
    fprintf(f, "pid=%d\n", st->pid);
    if (st->bid[0] != '\0') {
        fprintf(f, "bid=%s\n", st->bid);
    }
    fprintf(f, "start_sec=%llu\n", (unsigned long long)st->start_sec);
    fprintf(f, "start_usec=%llu\n", (unsigned long long)st->start_usec);
    fprintf(f, "uniqueid=%llu\n", (unsigned long long)st->uniqueid);
    fprintf(f, "path=%s\n", st->path);
    fprintf(f, "orig_active=%d\n", st->orig_active);
    fprintf(f, "orig_active_attr=%u\n", st->orig_active_attr);
    fprintf(f, "orig_inactive=%d\n", st->orig_inactive);
    fprintf(f, "orig_inactive_attr=%u\n", st->orig_inactive_attr);
    fprintf(f, "suspect=%d\n", st->suspect);
    fclose(f);

    if (rename(PULSE_STATE_TMP_PATH, PULSE_STATE_PATH) != 0) {
        unlink(PULSE_STATE_TMP_PATH);
        return -1;
    }
    return 0;
}

static int pulse_load_trusted_state_file(pid_t pid, const proc_identity_t *ident, pulse_saved_state_t *st_out) {
    if (!st_out || (pid <= 0 && ident != NULL)) return -1;
    memset(st_out, 0, sizeof(*st_out));

    struct stat dir_st;
    if (lstat(CONFIG_DIR, &dir_st) != 0 || S_ISLNK(dir_st.st_mode) || !S_ISDIR(dir_st.st_mode)) {
        return -1;
    }

    struct stat file_st;
    if (lstat(PULSE_STATE_PATH, &file_st) != 0) return -1;
    if (S_ISLNK(file_st.st_mode) || !S_ISREG(file_st.st_mode)) return -1;
    if (file_st.st_uid != geteuid()) return -1;
    if ((file_st.st_mode & 0777) != 0600) return -1;

    int fd = open(PULSE_STATE_PATH, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -1;

    FILE *f = fdopen(fd, "r");
    if (!f) {
        close(fd);
        return -1;
    }

    char line[512];
    int has_pid = 0, has_sec = 0, has_usec = 0;
    int has_act = 0, has_act_attr = 0;
    int has_inact = 0, has_inact_attr = 0;
    int has_suspect = 0;

    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        char *cr = strchr(line, '\r'); if (cr) *cr = '\0';

        if (strncmp(line, "pid=", 4) == 0) {
            st_out->pid = atoi(line + 4);
            has_pid = 1;
        } else if (strncmp(line, "bid=", 4) == 0) {
            snprintf(st_out->bid, sizeof(st_out->bid), "%s", line + 4);
        } else if (strncmp(line, "start_sec=", 10) == 0) {
            st_out->start_sec = strtoull(line + 10, NULL, 10);
            has_sec = 1;
        } else if (strncmp(line, "start_usec=", 11) == 0) {
            st_out->start_usec = strtoull(line + 11, NULL, 10);
            has_usec = 1;
        } else if (strncmp(line, "uniqueid=", 9) == 0) {
            st_out->uniqueid = strtoull(line + 9, NULL, 10);
        } else if (strncmp(line, "path=", 5) == 0) {
            snprintf(st_out->path, sizeof(st_out->path), "%s", line + 5);
        } else if (strncmp(line, "orig_active=", 12) == 0) {
            st_out->orig_active = (int32_t)atoi(line + 12);
            has_act = 1;
        } else if (strncmp(line, "orig_active_attr=", 17) == 0) {
            st_out->orig_active_attr = (uint32_t)strtoul(line + 17, NULL, 0);
            has_act_attr = 1;
        } else if (strncmp(line, "orig_inactive=", 14) == 0) {
            st_out->orig_inactive = (int32_t)atoi(line + 14);
            has_inact = 1;
        } else if (strncmp(line, "orig_inactive_attr=", 19) == 0) {
            st_out->orig_inactive_attr = (uint32_t)strtoul(line + 19, NULL, 0);
            has_inact_attr = 1;
        } else if (strncmp(line, "suspect=", 8) == 0) {
            st_out->suspect = atoi(line + 8);
            has_suspect = 1;
        }
    }
    fclose(f);

    if (!has_pid || !has_sec || !has_usec || !has_act || !has_act_attr || !has_inact || !has_inact_attr) {
        return -1;
    }

    /* R4-a: suspect 字段缺失时 fail closed，按可疑处理 (suspect=1) */
    if (!has_suspect) {
        st_out->suspect = 1;
    }

    if (ident != NULL) {
        if (st_out->pid != ident->pid ||
            st_out->start_sec != ident->start_sec ||
            st_out->start_usec != ident->start_usec) {
            return -1;
        }
    } else if (pid > 0 && st_out->pid != pid) {
        return -1;
    }

    return 0;
}

static void pulse_clear_state_file(pid_t pid) {
    (void)pid;
    struct stat dir_st;
    if (lstat(CONFIG_DIR, &dir_st) != 0 || S_ISLNK(dir_st.st_mode) || !S_ISDIR(dir_st.st_mode)) {
        return;
    }
    struct stat file_st;
    if (lstat(PULSE_STATE_PATH, &file_st) == 0) {
        if (!S_ISLNK(file_st.st_mode) && S_ISREG(file_st.st_mode) && file_st.st_uid == geteuid()) {
            unlink(PULSE_STATE_PATH);
        }
    }
}

static int pulse_get_or_record_initial_state(pid_t pid, const proc_identity_t *ident,
                                             const char *bid,
                                             pulse_saved_state_t *st_out) {
    if (!ident || pid <= 0 || !st_out) return -1;

    pulse_saved_state_t existing;
    if (pulse_load_trusted_state_file(pid, ident, &existing) == 0) {
        if (existing.pid == ident->pid &&
            existing.start_sec == ident->start_sec &&
            existing.start_usec == ident->start_usec) {
            *st_out = existing;
            if (bid && bid[0] && st_out->bid[0] == '\0') {
                snprintf(st_out->bid, sizeof(st_out->bid), "%s", bid);
            }
            return 0;
        }
    }

    memorystatus_memlimit_properties_t props;
    int err = pulse_get_memlimit_props(pid, &props);
    if (err != 0) {
        return -1;
    }

    memset(st_out, 0, sizeof(*st_out));
    st_out->pid = ident->pid;
    if (bid && bid[0]) {
        snprintf(st_out->bid, sizeof(st_out->bid), "%s", bid);
    }
    st_out->start_sec = ident->start_sec;
    st_out->start_usec = ident->start_usec;
    st_out->uniqueid = ident->uniqueid;
    snprintf(st_out->path, sizeof(st_out->path), "%s", ident->path);
    st_out->orig_active = props.memlimit_active;
    st_out->orig_active_attr = props.memlimit_active_attr;
    st_out->orig_inactive = props.memlimit_inactive;
    st_out->orig_inactive_attr = props.memlimit_inactive_attr;

    int act_fatal = (props.memlimit_active_attr & MEMORYSTATUS_MEMLIMIT_ATTR_FATAL) ? 1 : 0;
    int inact_fatal = (props.memlimit_inactive_attr & MEMORYSTATUS_MEMLIMIT_ATTR_FATAL) ? 1 : 0;
    if (props.memlimit_inactive != props.memlimit_active || inact_fatal != act_fatal) {
        st_out->suspect = 1;
        JPLOG(JP_INFO, "ORIG_SUSPECT inactive=%d/%u active=%d/%u",
              props.memlimit_inactive, props.memlimit_inactive_attr,
              props.memlimit_active, props.memlimit_active_attr);
    } else {
        st_out->suspect = 0;
    }

    if (pulse_save_state_file(st_out) != 0) {
        return -2;
    }
    return 0;
}

/* 统一 Restore 实现函数 (C2-3) */
static int pulse_do_restore(pid_t pid, const char *bid, char *verdict_out, size_t verdict_sz) {
    if (verdict_out && verdict_sz > 0) verdict_out[0] = '\0';

    char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
    if (proc_pidpath(pid, path, sizeof(path)) <= 0 || !pulse_is_supported_app(pid, path, bid)) {
        if (verdict_out) snprintf(verdict_out, verdict_sz, "RESTORE_REFUSED reason=validate");
        JPLOG(JP_INFO, "RESTORE_REFUSED reason=validate");
        return 1;
    }

    uint64_t fp_mb = 0;
    int fp_err = pulse_get_footprint_mb(pid, &fp_mb);
    if (fp_err != 0) {
        if (verdict_out) snprintf(verdict_out, verdict_sz, "RESTORE_REFUSED reason=fp_unreadable");
        JPLOG(JP_INFO, "RESTORE_REFUSED reason=fp_unreadable");
        return 1;
    }

    proc_identity_t ident;
    if (pulse_get_proc_identity(pid, &ident) != 0) {
        if (verdict_out) snprintf(verdict_out, verdict_sz, "RESTORE_REFUSED reason=ident");
        JPLOG(JP_INFO, "RESTORE_REFUSED reason=ident");
        return 1;
    }

    memorystatus_memlimit_properties_t cur_props;
    int cur_err = pulse_get_memlimit_props(pid, &cur_props);
    if (cur_err != 0) {
        if (verdict_out) snprintf(verdict_out, verdict_sz, "RESTORE_REFUSED reason=get");
        JPLOG(JP_INFO, "RESTORE_REFUSED reason=get");
        return 1;
    }

    pulse_saved_state_t state;
    const char *mirror_reason = NULL;
    if (pulse_load_trusted_state_file(pid, &ident, &state) != 0) {
        mirror_reason = "untrusted";
    } else if (state.orig_inactive <= 0) {
        mirror_reason = "none";
    } else if (state.suspect != 0) {
        mirror_reason = "suspect";
    } else {
        uint64_t floor_1_2 = calc_restore_floor(fp_mb);
        int implausible = 0;
        if ((uint64_t)state.orig_inactive < floor_1_2) {
            implausible = 1;
        }
        if (cur_props.memlimit_active > 0 &&
            (int64_t)state.orig_inactive * 2 < cur_props.memlimit_active) {
            implausible = 1;
        }
        if (implausible) {
            mirror_reason = "implausible";
        }
    }

    int32_t target_inactive = 0;
    uint32_t target_inactive_attr = 0;

    if (mirror_reason != NULL) {
        uint64_t floor_1_2 = calc_restore_floor(fp_mb);
        if (cur_props.memlimit_active <= 0 ||
            (uint64_t)cur_props.memlimit_active <= floor_1_2) {
            if (verdict_out) snprintf(verdict_out, verdict_sz, "RESTORE_REFUSED reason=active_low");
            JPLOG(JP_INFO, "RESTORE_REFUSED reason=active_low");
            return 1;
        }
        target_inactive = cur_props.memlimit_active;
        target_inactive_attr = cur_props.memlimit_active_attr;
    } else {
        target_inactive = state.orig_inactive;
        target_inactive_attr = state.orig_inactive_attr;
    }

    int success = 0;
    int write_attempted = 0;
    int readback_mismatch = 0;
    int ident_failed = 0;
    int get_failed = 0;

    for (int attempt = 0; attempt < 4; attempt++) {
        if (attempt > 0) {
            usleep(100 * 1000); /* 100 ms */
        }

        proc_identity_t check_ident;
        if (pulse_get_proc_identity(pid, &check_ident) != 0 || !pulse_compare_proc_identity(&ident, &check_ident)) {
            ident_failed = 1;
            break;
        }

        memorystatus_memlimit_properties_t cur_active;
        if (pulse_get_memlimit_props(pid, &cur_active) != 0) {
            get_failed = 1;
            continue;
        }

        memorystatus_memlimit_properties_t target_props;
        target_props.memlimit_active = cur_active.memlimit_active;
        target_props.memlimit_active_attr = cur_active.memlimit_active_attr;
        target_props.memlimit_inactive = target_inactive;
        target_props.memlimit_inactive_attr = target_inactive_attr;

        write_attempted = 1;
        if (pulse_set_memlimit_props(pid, &target_props) != 0) {
            continue;
        }

        memorystatus_memlimit_properties_t readback;
        memset(&readback, 0, sizeof(readback));
        if (pulse_get_memlimit_props(pid, &readback) != 0) {
            readback_mismatch = 1;
            continue;
        }

        /* M2-a: 每次回读都打一行原始 attr，十六进制 */
        JPLOG(JP_INFO, "READBACK active=%d/0x%x inactive=%d/0x%x",
              readback.memlimit_active, readback.memlimit_active_attr,
              readback.memlimit_inactive, readback.memlimit_inactive_attr);

        int rb_act_fatal = (readback.memlimit_active_attr & MEMORYSTATUS_MEMLIMIT_ATTR_FATAL) ? 1 : 0;
        int exp_act_fatal = (target_props.memlimit_active_attr & MEMORYSTATUS_MEMLIMIT_ATTR_FATAL) ? 1 : 0;
        int rb_inact_fatal = (readback.memlimit_inactive_attr & MEMORYSTATUS_MEMLIMIT_ATTR_FATAL) ? 1 : 0;
        int exp_inact_fatal = (target_props.memlimit_inactive_attr & MEMORYSTATUS_MEMLIMIT_ATTR_FATAL) ? 1 : 0;

        if (readback.memlimit_active == target_props.memlimit_active &&
            rb_act_fatal == exp_act_fatal &&
            readback.memlimit_inactive == target_props.memlimit_inactive &&
            rb_inact_fatal == exp_inact_fatal) {
            success = 1;
            break;
        } else {
            readback_mismatch = 1;
        }
    }

    if (!success) {
        if (!write_attempted) {
            const char *refused_reason = ident_failed ? "ident" : (get_failed ? "get" : "ident");
            if (verdict_out) snprintf(verdict_out, verdict_sz, "RESTORE_REFUSED reason=%s", refused_reason);
            JPLOG(JP_INFO, "RESTORE_REFUSED reason=%s", refused_reason);
            return 1;
        }
        if (readback_mismatch) {
            if (verdict_out) snprintf(verdict_out, verdict_sz, "RESTORE_FAILED reason=readback");
            JPLOG(JP_INFO, "RESTORE_FAILED reason=readback");
            return 1;
        } else {
            if (verdict_out) snprintf(verdict_out, verdict_sz, "RESTORE_FAILED reason=retry");
            JPLOG(JP_INFO, "RESTORE_FAILED reason=retry");
            return 1;
        }
    }

    pulse_clear_state_file(pid);

    if (mirror_reason != NULL) {
        if (verdict_out) snprintf(verdict_out, verdict_sz, "RESTORE_MIRROR reason=%s", mirror_reason);
        JPLOG(JP_INFO, "RESTORE_MIRROR reason=%s", mirror_reason);
        return 0;
    }

    if (verdict_out) snprintf(verdict_out, verdict_sz, "RESTORE_OK");
    JPLOG(JP_INFO, "RESTORE_OK");
    return 0;
}

/* 列出当天曾被 pulse 过的 App (S2) */
static void pulse_log_today_pulsed_bids(void) {
    char today_str[16] = {0};
    pulse_get_local_date_str(today_str, sizeof(today_str));

    char bids_summary[512] = {0};
    int found_count = 0;

    FILE *f = fopen(LOG_PATH, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, today_str, strlen(today_str)) == 0) {
                char *p = strstr(line, "PULSE app=");
                if (p) {
                    p += strlen("PULSE app=");
                    char bid_buf[128] = {0};
                    int bi = 0;
                    while (*p && *p != ' ' && *p != '\n' && *p != '\r' && bi < (int)sizeof(bid_buf) - 1) {
                        bid_buf[bi++] = *p++;
                    }
                    bid_buf[bi] = '\0';
                    if (bid_buf[0] && !strstr(bids_summary, bid_buf)) {
                        if (found_count > 0 && strlen(bids_summary) + strlen(bid_buf) + 2 < sizeof(bids_summary)) {
                            strcat(bids_summary, ", ");
                        }
                        if (strlen(bids_summary) + strlen(bid_buf) < sizeof(bids_summary)) {
                            strcat(bids_summary, bid_buf);
                            found_count++;
                        }
                    }
                }
            }
        }
        fclose(f);
    }

    if (found_count > 0) {
        JPLOG(JP_WARN, "%-6s corrupt_state unknown bid, today_pulsed_apps=[%s]", "PLS", bids_summary);
    } else {
        JPLOG(JP_WARN, "%-6s corrupt_state unknown bid, today_pulsed_apps=none", "PLS");
    }
}

/* 崩溃恢复状态与启动校验 (v0.7.8 C2-5 / Q5) */
static void pulse_startup_recovery(void) {
    struct stat dir_st;
    if (lstat(CONFIG_DIR, &dir_st) != 0 || S_ISLNK(dir_st.st_mode) || !S_ISDIR(dir_st.st_mode)) {
        JPLOG(JP_WARN, "%-6s state_invalid reason=dir_symlink path=%s", "PLS", PULSE_STATE_PATH);
        g_pulse_corrupt_state = 1;
        g_pulse_corrupt_bid[0] = '\0';
        pulse_log_today_pulsed_bids();
        return;
    }

    struct stat file_st;
    if (lstat(PULSE_STATE_PATH, &file_st) != 0) {
        return; /* 状态文件不存在，正常启动 */
    }

    /* 校验文件：不是符号链接，属主为 euid，权限 0600 */
    if (S_ISLNK(file_st.st_mode) || !S_ISREG(file_st.st_mode) ||
        file_st.st_uid != geteuid() || (file_st.st_mode & 0777) != 0600) {
        unlink(PULSE_STATE_PATH);
        JPLOG(JP_WARN, "%-6s state_invalid reason=file_attr path=%s", "PLS", PULSE_STATE_PATH);
        g_pulse_corrupt_state = 1;
        g_pulse_corrupt_bid[0] = '\0';
        pulse_log_today_pulsed_bids();
        return;
    }

    pulse_saved_state_t st;
    if (pulse_load_trusted_state_file(0, NULL, &st) != 0 || st.pid <= 0) {
        unlink(PULSE_STATE_PATH);
        JPLOG(JP_WARN, "%-6s state_invalid reason=parse_fail path=%s", "PLS", PULSE_STATE_PATH);
        g_pulse_corrupt_state = 1;
        if (st.bid[0] != '\0') {
            snprintf(g_pulse_corrupt_bid, sizeof(g_pulse_corrupt_bid), "%s", st.bid);
        } else {
            g_pulse_corrupt_bid[0] = '\0';
            pulse_log_today_pulsed_bids();
        }
        return;
    }

    /* pid 和启动时间校验 */
    proc_identity_t curr_ident;
    if (pulse_get_proc_identity(st.pid, &curr_ident) != 0) {
        /* 进程已经没了 */
        unlink(PULSE_STATE_PATH);
        JPLOG(JP_INFO, "%-6s state_cleanup pid=%d reason=process_gone", "PLS", st.pid);
        return;
    }

    if (curr_ident.start_sec != st.start_sec || curr_ident.start_usec != st.start_usec) {
        /* pid 对不上 (pid 已被其他进程复用) */
        unlink(PULSE_STATE_PATH);
        JPLOG(JP_INFO, "%-6s state_cleanup pid=%d reason=pid_reused", "PLS", st.pid);
        return;
    }

    /* 校验通过，并且 pid 和启动时间都对得上：先 restore，再进入正常流程 */
    char verdict_str[64] = {0};
    const char *bid = (st.bid[0] != '\0') ? st.bid : "com.tencent.xin";
    pulse_do_restore(st.pid, bid, verdict_str, sizeof(verdict_str));
    JPLOG(JP_INFO, "%-6s crash_recovered pid=%d bid=%s verdict=%s",
          "PLS", st.pid, bid, verdict_str[0] ? verdict_str : "RESTORE_OK");
    pulse_clear_state_file(st.pid);
}

static int pulse_get_cur_priority(pid_t pid, int32_t *prio_out) {
    int count = 0;
    memorystatus_priority_entry_t *list = get_list(&count);
    if (!list) return -1;
    int found = 0;
    for (int i = 0; i < count; i++) {
        if (list[i].pid == pid) {
            if (prio_out) *prio_out = list[i].priority;
            found = 1;
            break;
        }
    }
    free(list);
    return found ? 0 : ESRCH;
}

static void *pulse_worker_thread(void *arg) {
    pulse_queue_entry_t *target = (pulse_queue_entry_t *)arg;
    if (!target) return NULL;

    pid_t pid = target->pid;
    const char *bid = target->bid;

    /* 线程开始执行时重新检查条件 (M2) */
    uint64_t reval_fp = 0;
    if (!pulse_revalidate_conditions(pid, bid, &reval_fp)) {
        goto thread_done;
    }

    proc_identity_t ident;
    if (pulse_get_proc_identity(pid, &ident) != 0) {
        JPLOG(JP_INFO, "RESTORE_REFUSED reason=ident");
        goto thread_done;
    }

    pulse_saved_state_t state;
    int state_rc = pulse_get_or_record_initial_state(pid, &ident, bid, &state);
    if (state_rc != 0) {
        const char *ref_reason = (state_rc == -1) ? "get" : "state";
        JPLOG(JP_INFO, "RESTORE_REFUSED reason=%s", ref_reason);
        goto thread_done;
    }

    uint64_t fp0 = 0;
    if (pulse_get_footprint_mb(pid, &fp0) != 0) {
        JPLOG(JP_INFO, "RESTORE_REFUSED reason=fp_unreadable");
        pulse_clear_state_file(pid);
        goto thread_done;
    }

    uint64_t L = calc_pulse_limit(fp0);
    uint64_t hard_guard = calc_hard_guard_limit(fp0);
    if (L < hard_guard) L = hard_guard;

    memorystatus_memlimit_properties_t cur_props;
    if (pulse_get_memlimit_props(pid, &cur_props) != 0) {
        JPLOG(JP_INFO, "RESTORE_REFUSED reason=get");
        pulse_clear_state_file(pid);
        goto thread_done;
    }

    memorystatus_memlimit_properties_t new_props = cur_props;
    new_props.memlimit_inactive = (int32_t)L;
    new_props.memlimit_inactive_attr &= ~MEMORYSTATUS_MEMLIMIT_ATTR_FATAL;

    proc_identity_t check_ident;
    if (pulse_get_proc_identity(pid, &check_ident) != 0 || !pulse_compare_proc_identity(&ident, &check_ident)) {
        JPLOG(JP_INFO, "RESTORE_REFUSED reason=ident");
        pulse_clear_state_file(pid);
        goto thread_done;
    }

    if (pulse_set_memlimit_props(pid, &new_props) != 0) {
        JPLOG(JP_INFO, "RESTORE_REFUSED reason=set");
        pulse_clear_state_file(pid);
        goto thread_done;
    }

    memorystatus_memlimit_properties_t readback;
    if (pulse_get_memlimit_props(pid, &readback) != 0) {
        JPLOG(JP_WARN, "PULSE_SET_MISMATCH reason=readback_fail pid=%d", (int)pid);
        pulse_do_restore(pid, bid, NULL, 0);
        goto thread_done;
    }

    JPLOG(JP_INFO, "READBACK active=%d/0x%x inactive=%d/0x%x",
          readback.memlimit_active, readback.memlimit_active_attr,
          readback.memlimit_inactive, readback.memlimit_inactive_attr);

    int rb_act_fatal = (readback.memlimit_active_attr & MEMORYSTATUS_MEMLIMIT_ATTR_FATAL) ? 1 : 0;
    int exp_act_fatal = (new_props.memlimit_active_attr & MEMORYSTATUS_MEMLIMIT_ATTR_FATAL) ? 1 : 0;
    int rb_inactive_fatal = (readback.memlimit_inactive_attr & MEMORYSTATUS_MEMLIMIT_ATTR_FATAL) ? 1 : 0;
    if (readback.memlimit_active != new_props.memlimit_active ||
        rb_act_fatal != exp_act_fatal ||
        readback.memlimit_inactive != (int32_t)L ||
        rb_inactive_fatal != 0) {
        JPLOG(JP_WARN, "PULSE_SET_MISMATCH reason=mismatch pid=%d", (int)pid);
        pulse_do_restore(pid, bid, NULL, 0);
        goto thread_done;
    }

    JPLOG(JP_INFO, "PULSE_ARMED pid=%d fp=%llu L=%llu window=60s",
          pid, (unsigned long long)fp0, (unsigned long long)L);

    const char *end_reason = "WINDOW_END";
    int should_restore = 1;
    int t_reach = -1;
    uint64_t fp15 = 0, fp30 = 0, fp60 = 0;
    int has_fp15 = 0, has_fp30 = 0, has_fp60 = 0;

    for (int sec = 1; sec <= 60; sec++) {
        usleep(1000 * 1000);

        if (g_pulse_stop_requested) {
            end_reason = "WINDOW_END";
            should_restore = 1;
            break;
        }

        proc_identity_t loop_ident;
        if (pulse_get_proc_identity(pid, &loop_ident) != 0) {
            struct proc_bsdinfo bi;
            if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof(bi)) != sizeof(bi)) {
                JPLOG(JP_INFO, "PULSE_GONE L=%llu t=%d", (unsigned long long)L, sec);
                end_reason = "GONE";
                should_restore = 0;
                break;
            }
            /* 进程还活着但身份读取失败：按「不确定」处理，照常 restore，打 Warn PULSE_ID_UNKNOWN (A3 / Q1) */
            JPLOG(JP_WARN, "PULSE_ID_UNKNOWN pid=%d t=%d", (int)pid, sec);
            end_reason = "ABORT";
            should_restore = 1;
            break;
        } else if (!pulse_compare_proc_identity(&ident, &loop_ident)) {
            /* 只有确认身份读取成功、但与原进程不一致时，才判定为 PID_REUSED */
            memorystatus_memlimit_properties_t cur_p;
            int cur_inact = -1, cur_inact_attr = 0;
            if (pulse_get_memlimit_props(pid, &cur_p) == 0) {
                cur_inact = cur_p.memlimit_inactive;
                cur_inact_attr = cur_p.memlimit_inactive_attr;
            }
            JPLOG(JP_INFO, "PID_REUSED app=%s pid=%d inactive=%d/0x%x",
                  bid, (int)pid, cur_inact, cur_inact_attr);
            end_reason = "PID_REUSED";
            should_restore = 0;
            break;
        }

        uint64_t cur_fp = 0;
        int rerr = pulse_get_footprint_mb(pid, &cur_fp);
        if (rerr != 0) {
            struct proc_bsdinfo bi;
            if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof(bi)) != sizeof(bi)) {
                JPLOG(JP_INFO, "PULSE_GONE L=%llu t=%d", (unsigned long long)L, sec);
                end_reason = "GONE";
                should_restore = 0;
                break;
            }
            JPLOG(JP_WARN, "PULSE_FP_FAIL pid=%d t=%d", (int)pid, sec);
            end_reason = "ABORT";
            t_reach = sec;
            should_restore = 1;
            break;
        }

        if (sec == 15) { fp15 = cur_fp; has_fp15 = 1; }
        else if (sec == 30) { fp30 = cur_fp; has_fp30 = 1; }
        else if (sec == 60) { fp60 = cur_fp; has_fp60 = 1; }

        JPLOG(JP_DEBUG, "t=%ds fp=%llu L=%llu (%.1f%%)",
              sec, (unsigned long long)cur_fp, (unsigned long long)L,
              (double)cur_fp * 100.0 / (double)L);

        /* 5. 窗口内一旦 fp >= 0.95*L，马上 restore，记 ABORT near_limit */
        if (cur_fp * 100 >= 95 * L) {
            JPLOG(JP_INFO, "ABORT near_limit fp=%llu limit=%llu",
                  (unsigned long long)cur_fp, (unsigned long long)L);
            end_reason = "ABORT";
            t_reach = sec;
            should_restore = 1;
            break;
        }

        /* 4. 窗口到时间，或者 fp 降到 0.8*L 以下，就按原值 restore */
        if (cur_fp * 100 < 80 * L) {
            JPLOG(JP_INFO, "TARGET_REACHED fp=%llu < 0.8*L (%llu)",
                  (unsigned long long)cur_fp, (unsigned long long)((80 * L) / 100));
            end_reason = "TARGET_REACHED";
            t_reach = sec;
            should_restore = 1;
            break;
        }

        if (sec == 60) {
            end_reason = "WINDOW_END";
            should_restore = 1;
            break;
        }
    }

    char restore_verdict_str[64] = "NONE";
    if (should_restore) {
        memorystatus_memlimit_properties_t pre_restore;
        if (pulse_get_memlimit_props(pid, &pre_restore) == 0 &&
            pre_restore.memlimit_inactive != (int32_t)L) {
            JPLOG(JP_INFO, "OVERRIDDEN app=%s pid=%d inactive=%d/0x%x",
                  bid, (int)pid, pre_restore.memlimit_inactive, pre_restore.memlimit_inactive_attr);
            end_reason = "OVERRIDDEN";
            snprintf(restore_verdict_str, sizeof(restore_verdict_str), "OVERRIDDEN");
            pulse_clear_state_file(pid);
        } else {
            pulse_do_restore(pid, bid, restore_verdict_str, sizeof(restore_verdict_str));
        }
    } else {
        pulse_clear_state_file(pid);
        if (strcmp(end_reason, "PID_REUSED") == 0) {
            snprintf(restore_verdict_str, sizeof(restore_verdict_str), "PID_REUSED");
        } else if (strcmp(end_reason, "GONE") == 0) {
            snprintf(restore_verdict_str, sizeof(restore_verdict_str), "PULSE_GONE");
        }
    }

    int gone_post_restore = 0;
    int elapsed_post_restore = 0;

    /* 提前结束 (TARGET_REACHED 或 ABORT) 时，restore 完成后继续只读采样到 60 秒 (C2-4 / Q4) */
    if ((strcmp(end_reason, "TARGET_REACHED") == 0 || strcmp(end_reason, "ABORT") == 0) &&
        t_reach > 0 && t_reach < 60 && !g_pulse_stop_requested) {
        for (int sec = t_reach + 1; sec <= 60; sec++) {
            usleep(1000 * 1000);
            elapsed_post_restore++;
            if (g_pulse_stop_requested) break;

            proc_identity_t post_ident;
            if (pulse_get_proc_identity(pid, &post_ident) != 0 ||
                !pulse_compare_proc_identity(&ident, &post_ident)) {
                if (elapsed_post_restore <= 10) {
                    struct proc_bsdinfo bi;
                    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof(bi)) != sizeof(bi)) {
                        gone_post_restore = 1;
                    }
                }
                break;
            }

            int32_t cur_prio = -1;
            if (pulse_get_cur_priority(pid, &cur_prio) == 0 && cur_prio == 100) {
                break; /* 采样期间回到前台，停止采样 */
            }

            uint64_t post_fp = 0;
            if (pulse_get_footprint_mb(pid, &post_fp) != 0) {
                break;
            }

            if (sec == 15) { fp15 = post_fp; has_fp15 = 1; }
            else if (sec == 30) { fp30 = post_fp; has_fp30 = 1; }
            else if (sec == 60) { fp60 = post_fp; has_fp60 = 1; }
        }
    }

    /* restore 之后 10 秒内监控进程存活 (C2-6 熔断) */
    if (should_restore && strcmp(end_reason, "GONE") != 0 && !gone_post_restore && !g_pulse_stop_requested) {
        for (int psec = elapsed_post_restore; psec < 10; psec++) {
            if (g_pulse_stop_requested) break;
            usleep(1000 * 1000);
            proc_identity_t post_chk;
            if (pulse_get_proc_identity(pid, &post_chk) != 0 ||
                !pulse_compare_proc_identity(&ident, &post_chk)) {
                struct proc_bsdinfo bi;
                if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof(bi)) != sizeof(bi)) {
                    gone_post_restore = 1;
                    break;
                }
            }
        }
    }

    /* 汇总行输出 (C2-4) */
    char t_reach_str[32], fp15_str[32], fp30_str[32], fp60_str[32];
    if (t_reach > 0) snprintf(t_reach_str, sizeof(t_reach_str), "%d", t_reach);
    else snprintf(t_reach_str, sizeof(t_reach_str), "NA");

    if (has_fp15) snprintf(fp15_str, sizeof(fp15_str), "%llu", (unsigned long long)fp15);
    else snprintf(fp15_str, sizeof(fp15_str), "NA");

    if (has_fp30) snprintf(fp30_str, sizeof(fp30_str), "%llu", (unsigned long long)fp30);
    else snprintf(fp30_str, sizeof(fp30_str), "NA");

    if (has_fp60) snprintf(fp60_str, sizeof(fp60_str), "%llu", (unsigned long long)fp60);
    else snprintf(fp60_str, sizeof(fp60_str), "NA");

    JPLOG(JP_INFO, "PULSE app=%s pid=%d fp0=%llu L=%llu t_reach=%s fp15=%s fp30=%s fp60=%s end=%s restore=%s",
          bid, (int)pid, (unsigned long long)fp0, (unsigned long long)L,
          t_reach_str, fp15_str, fp30_str, fp60_str, end_reason, restore_verdict_str);

    /* 计次与状态更新 (M6: 持锁保护) */
    char today_str[16] = {0};
    pulse_get_local_date_str(today_str, sizeof(today_str));
    pthread_mutex_lock(&s_pulse_mutex);
    pulse_app_state_t *st = pulse_get_app_state(bid, today_str);
    if (st) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        st->last_pulse_mono = ts.tv_sec;
        st->day_count++;
        st->total_pulses++;

        /* 熔断判定 (C2-6): 窗口内或 restore 后 10 秒内 GONE；PID_REUSED 不计入熔断 (S7) */
        if ((strcmp(end_reason, "GONE") == 0 || gone_post_restore) && strcmp(end_reason, "PID_REUSED") != 0) {
            st->tripped_today = 1;
            JPLOG(JP_WARN, "%-6s circuit_break app=%s pid=%d reason=gone", "PLS", bid, (int)pid);
        }

        /* 无响应判定 (C2-6): 若已完成 >= 2 次 pulse 且最近连续 2 次降幅均 <= 5MB，标记为无响应 (只标记，不停用) */
        /* PID_REUSED、OVERRIDDEN、GONE 均不计入「没降」统计 (S7) */
        uint64_t fp_final = has_fp60 ? fp60 : (has_fp30 ? fp30 : (has_fp15 ? fp15 : fp0));
        if (strcmp(end_reason, "GONE") != 0 && !gone_post_restore &&
            strcmp(end_reason, "PID_REUSED") != 0 &&
            strcmp(end_reason, "OVERRIDDEN") != 0) {
            if (fp_final + 5 >= fp0) {
                st->no_drop_pulses++;
                if (st->total_pulses >= 2 && st->no_drop_pulses >= 2) {
                    if (!st->unresponsive) {
                        st->unresponsive = 1;
                        JPLOG(JP_INFO, "%-6s unresponsive app=%s total=%d no_drop=%d",
                              "PLS", bid, st->total_pulses, st->no_drop_pulses);
                    }
                }
            } else {
                st->no_drop_pulses = 0;
                st->unresponsive = 0;
            }
        }
    }
    g_pulse_global_today_count++;
    pthread_mutex_unlock(&s_pulse_mutex);

thread_done:
    g_pulse_in_progress = 0;
    g_pulse_current_pid = 0;
    g_pulse_current_bid[0] = '\0';
    g_pulse_thread_running = 0;
    free(target);
    return NULL;
}

/* 对照组工作线程参数 (M5 / Q2) */
typedef struct {
    char     bid[128];
    pid_t    pid;
    uint64_t fp0;
} pulse_ctrl_arg_t;

/* 对照组异步只读采样线程 (M5 / Q2)
 * 从头到尾只读，不写任何档位/内存限制。秒级时钟采样，误差超过 +-2s 记 NA */
static void *pulse_ctrl_worker_thread(void *arg) {
    pulse_ctrl_arg_t *carg = (pulse_ctrl_arg_t *)arg;
    if (!carg) return NULL;

    pid_t pid = carg->pid;
    char bid[128];
    rst_str_copy(bid, sizeof(bid), carg->bid);
    uint64_t fp0 = carg->fp0;
    free(carg);

    uint64_t fp15 = 0, fp30 = 0, fp60 = 0;
    int has_fp15 = 0, has_fp30 = 0, has_fp60 = 0;

    struct timespec t_start;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    for (int sec = 1; sec <= 60; sec++) {
        usleep(1000 * 1000);
        if (g_got_sigterm || g_pulse_stop_requested) break;

        struct proc_bsdinfo bi;
        if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof(bi)) != sizeof(bi)) {
            break; /* 进程消失，停止采样 */
        }

        int32_t cur_prio = -1;
        if (pulse_get_cur_priority(pid, &cur_prio) == 0 && (cur_prio == 100 || cur_prio == 120)) {
            break; /* 回前台或后台音频，停止采样 */
        }

        struct timespec t_now;
        clock_gettime(CLOCK_MONOTONIC, &t_now);
        int dt = (int)(t_now.tv_sec - t_start.tv_sec);

        uint64_t cur_fp = 0;
        int rerr = pulse_get_footprint_mb(pid, &cur_fp);
        if (rerr == 0) {
            if (sec == 15 && abs(dt - 15) <= 2) { fp15 = cur_fp; has_fp15 = 1; }
            else if (sec == 30 && abs(dt - 30) <= 2) { fp30 = cur_fp; has_fp30 = 1; }
            else if (sec == 60 && abs(dt - 60) <= 2) { fp60 = cur_fp; has_fp60 = 1; }
        }
    }

    char s_fp15[32], s_fp30[32], s_fp60[32];
    if (has_fp15) snprintf(s_fp15, sizeof(s_fp15), "%llu", (unsigned long long)fp15);
    else snprintf(s_fp15, sizeof(s_fp15), "NA");

    if (has_fp30) snprintf(s_fp30, sizeof(s_fp30), "%llu", (unsigned long long)fp30);
    else snprintf(s_fp30, sizeof(s_fp30), "NA");

    if (has_fp60) snprintf(s_fp60, sizeof(s_fp60), "%llu", (unsigned long long)fp60);
    else snprintf(s_fp60, sizeof(s_fp60), "NA");

    JPLOG(JP_INFO, "PULSE_SKIP_CTRL app=%s pid=%d fp0=%llu fp15=%s fp30=%s fp60=%s",
          bid, (int)pid, (unsigned long long)fp0, s_fp15, s_fp30, s_fp60);

    char today_str[16] = {0};
    pulse_get_local_date_str(today_str, sizeof(today_str));
    pthread_mutex_lock(&s_pulse_mutex);
    pulse_app_state_t *st = pulse_get_app_state(bid, today_str);
    if (st) {
        st->ctrl_day_count++;
    }
    g_pulse_ctrl_global_today_count++;
    pthread_mutex_unlock(&s_pulse_mutex);

    g_pulse_in_progress = 0;
    g_pulse_current_pid = 0;
    g_pulse_current_bid[0] = '\0';
    g_pulse_thread_running = 0;
    return NULL;
}

/* =========================================================================
 * App 进程列表单轮遍历缓存 (供 restart 与 pulse 共同复用，N5-a)
 * ========================================================================= */
#define RST_MAX_TRACKED_APPS 256
typedef struct {
    pid_t pid;
    int32_t priority;
    char path[PROC_PIDPATHINFO_MAXSIZE];
    char bid[128];
} rst_app_proc_t;

static rst_app_proc_t s_app_procs[RST_MAX_TRACKED_APPS];
static int s_napp_procs = -1;

static void rst_ensure_app_procs(memorystatus_priority_entry_t *list, int count);

static void pulse_round(memorystatus_priority_entry_t *list, int count) {
    if (g_npulse_rules == 0 || g_pulse_cfg_file_absent) return;

    /* 回收已结束的 pulse 工作线程 (M4) */
    if (g_pulse_thread_has_handle && !g_pulse_thread_running) {
        pthread_join(g_pulse_thread, NULL);
        g_pulse_thread_has_handle = 0;
    }

    /* 取不到优先级列表时不要清空已有状态，跳过本轮即可 (S12) */
    if (!list || count <= 0) return;

    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    time_t now_mono = now_ts.tv_sec;

    char today_str[16] = {0};
    pulse_get_local_date_str(today_str, sizeof(today_str));
    pulse_check_date_reset(today_str);

    rst_ensure_app_procs(list, count);

    pid_t best_ctrl_pid = 0;
    char best_ctrl_bid[128] = {0};
    uint64_t best_ctrl_fp = 0;

    /* 1. 维护队列：若队列中的 App 已回前台 (cur == 100)、处于 120 档或进程消失，移出队列 */
    for (int q = g_npulse_queue - 1; q >= 0; q--) {
        pid_t qpid = g_pulse_queue[q].pid;
        int found = 0;
        int32_t qcur = -1;
        for (int j = 0; j < s_napp_procs; j++) {
            if (s_app_procs[j].pid == qpid) {
                found = 1;
                qcur = s_app_procs[j].priority;
                break;
            }
        }
        if (!found || qcur == 100 || qcur == 120) {
            pulse_queue_remove_at(q);
        } else {
            uint64_t cfp = 0;
            if (pulse_get_footprint_mb(qpid, &cfp) == 0) {
                g_pulse_queue[q].fp_mb = cfp;
            }
        }
    }

    /* 2. 状态推进与候选 App 入队 (持锁保护共享状态) */
    pthread_mutex_lock(&s_pulse_mutex);
    for (int i = 0; i < g_npulse_rules; i++) {
        pulse_rule_t *rule = &g_pulse_rules[i];
        pulse_app_state_t *st = pulse_get_app_state(rule->bid, today_str);
        if (!st) continue;

        /* 在 s_app_procs 中查找该 rule 的运行进程 */
        pid_t main_pid = 0;
        int32_t main_cur = -1;
        const char *main_path = NULL;
        for (int j = 0; j < s_napp_procs; j++) {
            if (strcmp(s_app_procs[j].bid, rule->bid) == 0) {
                main_pid = s_app_procs[j].pid;
                main_cur = s_app_procs[j].priority;
                main_path = s_app_procs[j].path;
                break;
            }
        }

        if (main_pid <= 0) {
            st->tracked_pid = 0;
            st->in_bg = 0;
            st->bg_start_mono = 0;
            st->pulsed_this_bg = 0;
            st->ctrl_this_bg = 0;
            continue;
        }

        /* 档位状态推进与 delay 计时 (Q2 / delay 裁定) */
        if (main_cur == 100) {
            st->in_bg = 0;
            st->bg_start_mono = 0;
            st->pulsed_this_bg = 0;
            st->ctrl_this_bg = 0;
            st->tracked_pid = main_pid;
        } else if (main_cur == 120) {
            st->in_bg = 0;
            st->bg_start_mono = 0;
            st->ctrl_this_bg = 0;
            st->tracked_pid = main_pid;
        } else if (main_cur >= 0 && main_cur <= 95) {
            if (st->tracked_pid != main_pid) {
                st->tracked_pid = main_pid;
                st->in_bg = 1;
                st->bg_start_mono = now_mono;
                st->pulsed_this_bg = 0;
                st->ctrl_this_bg = 0;
            } else if (!st->in_bg || st->bg_start_mono == 0) {
                st->in_bg = 1;
                st->bg_start_mono = now_mono;
            }
        } else {
            /* 其他值一律不处理：不在 0..95 档，不累计进入后台 delay 计时 (M3) */
            st->in_bg = 0;
            st->bg_start_mono = 0;
            st->tracked_pid = main_pid;
        }

        /* 检查 7 项触发条件 */
        uint64_t cand_fp = 0;
        int skip_ctrl = 0;
        int eligible = pulse_check_candidate(rule, st, main_pid, main_cur, main_path,
                                             now_mono, &cand_fp, &skip_ctrl);
        if (eligible) {
            if (g_pulse_in_progress && g_pulse_current_pid == main_pid) {
                continue;
            }
            int in_queue = 0;
            for (int q = 0; q < g_npulse_queue; q++) {
                if (g_pulse_queue[q].pid == main_pid) {
                    in_queue = 1;
                    break;
                }
            }
            if (!in_queue && g_npulse_queue < PULSE_MAX_QUEUE) {
                pulse_queue_entry_t *qe = &g_pulse_queue[g_npulse_queue++];
                rst_str_copy(qe->bid, sizeof(qe->bid), rule->bid);
                qe->pid = main_pid;
                qe->fp_mb = cand_fp;
                qe->enqueued_mono = now_mono;
            }
        } else if (skip_ctrl) {
            /* 满 delay 时因 n 或 daymax 跳过，记录对照组候选 (M5 / Q2) */
            if (best_ctrl_pid == 0 || cand_fp > best_ctrl_fp) {
                best_ctrl_pid = main_pid;
                rst_str_copy(best_ctrl_bid, sizeof(best_ctrl_bid), rule->bid);
                best_ctrl_fp = cand_fp;
            }
        }
    }
    pthread_mutex_unlock(&s_pulse_mutex);

    /* 3. 对队列按 fp 从大到小排序 */
    pulse_sort_queue();

    /* 4. 若全局无 pulse 正在进行，且队列非空，弹出最大者并启动异步线程 */
    while (!g_pulse_in_progress && g_npulse_queue > 0) {
        pulse_queue_entry_t target = g_pulse_queue[0];
        pulse_queue_remove_at(0);

        /* 出队启动线程时重新检查条件 (M2) */
        uint64_t reval_fp = 0;
        if (!pulse_revalidate_conditions(target.pid, target.bid, &reval_fp)) {
            continue; /* 条件不满足，放弃这次 pulse，尝试队列中下一个 */
        }
        target.fp_mb = reval_fp;

        pthread_mutex_lock(&s_pulse_mutex);
        pulse_app_state_t *st = pulse_get_app_state(target.bid, today_str);
        if (st) {
            st->pulsed_this_bg = 1; /* 每次进入后台最多 1 次 */
        }
        pthread_mutex_unlock(&s_pulse_mutex);

        if (g_got_sigterm || g_pulse_stop_requested) {
            return;
        }

        g_pulse_in_progress = 1;
        g_pulse_current_pid = target.pid;
        rst_str_copy(g_pulse_current_bid, sizeof(g_pulse_current_bid), target.bid);

        pulse_queue_entry_t *thread_arg = (pulse_queue_entry_t *)malloc(sizeof(pulse_queue_entry_t));
        if (thread_arg) {
            *thread_arg = target;
            if (g_pulse_thread_has_handle && !g_pulse_thread_running) {
                pthread_join(g_pulse_thread, NULL);
                g_pulse_thread_has_handle = 0;
            }
            if (pthread_create(&g_pulse_thread, NULL, pulse_worker_thread, thread_arg) == 0) {
                g_pulse_thread_running = 1;
                g_pulse_thread_has_handle = 1;
            } else {
                free(thread_arg);
                g_pulse_in_progress = 0;
                g_pulse_current_pid = 0;
                g_pulse_current_bid[0] = '\0';
            }
        } else {
            g_pulse_in_progress = 0;
            g_pulse_current_pid = 0;
            g_pulse_current_bid[0] = '\0';
        }
        break;
    }

    /* 5. 若全局无 pulse 正在进行且队列为空，可启动对照组只读采样线程 (M5 / Q2)
     * 跟 pulse 共用「全局同时只做 1 个」名额，排队时 pulse 优先 */
    if (!g_pulse_in_progress && g_npulse_queue == 0 && best_ctrl_pid > 0) {
        if (g_got_sigterm || g_pulse_stop_requested) {
            return;
        }

        pulse_ctrl_arg_t *carg = (pulse_ctrl_arg_t *)malloc(sizeof(pulse_ctrl_arg_t));
        if (carg) {
            rst_str_copy(carg->bid, sizeof(carg->bid), best_ctrl_bid);
            carg->pid = best_ctrl_pid;
            carg->fp0 = best_ctrl_fp;

            g_pulse_in_progress = 1;
            g_pulse_current_pid = best_ctrl_pid;
            rst_str_copy(g_pulse_current_bid, sizeof(g_pulse_current_bid), best_ctrl_bid);

            if (g_pulse_thread_has_handle && !g_pulse_thread_running) {
                pthread_join(g_pulse_thread, NULL);
                g_pulse_thread_has_handle = 0;
            }
            if (pthread_create(&g_pulse_thread, NULL, pulse_ctrl_worker_thread, carg) == 0) {
                g_pulse_thread_running = 1;
                g_pulse_thread_has_handle = 1;
                pthread_mutex_lock(&s_pulse_mutex);
                pulse_app_state_t *cst = pulse_get_app_state(best_ctrl_bid, today_str);
                if (cst) {
                    cst->ctrl_this_bg = 1;
                }
                pthread_mutex_unlock(&s_pulse_mutex);
            } else {
                free(carg);
                g_pulse_in_progress = 0;
                g_pulse_current_pid = 0;
                g_pulse_current_bid[0] = '\0';
            }
        }
    }
}

/* =========================================================================
 * 超内存自动重启：状态持久化与状态机实现 (v0.7.7 C2)
 * ========================================================================= */
int rst_fbs_launch(const char *bid, const char **err_stage);

static int days_between(int y1, int m1, int d1, int y2, int m2, int d2) {
    struct tm t1;
    memset(&t1, 0, sizeof(t1));
    t1.tm_year = y1 - 1900;
    t1.tm_mon = m1 - 1;
    t1.tm_mday = d1;
    t1.tm_hour = 12;
    t1.tm_isdst = -1;
    time_t s1 = mktime(&t1);

    struct tm t2;
    memset(&t2, 0, sizeof(t2));
    t2.tm_year = y2 - 1900;
    t2.tm_mon = m2 - 1;
    t2.tm_mday = d2;
    t2.tm_hour = 12;
    t2.tm_isdst = -1;
    time_t s2 = mktime(&t2);

    if (s1 == (time_t)-1 || s2 == (time_t)-1) return 9999;
    double sec_diff = difftime(s1, s2);
    if (sec_diff >= 0) {
        return (int)((sec_diff + 43200.0) / 86400.0);
    } else {
        return (int)((sec_diff - 43200.0) / 86400.0);
    }
}

static inline void rst_str_copy(char *dst, size_t sz, const char *src) {
    if (!dst || sz == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    if (len >= sz) len = sz - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int rst_get_boot_uuid(char *out, size_t outsz) {
    if (!out || outsz == 0) return -1;
    memset(out, 0, outsz);
    size_t len = outsz - 1;
    if (sysctlbyname("kern.bootsessionuuid", out, &len, NULL, 0) == 0 && out[0] != '\0') {
        out[outsz - 1] = '\0';
        return 0;
    }
    rst_str_copy(out, outsz, "00000000-0000-0000-0000-000000000000");
    return 0;
}

static void restart_state_reset_corrupt(void) {
    JPLOG(JP_ERR, "%-6s state_reset reason=corrupt", "RST");
    g_rst_ndaily = 0;
    g_rst_nlast = 0;
    memset(g_rst_daily, 0, sizeof(g_rst_daily));
    memset(g_rst_last, 0, sizeof(g_rst_last));
}

static int parse_daily_line(const char *line, char *out_bid, size_t out_bidsz,
                            char *out_date, size_t out_datesz, int *out_cnt) {
    if (strncmp(line, "bid:", 4) != 0) return -1;
    const char *p = line + 4;
    const char *sp1 = strchr(p, ' ');
    if (!sp1 || sp1 == p) return -1;
    size_t bid_len = sp1 - p;
    if (bid_len >= out_bidsz) return -1;
    memcpy(out_bid, p, bid_len);
    out_bid[bid_len] = '\0';
    if (restart_is_blocked_bid(out_bid)) return -1;

    p = sp1 + 1;
    while (*p == ' ') p++;
    const char *sp2 = strchr(p, ' ');
    if (!sp2 || sp2 == p) return -1;
    size_t date_len = sp2 - p;
    if (date_len != 10 || date_len >= out_datesz) return -1;
    memcpy(out_date, p, date_len);
    out_date[date_len] = '\0';

    int y = 0, m = 0, d = 0;
    if (sscanf(out_date, "%4d-%2d-%2d", &y, &m, &d) != 3) return -1;
    if (y < 2000 || y > 2100 || m < 1 || m > 12 || d < 1 || d > 31) return -1;

    p = sp2 + 1;
    while (*p == ' ') p++;
    if (*p == '\0' || *p == '\r' || *p == '\n') return -1;

    char *endp = NULL;
    errno = 0;
    long cnt = strtol(p, &endp, 10);
    if (errno != 0 || !endp || endp == p || cnt < 0 || cnt > 1000000) return -1;

    while (*endp == ' ' || *endp == '\t' || *endp == '\r' || *endp == '\n') endp++;
    if (*endp != '\0') return -1;

    *out_cnt = (int)cnt;
    return 0;
}

static int parse_last_line(const char *line, char *out_bid, size_t out_bidsz,
                           char *out_uuid, size_t out_uuidsz, time_t *out_mono) {
    if (strncmp(line, "last bid:", 9) != 0) return -1;
    const char *p = line + 9;
    const char *sp1 = strchr(p, ' ');
    if (!sp1 || sp1 == p) return -1;
    size_t bid_len = sp1 - p;
    if (bid_len >= out_bidsz) return -1;
    memcpy(out_bid, p, bid_len);
    out_bid[bid_len] = '\0';
    if (restart_is_blocked_bid(out_bid)) return -1;

    p = sp1 + 1;
    while (*p == ' ') p++;
    const char *sp2 = strchr(p, ' ');
    if (!sp2 || sp2 == p) return -1;
    size_t uuid_len = sp2 - p;
    if (uuid_len == 0 || uuid_len >= out_uuidsz) return -1;
    memcpy(out_uuid, p, uuid_len);
    out_uuid[uuid_len] = '\0';

    p = sp2 + 1;
    while (*p == ' ') p++;
    if (*p == '\0' || *p == '\r' || *p == '\n') return -1;

    char *endp = NULL;
    errno = 0;
    long long mono = strtoll(p, &endp, 10);
    if (errno != 0 || !endp || endp == p || mono < 0) return -1;

    while (*endp == ' ' || *endp == '\t' || *endp == '\r' || *endp == '\n') endp++;
    if (*endp != '\0') return -1;

    *out_mono = (time_t)mono;
    return 0;
}

static int restart_state_load(void) {
    FILE *f = fopen(RESTART_STATE_PATH, "r");
    if (!f) {
        if (errno == ENOENT) {
            g_rst_ndaily = 0;
            g_rst_nlast = 0;
            return 0;
        }
        return -1;
    }

    char line[512];
    int line_no = 0;
    int is_v1 = 0;
    rst_daily_entry_t parsed_daily[RESTART_STATE_MAX_ENTRIES];
    int ndaily = 0;
    rst_last_entry_t parsed_last[RESTART_STATE_MAX_LAST];
    int nlast = 0;

    while (fgets(line, sizeof(line), f)) {
        line_no++;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        char *nl = strchr(p, '\n'); if (nl) *nl = '\0';
        char *cr = strchr(p, '\r'); if (cr) *cr = '\0';
        size_t len = strlen(p);
        while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t')) {
            p[--len] = '\0';
        }

        if (line_no == 1) {
            if (strcmp(p, "v1") != 0) {
                fclose(f);
                restart_state_reset_corrupt();
                return -1;
            }
            is_v1 = 1;
            continue;
        }

        if (*p == '#' || *p == '\0') continue;

        if (strncmp(p, "last bid:", 9) == 0) {
            char bid[128] = {0};
            char uuid[64] = {0};
            time_t mono = 0;
            if (parse_last_line(p, bid, sizeof(bid), uuid, sizeof(uuid), &mono) != 0) {
                fclose(f);
                restart_state_reset_corrupt();
                return -1;
            }
            int found = 0;
            for (int i = 0; i < nlast; i++) {
                if (strcmp(parsed_last[i].bid, bid) == 0) {
                    rst_str_copy(parsed_last[i].boot_uuid, sizeof(parsed_last[i].boot_uuid), uuid);
                    parsed_last[i].mono_sec = mono;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                if (nlast >= RESTART_STATE_MAX_LAST) {
                    fclose(f);
                    restart_state_reset_corrupt();
                    return -1;
                }
                rst_str_copy(parsed_last[nlast].bid, sizeof(parsed_last[nlast].bid), bid);
                rst_str_copy(parsed_last[nlast].boot_uuid, sizeof(parsed_last[nlast].boot_uuid), uuid);
                parsed_last[nlast].mono_sec = mono;
                nlast++;
            }
        } else if (strncmp(p, "bid:", 4) == 0) {
            char bid[128] = {0};
            char date[16] = {0};
            int cnt = 0;
            if (parse_daily_line(p, bid, sizeof(bid), date, sizeof(date), &cnt) != 0) {
                fclose(f);
                restart_state_reset_corrupt();
                return -1;
            }
            int found = 0;
            for (int i = 0; i < ndaily; i++) {
                if (strcmp(parsed_daily[i].bid, bid) == 0 &&
                    strcmp(parsed_daily[i].date, date) == 0) {
                    parsed_daily[i].count = cnt;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                if (ndaily >= RESTART_STATE_MAX_ENTRIES) {
                    fclose(f);
                    restart_state_reset_corrupt();
                    return -1;
                }
                rst_str_copy(parsed_daily[ndaily].bid, sizeof(parsed_daily[ndaily].bid), bid);
                rst_str_copy(parsed_daily[ndaily].date, sizeof(parsed_daily[ndaily].date), date);
                parsed_daily[ndaily].count = cnt;
                ndaily++;
            }
        } else {
            fclose(f);
            restart_state_reset_corrupt();
            return -1;
        }
    }
    fclose(f);

    if (!is_v1) {
        restart_state_reset_corrupt();
        return -1;
    }

    g_rst_ndaily = ndaily;
    memcpy(g_rst_daily, parsed_daily, sizeof(rst_daily_entry_t) * ndaily);
    g_rst_nlast = nlast;
    memcpy(g_rst_last, parsed_last, sizeof(rst_last_entry_t) * nlast);

    struct stat st;
    if (stat(RESTART_STATE_PATH, &st) == 0) {
        g_rst_state_mtime = st.st_mtime;
    }

    return 0;
}

static void restart_state_save(void) {
    FILE *f = fopen(RESTART_STATE_TMP_PATH, "w");
    if (!f) return;

    fprintf(f, "v1\n");

    time_t now_wall = time(NULL);
    struct tm tm_now;
    localtime_r(&now_wall, &tm_now);
    int y_now = tm_now.tm_year + 1900;
    int m_now = tm_now.tm_mon + 1;
    int d_now = tm_now.tm_mday;

    int new_ndaily = 0;
    for (int i = 0; i < g_rst_ndaily; i++) {
        int ey = 0, em = 0, ed = 0;
        if (sscanf(g_rst_daily[i].date, "%4d-%2d-%2d", &ey, &em, &ed) == 3) {
            int diff = days_between(y_now, m_now, d_now, ey, em, ed);
            if (diff >= -1 && diff < 8) {
                if (new_ndaily != i) {
                    g_rst_daily[new_ndaily] = g_rst_daily[i];
                }
                fprintf(f, "bid:%s %s %d\n", g_rst_daily[new_ndaily].bid, g_rst_daily[new_ndaily].date, g_rst_daily[new_ndaily].count);
                new_ndaily++;
            }
        }
    }
    g_rst_ndaily = new_ndaily;

    for (int i = 0; i < g_rst_nlast; i++) {
        fprintf(f, "last bid:%s %s %lld\n",
                g_rst_last[i].bid, g_rst_last[i].boot_uuid, (long long)g_rst_last[i].mono_sec);
    }

    fflush(f);
    int berr = ferror(f);
    fclose(f);

    if (berr) {
        unlink(RESTART_STATE_TMP_PATH);
    } else {
        rename(RESTART_STATE_TMP_PATH, RESTART_STATE_PATH);
        struct stat st;
        if (stat(RESTART_STATE_PATH, &st) == 0) {
            g_rst_state_mtime = st.st_mtime;
        }
    }
}

static void restart_state_check_reload(void) {
    struct stat st;
    if (stat(RESTART_STATE_PATH, &st) == 0) {
        if (st.st_mtime != g_rst_state_mtime) {
            restart_state_load();
            g_rst_state_mtime = st.st_mtime;
        }
    } else {
        if (g_rst_state_mtime != 0) {
            g_rst_state_mtime = 0;
            g_rst_ndaily = 0;
            g_rst_nlast = 0;
        }
    }
}

static int restart_state_get_today_count(const char *bid) {
    time_t now_wall = time(NULL);
    struct tm tm_now;
    localtime_r(&now_wall, &tm_now);
    char today_str[32] = {0};
    snprintf(today_str, sizeof(today_str), "%04d-%02d-%02d",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday);

    for (int i = 0; i < g_rst_ndaily; i++) {
        if (strcmp(g_rst_daily[i].bid, bid) == 0 &&
            strcmp(g_rst_daily[i].date, today_str) == 0) {
            return g_rst_daily[i].count;
        }
    }
    return 0;
}

static int restart_state_get_last(const char *bid, int *same_boot, time_t *last_mono) {
    if (same_boot) *same_boot = 0;
    if (last_mono) *last_mono = 0;

    for (int i = 0; i < g_rst_nlast; i++) {
        if (strcmp(g_rst_last[i].bid, bid) == 0) {
            char cur_uuid[64] = {0};
            rst_get_boot_uuid(cur_uuid, sizeof(cur_uuid));
            if (same_boot) {
                *same_boot = (strcmp(cur_uuid, g_rst_last[i].boot_uuid) == 0);
            }
            if (last_mono) {
                *last_mono = g_rst_last[i].mono_sec;
            }
            return 1;
        }
    }
    return 0;
}

__attribute__((unused)) static int restart_state_get_7day_sum(const char *bid) {
    time_t now_wall = time(NULL);
    struct tm tm_now;
    localtime_r(&now_wall, &tm_now);
    int y_now = tm_now.tm_year + 1900;
    int m_now = tm_now.tm_mon + 1;
    int d_now = tm_now.tm_mday;

    int sum = 0;
    for (int i = 0; i < g_rst_ndaily; i++) {
        if (strcmp(g_rst_daily[i].bid, bid) == 0) {
            int ey = 0, em = 0, ed = 0;
            if (sscanf(g_rst_daily[i].date, "%4d-%2d-%2d", &ey, &em, &ed) == 3) {
                int diff = days_between(y_now, m_now, d_now, ey, em, ed);
                if (diff >= 0 && diff <= 6) {
                    sum += g_rst_daily[i].count;
                }
            }
        }
    }
    return sum;
}

static int restart_state_record_restart(const char *bid, time_t now_mono) {
    time_t now_wall = time(NULL);
    struct tm tm_now;
    localtime_r(&now_wall, &tm_now);
    char today_str[32] = {0};
    snprintf(today_str, sizeof(today_str), "%04d-%02d-%02d",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday);

    int today_count = 1;
    int found_daily = 0;
    for (int i = 0; i < g_rst_ndaily; i++) {
        if (strcmp(g_rst_daily[i].bid, bid) == 0 &&
            strcmp(g_rst_daily[i].date, today_str) == 0) {
            g_rst_daily[i].count++;
            today_count = g_rst_daily[i].count;
            found_daily = 1;
            break;
        }
    }
    if (!found_daily && g_rst_ndaily < RESTART_STATE_MAX_ENTRIES) {
        rst_str_copy(g_rst_daily[g_rst_ndaily].bid, sizeof(g_rst_daily[g_rst_ndaily].bid), bid);
        rst_str_copy(g_rst_daily[g_rst_ndaily].date, sizeof(g_rst_daily[g_rst_ndaily].date), today_str);
        g_rst_daily[g_rst_ndaily].count = 1;
        today_count = 1;
        g_rst_ndaily++;
    }

    char boot_uuid[64] = {0};
    rst_get_boot_uuid(boot_uuid, sizeof(boot_uuid));

    int found_last = 0;
    for (int i = 0; i < g_rst_nlast; i++) {
        if (strcmp(g_rst_last[i].bid, bid) == 0) {
            rst_str_copy(g_rst_last[i].boot_uuid, sizeof(g_rst_last[i].boot_uuid), boot_uuid);
            g_rst_last[i].mono_sec = now_mono;
            found_last = 1;
            break;
        }
    }
    if (!found_last) {
        for (int i = 0; i < g_rst_nlast; i++) {
            if (g_rst_last[i].bid[0] == '\0') {
                rst_str_copy(g_rst_last[i].bid, sizeof(g_rst_last[i].bid), bid);
                rst_str_copy(g_rst_last[i].boot_uuid, sizeof(g_rst_last[i].boot_uuid), boot_uuid);
                g_rst_last[i].mono_sec = now_mono;
                found_last = 1;
                break;
            }
        }
    }
    if (!found_last) {
        if (g_rst_nlast < RESTART_STATE_MAX_LAST) {
            rst_str_copy(g_rst_last[g_rst_nlast].bid, sizeof(g_rst_last[g_rst_nlast].bid), bid);
            rst_str_copy(g_rst_last[g_rst_nlast].boot_uuid, sizeof(g_rst_last[g_rst_nlast].boot_uuid), boot_uuid);
            g_rst_last[g_rst_nlast].mono_sec = now_mono;
            g_rst_nlast++;
            found_last = 1;
        } else {
            /* 槽满，按顺序淘汰 (C1-M5-a) */
            /* 1. 最后一次重启超过 24 小时 (86400 秒) 的记录，最旧的先淘汰 */
            int evict_idx = -1;
            time_t max_age = 0;

            for (int i = 0; i < g_rst_nlast; i++) {
                time_t age = 0;
                int same_boot = (strcmp(boot_uuid, g_rst_last[i].boot_uuid) == 0);
                if (same_boot) {
                    age = (now_mono >= g_rst_last[i].mono_sec) ? (now_mono - g_rst_last[i].mono_sec) : 0;
                } else {
                    age = 86400 + now_mono;
                }
                if (age > 86400) {
                    if (evict_idx == -1 || age > max_age) {
                        evict_idx = i;
                        max_age = age;
                    }
                }
            }

            if (evict_idx >= 0) {
                JPLOG(JP_DEBUG, "%-6s RST_LAST_EVICT bid=%s age=%ld why=old",
                      "RST", g_rst_last[evict_idx].bid, (long)max_age);
                rst_str_copy(g_rst_last[evict_idx].bid, sizeof(g_rst_last[evict_idx].bid), bid);
                rst_str_copy(g_rst_last[evict_idx].boot_uuid, sizeof(g_rst_last[evict_idx].boot_uuid), boot_uuid);
                g_rst_last[evict_idx].mono_sec = now_mono;
                found_last = 1;
            } else {
                /* 2. 不在当前规则里（包括 #off 和已删除）的记录，最旧的先淘汰 */
                evict_idx = -1;
                max_age = 0;
                for (int i = 0; i < g_rst_nlast; i++) {
                    int in_rules = 0;
                    for (int j = 0; j < g_nrestart_rules; j++) {
                        if (strcmp(g_rst_last[i].bid, g_restart_rules[j].bid) == 0) {
                            in_rules = 1;
                            break;
                        }
                    }
                    if (!in_rules) {
                        time_t age = 0;
                        int same_boot = (strcmp(boot_uuid, g_rst_last[i].boot_uuid) == 0);
                        if (same_boot) {
                            age = (now_mono >= g_rst_last[i].mono_sec) ? (now_mono - g_rst_last[i].mono_sec) : 0;
                        } else {
                            age = 86400 + now_mono;
                        }
                        if (evict_idx == -1 || age > max_age) {
                            evict_idx = i;
                            max_age = age;
                        }
                    }
                }

                if (evict_idx >= 0) {
                    JPLOG(JP_DEBUG, "%-6s RST_LAST_EVICT bid=%s age=%ld why=norule",
                          "RST", g_rst_last[evict_idx].bid, (long)max_age);
                    rst_str_copy(g_rst_last[evict_idx].bid, sizeof(g_rst_last[evict_idx].bid), bid);
                    rst_str_copy(g_rst_last[evict_idx].boot_uuid, sizeof(g_rst_last[evict_idx].boot_uuid), boot_uuid);
                    g_rst_last[evict_idx].mono_sec = now_mono;
                    found_last = 1;
                } else {
                    /* 3. 还没有：不写入，打 Warn RST_SLOT_FULL bid=<新bid> slots=<n>，每个 bid 每天最多打一次 */
                    static struct {
                        char bid[128];
                        char date[16];
                    } s_slot_warns[64];
                    static int s_nslot_warns = 0;

                    int already_warned = 0;
                    for (int w = 0; w < s_nslot_warns; w++) {
                        if (strcmp(s_slot_warns[w].bid, bid) == 0 &&
                            strcmp(s_slot_warns[w].date, today_str) == 0) {
                            already_warned = 1;
                            break;
                        }
                    }
                    if (!already_warned) {
                        if (s_nslot_warns < 64) {
                            rst_str_copy(s_slot_warns[s_nslot_warns].bid, sizeof(s_slot_warns[s_nslot_warns].bid), bid);
                            rst_str_copy(s_slot_warns[s_nslot_warns].date, sizeof(s_slot_warns[s_nslot_warns].date), today_str);
                            s_nslot_warns++;
                        } else {
                            rst_str_copy(s_slot_warns[0].bid, sizeof(s_slot_warns[0].bid), bid);
                            rst_str_copy(s_slot_warns[0].date, sizeof(s_slot_warns[0].date), today_str);
                        }
                        JPLOG(JP_WARN, "%-6s RST_SLOT_FULL bid=%s slots=%d",
                              "RST", bid, RESTART_STATE_MAX_LAST);
                    }
                }
            }
        }
    }

    restart_state_save();
    return today_count;
}

static char s_rst_kill_path[PROC_PIDPATHINFO_MAXSIZE] = {0};

/* C4: 清底线 (在 kill 那一刻，C2 状态机 IDLE 步骤 3) */
static void rst_c4_floor_reset(pid_t old_pid, const char *bid) {
    if (old_pid <= 0) return;

    /* 获取被杀旧主进程的完整二进制路径 (优先从内核查，查不到则回退到 kill 前暂存的路径) */
    char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
    if (proc_pidpath(old_pid, path, sizeof(path)) <= 0) {
        if (s_rst_kill_path[0] != '\0') {
            rst_str_copy(path, sizeof(path), s_rst_kill_path);
        }
    }
    const char *app_bid = bid;
    if (!app_bid && path[0] != '\0') {
        app_bid = app_bid_for_path(path);
    }

    /* 如果观察槽 g_memp_post.pid 等于旧 pid，把 valid 置 0 */
    if (g_memp_post.pid == old_pid) {
        g_memp_post.valid = 0;
    }

    /* 遍历 memp 规则，用现有的匹配函数找出所有匹配被杀 pid 的规则 (bid 规则和路径片段规则都算) */
    for (int i = 0; i < g_nmemp_rules; i++) {
        memp_rule_t *rule = &g_memp_rules[i];
        if (!memp_rule_matches(rule, path, app_bid)) {
            continue;
        }

        /* 被清规则的 ineff 清零，eff_cd 回到 base */
        int32_t base_cd = rule->cooldown > 0 ? rule->cooldown : g_memp_cooldown;
        rule->ineff = 0;
        rule->eff_cd = base_cd;

        /* 清掉它们在底线表里的那一条 */
        int cleared = 0;
        for (int f = 0; f < g_nmemp_floors; ) {
            if (strcmp(g_memp_floors[f].frag, rule->frag) == 0) {
                for (int k = f; k < g_nmemp_floors - 1; k++) {
                    g_memp_floors[k] = g_memp_floors[k + 1];
                }
                g_nmemp_floors--;
                cleared = 1;
            } else {
                f++;
            }
        }
        /* 每清一条记一行 INFO：MEMP floor_reset reason=restart bid=<b> frag=<frag>，然后 memp_floor_save() */
        if (cleared) {
            JPLOG(JP_INFO, "MEMP floor_reset reason=restart bid=%s frag=%s",
                  app_bid ? app_bid : "", rule->frag);
            memp_floor_save();
        }
    }
}

/* C4: 读取 band_settled (在 WAIT_SETTLE 读：距 t_launch >= 10 秒后的第一轮) */
static int32_t rst_c4_read_band_settled(pid_t new_pid, int32_t cur_band) {
    /* 口径 (Q2/C4)：新 pid 在 applied 表里且 orig_known=1 取 orig，否则取当前档位 */
    applied_t *a = applied_find(new_pid);
    if (a && a->orig_known) {
        return a->orig;
    }
    return cur_band;
}

static rst_rule_state_t *rst_get_rule_state(const char *bid) {
    if (!bid || bid[0] == '\0') return NULL;
    for (int i = 0; i < g_nrst_rule_states; i++) {
        if (strcmp(g_rst_rule_states[i].bid, bid) == 0) {
            return &g_rst_rule_states[i];
        }
    }
    for (int i = 0; i < g_nrst_rule_states; i++) {
        if (g_rst_rule_states[i].bid[0] == '\0') {
            rst_rule_state_t *st = &g_rst_rule_states[i];
            memset(st, 0, sizeof(*st));
            rst_str_copy(st->bid, sizeof(st->bid), bid);
            return st;
        }
    }
    if (g_nrst_rule_states < RESTART_MAX_RULES) {
        rst_rule_state_t *st = &g_rst_rule_states[g_nrst_rule_states++];
        memset(st, 0, sizeof(*st));
        rst_str_copy(st->bid, sizeof(st->bid), bid);
        return st;
    }
    JPLOG(JP_WARN, "%-6s rule state slots full (max %d), cannot track bid=%s",
          "RST", RESTART_MAX_RULES, bid);
    return NULL;
}

static void restart_log_skip(const restart_rule_t *rule, rst_rule_state_t *r_state,
                             const char *reason, int32_t sys_band, time_t now_mono) {
    int should_log = 0;
    if (r_state->last_skip_mono == 0 || now_mono - r_state->last_skip_mono >= 600) {
        should_log = 1;
    } else if (strcmp(r_state->last_skip_reason, reason) != 0) {
        should_log = 1;
    }

    if (should_log) {
        r_state->last_skip_mono = now_mono;
        strncpy(r_state->last_skip_reason, reason, sizeof(r_state->last_skip_reason) - 1);
        r_state->last_skip_reason[sizeof(r_state->last_skip_reason) - 1] = '\0';
        if (strcmp(reason, "band") == 0) {
            JPLOG(JP_DEBUG, "%-6s skip=band bid=%s sys_band=%d", "RST", rule->bid, (int)sys_band);
        } else {
            JPLOG(JP_DEBUG, "%-6s skip=%s bid=%s", "RST", reason, rule->bid);
        }
    }
}

/*
 * N5-a 限制说明：
 * 1. s_app_procs 上限 256 (RST_MAX_TRACKED_APPS)：单轮遍历最多缓存 256 个用户 App 进程，超出部分静默截断。
 * 2. bid 缓冲区 128 (sizeof(bid))：bundle ID 长度上限 127 字节，超出部分静默截断。
 * 注: rst_app_proc_t、s_app_procs、s_napp_procs 已前置声明于上方供 pulse 与 restart 复用。
 */
static void rst_ensure_app_procs(memorystatus_priority_entry_t *list, int count) {
    if (s_napp_procs >= 0) return; /* 本轮已构建 */
    s_napp_procs = 0;
    if (list && count > 0) {
        for (int i = 0; i < count && s_napp_procs < RST_MAX_TRACKED_APPS; i++) {
            pid_t pid = list[i].pid;
            if (pid <= 0) continue;
            char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
            if (proc_pidpath(pid, path, sizeof(path)) <= 0) continue;
            if (!memp_is_main_app_binary(path)) continue;
            const char *bid = app_bid_for_path(path);
            if (!bid || bid[0] == '\0') continue;

            s_app_procs[s_napp_procs].pid = pid;
            s_app_procs[s_napp_procs].priority = list[i].priority;
            rst_str_copy(s_app_procs[s_napp_procs].path, sizeof(s_app_procs[s_napp_procs].path), path);
            rst_str_copy(s_app_procs[s_napp_procs].bid, sizeof(s_app_procs[s_napp_procs].bid), bid);
            s_napp_procs++;
        }
    }
}

static void restart_round(memorystatus_priority_entry_t *list, int count) {
    restart_state_check_reload();

    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    time_t now_mono = now_ts.tv_sec;

    /* 本轮按需懒建 bid -> pid 映射 (C1-N5-a) */
    s_napp_procs = -1;

    /* 1. 推进状态机 (WAIT_PID / WAIT_SETTLE) */
    if (g_rst_sm.state == RST_STATE_WAIT_PID) {
        rst_ensure_app_procs(list, count);
        pid_t found_new_pid = 0;
        for (int i = 0; i < s_napp_procs; i++) {
            if (s_app_procs[i].pid == g_rst_sm.old_pid) continue;
            if (strcmp(s_app_procs[i].bid, g_rst_sm.bid) == 0) {
                found_new_pid = s_app_procs[i].pid;
                break;
            }
        }

        if (found_new_pid > 0) {
            g_rst_sm.new_pid = found_new_pid;
            g_rst_sm.state = RST_STATE_WAIT_SETTLE;
        } else {
            double elapsed = (now_ts.tv_sec - g_rst_sm.t_launch.tv_sec) +
                             (now_ts.tv_nsec - g_rst_sm.t_launch.tv_nsec) / 1e9;
            if (elapsed > 30.0) {
                JPLOG(JP_INFO, "%-6s relaunch bid=%s new_pid=none ok=0 err=nopid",
                      "RST", g_rst_sm.bid);
                g_rst_sm.state = RST_STATE_IDLE;
            }
        }
    }

    if (g_rst_sm.state == RST_STATE_WAIT_SETTLE) {
        double elapsed = (now_ts.tv_sec - g_rst_sm.t_launch.tv_sec) +
                         (now_ts.tv_nsec - g_rst_sm.t_launch.tv_nsec) / 1e9;
        if (elapsed >= 10.0) {
            rst_ensure_app_procs(list, count);
            int32_t cur_band = 0;
            int found_pid = 0;
            for (int i = 0; i < s_napp_procs; i++) {
                if (s_app_procs[i].pid == g_rst_sm.new_pid) {
                    cur_band = s_app_procs[i].priority;
                    found_pid = 1;
                    break;
                }
            }
            if (!found_pid && kill(g_rst_sm.new_pid, 0) != 0 && errno == ESRCH) {
                JPLOG(JP_INFO, "%-6s relaunch bid=%s new_pid=%d ok=0 err=gone",
                      "RST", g_rst_sm.bid, (int)g_rst_sm.new_pid);
            } else {
                int32_t band_settled = rst_c4_read_band_settled(g_rst_sm.new_pid, cur_band);
                JPLOG(JP_INFO, "%-6s relaunch bid=%s new_pid=%d ok=1 band_settled=%d",
                      "RST", g_rst_sm.bid, (int)g_rst_sm.new_pid, (int)band_settled);
            }
            g_rst_sm.state = RST_STATE_IDLE;
        }
    }

    /* 2. 检查重启规则 (遍历 g_restart_rules) */
    if (g_nrestart_rules == 0 || g_restart_cfg_file_absent) return;
    if (!list || count <= 0) return;

    rst_ensure_app_procs(list, count);

    for (int i = 0; i < g_nrestart_rules; i++) {
        restart_rule_t *rule = &g_restart_rules[i];
        rst_rule_state_t *r_state = rst_get_rule_state(rule->bid);
        if (!r_state) continue;

        /* 查找该 bid 的用户 App 主进程 */
        pid_t main_pid = 0;
        int32_t main_cur = 0;
        if (!restart_is_blocked_bid(rule->bid)) {
            for (int j = 0; j < s_napp_procs; j++) {
                if (strcmp(s_app_procs[j].bid, rule->bid) == 0) {
                    main_pid = s_app_procs[j].pid;
                    main_cur = s_app_procs[j].priority;
                    rst_str_copy(s_rst_kill_path, sizeof(s_rst_kill_path), s_app_procs[j].path);
                    break;
                }
            }
        }

        if (main_pid <= 0) {
            r_state->tracked_pid = 0;
            r_state->in_bg = 0;
            r_state->bg_start_mono = 0;
            r_state->sample_count = 0;
            continue;
        }

        /* C2-2: pulse 进行期间，0.7.7 重启规则跳过该 pid */
        if (g_pulse_in_progress && g_pulse_current_pid == main_pid) {
            restart_log_skip(rule, r_state, "pulse_in_progress", main_cur, now_mono);
            continue;
        }

        if (main_pid != r_state->tracked_pid) {
            r_state->tracked_pid = main_pid;
            r_state->in_bg = (main_cur < 90) ? 1 : 0;
            r_state->bg_start_mono = (main_cur < 90) ? now_mono : 0;
            r_state->sample_count = 0;
        }

        int bg_sec_elapsed = 0;
        const char *skip_reason = NULL;

        /* 判定 1: 后台计时 (任何一轮档位 >= 90，后台计时清零；连续低于 90 满 bg 秒) */
        if (main_cur >= 90) {
            r_state->in_bg = 0;
            r_state->bg_start_mono = 0;
            r_state->sample_count = 0;
            skip_reason = "fg";
        } else {
            if (!r_state->in_bg) {
                r_state->in_bg = 1;
                r_state->bg_start_mono = now_mono;
            }
            bg_sec_elapsed = (int)(now_mono - r_state->bg_start_mono);
            if (bg_sec_elapsed < rule->bg_sec) {
                skip_reason = "bgwait";
            }
        }

        /* 判定 2: 系统档位 (回退链: sys_band -> orig -> cur; <= 30 允许杀) */
        applied_t *a = applied_find(main_pid);
        int32_t sys_band = 0;
        if (a && a->sys_band_known) sys_band = a->sys_band;
        else if (a && a->orig_known) sys_band = a->orig;
        else sys_band = main_cur;

        if (!skip_reason && sys_band > BAND_BACKGROUND) {
            skip_reason = "band";
            r_state->sample_count = 0;
        }

        /* 判定 3: 当天次数 < max */
        int today_count = restart_state_get_today_count(rule->bid);
        if (!skip_reason && today_count >= rule->max_count) {
            skip_reason = "daymax";
            r_state->sample_count = 0;
        }

        /* 判定 4: 距上次重启 >= 30 分钟 (1800 秒) */
        int same_boot = 0;
        time_t last_mono = 0;
        if (!skip_reason && restart_state_get_last(rule->bid, &same_boot, &last_mono)) {
            if (same_boot && (now_mono - last_mono < 1800)) {
                skip_reason = "gap";
                r_state->sample_count = 0;
            }
        }

        /* 判定 5: 采样 (连续两次 phys_footprint > thr，间隔 >= 60 秒) */
        uint64_t fp_bytes = 0;
        int rerr = memp_get_phys_footprint(main_pid, &fp_bytes);
        uint64_t fp_mb = (rerr == 0) ? (fp_bytes / (1024 * 1024)) : 0;

        if (!skip_reason) {
            if (rerr != 0 || fp_mb <= (uint64_t)rule->thr_mb) {
                r_state->sample_count = 0;
                skip_reason = "sample";
            } else {
                if (r_state->sample_count == 0) {
                    r_state->sample_count = 1;
                    r_state->sample_1_mono = now_mono;
                    r_state->sample_1_fp_mb = fp_mb;
                    skip_reason = "sample";
                } else if (r_state->sample_count == 1) {
                    if (now_mono - r_state->sample_1_mono < 60) {
                        skip_reason = "sample";
                    } else {
                        r_state->sample_count = 2;
                    }
                }
            }
        }

        /* 判定 6: 全局状态机是否空闲 (非 IDLE 期间记 skip=busy，不消耗任何东西) */
        if (!skip_reason && g_rst_sm.state != RST_STATE_IDLE) {
            skip_reason = "busy";
        }

        if (skip_reason) {
            int suppress_normal = 0;
            if (strcmp(skip_reason, "sample") == 0 && r_state->sample_count == 0 && fp_mb <= (uint64_t)rule->thr_mb) {
                suppress_normal = 1;
            }
            if (strcmp(skip_reason, "fg") == 0 && fp_mb <= (uint64_t)rule->thr_mb) {
                suppress_normal = 1;
            }
            if (strcmp(skip_reason, "bgwait") == 0 && fp_mb <= (uint64_t)rule->thr_mb) {
                suppress_normal = 1;
            }
            if (!suppress_normal) {
                restart_log_skip(rule, r_state, skip_reason, sys_band, now_mono);
            }
            continue;
        }

        /* 全部条件满足：触发重启！ */
        int krc = kill(main_pid, SIGKILL);
        int kill_errno = (krc == 0) ? 0 : errno;
        int today_n = 0;
        if (kill_errno == 0 || kill_errno == ESRCH) {
            today_n = restart_state_record_restart(rule->bid, now_mono);
        } else {
            today_n = restart_state_get_today_count(rule->bid);
        }

        JPLOG(JP_INFO, "%-6s kill pid=%d bid=%s fp=%llu thr=%d bg=%d sys_band=%d n=%d rc=%d",
              "RST", (int)main_pid, rule->bid,
              (unsigned long long)fp_mb, (int)rule->thr_mb,
              bg_sec_elapsed, (int)sys_band, today_n, kill_errno);

        if (kill_errno == 0 || kill_errno == ESRCH) {
            rst_c4_floor_reset(main_pid, rule->bid);
        }
        s_rst_kill_path[0] = '\0';

        if (kill_errno != 0 && kill_errno != ESRCH) {
            g_rst_sm.state = RST_STATE_IDLE;
            r_state->sample_count = 0;
            continue;
        }

        r_state->sample_count = 0;
        r_state->in_bg = 0;
        r_state->bg_start_mono = 0;
        r_state->last_skip_mono = 0;
        r_state->last_skip_reason[0] = '\0';

        if (rule->relaunch == 0) {
            g_rst_sm.state = RST_STATE_IDLE;
        } else {
            sleep(3);
            const char *err_stage = NULL;
            int c3_rc = rst_fbs_launch(rule->bid, &err_stage);
            if (c3_rc != 0) {
                JPLOG(JP_INFO, "%-6s relaunch bid=%s ok=0 err=%s", "RST", rule->bid, err_stage ? err_stage : "unknown");
                g_rst_sm.state = RST_STATE_IDLE;
            } else {
                clock_gettime(CLOCK_MONOTONIC, &g_rst_sm.t_launch);
                g_rst_sm.state = RST_STATE_WAIT_PID;
                g_rst_sm.old_pid = main_pid;
                g_rst_sm.new_pid = 0;
                strncpy(g_rst_sm.bid, rule->bid, sizeof(g_rst_sm.bid) - 1);
                g_rst_sm.bid[sizeof(g_rst_sm.bid) - 1] = '\0';
                g_rst_sm.relaunch = rule->relaunch;
            }
        }
        /* 本轮已触发一个重启，后续规则在本轮中会看到 g_rst_sm.state != IDLE，记 skip=busy 并顺延重判 */
    }
}

static void mempressure_round(memorystatus_priority_entry_t *list, int count) {
    /* 1. 每一轮开头检查观察槽与采样（§八、§三-A） */
    memp_post_sample(list, count);

    /* 2. 重新加载配置（§四） */
    mempressure_load_config();
    restart_load_config();
    pulse_load_config();

    /* 2.5 自动重启判定与状态机 (v0.7.7 C2) */
    restart_round(list, count);

    /* 2.6 后台内存释放判定 (v0.7.8 C2-1) */
    pulse_round(list, count);

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
                JPLOG(JP_ERR, "MEMP marker_rm_fail n=%d errno=%s", g_memp_marker_rm_fail_count, eb);
            }
        }
        return;
    }

    if (g_memp_reset_pending == MEMP_RESET_PENDING_NORMAL) {
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        long secs_since_warn = (long)(now.tv_sec - g_memp_warn_monotonic.tv_sec);
        if (secs_since_warn >= 600 && !g_memp_reset_overdue_logged) {
            g_memp_reset_overdue_logged = 1;
            JPLOG(JP_WARN, "MEMP reset_overdue secs=%ld", secs_since_warn);
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
                JPLOG(JP_ERR, "MEMP marker_rm_fail n=1 errno=%s", eb);
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
                JPLOG(JP_INFO, "MEMP reset_retry n=%d ret=%d errno=%s", g_memp_reset_retry_count, rc, eb);
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
            JPLOG(JP_DEBUG, "MEMP repair app=%s pid=%d band_before=100 band_after=%d comp=%s ret=0 errno=0(-) result=skip",
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
            JPLOG(JP_INFO, "MEMP repair app=%s pid=%d band_before=100 band_after=%s comp=%s ret=%d errno=%s result=ok",
                   repair_rule ? repair_rule->frag : "-", (int)repair_pid, band_after_str, comp_buf2, rc, eb);
        } else {
            int n = seen_entry ? ++seen_entry->fail_count : 1;
            JPLOG(JP_INFO, "MEMP repair app=%s pid=%d band_before=100 band_after=%s comp=%s ret=%d errno=%s result=retry n=%d",
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
            JPLOG(JP_DEBUG, "MEMP fg_first pid=%d main=%d path=%s", (int)pid, memp_is_main_app_binary(path), path);
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
                    JPLOG(JP_ERR, "MEMP rusage_fail pid=%d errno=%s", (int)pid, eb);
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
            JPLOG(JP_DEBUG, "MEMP target app=%s pid=%d rule=%s path=%s",
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
                JPLOG(JP_ERR, "MEMP yield_sys_fail pid=%d name=%s step=down errno=%s",
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
    JPLOG(JP_WARN, "MEMP warn app=%s pid=%d fp_before=%.1f thr=%d yielded=%d yielded_sys=%d fg_others=%d gate_ms=%ld ret=%d errno=%s recheck=%s",
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
    JPLOG(JP_INFO, "MEMP normal app=%s pid=%d fp_after5=%s band_now=%s stuck=%d early_stop=%d ret=%d errno=%s yielded=%d yielded_sys=%d restored=ok:%d,moved:%d,gone:%d,rfail:%d",
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
            JPLOG(JP_ERR, "MEMP marker_rm_fail n=1 errno=%s", meb);
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
                JPLOG(JP_INFO, "MEMP reset_retry n=1 ret=%d errno=%s", normal_rc, reb);
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
                JPLOG(JP_WARN, "MEMP disabled fails=3 last_errno=%s", deb);
            }
        }
    }

    /* 步骤 12：登记 post 观察槽（§八）——当前门控确保 target_pid 处于 100 档（见 2275 行） */
    memp_post_register(target_rule->frag, target_pid, target_sec, target_usec, warn_mono.tv_sec, fp_before_mb, 100);

    /* 步骤 13：记录冷却起点 */
    target_rule->last_warn_sec = warn_mono.tv_sec;
}

/* ========================================================================= */
/* --- C3: FrontBoardServices 挂起拉起模块 --- */
/* ========================================================================= */
#ifndef OBJC_TYPEDEFS_DEFINED
#define OBJC_TYPEDEFS_DEFINED
typedef void *jp_objc_id;
typedef void *jp_objc_SEL;
typedef void *jp_objc_Class;
#endif

int rst_fbs_launch(const char *bid, const char **err_stage);

int rst_fbs_launch(const char *bid, const char **err_stage) {
    if (err_stage) *err_stage = NULL;

    if (!bid || bid[0] == '\0') {
        if (err_stage) *err_stage = "bad_arg";
        return -1;
    }

    void *h_objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_LAZY);
    if (!h_objc) {
        if (err_stage) *err_stage = "dlopen_objc";
        return -1;
    }

    void *h_fbs = dlopen("/System/Library/PrivateFrameworks/FrontBoardServices.framework/FrontBoardServices", RTLD_LAZY);
    if (!h_fbs) {
        if (err_stage) *err_stage = "dlopen_fbs";
        return -1;
    }

    jp_objc_Class (*fn_objc_getClass)(const char *) =
        (jp_objc_Class (*)(const char *))dlsym(h_objc, "objc_getClass");
    jp_objc_SEL (*fn_sel_registerName)(const char *) =
        (jp_objc_SEL (*)(const char *))dlsym(h_objc, "sel_registerName");
    void *fn_objc_msgSend = dlsym(h_objc, "objc_msgSend");

    if (!fn_objc_getClass || !fn_sel_registerName || !fn_objc_msgSend) {
        if (err_stage) *err_stage = "nosym";
        return -1;
    }

    jp_objc_Class clsFBSSystemService = fn_objc_getClass("FBSSystemService");
    if (!clsFBSSystemService) {
        if (err_stage) *err_stage = "noclass";
        return -1;
    }

    jp_objc_SEL selSharedService = fn_sel_registerName("sharedService");
    jp_objc_id service = NULL;
    if (selSharedService) {
        service = ((jp_objc_id (*)(jp_objc_id, jp_objc_SEL))fn_objc_msgSend)(
            (jp_objc_id)clsFBSSystemService, selSharedService);
    }
    if (!service) {
        if (err_stage) *err_stage = "noservice";
        return -1;
    }

    CFStringRef *pKey = (CFStringRef *)dlsym(h_fbs, "FBSOpenApplicationOptionKeyActivateSuspended");
    if (!pKey || !*pKey) {
        if (err_stage) *err_stage = "nokey";
        return -1;
    }

    jp_objc_SEL selResponds = fn_sel_registerName("respondsToSelector:");
    jp_objc_SEL selWithResult = fn_sel_registerName("openApplication:options:withResult:");
    if (!selResponds || !selWithResult) {
        if (err_stage) *err_stage = "noselector";
        return -1;
    }

    intptr_t responds = ((intptr_t (*)(jp_objc_id, jp_objc_SEL, jp_objc_SEL))fn_objc_msgSend)(
        service, selResponds, selWithResult);
    if (!responds) {
        if (err_stage) *err_stage = "noselector";
        return -1;
    }

    CFStringRef cf_bid = CFStringCreateWithCString(kCFAllocatorDefault, bid, kCFStringEncodingUTF8);
    if (!cf_bid) {
        if (err_stage) *err_stage = "bad_arg";
        return -1;
    }

    const void *keys[] = { *pKey };
    const void *values[] = { kCFBooleanTrue };
    CFDictionaryRef options = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                                                 &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks);
    if (!options) {
        CFRelease(cf_bid);
        if (err_stage) *err_stage = "bad_arg";
        return -1;
    }

    void *(*fn_poolPush)(void) = (void *(*)(void))dlsym(h_objc, "objc_autoreleasePoolPush");
    void (*fn_poolPop)(void *) = (void (*)(void *))dlsym(h_objc, "objc_autoreleasePoolPop");

    void *pool = NULL;
    if (fn_poolPush && fn_poolPop) {
        pool = fn_poolPush();
    } else {
        JPLOG(JP_WARN, "%-6s objc_autoreleasePool symbols missing, calling without pool", "RST");
    }

    ((void (*)(jp_objc_id, jp_objc_SEL, jp_objc_id, jp_objc_id, jp_objc_id))fn_objc_msgSend)(
        service, selWithResult, (jp_objc_id)cf_bid, (jp_objc_id)options, NULL);

    if (pool && fn_poolPop) {
        fn_poolPop(pool);
    }

    CFRelease(options);
    CFRelease(cf_bid);

    if (err_stage) *err_stage = NULL;
    return 0;
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
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sa.sa_flags = 0; /* 不要 SA_RESTART，确保 select/sleep 被信号打断后立即退出 */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    init_loglevel();  /* 从启动第一行起按配置 loglevel 过滤 (P1) */
    lockmon_init();  /* 锁屏监听（失败则保持常速轮询，不影响保护） */
    load_old_targets();  /* 启动时导入旧 status.txt 记录供首轮认领 */
    restart_state_load();  /* 启动时载入重启计数状态 (v0.7.7 C2) */
    pulse_startup_recovery();  /* 启动时脉冲状态恢复 (v0.7.8 C2-5) */
    JPLOG(JP_INFO, "%-6s v%s commit=%s pid=%d old_records=%d locked=%d",
           "START", JP_VERSION, JP_COMMIT, (int)getpid(), g_nold_targets, g_locked);
    mempressure_init();  /* 前台 App 内存压力触发初始化 (v0.7.0) */

    while (!should_stop()) {
        if (load_config() != 0) { sleep(5); continue; }

        char bands_token[128] = {0};
        int handle_bands_req = 0;
        if (rename(BANDS_REQ_PATH, BANDS_TAKEN_PATH) == 0) {
            FILE *rf = fopen(BANDS_TAKEN_PATH, "r");
            if (rf) {
                if (fgets(bands_token, sizeof(bands_token), rf)) {
                    char *nl = strchr(bands_token, '\n'); if (nl) *nl = '\0';
                    char *cr = strchr(bands_token, '\r'); if (cr) *cr = '\0';
                    char *p = bands_token;
                    while (*p == ' ' || *p == '\t') p++;
                    if (p != bands_token) memmove(bands_token, p, strlen(p) + 1);
                    int tlen = (int)strlen(bands_token);
                    while (tlen > 0 && (bands_token[tlen - 1] == ' ' || bands_token[tlen - 1] == '\t')) {
                        bands_token[--tlen] = '\0';
                    }
                    if (bands_token[0] != '\0') {
                        handle_bands_req = 1;
                    }
                }
                fclose(rf);
            }
            unlink(BANDS_TAKEN_PATH);
        }

        FILE *bf = NULL;
        if (handle_bands_req) {
            bf = fopen(BANDS_TMP_PATH, "w");
            if (bf) {
                time_t bt = time(NULL);
                struct tm btm;
                localtime_r(&bt, &btm);
                char bts[64];
                strftime(bts, sizeof(bts), "%Y-%m-%d %H:%M:%S", &btm);
                fprintf(bf, "req=%s\nupdated=%s\n", bands_token, bts);
            }
        }

        int count = 0;
        memorystatus_priority_entry_t *list = get_list(&count);
        int protected_now = 0, demote_actions = 0;
        protected_entry_t plist[MAX_PROTECTED_LIST];
        int nplist = 0;
        protected_entry_t dlist[MAX_PROTECTED_LIST];
        int ndlist = 0;

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
                int32_t demote_target = BAND_BACKGROUND;
                int skip_this_round = 0;   /* 本轮是否走了 SKIP：非 SKIP 路径要清去重项 */
                applied_t *prev = applied_find(pid);
                if (prev) {
                    prev->seen = 1;
                    if (cur != prev->band) {
                        prev->sys_band = cur;
                        prev->sys_band_known = 1;
                    }
                }
                /* 首轮认领：上个实例记录过、且当前档位仍等于它设的值 */
                if (!prev && g_nold_targets > 0 && cur < BAND_FOREGROUND) {
                    int32_t ot = 0, oorig = 0;
                    int ok = 0;
                    if (old_target_find(pid, &ot, &oorig, &ok) && cur == ot) {
                        applied_set(pid, cur);
                        prev = applied_find(pid);   /* 表满时为 NULL，安全 */
                        if (prev) {
                            if (ok) {
                                prev->orig = oorig;
                                prev->orig_known = 1;
                            } else {
                                prev->orig_known = 0;
                            }
                            prev->sys_band = 0;
                            prev->sys_band_known = 0;
                        }
                        JPLOG(JP_INFO, "%-6s pid=%d band=%d", "CLAIM", (int)pid, (int)cur);
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
                            applied_set_ex(pid, target, cur);
                            act_log_success(ACT_KIND_RAISE, pid, path, frag, cur, target);
                            cur = target;
                        } else {
                            /* errno 已被 set_priority 失败分支的日志调用污染，用 g_last_err_no */
                            JPLOG(JP_ERR, "%-6s pid=%d %s %s %d->%d FAIL errno=%d %s",
                                   "RAISE", (int)pid, proc_label_log(path), frag, (int)cur, (int)target,
                                   g_last_err_no, strerror(g_last_err_no));
                        }
                    } else if (cur > target && cur < BAND_FOREGROUND) {
                        if (prev && prev->band == cur) {
                            /* 高于目标且是我们（或旧实例）设的：跟着降 */
                            int rc = set_priority(pid, target);
                            if (rc == 0) {
                                g_reassert_count++;
                                applied_set_ex(pid, target, cur);
                                JPLOG(JP_INFO, "%-6s pid=%d %s %s %d->%d ok",
                                       "LOWER", (int)pid, proc_label_log(path), frag, (int)cur, (int)target);
                                cur = target;
                            } else {
                                /* errno 已被 set_priority 失败分支的日志调用污染，用 g_last_err_no */
                                JPLOG(JP_ERR, "%-6s pid=%d %s %s %d->%d FAIL errno=%d %s",
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
                                JPLOG(JP_DEBUG, "%-6s pid=%d %s %s cur=%d target=%d ours=%s",
                                       "SKIP", (int)pid, proc_label_log(path), frag, (int)cur, (int)target, ours_str);
                            }
                        }
                    }
                } else if (path_matches(path, app_bid, g_demote, g_ndemote, &demote_target, &demote_frag)) {
                    if (!app_bid) {
                        app_bid = app_bid_for_path(path);
                    }
                    protected_entry_t *de = NULL;
                    if (ndlist < MAX_PROTECTED_LIST) {
                        de = &dlist[ndlist++];
                        de->frag = demote_frag; de->pid = pid; de->cur = cur; de->target = demote_target;
                        proc_label(path, de->label, sizeof(de->label));
                        if (app_bid) {
                            strncpy(de->bid, app_bid, sizeof(de->bid) - 1);
                            de->bid[sizeof(de->bid) - 1] = '\0';
                        } else {
                            de->bid[0] = '\0';
                        }
                    }
                    /* 只在非前台且当前档位高于目标档位时降 */
                    if (cur < BAND_FOREGROUND && cur > demote_target) {
                        demote_actions++;
                        int rc = set_priority(pid, demote_target);
                        if (rc == 0) {
                            if (de) de->cur = demote_target;
                            applied_set_ex(pid, demote_target, cur);
                            act_log_success(ACT_KIND_DEMOTE, pid, path, demote_frag, cur, demote_target);
                            cur = demote_target;
                        } else {
                            /* errno 已被 set_priority 失败分支的日志调用污染，用 g_last_err_no */
                            JPLOG(JP_ERR, "%-6s pid=%d %s %s %d->%d FAIL errno=%d %s",
                                   "DEMOTE", (int)pid, proc_label_log(path), demote_frag, (int)cur, (int)demote_target,
                                   g_last_err_no, strerror(g_last_err_no));
                        }
                    }
                } else if (prev && prev->band == cur && cur < BAND_FOREGROUND) {
                    if (cur > BAND_BACKGROUND) {
                        /* 原逻辑：protect 抬过的档位还回普通后台 (30) */
                        int rc = set_priority(pid, BAND_BACKGROUND);
                        if (rc == 0) {
                            applied_remove(pid);   /* 还回后不再是我们的档位，内含 act_dedup_clear */
                            prev = NULL;           /* 指针已失效 */
                            cur = BAND_BACKGROUND;
                            JPLOG(JP_INFO, "%-6s pid=%d %s %d->%d ok",
                                   "RETURN", (int)pid, proc_label_log(path), (int)cur, BAND_BACKGROUND);
                        } else {
                            /* errno 已被 set_priority 失败分支的日志调用污染，用 g_last_err_no */
                            JPLOG(JP_ERR, "%-6s pid=%d %s %d->%d FAIL errno=%d %s",
                                   "RETURN", (int)pid, proc_label_log(path), (int)cur, BAND_BACKGROUND,
                                   g_last_err_no, strerror(g_last_err_no));
                        }
                    } else {
                        /* cur <= 30：demote 压过的，或已经是 30；不动档位，只把记录忘掉，交还系统 */
                        applied_remove(pid);
                        prev = NULL;
                    }
                }
                if (bf) {
                    prev = applied_find(pid);
                    const char *b_bid = app_bid ? app_bid : app_bid_for_path(path);
                    if (!b_bid) b_bid = "";
                    char plabel[64];
                    proc_label(path, plabel, sizeof(plabel));
                    int ours = (prev != NULL) ? 1 : 0;
                    char orig_str[32];
                    if (prev && prev->orig_known) {
                        snprintf(orig_str, sizeof(orig_str), "%d", (int)prev->orig);
                    } else {
                        snprintf(orig_str, sizeof(orig_str), "NA");
                    }
                    uint64_t fp_bytes = 0;
                    int rerr = memp_get_phys_footprint(pid, &fp_bytes);
                    char fp_str[32];
                    if (rerr == 0) {
                        snprintf(fp_str, sizeof(fp_str), "%llu", (unsigned long long)(fp_bytes / (1024 * 1024)));
                    } else {
                        snprintf(fp_str, sizeof(fp_str), "NA");
                    }
                    fprintf(bf, "%s|%s|%d|%d|%d|%s|%s\n",
                            b_bid, plabel, (int)pid, (int)cur, ours, orig_str, fp_str);
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

        if (bf) {
            int berr = ferror(bf);
            fclose(bf);
            if (berr) {
                unlink(BANDS_TMP_PATH);
            } else {
                rename(BANDS_TMP_PATH, BANDS_PATH);
            }
        }

        int demoted_pids = 0;
        for (int i = 0; i < ndlist; i++) {
            if (dlist[i].cur <= dlist[i].target) {
                demoted_pids++;
            }
        }

        write_status(protected_now, demoted_pids, plist, nplist, dlist, ndlist);
        mempressure_write_state();

        if (should_stop() || g_got_sigterm) break;

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

    if (g_pulse_thread_running) {
        g_pulse_stop_requested = 1;
        pthread_join(g_pulse_thread, NULL);
        g_pulse_thread_running = 0;
        g_pulse_thread_has_handle = 0;
    } else if (g_pulse_thread_has_handle) {
        pthread_join(g_pulse_thread, NULL);
        g_pulse_thread_has_handle = 0;
    }

    act_dedup_flush_all();
    JPLOG(JP_INFO, "STOP");
    pf = fopen(PID_PATH, "r");
    if (pf) {
        int cur_pid = 0;
        if (fscanf(pf, "%d", &cur_pid) == 1 && cur_pid == (int)getpid()) {
            fclose(pf);
            unlink(PID_PATH);
        } else {
            fclose(pf);
        }
    }
    fprintf(stderr, "[JetPilot] 收到停止信号，退出\n");
    return 0;
}
