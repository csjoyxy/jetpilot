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
#include <stdbool.h>
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
#include <fcntl.h>
#include <sys/file.h>
#include <stdatomic.h>
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
#include <mach/mach_time.h>

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
#ifndef PROC_PIDTASKINFO
#define PROC_PIDTASKINFO 4
struct proc_taskinfo {
    uint64_t  pti_virtual_size;      /* virtual memory size (bytes) */
    uint64_t  pti_resident_size;     /* resident memory size (bytes) */
    uint64_t  pti_total_user;        /* total time (user) */
    uint64_t  pti_total_system;      /* total time (system) */
    uint64_t  pti_threads_user;      /* existing threads only */
    uint64_t  pti_threads_system;
    int32_t   pti_policy;            /* default policy for new threads */
    int32_t   pti_faults;            /* number of page faults */
    int32_t   pti_pageins;           /* number of actual pageins */
    int32_t   pti_cow_faults;        /* number of copy-on-write faults */
    int32_t   pti_messages_sent;     /* number of messages sent */
    int32_t   pti_messages_received; /* number of messages received */
    int32_t   pti_syscalls_mach;     /* number of mach syscalls */
    int32_t   pti_syscalls_unix;     /* number of unix syscalls */
    int32_t   pti_csw;               /* number of context switches */
    int32_t   pti_threadnum;         /* number of threads */
    int32_t   pti_numrunning;        /* number of running threads */
    int32_t   pti_flags;             /* flags */
};
#endif
extern int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);

/* ---- XNU kern_memorystatus.h 相关定义（头文件不公开，这里自己声明） ---- */
#define MEMORYSTATUS_CMD_GET_PRIORITY_LIST        1
#define MEMORYSTATUS_CMD_SET_PRIORITY_PROPERTIES  2
/* E4 修正：XNU bsd/sys/kern_memorystatus.h 中 command=3 实为 GET_JETSAM_SNAPSHOT（无快照时
 * 忽略 pid 返回 0 且不写缓冲区，会导致读出假 0 档）；XNU 原生按 pid 定向查有效档位是
 * MEMORYSTATUS_CMD_GET_PRIORITY_LIST(1) 传入 pid>0 与 sizeof(memorystatus_priority_entry_t)
 * （内核走 memorystatus_get_priority_pid，返回 sizeof(entry) 并填 p_memstat_effectivepriority）。
 * 快轮仍保留三级兜底链（PID1 -> LISTSCAN -> SLEEP -> LEGACY）。 */

/* iOS 16+ 档位参考值 —— T3-IMP: 档位数值的唯一权威来源（模块内零字面量） */
#define BAND_IDLE            0
#define BAND_BACKGROUND      30  /* 自然后台上界 / 无状态 kill 允许上界 */
#define BAND_FREEZER         75  /* XNU JETSAM_PRIORITY_FREEZER（冷冻室） */
#define BAND_PROTECT_DEFAULT 90  /* 保护规则默认目标 (= BAND_FOREGROUND - 10) */
#define BAND_MAX_PROTECT     95  /* 保护封顶，永不触及前台档与系统抬升档 */
#define BAND_FOREGROUND      100 /* 前台：保护环绝不触碰 */
#define BAND_SYSTEM_ELEVATED 120 /* 系统抬升档（原字面量 120） */

/* XNU bsd/sys/kern_memorystatus.h entry.state 位掩码（真机 121 进程全集核实） */
#define JP_MEMSTAT_SUSPENDED 0x001U /* kMemorystatusSuspended */
#define JP_MEMSTAT_FROZEN    0x002U /* kMemorystatusFrozen */
#define JP_MEMSTAT_ASSERTION 0x040U /* kMemorystatusAssertion */
#define JP_MEMSTAT_ACTIVE    0x080U /* kMemorystatusActive */

/* 常量关系固化，防后续模块漂移 */
_Static_assert(BAND_BACKGROUND < BAND_FREEZER &&
               BAND_FREEZER < BAND_PROTECT_DEFAULT &&
               BAND_MAX_PROTECT < BAND_FOREGROUND &&
               BAND_PROTECT_DEFAULT == BAND_FOREGROUND - 10,
               "band constants drift: protect/candidate/foreground mismatch");

/* T3-IMP 3.3: applied 快照新鲜度上限（240s，与 E2 最慢慢轮周期同值；
 * SLOW_MAX_SECS 在本文件下方定义为本宏，避免两处数字各自漂移）。
 * 纯内存时间戳，不落盘、不改变任何状态文件格式。 */
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

extern int memorystatus_control(uint32_t command, int32_t pid, uint32_t flags,
                                void *buffer, size_t buffersize);

/* ---- 配置 ---- */
#ifndef CONFIG_DIR
#define CONFIG_DIR  "/var/mobile/Library/JetPilot"
#endif
#define CONFIG_PATH CONFIG_DIR "/config.txt"
/* DIAG2: 用户可见「测试模式」配置文件（单行 level=off|basic|detailed|full）。
 * daemon 只读不写；App 是唯一写入方。与 pulse.txt 语法完全解耦。 */
#define DIAG_CONFIG_PATH CONFIG_DIR "/diag.txt"
#define FLAG_PATH   CONFIG_DIR "/running.flag"
#define STATUS_PATH CONFIG_DIR "/status.txt"
#define PID_PATH    CONFIG_DIR "/daemon.pid"
#define LOCK_PATH   CONFIG_DIR "/daemon.lock"
#define BANDS_REQ_PATH   CONFIG_DIR "/bands.req"
#define BANDS_TAKEN_PATH CONFIG_DIR "/bands.req.taken"
#define BANDS_PATH       CONFIG_DIR "/bands.txt"
#define BANDS_TMP_PATH   CONFIG_DIR "/bands.txt.tmp"
#define JP_VERSION   "0.8.7"
#ifndef JP_COMMIT
#define JP_COMMIT    "dev"
#endif

/* v0.8.7: AssertionServices BKSApplicationStateMonitor 状态常量（真机框架核实） */
#define JP_BKS_STATE_TERMINATED   1U
#define JP_BKS_STATE_BG_SUSPENDED 2U
#define JP_BKS_STATE_BG_RUNNING   4U
#define JP_BKS_STATE_FOREGROUND   8U
#define JP_BKS_STATE_ALL          0xFFFFFFFFU
#define APPMON_RING_CAP           64
#define APPMON_MAX_BIDS           128
#define APPMON_BG_PENDING_MAX_SEC 3
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

/* --------------------------------------------------------------------------
 * INT1: root 写路径安全加固（移植 T1 d75def5，保留远端 M1 原子提交）
 *
 * 所有以 root 落盘的文件在 open 前/后都经过下列两道校验，缺一不可：
 *   1) jp_secure_precheck: CONFIG_DIR lstat 必须是真实目录且非符号链接；目标
 *      若存在，必须是普通文件、root 属主、且组/其他不可写 (& 0022)。ENOENT
 *      允许（首次创建）；其余 lstat 错误一律拒绝。前置校验在 O_TRUNC 生效
 *      之前完成，避免"校验失败却已截断文件"。
 *   2) jp_secure_fd_check: open(O_NOFOLLOW) 后基于 fd 再校验一次（防
 *      lstat→open 竞态替换），并 fchmod 回调用方给定的预期 mode。
 * 任何异常一律拒绝写入，调用方不得回退 fopen/unlink 重建等不安全写法。
 * -------------------------------------------------------------------------- */

/* 打开前校验：0 = 允许写入，非 0 = 拒绝 */
static int jp_secure_precheck(const char *path) {
    struct stat dir_st;
    if (lstat(CONFIG_DIR, &dir_st) != 0 || S_ISLNK(dir_st.st_mode) ||
        !S_ISDIR(dir_st.st_mode)) {
        return -1;
    }

    struct stat pre;
    if (lstat(path, &pre) == 0) {
        if (S_ISLNK(pre.st_mode) || !S_ISREG(pre.st_mode) ||
            pre.st_uid != 0 || (pre.st_mode & 0022) != 0) {
            return -1;
        }
    } else if (errno != ENOENT) {
        return -1;
    }
    return 0;
}

/* 打开后复核并强制回写预期权限：0 = 允许写入，非 0 = 拒绝 */
static int jp_secure_fd_check(int fd, mode_t mode) {
    struct stat st;
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != 0 || (st.st_mode & 0022) != 0 ||
        fchmod(fd, mode) != 0) {
        return -1;
    }
    return 0;
}

static void jp_log_write(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define JPLOG(lvl, fmt, ...) do { \
    if (g_loglevel != JP_LOG_OFF && (lvl) <= g_loglevel) { \
        jp_log_write((fmt), ##__VA_ARGS__); \
    } \
} while (0)

/* =========================================================================
 * DIAG2: 用户可见「测试模式」四档日志（off/basic/detailed/full）。
 *
 * 设计要点（与 g_loglevel 完全解耦）：
 *   - DIAGLOG 只做「等级 / 每日预算 / 去重」三道闸；底层唯一落盘通道仍是
 *     jp_log_write（同一 s_log_mutex、同一 O_APPEND|O_NOFOLLOW、同一 daemon.log
 *     轮转）。绝不复用受 g_loglevel 过滤的 JPLOG 宏，否则 loglevel=off 时
 *     「测试模式」会静默失效；零新增日志文件、零新增 fd。
 *   - off 时 DIAGLOG 的第一个判断即为 false：零 vsnprintf、零加锁、零写盘。
 *   - 预算：basic 100 / detailed 300 / full 600 事件每日，180KB 字节软上限；
 *     超限只累加 g_diag_budget_dropped，不输出；闩锁/去重抑制另计 g_diag_latch_suppressed。
 * ========================================================================= */
enum { DIAG_OFF = 0, DIAG_BASIC = 1, DIAG_DETAILED = 2, DIAG_FULL = 3 };
#define DIAG_BYTE_CAP     (180UL * 1024UL)   /* 每日字节软上限 */
#define DIAG_LINE_BUDGET  512                 /* 单行格式化暂存（容纳最长 bid×2 + 字段） */
#define DIAG_DEDUP_SLOTS  64
#define DIAG_DEDUP_KEY_SZ 160
#define DIAG_SKIP_LATCH_N 16

/* 固定原因码枚举（不含任何用户数据；diag.txt 原文绝不入日志） */
#define DSK_INVALID_ARG       "invalid_arg"
#define DSK_RULE_OFF          "rule_off"
#define DSK_UNSUPPORTED       "unsupported"
#define DSK_RESTORE_TRIPPED   "restore_tripped"
#define DSK_NOT_CANDIDATE_BAND "not_candidate_band"
#define DSK_BG_NOT_TRACKED    "bg_not_tracked"
#define DSK_DELAY_NOT_MET     "delay_not_met"
#define DSK_ALREADY_DONE      "already_done"
#define DSK_FP_UNREADABLE     "fp_unreadable"
#define DSK_FP_BELOW_TARGET   "fp_below_target"
#define DSK_OVER_RESTART_THR  "over_restart_thr"
#define DSK_TOO_LATE          "too_late"
#define DSK_CIRCUIT_TRIPPED   "circuit_tripped"
#define DSK_CIRCUIT_COOLDOWN  "circuit_cooldown"
#define DSK_CIRCUIT_ESCALATED "circuit_escalated"
#define DSK_GONE              "gone"
#define DSK_COOLDOWN          "cooldown"
#define DSK_DAYMAX            "daymax"
#define DSK_GLOBAL_CAP        "global_cap"
#define DSK_QUEUE_FULL        "queue_full"

typedef struct {
    char   key[DIAG_DEDUP_KEY_SZ];
    time_t first_mono;
    unsigned count;
} diag_dedup_t;

typedef struct {
    char bid[128];
    int  bg_seq;
    int  used;
    char first[32];       /* 首因（新会话时置） */
    char final[32];       /* 终态因（覆盖槽） */
    int  final_seen;      /* 本段是否已写入终态因 */
} diag_skip_latch_t;

/* DIAG2 并发保护：纯叶锁 s_diag_mutex（严格低于 s_pulse_mutex，且绝不嵌套 s_log_mutex） */
static pthread_mutex_t s_diag_mutex     = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int     g_diag_level     = DIAG_OFF;   /* 全局单值 0..3 */
static int             g_diag_events    = 0;          /* 当日事件计数 (s_diag_mutex) */
static unsigned long   g_diag_bytes     = 0;          /* 当日字节计数 (s_diag_mutex) */
/* v0.8.6 C2: 单一 dropped 拆两路——闩锁/去重抑制 vs 预算丢弃；
 * diag_dropped_now() 仍返回两者之和，兼容所有现有日志字段。 */
static _Atomic int     g_diag_latch_suppressed = 0;  /* 闩锁/去重抑制 */
static _Atomic int     g_diag_budget_dropped   = 0;  /* 事件数/字节预算丢弃 */
static int             g_diag_stats_hour = -1;        /* 上次小时汇总的小时 (s_diag_mutex) */
static char            g_diag_day[16]   = {0};        /* 当日 YYYY-MM-DD (s_diag_mutex) */
static diag_dedup_t    g_diag_dedup[DIAG_DEDUP_SLOTS];
static unsigned        g_diag_dedup_head = 0;
static diag_skip_latch_t g_diag_skip_latch[DIAG_SKIP_LATCH_N];
static unsigned        g_diag_skip_latch_head = 0;

static inline int diag_level_now(void) {
    return atomic_load_explicit(&g_diag_level, memory_order_acquire);
}

static inline int diag_dropped_now(void) {
    return atomic_load_explicit(&g_diag_latch_suppressed, memory_order_relaxed) +
           atomic_load_explicit(&g_diag_budget_dropped, memory_order_relaxed);
}

/* v0.8.6 C2: 分路 accessor，供新增的 diag_latch=/diag_budget= 字段使用 */
static inline int diag_latch_now(void) {
    return atomic_load_explicit(&g_diag_latch_suppressed, memory_order_relaxed);
}

static inline int diag_budget_now(void) {
    return atomic_load_explicit(&g_diag_budget_dropped, memory_order_relaxed);
}

static const char *diag_level_to_str(int lv) {
    switch (lv) {
        case DIAG_BASIC:    return "basic";
        case DIAG_DETAILED: return "detailed";
        case DIAG_FULL:     return "full";
        case DIAG_OFF:
        default:            return "off";
    }
}

static int diag_level_cap(int lv) {
    switch (lv) {
        case DIAG_BASIC:    return 100;
        case DIAG_DETAILED: return 300;
        case DIAG_FULL:     return 600;
        case DIAG_OFF:
        default:            return 0;
    }
}

/* 相对秒 → 字符串；负值（未到达）统一记 NA */
static void diag_sec_str(char *buf, size_t sz, time_t sec) {
    if (sec >= 0) snprintf(buf, sz, "%ld", (long)sec);
    else snprintf(buf, sz, "NA");
}

/* v0.8.6 A2 —— memorystatus_priority_entry_t.state 状态跳变观测缓存。
 * 扩容至 256 槽并按轮次 epoch 优先回收已退出 pid，且仅追踪主应用进程、不挤占公共
 * 64 槽 g_diag_dedup 环，根治进程数 >64 时每轮慢扫全量淘汰重打耗尽 600 条日配额问题。 */
#define JETSAM_STATE_CACHE_SLOTS 256
typedef struct {
    pid_t         pid;
    int32_t       band;
    uint32_t      state;
    unsigned long seen_epoch;
    int           used;
} jetsam_state_slot_t;
static jetsam_state_slot_t s_jetsam_state_cache[JETSAM_STATE_CACHE_SLOTS];
static unsigned s_jetsam_state_head = 0;

/* 跨日重置：复用 pulse_check_date_reset 的触发点；重置预算、去重表与闩锁，
 * 等级本身不清零（用户设置持续有效）。调用方可持 s_pulse_mutex，锁序安全。 */
static void diag_reset_day(void) {
    pthread_mutex_lock(&s_diag_mutex);
    g_diag_events = 0;
    g_diag_bytes = 0;
    atomic_store_explicit(&g_diag_latch_suppressed, 0, memory_order_relaxed);
    atomic_store_explicit(&g_diag_budget_dropped, 0, memory_order_relaxed);
    g_diag_stats_hour = -1;
    memset(g_diag_dedup, 0, sizeof(g_diag_dedup));
    g_diag_dedup_head = 0;
    memset(g_diag_skip_latch, 0, sizeof(g_diag_skip_latch));
    g_diag_skip_latch_head = 0;
    memset(s_jetsam_state_cache, 0, sizeof(s_jetsam_state_cache));
    s_jetsam_state_head = 0;
    pthread_mutex_unlock(&s_diag_mutex);
}

/* 通用去重（调用方须持 s_diag_mutex）：命中返回 1（count++ 且 dropped++），新键返回 0 并写表。 */
static int diag_dedup_check_locked(const char *key) {
    if (!key || !*key) return 0;
    for (int i = 0; i < DIAG_DEDUP_SLOTS; i++) {
        if (g_diag_dedup[i].key[0] && strcmp(g_diag_dedup[i].key, key) == 0) {
            g_diag_dedup[i].count++;
            atomic_fetch_add_explicit(&g_diag_latch_suppressed, 1, memory_order_relaxed);
            return 1;
        }
    }
    unsigned slot = g_diag_dedup_head++ % DIAG_DEDUP_SLOTS;
    snprintf(g_diag_dedup[slot].key, sizeof(g_diag_dedup[slot].key), "%s", key);
    g_diag_dedup[slot].first_mono = time(NULL);
    g_diag_dedup[slot].count = 1;
    return 0;
}

/* 三道闸：off → 等级 → 事件数 → 字节数。格式化一次，锁内原子扣预算，锁外落盘。 */
static void diag_log_emit(int minlv, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void diag_log_emit(int minlv, const char *fmt, ...) {
    int lv = diag_level_now();
    if (lv == DIAG_OFF || lv < minlv) return;

    char scratch[DIAG_LINE_BUDGET];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(scratch, sizeof(scratch), fmt, ap);
    va_end(ap);
    if (n < 0) return;

    unsigned long bytes = (unsigned long)n + 24UL; /* 时间戳+换行 */
    int allowed = 0;
    pthread_mutex_lock(&s_diag_mutex);
    lv = diag_level_now();
    if (lv != DIAG_OFF && lv >= minlv) {
        if (g_diag_events >= diag_level_cap(lv)) {
            atomic_fetch_add_explicit(&g_diag_budget_dropped, 1, memory_order_relaxed);
        } else if (g_diag_bytes + bytes > DIAG_BYTE_CAP) {
            atomic_fetch_add_explicit(&g_diag_budget_dropped, 1, memory_order_relaxed);
        } else {
            g_diag_events++;
            g_diag_bytes += bytes;
            allowed = 1;
        }
    }
    pthread_mutex_unlock(&s_diag_mutex);

    if (allowed) {
        jp_log_write("%s", scratch);
    }
}

/* 唯一 DIAG 输出宏：off/低等级时短路，绝不求值可变参数；底层仍走 jp_log_write，与 g_loglevel 无关。 */
#define DIAGLOG(minlv, ...) do { \
    if (diag_level_now() >= (minlv)) { \
        diag_log_emit((minlv), __VA_ARGS__); \
    } \
} while (0)

static void diag_jetsam_state_note(pid_t pid, const char *bid, int32_t band,
                                   uint32_t state, unsigned long round_epoch) {
    if (diag_level_now() < DIAG_DETAILED) return;   /* off/basic 零开销 */
    int changed = 1;
    int latch = 0, budget = 0;

    pthread_mutex_lock(&s_diag_mutex);
    jetsam_state_slot_t *slot = NULL;
    for (int i = 0; i < JETSAM_STATE_CACHE_SLOTS; i++) {
        if (s_jetsam_state_cache[i].used && s_jetsam_state_cache[i].pid == pid) {
            slot = &s_jetsam_state_cache[i];
            break;
        }
    }
    if (!slot) {
        for (int i = 0; i < JETSAM_STATE_CACHE_SLOTS; i++) {
            if (!s_jetsam_state_cache[i].used) {
                slot = &s_jetsam_state_cache[i];
                break;
            }
        }
        /* 槽满时优先回收非本轮出现的陈旧槽（已退出进程），绝不盲淘本轮活跃进程 */
        if (!slot) {
            for (int i = 0; i < JETSAM_STATE_CACHE_SLOTS; i++) {
                unsigned idx = (s_jetsam_state_head + (unsigned)i) % JETSAM_STATE_CACHE_SLOTS;
                if (s_jetsam_state_cache[idx].seen_epoch != round_epoch) {
                    slot = &s_jetsam_state_cache[idx];
                    s_jetsam_state_head = idx + 1;
                    break;
                }
            }
        }
        if (!slot) {
            slot = &s_jetsam_state_cache[s_jetsam_state_head++ % JETSAM_STATE_CACHE_SLOTS];
        }
        slot->used = 1;
        slot->pid = pid;
    } else if (slot->band == band && slot->state == state) {
        changed = 0;
    }
    slot->band = band;
    slot->state = state;
    slot->seen_epoch = round_epoch;
    if (changed) {
        latch = diag_latch_now();
        budget = diag_budget_now();
    }
    pthread_mutex_unlock(&s_diag_mutex);

    if (!changed) return;

    char key[96], s_state[24];
    snprintf(key, sizeof(key), "%d|%d|%u", (int)pid, (int)band, (unsigned)state);
    snprintf(s_state, sizeof(s_state), "0x%x", (unsigned)state);
    DIAGLOG(DIAG_DETAILED,
            "DIAG event=JETSAM_STATE bid=%s pid=%d band=%d state=%s state_raw=0x%x dedup_key=%s diag_dropped=%d diag_latch=%d diag_budget=%d",
            (bid && bid[0]) ? bid : "-", (int)pid, (int)band, s_state, (unsigned)state, key,
            latch + budget, latch, budget);
}

static const char *proc_label_log(const char *path);  /* 前置声明：act_log_success 在其定义前调用 */
static pthread_mutex_t s_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static void jp_log_write(const char *fmt, ...) {
    pthread_mutex_lock(&s_log_mutex);
    /* INT1：日志路径先过安全前置校验——symlink/非普通文件/非 root 属主/
     * 组或其他可写时直接拒绝写入（也不轮转），绝不回退 fopen。 */
    if (jp_secure_precheck(LOG_PATH) != 0) {
        pthread_mutex_unlock(&s_log_mutex);
        return;
    }
    struct stat st;
    /* lstat：不跟随符号链接；仅当确认为普通文件且超限才轮转 */
    if (lstat(LOG_PATH, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > LOG_MAX_BYTES) {
        /* .1 若为符号链接，先摘掉链接本身（unlink 不跟随），再轮转，避免写穿链接目标 */
        struct stat old_st;
        if (lstat(LOG_OLD_PATH, &old_st) == 0 && S_ISLNK(old_st.st_mode)) {
            unlink(LOG_OLD_PATH);
        }
        rename(LOG_PATH, LOG_OLD_PATH);          /* 覆盖旧的 .1；rename 不跟随末段符号链接 */
    }
    /* INT1：O_APPEND + O_NOFOLLOW 打开，fstat 复核并 fchmod 回 0644（保留远端权限位） */
    FILE *f = NULL;
    int log_fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (log_fd >= 0) {
        if (jp_secure_fd_check(log_fd, 0644) == 0) {
            f = fdopen(log_fd, "a");
        }
        if (!f) close(log_fd);
    }
    if (!f) {
        pthread_mutex_unlock(&s_log_mutex);
        return;
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
    int32_t target;   /* 目标档位：protect 为 BAND_BACKGROUND..BAND_MAX_PROTECT，demote 为 BAND_IDLE..BAND_BACKGROUND */
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
 * 避免把系统给的前台档（BAND_FOREGROUND）等档位误降 */
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
    time_t  band_seen_mono;     /* 最近一次由我方写入 band 的时刻（归属判定用） */
    time_t  orig_seen_mono;     /* 首次记录 orig 的时刻 */
    time_t  sys_band_seen_mono; /* 最近一次观察到 sys_band 的时刻 */
    int     claim_origin;       /* 1 = 旧实例 target 认领；无 E1 身份缓存但来源可信 */
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

/* =========================================================================
 * T3-IMP 3.3: 快照新鲜度 / 现读兜底（I3）
 * 所有纯内存时间戳用 CLOCK_MONOTONIC；TTL 内可用，超时/缺失视为未知。
 * ========================================================================= */
static time_t applied_mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static int jp_snapshot_fresh(time_t seen_mono, time_t now_mono) {
    return seen_mono > 0 && now_mono >= seen_mono &&
           (now_mono - seen_mono) <= JP_SNAPSHOT_TTL_SECS;
}

/* 定向现读当前有效档位与内核状态位（XNU memorystatus_get_priority_pid 单 pid 分支，O(1)）；
 * 严格校验返回字节数 == sizeof(entry) 且 entry.pid == pid，杜绝未写入缓冲区假阳性；
 * 失败返回 -1 并保留内核原始 errno（进程退出为 ESRCH，协议不符置 EPROTO）。 */
static int jp_read_current_entry(pid_t pid, int32_t *band_out, uint32_t *state_out) {
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
    if (state_out) *state_out = entry.state;
    return 0;
}

static int jp_read_current_band(pid_t pid, int32_t *band_out) {
    return jp_read_current_entry(pid, band_out, NULL);
}

/* =========================================================================
 * T3-IMP: 前后台 / kill 判定的唯一权威入口（模块调用点禁止档位字面量）
 * 阈值语义：前台保护环用 BAND_FOREGROUND，重启/bgkill 的"可操作"快筛用
 * BAND_PROTECT_DEFAULT，kill 允许条件为 natural_band <= BAND_BACKGROUND
 * 或经实时 state 确认为 XNU 冷冻室闲置冻结态 (BAND_FREEZER=75, Suspended|Frozen, 无 Assertion/Active)。
 * ========================================================================= */
static int jp_is_foreground(int32_t band) {
    return band >= BAND_FOREGROUND;
}

static int jp_is_candidate(int32_t band) {
    return band >= BAND_IDLE && band <= BAND_MAX_PROTECT;
}

/* XNU 冷冻室闲置态判定（真机 121 进程全集实测核实）：
 * 仅当自然档位 == BAND_FREEZER(75)、持有实时读取的 state（state_known!=0）、
 * 同时置位 Suspended(0x1)|Frozen(0x2) 且无 Assertion(0x40)|Active(0x80) 时放行；
 * 缺状态或带活跃断言一律返回 0（fail-closed）。 */
static int jp_is_frozen_idle(int32_t natural_band, uint32_t state, int state_known) {
    if (!state_known || natural_band != BAND_FREEZER) return 0;
    const uint32_t req_mask = JP_MEMSTAT_SUSPENDED | JP_MEMSTAT_FROZEN;
    const uint32_t forbid_mask = JP_MEMSTAT_ASSERTION | JP_MEMSTAT_ACTIVE;
    return ((state & req_mask) == req_mask) && ((state & forbid_mask) == 0);
}

static int jp_kill_allowed_with_state(int32_t natural_band, uint32_t state, int state_known) {
    if (natural_band <= BAND_BACKGROUND) return 1;
    return jp_is_frozen_idle(natural_band, state, state_known);
}

static inline int jp_kill_allowed(int32_t natural_band) {
    return jp_kill_allowed_with_state(natural_band, 0, 0);
}

/* 归属判定可信条件：有明确来源（E1 身份缓存或旧实例 CLAIM）
 * 且本次 band 观察在 TTL 内；禁止仅凭数值相等推断"档位是我们设的"。
 * v0.8.6 A1: jp_natural_band 在其定义之前需要调用，故前置声明。 */
static int jp_attribution_trusted(const applied_t *a, time_t now_mono);

/* 自然档位回退链 sys_band -> orig -> cur（restart 与 bgkill 共用）。
 * 3.3: 每段快照过 TTL 才可用；过期/缺失时现读 cmd3 兜底，
 * 现读失败才退回手头 cur（保守：cur 是我方 protect 值时门禁更严）。 */
static int32_t jp_natural_band(pid_t pid, int32_t cur_now) {
    time_t now = applied_mono_now();
    applied_t *a = applied_find(pid);
    if (a && a->sys_band_known && jp_snapshot_fresh(a->sys_band_seen_mono, now))
        return a->sys_band;
    /* v0.8.6 A1（安全侧 S2/S3）：保护档长期稳定 >240s 时 applied_observe_band
     * 只续 band_seen_mono、不续 orig_seen_mono，旧链会掉到 read-current-band
     * 读回我方保护档。归属可信 + cur==band + orig 已知时直接取 orig，绕过 TTL；
     * fresh sys_band 仍优先，jp_kill_allowed 门槛与 applied 生命周期均不改。 */
    if (a && cur_now == a->band && a->orig_known &&
        jp_attribution_trusted(a, now))
        return a->orig;
    if (a && a->orig_known && jp_snapshot_fresh(a->orig_seen_mono, now))
        return a->orig;
    int32_t live = cur_now;
    if (jp_read_current_band(pid, &live) == 0) return live;
    return cur_now;
}

/* 记一次 sys_band 观察并刷新时间戳（快轮/慢轮共用） */
static void applied_note_sys_band(applied_t *a, int32_t cur) {
    if (!a) return;
    a->sys_band = cur;
    a->sys_band_known = 1;
    a->sys_band_seen_mono = applied_mono_now();
}

/* 档位只读观察：仅在系统把档位漂移离己方设定值 (cur != band) 时记 sys_band，
 * 避免把自己设的 protect/demote 档位污染进自然档位门控；仍停留在己方设定档
 * (cur == band) 时仅在 TTL 内续期 band_seen_mono，既防稳态 >240s 后 LOWER/RETURN
 * 失灵，又保留休眠/停顿超时后的归属保护。 */
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

/* 归属判定可信条件：有明确来源（E1 身份缓存或旧实例 CLAIM）
 * 且本次 band 观察在 TTL 内；禁止仅凭数值相等推断"档位是我们设的"。 */
static int jp_attribution_trusted(const applied_t *a, time_t now_mono) {
    if (!a) return 0;
    if (!a->has_identity && !a->claim_origin) return 0;
    return jp_snapshot_fresh(a->band_seen_mono, now_mono);
}

/* applied_set_ex:
 * 语义说明：orig 是我们第一次改这个 pid 之前那一刻的档位，不代表「如果没有我们，它现在会是多少」。
 * 只有新建条目时才写 orig = cur_before, orig_known = 1；已有条目只更新 band。
 */
static void applied_set_ex(pid_t pid, int32_t band, int32_t cur_before) {
    applied_t *a = applied_find(pid);
    time_t now = applied_mono_now();
    if (!a) {
        if (g_napplied >= MAX_APPLIED) return;  /* 表满就不记，只是失去降档能力 */
        a = &g_applied[g_napplied++];
        a->pid = pid;
        a->orig = cur_before;
        a->orig_known = 1;
        a->orig_seen_mono = now;
        a->sys_band = 0;
        a->sys_band_known = 0;
        a->sys_band_seen_mono = now;
        a->claim_origin = 0;
        a->has_identity = 0;   /* E1: 身份缓存由慢轮调用点随后填充 */
        g_applied_creates++;   /* E2: 空转判定（仅计数，不改既有语义） */
    }
    a->band = band;
    a->band_seen_mono = now;   /* T3-IMP 3.3: 归属判定新鲜度 */
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
        a->has_identity = 0;   /* E1: 同上 */
        g_applied_creates++;   /* E2: 同上 */
    }
    a->band = band;
    a->band_seen_mono = now;
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
            if (n < 3 || band <= 0) band = BAND_PROTECT_DEFAULT; /* 默认保护档 */
            if (band < BAND_BACKGROUND) band = BAND_BACKGROUND;   /* 与 App 滑杆最小值一致 */
            if (band > BAND_MAX_PROTECT) band = BAND_MAX_PROTECT;
            strncpy(g_protect[g_nprotect].frag, frag, 255);
            g_protect[g_nprotect].target = band;
            g_protect[g_nprotect].is_bid = is_bid;
            if (is_bid) g_has_bid_rules = 1;
            g_nprotect++;
        } else if (strcmp(verb, "demote") == 0 && g_ndemote < MAX_RULES) {
            if (n < 3) band = BAND_BACKGROUND;                    /* 默认后台档 */
            if (band < BAND_IDLE) band = BAND_IDLE;               /* 夹到自然后台区间 */
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

/* status 按需写（E3）：内容变化才写盘；bands.req 在手时 force 强制刷新。
 * 原 60s 心跳已删——App 判活靠 daemon.pid 的 kill(pid,0)，不依赖 status.txt mtime 新鲜度；
 * 守护进程首轮 body 必与 g_last_body(NULL) 不同 → 必写一次，满足「开机后写过至少一次」判据 */

/* =========================================================================
 * M1 原子写统一：状态文件统一提交助手
 * ========================================================================= */
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif

/* M1: 提交 tmp 内容到 final——fflush → ferror → fsync(数据) → fclose →
 * rename → fsync(CONFIG_DIR 目录)。rename 前数据已落盘、rename 后目录项
 * 已落盘，崩溃/掉电不会再出现「旧文件被空/截断文件替换」；任一步失败清
 * 理 tmp 并返回 -1，旧版本目标文件保持完整。所有状态文件都在 CONFIG_DIR，
 * 目录 fsync 只对该目录做一次。 */
static int jp_atomic_commit(FILE *f, const char *tmp_path, const char *final_path) {
    if (!f || !tmp_path || !final_path) {
        if (f) fclose(f);
        if (tmp_path) unlink(tmp_path);
        return -1;
    }
    if (fflush(f) != 0 || ferror(f)) {
        fclose(f);
        unlink(tmp_path);
        return -1;
    }
    if (fsync(fileno(f)) != 0) {
        fclose(f);
        unlink(tmp_path);
        return -1;
    }
    fclose(f);
    if (rename(tmp_path, final_path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    int dfd = open(CONFIG_DIR, O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        (void)fsync(dfd);
        close(dfd);
    }
    /* 目录 fsync 尽力而为：rename 已提交不可回滚，目录 fsync 失败不改变
     * 返回值（数据 fsync + rename 已保证崩溃安全的目标文件内容） */
    return 0;
}

static char  *g_last_body = NULL;   /* 上次成功写盘的内容（不含 updated= 行） */
static size_t g_last_body_len = 0;
static int    g_restart_cfg_bad_count = 0; /* restart.txt 无效行数 (v0.7.8 C1-N6) */
static void pulse_write_status(FILE *m);
static void bgkill_write_status(FILE *m);

/* E3: force=1 时即使内容没变也写盘（慢轮 bands.req 到达时传入） */
static void write_status(int protected_now, int demoted_pids,
                         protected_entry_t *plist, int nplist,
                         protected_entry_t *dlist, int ndlist, int force) {
    /* 1. 先在内存里拼出除 updated= 以外的全部内容 */
    char *body = NULL; size_t blen = 0;
    FILE *m = open_memstream(&body, &blen);
    if (!m) return;
    fprintf(m, "bid_support=1\n"
               "restart_support=1\n"
               "version=" JP_VERSION "\n"
               "commit=" JP_COMMIT "\n"
               "mempressure_state=retired\n"
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
    bgkill_write_status(m);
    int merr = ferror(m);
    fclose(m);                         /* fclose 后 body/blen 才有效 */
    if (merr || !body) { free(body); return; }

    /* 2. 内容没变且不强制 → 不写盘 (E3) */
    int same = g_last_body && blen == g_last_body_len &&
               memcmp(body, g_last_body, blen) == 0;
    if (same && !force) { free(body); return; }

    /* 3. 写盘：O_NOFOLLOW 打开 tmp，走 jp_atomic_commit 统一原子替换（M1：
     * fsync 数据 → rename → fsync 目录），App 永远读不到写一半的 status.txt */
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", STATUS_PATH);
    /* INT1：root 写路径安全加固——先 lstat 校验，再 O_NOFOLLOW 打开，最后 fstat
     * 复核并 fchmod 回 0644；异常一律拒绝写入，不回退 fopen。 */
    if (jp_secure_precheck(tmp) != 0) { free(body); return; }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) { free(body); return; }
    if (jp_secure_fd_check(fd, 0644) != 0) { close(fd); unlink(tmp); free(body); return; }
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp); free(body); return; }
    time_t t = time(NULL);
    struct tm tm_val;
    localtime_r(&t, &tm_val);
    char ts[64]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_val);
    fprintf(f, "updated=%s\n", ts);
    fwrite(body, 1, blen, f);
    /* ferror 检查、fsync、fclose、rename、目录 fsync 统一交给 jp_atomic_commit；
     * 失败就不转正，保留旧 status.txt */
    if (jp_atomic_commit(f, tmp, STATUS_PATH) != 0) { free(body); return; }
    /* 4. 成功才记账；失败下轮重试 */
    free(g_last_body);
    g_last_body = body; g_last_body_len = blen;
}

static volatile sig_atomic_t g_locked = 0;
static int g_lock_token = -1;
static int g_lock_fd = -1;

/* v0.8.7 APPMON: BKSApplicationStateMonitor 事件环与自唤醒管道（严格叶锁 s_appmon_mutex） */
typedef struct {
    char     bid[128];
    pid_t    pid;
    uint32_t state;
    time_t   mono;
} appmon_event_t;

static pthread_mutex_t s_appmon_mutex = PTHREAD_MUTEX_INITIALIZER;
static appmon_event_t  s_appmon_ring[APPMON_RING_CAP];
static unsigned        s_appmon_head = 0;
static unsigned        s_appmon_count = 0;
static int             s_appmon_pipe_pending = 0;
static unsigned long   s_appmon_dropped = 0;
static int             g_appmon_pipe[2] = { -1, -1 };
static int             g_appmon_active = 0;

static void appmon_drain_pipe(void) {
    pthread_mutex_lock(&s_appmon_mutex);
    if (g_appmon_pipe[0] >= 0) {
        char buf[64];
        while (read(g_appmon_pipe[0], buf, sizeof(buf)) > 0) {}
    }
    s_appmon_pipe_pending = 0;
    pthread_mutex_unlock(&s_appmon_mutex);
}

/* 规则：lock_refresh() 只允许在 lockmon_init() 和 wait_events_ms() 里调用，
 * 别处一律不得调用。其余代码，不论哪个线程，都只读 g_locked。 */
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

/* 多路事件睡眠（毫秒精度）：同时监听锁屏状态变化 fd 与 APPMON 前后台推送管道。
 * 任一事件到达立即唤醒；APPMON 唤醒后做 80ms 微聚合，待 runningboardd 完成
 * XNU Jetsam 档位切换再返回主循环，兼顾毫秒响应与单次到位。 */
static void wait_events_ms(int timeout_ms, int *lock_changed_out, int *appmon_woke_out) {
    if (lock_changed_out) *lock_changed_out = 0;
    if (appmon_woke_out) *appmon_woke_out = 0;
    if (g_got_sigterm || timeout_ms <= 0) return;

    int lfd = g_lock_fd;
    int afd = g_appmon_pipe[0];
    if (lfd < 0 && afd < 0) {
        int rem_ms = timeout_ms;
        while (rem_ms > 0 && !g_got_sigterm) {
            int step_ms = (rem_ms > 1000) ? 1000 : rem_ms;
            usleep((useconds_t)step_ms * 1000U);
            rem_ms -= step_ms;
        }
        return;
    }

    fd_set rf;
    FD_ZERO(&rf);
    int max_fd = -1;
    if (lfd >= 0) {
        FD_SET(lfd, &rf);
        if (lfd > max_fd) max_fd = lfd;
    }
    if (afd >= 0) {
        FD_SET(afd, &rf);
        if (afd > max_fd) max_fd = afd;
    }
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    if (select(max_fd + 1, &rf, NULL, NULL, &tv) > 0) {
        int lc = 0;
        if (lfd >= 0 && FD_ISSET(lfd, &rf)) {
            int tok;
            while (read(lfd, &tok, sizeof(tok)) == sizeof(tok)) {
                fd_set r2; FD_ZERO(&r2); FD_SET(lfd, &r2);
                struct timeval z = {0, 0};
                if (select(lfd + 1, &r2, NULL, NULL, &z) <= 0) break;
            }
            int was = g_locked;
            lock_refresh();
            lc = (was != g_locked);
            if (lock_changed_out) *lock_changed_out = lc;
        }
        if (afd >= 0 && FD_ISSET(afd, &rf)) {
            appmon_drain_pipe();
            if (appmon_woke_out) *appmon_woke_out = 1;
            if (!g_got_sigterm && !lc) {
                usleep(80 * 1000);
                appmon_drain_pipe();
            }
        }
    }
}

/* =========================================================================
 * 内存压力触发与启动收尾模块 (v0.7.0; 路线 E 已停用，0.8.0 仅保留收尾与 memcap)
 * ========================================================================= */

#define MEMP_PENDING_PATH  CONFIG_DIR "/mempressure_pending"
#define MEMP_STATE_PATH    CONFIG_DIR "/mempressure_state.txt"

static void mempressure_write_state(void);

static uint64_t g_memp_memcap = 2048;
static int g_memp_memcap_fallback = 0;

/* =========================================================================
 * 超内存自动重启规则模块 (v0.7.7 C1)
 * ========================================================================= */
#define RESTART_CONFIG_PATH CONFIG_DIR "/restart.txt"
#define RESTART_MAX_RULES   32
#define RESTART_MIN_THR_MB  50
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

/* H2 规则数组撕裂读：g_restart_rules / g_pulse_rules / g_bgkill_rules 的
 * 配置加载 memcpy 与 worker 可达读点互斥。
 * 锁序说明：s_rule_lock 与 s_pulse_mutex 从不嵌套持有——worker 拿 rd 锁的
 * 代码段内不含 s_pulse_mutex，主线程 load_config 只持 wr 锁不碰 s_pulse_mutex。
 * 新代码请保持两锁互不嵌套，勿在其中一把的临界区内再取另一把。 */
static pthread_rwlock_t s_rule_lock = PTHREAD_RWLOCK_INITIALIZER;

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
#define PULSE_MIN_TARGET_MB   100
#define PULSE_MAX_TARGET_MB   1500
#define PULSE_MIN_N_MIN       0
#define PULSE_MAX_N_MIN       60
#define PULSE_MIN_DAYMAX      1
#define PULSE_MAX_DAYMAX      100
#define PULSE_MIN_DELAY_S     0
#define PULSE_MAX_DELAY_S     20
#define PULSE_DEFAULT_DELAY_S 10
#define PULSE_DEADLINE_BG_SEC 25
#define PULSE_REASSERT_MAX_SEC 30 /* 卡 P-2: 窗口前 30 秒内允许重设限额一次 */
/* D-003: 门控打开时快轮的唤醒上限，须显著小于 25s 死线，保证窗口内至少多次判定 */
#define PULSE_FAST_GATE_POLL_SEC 5
/* XNU 黄金警告带越界前保护比例：L=ceil(fp/0.85) 使起始占用落在 ~85%×L，
 * 涨至 95%×L 即提前恢复，绝不触碰 100%×L 引发 highwater 误杀 */
#define PULSE_GUARD_PCT      95
/* P1-1: 有启用 pulse 规则时慢轮周期上限，保证首次进后台 <=20s 被观测 */
#define PULSE_FIRST_OBSERVE_MAX_SLOW 20
#define PULSE_GLOBAL_DAYMAX   100
#define PULSE_DEFAULT_WECHAT_TARGET 550

typedef struct {
    char    bid[128];
    int     enabled;        /* 1: on, 0: off */
    int32_t target_mb;      /* 100..1500 */
    int32_t n_min;          /* 0..60 */
    int32_t daymax;         /* 1..100 */
    int32_t delay_s;        /* 0..20 秒 (默认 10)；0 = 一进后台就判定 */
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
    /* P2: 显式格式魔数；self_written 只在魔数匹配且值为 0/1 时才可信。
     * self_written=1 表示本进程写限额前的快照；身份校验通过须原样恢复，
     * 0 / 魔数缺失 / 非法值一律走旧合理性判定（fail closed）。 */
    uint32_t fmt_magic;
#define PULSE_STATE_FMT_MAGIC 0x4A505331U  /* "JPS1" */
    int      self_written;
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
/* DIAG2: diag.txt 变更戳（复用 jp_file_stat_changed 的 ino+size+mtime 去重） */
static jp_file_stamp_t s_diag_cfg_stamp = {0};
static int g_diag_cfg_file_absent = 1;

/* =========================================================================
 * 后台定时清理规则模块 (v0.8.0 卡 K)
 * ========================================================================= */
#define BGKILL_CONFIG_PATH   CONFIG_DIR "/bgkill.txt"
#define BGKILL_MAX_RULES     32
#define BGKILL_MIN_MIN       0
#define BGKILL_MAX_MIN       720
#define BGKILL_MIN_MAX       1
#define BGKILL_MAX_MAX       30
#define BGKILL_DEFAULT_MIN   60
#define BGKILL_DEFAULT_MAX   5
/* 系统 dasd 应用预热空壳识别兜底上限：仅对「自诞生从未进过前台 (ever_fg=0)」的进程生效 */
#define BGKILL_PREWARM_MAX_FP_MB 40

typedef struct {
    char    bid[128];
    int     enabled;
    int32_t min;
    int32_t max;
} bgkill_rule_t;

typedef struct {
    char   bid[128];
    pid_t  tracked_pid;
    int    in_bg;
    time_t bg_start_mono;
    int    bg_rounds;
    int    ever_fg;        /* 1: 当前 tracked_pid 曾观测到处于前台 (band>=100) */
    int    prewarm_hold;   /* 1: 诞生于后台且初始 fp<40MB，按系统预热空壳暂缓清理 */
    int    prewarm_logged; /* 1: 当前 tracked_pid 已输出过单次 prewarm_skip 日志 */
    int    today_count;
    int    today_y;
    int    today_m;
    int    today_d;
    char   last_kill_hhmm[8];
    time_t last_skip_mono;
    char   last_skip_reason[16];
} bgkill_rule_state_t;

static bgkill_rule_t g_bgkill_rules[BGKILL_MAX_RULES];
static int g_nbgkill_rules = 0;
static int g_bgkill_master = 1;
/* T3-IMP 3.2: bgkill 自然档安全门槛开关（bgkill.txt 的 bandgate=0 可关闭，
 * 无需重编包即回滚新门禁；默认 1 = 开启，与 restart 的 <= BAND_BACKGROUND 契约一致） */
static int g_bgkill_bandgate = 1;
static int g_bgkill_first_load = 1;
static int g_bgkill_cfg_file_absent = 1;
static int g_bgkill_cfg_bad_count = 0;
static jp_file_stamp_t s_bgkill_cfg_stamp = {0};

static bgkill_rule_state_t g_bgkill_rule_states[BGKILL_MAX_RULES];
static int g_nbgkill_rule_states = 0;

static int g_memp_level_first_logged = 0;

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

static void mempressure_write_state(void) {
    const char *line = "state=retired fails=0 last_errno=0 reason=- tainted=0\n";
    static char s_last_line[256] = "";
    if (strcmp(line, s_last_line) == 0) return; /* 内容无变化不写 */

    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", MEMP_STATE_PATH);
    /* M1: O_NOFOLLOW + 统一原子提交（原 fopen("w") 缺 O_NOFOLLOW、rename 前缺 fsync） */
    /* INT1：lstat/fstat 双重 root 属主+权限校验，异常拒绝写入，不回退 fopen；0644 保持原权限位 */
    if (jp_secure_precheck(tmp) != 0) return;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return;
    if (jp_secure_fd_check(fd, 0644) != 0) { close(fd); unlink(tmp); return; }
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp); return; }
    fputs(line, f);
    if (jp_atomic_commit(f, tmp, MEMP_STATE_PATH) != 0) return;
    strncpy(s_last_line, line, sizeof(s_last_line) - 1);
    s_last_line[sizeof(s_last_line) - 1] = '\0';
}

static void memp_check_boot_reset(void) {
    int has_pending = (access(MEMP_PENDING_PATH, F_OK) == 0);
    int has_state_reset = 0;

    FILE *sf = fopen(MEMP_STATE_PATH, "r");
    if (sf) {
        char sline[256];
        if (fgets(sline, sizeof(sline), sf)) {
            if (strstr(sline, "reset_pending_normal") != NULL) {
                has_state_reset = 1;
            }
        }
        fclose(sf);
    }

    if (has_pending || has_state_reset) {
        int v = (TEST_LOW_MEMORY_TRIGGER_ONE << 16) | DISPATCH_MEMORYPRESSURE_NORMAL;
        int rc = sysctlbyname("kern.memorypressure_manual_trigger", NULL, NULL, &v, sizeof(int));
        int err = (rc != 0) ? errno : 0;
        if (rc == 0) {
            unlink(MEMP_PENDING_PATH);
            JPLOG(JP_INFO, "MEMP_RETIRED_CLEANUP ok");
        } else {
            JPLOG(JP_WARN, "MEMP_RETIRED_CLEANUP fail errno=%d", err);
        }
    }

    /* 收尾完成后，把 mempressure_state.txt 改写成 state=retired */
    mempressure_write_state();

    /* 收尾完成后调用一次 memp_read_level（读 kern.memorystatus_vm_pressure_level，期望值 1 = NORMAL） */
    int cur_lvl = 0, lerr = 0;
    int lrc = memp_read_level(&cur_lvl, &lerr);
    if (lrc == 0) {
        JPLOG(JP_INFO, "MEMP_RETIRED level=%d", cur_lvl);
    } else {
        JPLOG(JP_INFO, "MEMP_RETIRED level=NA");
    }
}

static void mempressure_init(void) {
    mempressure_init_memcap();
    memp_check_boot_reset();
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

/* =========================================================================
 * E1 快慢双节奏 / E2 自适应退避 / E4 定向查询兜底 (docs/optimization-design.md)
 * ========================================================================= */
/* E2: 退避常量集中定义（亮屏 5→60s；慢轮周期 30→240s；锁屏 10/60s 独立、不参与退避） */
#define BACKOFF_BASE_SECS 5
#define BACKOFF_MAX_SECS  60
#define SLOW_BASE_SECS    30
#define SLOW_MAX_SECS     JP_SNAPSHOT_TTL_SECS

/* E4: 快轮模式（三级兜底链）
 *   PID1     GET_PRIORITY_LIST(1, pid>0) 单 pid 定向查有效档位（XNU memorystatus_get_priority_pid）
 *   LISTSCAN get_list() 全表取回后只遍历已知 pid（省 proc_pidpath，不省全表 syscall）
 *   SLEEP    快轮纯休眠，重断言交给慢轮（正确性不丢、只牺牲响应度）
 *   LEGACY   最坏回退现状：每轮全表扫（慢轮连续 3 次取不到全表时进入） */
#define FAST_MODE_PID1     0
#define FAST_MODE_CMD3     FAST_MODE_PID1
#define FAST_MODE_LISTSCAN 1
#define FAST_MODE_SLEEP    2
#define FAST_MODE_LEGACY   3

static int          g_fast_mode       = FAST_MODE_PID1;
static int          g_fast_mode_env_forced = 0; /* JP_FAST_SCAN=legacy 用户显式回退，排除在自动恢复外 */
static time_t       g_slow_due_mono   = 0;              /* 下次慢轮到期（CLOCK_MONOTONIC 秒），0=首轮 */
static int          g_slow_period     = SLOW_BASE_SECS; /* 当前慢轮周期（30→240，E2 退避） */
static int          g_idle_rounds     = 0;              /* 连续空转轮数（翻倍指数） */
static int          g_interval        = BACKOFF_BASE_SECS; /* 亮屏休眠间隔（5→60，E2 退避） */
static int          g_last_protected_now = -1;          /* 上个慢轮 protected_now 基准（空转判定） */
static int          g_slow_list_fail_streak = 0;        /* 慢轮 get_list 连续失败计数（LEGACY 触发） */
static int          g_fast_pulse_list_fail_streak = 0;   /* D-003: 门控快轮为 pulse 取全表连续失败计数 */
static jp_file_stamp_t s_config_stamp = {0};            /* config.txt 变更戳（E1 慢轮判定） */

static time_t mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

/* E4: 启动探测单 pid 定向档位查询可用性（GET_PRIORITY_LIST, pid>0），决定快轮初始模式。
 * 运行时注入开关（真机验证/降级演练用）：
 *   JP_E4_PROBE_FAIL=1  模拟单 pid 查询不可用 → 快轮降级 LISTSCAN（验证 0→1）
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
 *   前台档位（BAND_FOREGROUND 及以上）一律不碰，交慢轮。
 * 返回 1 表示发生了一次重断言（E2 空转判定用）。 */
static int fast_reassert_one(applied_t *a, int32_t cur) {
    if (jp_is_foreground(cur)) {
        if (cur != a->band) applied_note_sys_band(a, cur);
        return 0;
    }
    int do_reassert = (a->kind == 0) ? (cur < a->target) : (cur > a->target);
    if (!do_reassert) {
        /* 不重断言：只读观察档位（漂移记 sys_band，稳态续期 band_seen_mono） */
        applied_observe_band(a, cur);
        return 0;
    }
    if (set_priority(a->pid, a->target) == 0) {
        g_reassert_count++;
        applied_note_sys_band(a, cur);   /* P3-1: 动作前记漂移观察，与慢轮语义对齐 */
        applied_set_ex(a->pid, a->target, cur);  /* 已有条目只更新 band，身份缓存保留 */
        act_log_success((a->kind == 0) ? ACT_KIND_RAISE : ACT_KIND_DEMOTE,
                        a->pid, a->path, a->frag, cur, a->target);
        return 1;
    }
    return 0;
}

/* E4/E1 快轮 · 模式 0：GET_PRIORITY_LIST(1, pid>0) 逐已知 pid 定向查有效档位（O(已知数)，不取全表）。
 * 返回本轮重断言次数；-1 表示中途异常降级（本轮未 prune，防误剪存活条目）。 */
static int fast_round_pid1(void) {
    int actions = 0;
    for (int i = 0; i < g_napplied; i++) {
        applied_t *a = &g_applied[i];
        int32_t cur = -1;
        if (jp_read_current_band(a->pid, &cur) != 0) {
            if (errno == ESRCH) continue;   /* 进程退出：不置 seen，applied_prune 剪除 */
            /* E4 运行中异常失败（非 ESRCH）：整体降级到列表扫描模式。
             * P2（代码审核）：return 前清掉本轮已置的 seen 残留——本轮部分条目已
             * 成功置 seen=1，带着陈旧 seen 进下一轮 LISTSCAN 时，轮间退出的 pid
             * 会被 prune 多保留一轮（listscan 对不在全表的条目不清 seen）。
             * 清零后下一轮 LISTSCAN 从全表重新推导存活状态，死条目当轮剪除。
             * 本轮不 prune：防误剪存活进程（重断言动作不回滚，仅丢弃存活观察）。 */
            for (int k = 0; k < g_napplied; k++) g_applied[k].seen = 0;
            JPLOG(JP_WARN, "%-6s pid1_fail pid=%d errno=%d -> fast=list-scan fallback",
                   "E4", (int)a->pid, errno);
            g_fast_mode = FAST_MODE_LISTSCAN;
            return -1;
        }
        a->seen = 1;
        if (!a->has_identity) {
            /* 无身份缓存（含旧实例 CLAIM）：只读观察不重断言，避免空闲退避 >240s 令归属过期 */
            applied_observe_band(a, cur);
            continue;
        }
        actions += fast_reassert_one(a, cur);
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

/* E1 快轮分发：按 g_fast_mode 选择实现；SLEEP 模式纯休眠（无动作、不 prune，
 * 退出条目交慢轮剪除）。 */
static int fast_round(void) {
    switch (g_fast_mode) {
    case FAST_MODE_PID1:     return fast_round_pid1();
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

/* DIAG2: 小时级统计汇总行。不经 DIAGLOG（不占用事件预算），仅按等级 gate；
 * 落盘仍走 jp_log_write，共享 daemon.log 轮转。 */
static void diag_stats_emit(void) {
    int lv = diag_level_now();
    if (lv == DIAG_OFF) return;
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    char day[16], hh[8];
    strftime(day, sizeof(day), "%Y-%m-%d", &lt);
    snprintf(hh, sizeof(hh), "%02d", lt.tm_hour);
    pthread_mutex_lock(&s_diag_mutex);
    lv = diag_level_now();
    if (lv == DIAG_OFF) {
        pthread_mutex_unlock(&s_diag_mutex);
        return;
    }
    if (g_diag_day[0] == '\0' || strcmp(g_diag_day, day) != 0) {
        snprintf(g_diag_day, sizeof(g_diag_day), "%s", day);
    }
    int events = g_diag_events;
    unsigned long bytes = g_diag_bytes;
    int dropped = diag_dropped_now();
    pthread_mutex_unlock(&s_diag_mutex);
    jp_log_write("DIAG event=DIAG_STATS level=%s events=%d bytes=%lu dropped=%d diag_latch=%d diag_budget=%d day=%s hh=%s",
                 diag_level_to_str(lv), events, bytes, dropped, diag_latch_now(), diag_budget_now(), day, hh);
}

/* DIAG2: 感知 diag.txt 变更并解析等级。
 * - 复用 jp_file_stat_changed 的 ino+size+mtime 去重，未变时零开销、零日志；
 * - fopen 不带 O_NOFOLLOW，故 fopen 前必须先 jp_diag_config_precheck；
 * - 严格单行 level=off|basic|detailed|full；文件缺失/超长/多键/未知值 → off；
 * - daemon 只读不写，绝不回写默认值；仅在解析结果变化时输出 PLS_CFG。 */

/* FIX1: diag.txt 由 App(mobile) 原子写创建，不能套用守护进程状态文件的 root
 * 属主规则。专用只读校验与 jp_secure_precheck 等价但去掉 uid==0：CONFIG_DIR
 * 仍须真实目录且非符号链接；目标若存在须非符号链接、S_ISREG、组/其他不可写
 * (& 0022)；ENOENT 允许（off）；其余 lstat 错误一律拒绝。不回写、不 fchmod，
 * 且不改动 jp_secure_precheck 本身及任何其他调用点。 */
static int jp_diag_config_precheck(const char *path) {
    struct stat dir_st;
    if (lstat(CONFIG_DIR, &dir_st) != 0 || S_ISLNK(dir_st.st_mode) ||
        !S_ISDIR(dir_st.st_mode)) {
        return -1;
    }

    struct stat st;
    if (lstat(path, &st) == 0) {
        if (S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode) ||
            (st.st_mode & 0022) != 0) {
            return -1;
        }
    } else if (errno != ENOENT) {
        return -1;
    }
    return 0;
}

static void diag_load_config(void) {
    int first = !s_diag_cfg_stamp.initialized;
    int absent = 0;
    int changed = jp_file_stat_changed(DIAG_CONFIG_PATH, &s_diag_cfg_stamp, &absent);
    g_diag_cfg_file_absent = absent;
    if (!changed) return;

    int new_level = DIAG_OFF;
    const char *reason = first ? "boot" : "file_change";
    const char *file_state = absent ? "absent" : "ok";

    if (absent) {
        new_level = DIAG_OFF;
        reason = "absent";
    } else {
        /* 符号链接 / 非普通文件 / 组或其他可写 → 拒绝解析（不再要求 root 属主），
         * 保持上一合法等级；stamp 已推进，下一轮不会重复打日志。 */
        if (jp_diag_config_precheck(DIAG_CONFIG_PATH) != 0) {
            int cur_lv = diag_level_now();
            DIAGLOG(DIAG_BASIC,
                    "DIAG event=PLS_CFG reason=diag_cfg_rejected from=%s to=%s file=ok diag_dropped=%d diag_latch=%d diag_budget=%d",
                    diag_level_to_str(cur_lv), diag_level_to_str(cur_lv), diag_dropped_now(), diag_latch_now(), diag_budget_now());
            return;
        }
        FILE *f = fopen(DIAG_CONFIG_PATH, "r");
        if (!f) {
            s_diag_cfg_stamp.initialized = 0;  /* 打开失败：下一轮重试 */
            return;
        }
        char line[96];
        int valid = 0;
        if (fgets(line, sizeof(line), f)) {
            size_t llen = strlen(line);
            while (llen > 0 && (line[llen - 1] == '\n' || line[llen - 1] == '\r')) {
                line[--llen] = '\0';
            }
            /* 超长行 / 文件里还有第二行 → 非法 */
            if (llen + 2 < sizeof(line) && fgetc(f) == EOF) {
                if (strncmp(line, "level=", 6) == 0) {
                    const char *v = line + 6;
                    if (strcmp(v, "off") == 0)      { new_level = DIAG_OFF; valid = 1; }
                    else if (strcmp(v, "basic") == 0)    { new_level = DIAG_BASIC; valid = 1; }
                    else if (strcmp(v, "detailed") == 0) { new_level = DIAG_DETAILED; valid = 1; }
                    else if (strcmp(v, "full") == 0)     { new_level = DIAG_FULL; valid = 1; }
                }
            }
        }
        fclose(f);
        if (!valid) {
            new_level = DIAG_OFF;
            reason = "invalid";  /* 非法值安全回落 off，不回写文件 */
        }
    }

    int old = diag_level_now();
    if (new_level != old) {
        atomic_store_explicit(&g_diag_level, new_level, memory_order_release);
        DIAGLOG(DIAG_BASIC,
                "DIAG event=PLS_CFG reason=%s from=%s to=%s file=%s diag_dropped=%d diag_latch=%d diag_budget=%d",
                reason, diag_level_to_str(old), diag_level_to_str(new_level),
                file_state, diag_dropped_now(), diag_latch_now(), diag_budget_now());
        diag_stats_emit();  /* 等级切换即时汇总，验证即时生效 */
    }
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
        /* H2: 整块替换规则数组必须在写锁内完成（worker 可能正在读） */
        pthread_rwlock_wrlock(&s_rule_lock);
        memcpy(g_restart_rules, parsed_rules, sizeof(restart_rule_t) * nparsed);
        g_nrestart_rules = nparsed;
        pthread_rwlock_unlock(&s_rule_lock);
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
    if (a->delay_s != b->delay_s) return 0;
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

/* H2: 按 bid 取 pulse 规则快照。worker 可达（pulse_worker_thread 内调用），
 * 数组访问必须在读锁内完成并整体拷贝到 out，锁外只用快照，
 * 杜绝与配置加载 memcpy 的撕裂读。找到返回 1，否则 0。 */
static int pulse_find_rule(const char *bid, pulse_rule_t *out) {
    if (!bid || bid[0] == '\0' || !out) return 0;
    pthread_rwlock_rdlock(&s_rule_lock);
    for (int i = 0; i < g_npulse_rules; i++) {
        if (strcmp(g_pulse_rules[i].bid, bid) == 0) {
            *out = g_pulse_rules[i];
            pthread_rwlock_unlock(&s_rule_lock);
            return 1;
        }
    }
    pthread_rwlock_unlock(&s_rule_lock);
    return 0;
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

            /* 格式: bid:<bid> on|off target=<MB> n=<分钟> daymax=<次> delay_s=<秒> (6 个 token) */
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
            int32_t val_target = 0, val_n = 0, val_daymax = 0, val_delay_s = 0;
            int is_legacy_delay = 0;
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
                } else if (strcmp(key, "delay_s") == 0) {
                    if (has_delay) { bad = 1; break; }
                    if (v < PULSE_MIN_DELAY_S || v > PULSE_MAX_DELAY_S) { bad = 1; break; }
                    val_delay_s = (int32_t)v;
                    has_delay = 1;
                } else if (strcmp(key, "delay") == 0) {
                    if (has_delay) { bad = 1; break; }
                    /* 旧的 delay= 照样能解析，但数值忽略，按 10 秒执行 (C2-T) */
                    val_delay_s = PULSE_DEFAULT_DELAY_S;
                    has_delay = 1;
                    is_legacy_delay = 1;
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

            if (is_legacy_delay) {
                JPLOG(JP_INFO, "PULSE delay_legacy app=%s -> delay_s=10", bid);
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
            pr->delay_s = val_delay_s;
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
        /* DIAG2: 配置变更观测行；basic 起输出，仅在 changed==1 命中 */
        DIAGLOG(DIAG_BASIC,
                "DIAG event=PLS_CFG reason=cfg_change file=%s rules=%d dedup_key=-|-|cfg diag_dropped=%d diag_latch=%d diag_budget=%d",
                file_absent ? "absent" : "ok", nparsed, diag_dropped_now(), diag_latch_now(), diag_budget_now());
        for (int i = 0; i < nparsed; i++) {
            JPLOG(JP_DEBUG, "%-6s cfg_rule i=%d bid=%s on=%d target=%d n=%d daymax=%d delay_s=%d",
                  "PLS", i, parsed_rules[i].bid, parsed_rules[i].enabled,
                  (int)parsed_rules[i].target_mb, (int)parsed_rules[i].n_min,
                  (int)parsed_rules[i].daymax, (int)parsed_rules[i].delay_s);
        }
        /* H2: 整块替换规则数组必须在写锁内完成（worker 可能正在读） */
        pthread_rwlock_wrlock(&s_rule_lock);
        memcpy(g_pulse_rules, parsed_rules, sizeof(pulse_rule_t) * nparsed);
        g_npulse_rules = nparsed;
        pthread_rwlock_unlock(&s_rule_lock);
        g_pulse_cfg_file_absent = file_absent;
    }

    return file_absent ? -1 : 0;
}

/* =========================================================================
 * 后台内存释放：档位判定、状态跟踪与触发条件校验 (v0.7.8 C2-1)
 * ========================================================================= */
/* BL-010: 熔断由「当日布尔」改为「冷却 + 重复升级」状态机。
 * 全部为内存态，与 DIAG 观测字段同级，不写入 pulse_saved_state_t 磁盘结构。 */
typedef enum {
    CIRCUIT_NONE = 0,
    CIRCUIT_COOLDOWN = 1,
    CIRCUIT_ESCALATED = 2
} pulse_circuit_level_t;

typedef struct {
    char    bid[128];
    pid_t   tracked_pid;
    int     in_bg;              /* 1: 当前处于候选档区间（jp_is_candidate） */
    time_t  bg_start_mono;      /* 进入候选档区间的单调时间 (秒)；25s 死线起点，永不回推 */
    time_t  last_fg_seen_mono;  /* 上次在前台档(>=100，含120)看到的单调时间 (秒) (D1-a / v0.8.6 B0) */
    /* v0.8.6 B0: 纯内存取证字段，不进 pulse_saved_state_t 磁盘结构 */
    time_t  first_bg_seen_mono; /* 本段首次观测到 main_cur<100 的时刻；0=NA */
    time_t  bg_lag_max;         /* 本段观测到的最大滞后秒数；-1=NA */
    time_t  bg_lag;             /* 切后台时的估计延迟秒数，src=now 时为 -1 (NA) (D1-a) */
    time_t  last_pulse_mono;    /* 上次完成 pulse 的单调时间 (秒) */
    time_t  last_ctrl_start_mono; /* 上次启动对照组采样的单调时间 (秒) */
    int     pulsed_this_bg;     /* 1: 本次进入后台期间已触发过 pulse (每次进入后台最多 1 次) */
    int     ctrl_this_bg;       /* 1: 本次进入后台期间已采过对照组 (每段后台最多 1 次，与 pulse 二选一) (M5 / Q2) */
    int     day_count;          /* 当天已触发 pulse 次数 */
    int     ctrl_day_count;     /* 当天已采对照组次数 (每天上限 10) (M5 / Q2) */
    int     pulse_late_this_bg; /* 1: 本次进入后台期间已超时放弃 (不计入 pulse 统计) (C2-T) */
    char    day_date[16];       /* YYYY-MM-DD 本地日期 */
    /* BL-010: 熔断冷却状态机（内存态）；tripped_today 由三字段替代。 */
    time_t                tripped_until;        /* wall-clock epoch；0=无熔断。唯一权威"到点"字段 */
    time_t                tripped_until_mono;   /* CLOCK_MONOTONIC 秒；仅 COOLDOWN 期设，防墙上钟前跳缩短安全窗；0=无 */
    pulse_circuit_level_t circuit_level;        /* 供原因码/升级判定使用 */
    int                   cooldowns_today;      /* 当日已开冷却次数；冷却过期后仍保留，作为"升级资格"标记 */
    int     restore_fail_streak; /* P1-3: 当日连续恢复失败/拒绝次数 */
    int     restore_tripped_today; /* P1-3: 当日因连续恢复失败停止写新水位 */
    int     unresponsive;       /* 1: 标记为对释放无响应 */
    int     total_pulses;       /* 累计 pulse 次数 */
    int     no_drop_pulses;     /* 降幅 <= 5MB 的 pulse 次数 */
    /* DIAG2: 仅内存观测字段，绝不进 pulse_saved_state_t 磁盘结构 */
    int     diag_bg_seq;        /* 每 bid 后台段序号（单调递增，不持久化） */
    int     diag_round;         /* 本后台段候选评估次数（从 1 起） */
    int     diag_band_in;       /* 进入候选档时的档位整数 */
    time_t  diag_first_observe_s; /* 首次 pulse_round 观测到本段的相对秒，-1=NA */
    time_t  diag_gate_t_s;      /* 本段门控首次开门的相对秒，-1=NA */
    /* v0.8.7 APPMON: 纯内存前后台事件追踪字段，不进磁盘结构 */
    int     appmon_in_fg;           /* 1: BKSApplicationStateMonitor 最新回报在前台 */
    time_t  appmon_bg_pending_mono; /* 已收到切后台推送、待 XNU 档位跌落 <100 的起点；0=无 */
} pulse_app_state_t;

#define PULSE_CTRL_APP_DAYMAX    10
#define PULSE_CTRL_GLOBAL_DAYMAX 30
/* P1-3: 同一 bid 当日连续恢复失败达此值即停止为其写新水位（不停 daemon） */
#define PULSE_RESTORE_CIRCUIT_N  3
/* BL-010: 首次 GONE 的冷却时长（秒）。QA 预览包可临时改短压缩等待，合入前必须恢复 7200。 */
#define PULSE_CIRCUIT_COOLDOWN_SEC 7200
/* BL-010 行为回退开关：置 1 则首次 GONE 直接走 CIRCUIT_ESCALATED（全天），等价 v0.8.4。 */
#define PULSE_CIRCUIT_LEGACY_ALLDAY 0
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
        time_t now_wall = time(NULL);
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        time_t now_mono = ts.tv_sec;
        /* 第 3 字段保持派生 0/1 active 布尔（旧 App parts[2]==1 兼容）。
         * active = circuit 有状态且 wall/mono 双截止尚未同时过期。 */
        int circuit_active = (st->circuit_level != CIRCUIT_NONE) &&
            ((st->tripped_until > 0 && st->tripped_until > now_wall) ||
             (st->tripped_until_mono > 0 && st->tripped_until_mono > now_mono));
        fprintf(m, "pulse_app=%s|%d|%d|%d|%ld\n",
                st->bid, st->day_count, circuit_active ? 1 : 0, st->unresponsive,
                (long)(st->tripped_until > 0 ? st->tripped_until : 0));
    }
    pthread_mutex_unlock(&s_pulse_mutex);
}

static void pulse_get_local_date_str(char *buf, size_t sz) {
    time_t t = time(NULL);
    struct tm tm_val;
    localtime_r(&t, &tm_val);
    strftime(buf, sz, "%Y-%m-%d", &tm_val);
}

/* BL-010 跨日处理（调用点均持 s_pulse_mutex，函数内不写日志避免锁嵌套）：
 * ESCALATED → NONE（当日剩余熔断到期）；COOLDOWN 跨午夜保留到原双截止；
 * 一律 cooldowns_today=0（新日首次 GONE 仍是 2h 冷却，不会立即全天封）。 */
static void pulse_circuit_on_new_day(pulse_app_state_t *st) {
    if (st->circuit_level == CIRCUIT_ESCALATED) {
        st->circuit_level = CIRCUIT_NONE;
        st->tripped_until = 0;
        st->tripped_until_mono = 0;
    }
    st->cooldowns_today = 0;
}

/* BL-010 惰性判定（纯只读、不加锁、不改状态；调用点沿用各自既有锁约定）：
 * active = circuit 有状态且 wall/mono 双截止尚未"同时"过期（任一未过即 active）。
 * 返回 active 时应发的 skip 原因码；非 active 返回 NULL（两道门据此放行）。 */
static const char *pulse_circuit_skip_reason(const pulse_app_state_t *st,
                                             time_t now_wall, time_t now_mono) {
    if (!st || st->circuit_level == CIRCUIT_NONE) return NULL;
    int wall_active = (st->tripped_until > 0 && st->tripped_until > now_wall);
    int mono_active = (st->tripped_until_mono > 0 && st->tripped_until_mono > now_mono);
    if (!wall_active && !mono_active) return NULL;
    return (st->circuit_level == CIRCUIT_ESCALATED) ? DSK_CIRCUIT_ESCALATED
                                                    : DSK_CIRCUIT_COOLDOWN;
}

/* BL-010 状态迁移：一次 GONE 命中后的置位（调用点持 s_pulse_mutex）。
 * 返回本次处于的 circuit 原因码，供 PLS_CIRCUIT/日志判读使用。 */
static const char *pulse_circuit_on_gone(pulse_app_state_t *st,
                                         time_t now_wall, time_t now_mono) {
    const char *cir = pulse_circuit_skip_reason(st, now_wall, now_mono);
    /* ARCH §1.3：已 ESCALATED 不变；COOLDOWN 仍在冷却期不变、不重复计数；
     * COOLDOWN 且双截止已过、cooldowns_today>=1 升级到当日结束（S2 上界）；
     * 其余（NONE / 跨日资格已清零）开 PULSE_CIRCUIT_COOLDOWN_SEC 的冷却。 */
    int escalate = (st->circuit_level == CIRCUIT_COOLDOWN && cir == NULL && st->cooldowns_today >= 1);
#if PULSE_CIRCUIT_LEGACY_ALLDAY
    escalate = 1;  /* 行为回退：首次 GONE 也直接全天熔断，等价 v0.8.4 */
#endif
    if (st->circuit_level != CIRCUIT_ESCALATED) {
        if (escalate) {
            struct tm tm_end;
            localtime_r(&now_wall, &tm_end);
            tm_end.tm_hour = 23; tm_end.tm_min = 59; tm_end.tm_sec = 59;
            tm_end.tm_isdst = -1;
            time_t eod = mktime(&tm_end);
            if (eod <= now_wall) eod = now_wall + 1;  /* 安全兜底，绝不超过现在 */
            st->circuit_level = CIRCUIT_ESCALATED;
            st->tripped_until = eod;
            st->tripped_until_mono = 0;  /* ESCALATED 只按墙上钟到当日结束 */
        } else if (!(st->circuit_level == CIRCUIT_COOLDOWN && cir != NULL)) {
            st->circuit_level = CIRCUIT_COOLDOWN;
            st->tripped_until = now_wall + PULSE_CIRCUIT_COOLDOWN_SEC;
            st->tripped_until_mono = now_mono + PULSE_CIRCUIT_COOLDOWN_SEC;
            st->cooldowns_today++;  /* 首次=1；跨日资格归零后新日首次=1 */
        }
    }
    return (st->circuit_level == CIRCUIT_ESCALATED) ? DSK_CIRCUIT_ESCALATED
                                                    : DSK_CIRCUIT_COOLDOWN;
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
            pulse_circuit_on_new_day(&g_pulse_app_states[i]);
            g_pulse_app_states[i].restore_fail_streak = 0;
            g_pulse_app_states[i].restore_tripped_today = 0;
            snprintf(g_pulse_app_states[i].day_date, sizeof(g_pulse_app_states[i].day_date), "%s", today_str);
        }
        diag_reset_day();  /* DIAG2: 跨日重置预算/去重，等级不清零 */
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
                pulse_circuit_on_new_day(&g_pulse_app_states[i]);
                g_pulse_app_states[i].restore_fail_streak = 0;
                g_pulse_app_states[i].restore_tripped_today = 0;
                snprintf(g_pulse_app_states[i].day_date, sizeof(g_pulse_app_states[i].day_date), "%s", today_str);
            }
            return &g_pulse_app_states[i];
        }
    }
    if (g_npulse_app_states < PULSE_MAX_RULES) {
        pulse_app_state_t *st = &g_pulse_app_states[g_npulse_app_states++];
        memset(st, 0, sizeof(*st));
        st->bg_lag = -1;
        st->first_bg_seen_mono = 0;   /* v0.8.6 B0 */
        st->bg_lag_max = -1;          /* v0.8.6 B0 */
        st->diag_first_observe_s = -1;
        st->diag_gate_t_s = -1;
        st->diag_bg_seq = 0;
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

/* DIAG2: PLS_SKIP 闩锁 —— 每 (bid,bg_session) 保留「首因 + 终态因」双槽。
 * 表定义与 g_diag_dedup 一并受 s_diag_mutex 保护；
 * 首因仍按 bid 闩锁、后续同段原因照旧只累加 diag_dropped；但终态原因
 * (too_late/fp_below_target/gone/circuit_cooldown/circuit_escalated) 在首因
 * 之外时，额外发一条 PLS_FINAL 供日志判读，且走既有三闸/去重，不改预算。
 * bg_seq < 0 的无会话事件不覆写有效会话闩锁槽，直接走去重输出。 */

/* 终态原因集：命中者可作为「终态」写入第二槽并触发 PLS_FINAL。 */
static int diag_is_final_reason(const char *r) {
    return strcmp(r, DSK_TOO_LATE) == 0 ||
           strcmp(r, DSK_FP_BELOW_TARGET) == 0 ||
           strcmp(r, DSK_GONE) == 0 ||
           strcmp(r, DSK_CIRCUIT_COOLDOWN) == 0 ||
           strcmp(r, DSK_CIRCUIT_ESCALATED) == 0;
}

static void diag_skip_note(const char *bid, int bg_seq, const char *reason,
                           int round, time_t elapsed, int band) {
    if (diag_level_now() < DIAG_DETAILED || !bid || !reason) return;
    /* invalid_arg 是内部防御码：只计数，不输出（正常路径不可达） */
    if (strcmp(reason, DSK_INVALID_ARG) == 0) {
        atomic_fetch_add_explicit(&g_diag_latch_suppressed, 1, memory_order_relaxed);
        return;
    }

    int is_final = diag_is_final_reason(reason);
    char key[DIAG_DEDUP_KEY_SZ];
    char first_copy[32] = {0};
    int emit_kind = 0; /* 0=none, 1=PLS_SKIP, 2=PLS_FINAL */
    int dropped_snap = 0;

    pthread_mutex_lock(&s_diag_mutex);
    if (bg_seq < 0) {
        /* 无会话号事件不进入/覆盖有效会话双槽闩锁，仅走去重防刷屏。 */
        snprintf(key, sizeof(key), "%s|%d|%s", bid, bg_seq, reason);
        if (!diag_dedup_check_locked(key)) {
            emit_kind = 1;
            dropped_snap = diag_dropped_now();
        }
        pthread_mutex_unlock(&s_diag_mutex);
    } else {
        diag_skip_latch_t *slot = NULL;
        for (int i = 0; i < DIAG_SKIP_LATCH_N; i++) {
            if (g_diag_skip_latch[i].used && strcmp(g_diag_skip_latch[i].bid, bid) == 0) {
                slot = &g_diag_skip_latch[i];
                break;
            }
        }
        int fresh = 0;
        if (!slot) {
            for (int i = 0; i < DIAG_SKIP_LATCH_N; i++) {
                if (!g_diag_skip_latch[i].used) { slot = &g_diag_skip_latch[i]; break; }
            }
            if (!slot) {
                slot = &g_diag_skip_latch[g_diag_skip_latch_head++ % DIAG_SKIP_LATCH_N];
            }
            rst_str_copy(slot->bid, sizeof(slot->bid), bid);
            slot->used = 1;
            slot->bg_seq = -1;
            fresh = 1;
        }
        /* 新会话/新槽：重置双槽，避免串段；首因无条件置为本条 reason。 */
        int new_session = (fresh || slot->bg_seq != bg_seq);
        if (new_session) {
            rst_str_copy(slot->first, sizeof(slot->first), reason);
            slot->final[0] = '\0';
            slot->final_seen = 0;
            slot->bg_seq = bg_seq;
        }

        /* 同段终态且首因不同 → 写第二槽并发 PLS_FINAL（不被闩锁吞）；
         * 同段重复终态由 f: 前缀去重键吸收。 */
        int emit_final = 0;
        if (is_final && !new_session && strcmp(slot->first, reason) != 0) {
            if (!slot->final_seen || strcmp(slot->final, reason) != 0) {
                rst_str_copy(slot->final, sizeof(slot->final), reason);
                slot->final_seen = 1;
                emit_final = 1;
            }
        }

        if (emit_final) {
            snprintf(key, sizeof(key), "%s|%d|f:%s", bid, bg_seq, reason);
            if (!diag_dedup_check_locked(key)) {
                rst_str_copy(first_copy, sizeof(first_copy), slot->first[0] ? slot->first : "NA");
                emit_kind = 2;
                dropped_snap = diag_dropped_now();
            }
        } else if (!new_session) {
            atomic_fetch_add_explicit(&g_diag_latch_suppressed, 1, memory_order_relaxed);
        } else {
            snprintf(key, sizeof(key), "%s|%d|%s", bid, bg_seq, reason);
            if (!diag_dedup_check_locked(key)) {
                emit_kind = 1;
                dropped_snap = diag_dropped_now();
            }
        }
        pthread_mutex_unlock(&s_diag_mutex);
    }

    if (emit_kind == 0) return;

    char s_elapsed[16];
    diag_sec_str(s_elapsed, sizeof(s_elapsed), elapsed);
    if (emit_kind == 2) {
        DIAGLOG(DIAG_DETAILED,
                "DIAG event=PLS_FINAL bid=%s bg_session=%d first=%s reason=%s round=%d elapsed=%s band=%d dedup_key=%s diag_dropped=%d diag_latch=%d diag_budget=%d",
                bid, bg_seq, first_copy, reason, round, s_elapsed, (int)band, key, dropped_snap, diag_latch_now(), diag_budget_now());
    } else {
        DIAGLOG(DIAG_DETAILED,
                "DIAG event=PLS_SKIP bid=%s reason=%s bg_session=%d round=%d elapsed=%s band=%d dedup_key=%s diag_dropped=%d diag_latch=%d diag_budget=%d",
                bid, reason, bg_seq, round, s_elapsed, (int)band, key, dropped_snap, diag_latch_now(), diag_budget_now());
    }
}

/* 25s 硬截止四处（pulse_check_candidate / revalidate / worker / 对照组）
 * 统一走此 helper，禁止各自拼字符串；现有 PULSE_LATE 行保持原样。 */
static void diag_skip_too_late(const char *bid, int bg_seq, int round,
                               time_t elapsed, int band) {
    diag_skip_note(bid, bg_seq, DSK_TOO_LATE, round, elapsed, band);
}

/*
 * 评估 App 是否满足 pulse 的 7 项触发条件 (C2-1)。
 * 返回 1 表示满足触发条件；返回 0 表示不满足。
 * 输出参数:
 *   out_fp: 读取到的当前物理内存占用 (MB)
 *   out_skip_ctrl: 1 表示满足前置条件但因频控 (n / daymax / 全局上限) 跳过，可供对照组采样
 */
/* 复审 P3：本函数无锁直读 g_restart_rules（条件 6），是全文件唯一无锁
 * 直读规则数组的点——仅允许主线程调用（pulse_round），勿被 worker 线程
 * 复用；worker 侧读规则一律走 pulse_find_rule / pulse_revalidate_conditions */
static int pulse_check_candidate(const pulse_rule_t *rule, pulse_app_state_t *st,
                                 pid_t pid, int32_t cur, const char *path,
                                 time_t now_mono, uint64_t *out_fp, int *out_skip_ctrl) {
    if (out_skip_ctrl) *out_skip_ctrl = 0;
    /* DIAG2: 本地观测快照（st / rule 可能为空，逐一兜底；不改任何判定语义） */
    int dseq = st ? st->diag_bg_seq : -1;
    int drnd = st ? st->diag_round : 0;
    time_t delp = (st && st->bg_start_mono > 0) ? (now_mono - st->bg_start_mono) : -1;
    const char *dbid = rule ? rule->bid : "-";
    if (!rule || !st || pid <= 0) {
        diag_skip_note(dbid, dseq, DSK_INVALID_ARG, drnd, delp, (int)cur);
        return 0;
    }

    /* 条件 1: pulse.txt 里该 App 为 on，且为受支持的主进程 */
    if (!rule->enabled) {
        diag_skip_note(dbid, dseq, DSK_RULE_OFF, drnd, delp, (int)cur);
        return 0;
    }
    if (!pulse_is_supported_app(pid, path, rule->bid)) {
        diag_skip_note(dbid, dseq, DSK_UNSUPPORTED, drnd, delp, (int)cur);
        return 0;
    }
    /* P1-3: 当日恢复熔断——停止为该 bid 写新水位（调用时持 s_pulse_mutex） */
    if (st->restore_tripped_today) {
        diag_skip_note(dbid, dseq, DSK_RESTORE_TRIPPED, drnd, delp, (int)cur);
        return 0;
    }

    /* 当前档位检查：必须在候选档区间；其他值一律不处理 (M3 / C2-1) */
    if (!jp_is_candidate(cur)) {
        diag_skip_note(dbid, dseq, DSK_NOT_CANDIDATE_BAND, drnd, delp, (int)cur);
        return 0;
    }

    /* 条件 2: 同一个 pid 连续在候选档区间满 delay_s 秒 (C2-T) */
    if (!st->in_bg || st->tracked_pid != pid || st->bg_start_mono <= 0) {
        diag_skip_note(dbid, dseq, DSK_BG_NOT_TRACKED, drnd, delp, (int)cur);
        return 0;
    }

    /* 条件 7: 该 App 当前不在熔断/冷却期（BL-010：前移至 delay/fp/25s 死线之前，
     * 使冷却期进后台首轮即记录 circuit_cooldown/escalated，避免无谓读 fp 与误报 PULSE_LATE） */
    {
        const char *cir = pulse_circuit_skip_reason(st, time(NULL), now_mono);
        if (cir) {
            diag_skip_note(dbid, dseq, cir, drnd, delp, (int)cur);
            return 0;
        }
    }

    /* v0.8.6 B0: 25s 死线仍用 now-bg_start（bg_start 不回推），
     * delay 起点另用"最后一次 >=100"；慢轮晚观测到 fg→bg 时当轮即可 arm。 */
    time_t elapsed_sec = now_mono - st->bg_start_mono;
    time_t delay_elapsed = elapsed_sec;
    if (st->last_fg_seen_mono > 0 && st->first_bg_seen_mono >= st->last_fg_seen_mono) {
        delay_elapsed = now_mono - st->last_fg_seen_mono;
    }
    if (delay_elapsed < (time_t)rule->delay_s) {
        diag_skip_note(dbid, dseq, DSK_DELAY_NOT_MET, drnd, delp, (int)cur);
        return 0;
    }

    /* 每次进入后台最多做 1 次 pulse (与对照组二选一) (M5 / Q2)；本段已超时的不再尝试 (C2-T) */
    if (st->pulsed_this_bg || st->ctrl_this_bg || st->pulse_late_this_bg) {
        diag_skip_note(dbid, dseq, DSK_ALREADY_DONE, drnd, delp, (int)cur);
        return 0;
    }

    /* 条件 3: 当前 fp > target */
    uint64_t fp_mb = 0;
    if (pulse_get_footprint_mb(pid, &fp_mb) != 0) {
        diag_skip_note(dbid, dseq, DSK_FP_UNREADABLE, drnd, delp, (int)cur);
        return 0;
    }
    if (out_fp) *out_fp = fp_mb;
    if (fp_mb <= (uint64_t)rule->target_mb) {
        diag_skip_note(dbid, dseq, DSK_FP_BELOW_TARGET, drnd, delp, (int)cur);
        return 0;
    }

    /* 条件 6: 有重启规则时，fp 不能超过重启阈值 (超过则交由 0.7.7 重启规则处理) */
    for (int r = 0; r < g_nrestart_rules; r++) {
        if (strcmp(g_restart_rules[r].bid, rule->bid) == 0) {
            if (fp_mb > (uint64_t)g_restart_rules[r].thr_mb) {
                diag_skip_note(dbid, dseq, DSK_OVER_RESTART_THR, drnd, delp, (int)cur);
                return 0; /* 超过重启阈值，跳过 pulse */
            }
            break;
        }
    }

    /* 截止时间检查 (Place 1 入队前检查)：bg_start + 25 秒之前才能真正开始 (C2-T / D1-a) */
    if (elapsed_sec >= PULSE_DEADLINE_BG_SEC) {
        if (!st->pulse_late_this_bg) {
            st->pulse_late_this_bg = 1;
            JPLOG(JP_INFO, "PULSE_LATE app=%s pid=%d elapsed=%ld locked=%d", rule->bid, (int)pid, (long)elapsed_sec, g_locked);
        }
        diag_skip_too_late(dbid, dseq, drnd, elapsed_sec, (int)cur);
        return 0;
    }

    /* 检查是否命中频控 (条件 4 和 5)：命中频控者可作为对照组采样来源 (M5 / Q2) */
    /* 条件 4: 距该 App 上一次 pulse 至少 n 分钟 (按 App 计，换 pid 也不清零；Hark-A 顺带裁定)
     * n=0 时没有冷却判定，永远满足，也就没有对照组样本 (用户已知) */
    int cooldown_hit = 0;
    if (st->last_pulse_mono > 0 && (now_mono - st->last_pulse_mono) < (time_t)rule->n_min * 60) {
        cooldown_hit = 1;
    }
    int daymax_hit = (st->day_count >= rule->daymax);
    int global_hit = (g_pulse_global_today_count >= PULSE_GLOBAL_DAYMAX);

    if (cooldown_hit || daymax_hit) {
        /* 满 delay 那一刻，因 n 或 daymax 跳过才记对照组；全局上限不记；每段后台最多 1 个 (Q2)
         * 仅标记候选，不在此处置 ctrl_this_bg；只有真正启动采样时才标记 (C2R2-N1) */
        if (!global_hit && !st->pulsed_this_bg && !st->ctrl_this_bg && !st->pulse_late_this_bg &&
            st->ctrl_day_count < PULSE_CTRL_APP_DAYMAX &&
            g_pulse_ctrl_global_today_count < PULSE_CTRL_GLOBAL_DAYMAX) {
            if (out_skip_ctrl) *out_skip_ctrl = 1;
        }
        diag_skip_note(dbid, dseq, cooldown_hit ? DSK_COOLDOWN : DSK_DAYMAX,
                       drnd, delp, (int)cur);
        return 0;
    }
    if (global_hit) {
        diag_skip_note(dbid, dseq, DSK_GLOBAL_CAP, drnd, delp, (int)cur);
        return 0;
    }

    return 1;
}

/* 出队与线程启动时的前置条件复核 (M2)
 * 检查：规则是否还是 on、fp 是否还大于 target、fp 是否超过重启阈值、
 * 是否已熔断、是否超过 daymax 或全局上限、是否已超过截止时间。任一不满足返回 0 */
static int pulse_revalidate_conditions(pid_t pid, const char *bid, uint64_t *out_fp) {
    /* DIAG2: 本地观测快照；不触碰任何判定/计数/锁语义 */
    const char *dbid = bid ? bid : "-";
    int dseq = -1, drnd = 0;
    time_t delp = -1;
    if (pid <= 0 || !bid || bid[0] == '\0') {
        diag_skip_note(dbid, -1, DSK_INVALID_ARG, 0, -1, -1);
        return 0;
    }

    /* H2: 读锁内取规则快照；锁外只用快照（enabled/daymax/target_mb 的使用都在锁外） */
    pulse_rule_t rule_snap;
    if (!pulse_find_rule(bid, &rule_snap) || !rule_snap.enabled) {
        diag_skip_note(dbid, -1, DSK_RULE_OFF, 0, -1, -1);
        return 0;
    }

    char today_str[16] = {0};
    pulse_get_local_date_str(today_str, sizeof(today_str));

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    time_t now_mono = ts.tv_sec;

    pthread_mutex_lock(&s_pulse_mutex);
    pulse_app_state_t *st = pulse_get_app_state(bid, today_str);
    dseq = st ? st->diag_bg_seq : -1;
    drnd = st ? st->diag_round : 0;
    delp = (st && st->bg_start_mono > 0) ? (now_mono - st->bg_start_mono) : -1;
    const char *cir_reason = pulse_circuit_skip_reason(st, time(NULL), now_mono);
    int invalid = (!st || cir_reason || st->day_count >= rule_snap.daymax ||
                   g_pulse_global_today_count >= PULSE_GLOBAL_DAYMAX);
    if (invalid) {
        /* cir_reason 优先（cooldown/escalated）；DSK_CIRCUIT_TRIPPED 仅作未知原因/无状态兜底 */
        const char *inv_reason = cir_reason ? cir_reason
                               : (!st) ? DSK_CIRCUIT_TRIPPED
                               : (st->day_count >= rule_snap.daymax) ? DSK_DAYMAX
                               : DSK_GLOBAL_CAP;
        diag_skip_note(dbid, dseq, inv_reason, drnd, delp, -1);
        pthread_mutex_unlock(&s_pulse_mutex);
        return 0;
    }

    /* 截止时间检查 (Place 2 出队复核)：bg_start + 25 秒之前才能真正开始 (C2-T / D1-a) */
    if (!st->in_bg || st->tracked_pid != pid || st->bg_start_mono <= 0) {
        diag_skip_note(dbid, dseq, DSK_BG_NOT_TRACKED, drnd, delp, -1);
        pthread_mutex_unlock(&s_pulse_mutex);
        return 0;
    }
    time_t elapsed_sec = now_mono - st->bg_start_mono;
    if (elapsed_sec >= PULSE_DEADLINE_BG_SEC) {
        if (!st->pulse_late_this_bg) {
            st->pulse_late_this_bg = 1;
            JPLOG(JP_INFO, "PULSE_LATE app=%s pid=%d elapsed=%ld locked=%d", bid, (int)pid, (long)elapsed_sec, g_locked);
        }
        diag_skip_too_late(dbid, dseq, drnd, elapsed_sec, -1);
        pthread_mutex_unlock(&s_pulse_mutex);
        return 0;
    }
    pthread_mutex_unlock(&s_pulse_mutex);

    uint64_t cur_fp = 0;
    if (pulse_get_footprint_mb(pid, &cur_fp) != 0) {
        diag_skip_note(dbid, dseq, DSK_FP_UNREADABLE, drnd, delp, -1);
        return 0;
    }
    if (cur_fp <= (uint64_t)rule_snap.target_mb) {
        diag_skip_note(dbid, dseq, DSK_FP_BELOW_TARGET, drnd, delp, -1);
        return 0;
    }

    /* H2: 重启规则数组读锁——锁内只读值、锁外判断，保证单次 unlock。
     * （复审修复：原写法在「匹配且 fp ≤ thr_mb」路径下循环内 unlock 后
     * break 又落到循环外 unlock，双重 unlock 属 POSIX 未定义行为） */
    pthread_rwlock_rdlock(&s_rule_lock);
    int over_thr = 0;
    for (int r = 0; r < g_nrestart_rules; r++) {
        if (strcmp(g_restart_rules[r].bid, bid) == 0) {
            over_thr = (cur_fp > (uint64_t)g_restart_rules[r].thr_mb);
            break;
        }
    }
    pthread_rwlock_unlock(&s_rule_lock);
    if (over_thr) {
        diag_skip_note(dbid, dseq, DSK_OVER_RESTART_THR, drnd, delp, -1);
        return 0;
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
    time_t   bg_lag;
    time_t   bg_lag_max;   /* v0.8.6 B0: 纯内存快照，不落盘 */
    int      locked;
} pulse_queue_entry_t;

#define PULSE_MAX_QUEUE 32
static pulse_queue_entry_t g_pulse_queue[PULSE_MAX_QUEUE];
static int g_npulse_queue = 0;

/* H1 并发原子性：in_progress/pid/running 全部原子化。
 * running 是 worker 完成的唯一权威信号：worker 先清逻辑态，最后
 * atomic_store(running,0,release)；主循环以 atomic_load(running,acquire)==0
 * 判定旧线程已收尾，才允许 join/spawn。 */
static _Atomic int   g_pulse_in_progress = 0;
static _Atomic pid_t g_pulse_current_pid = 0;
static char          g_pulse_current_bid[128] = {0}; /* H1 范围确认：全文件只写不读，无撕裂读风险，保持原样 */
static pthread_t      g_pulse_thread;
static _Atomic int    g_pulse_thread_running = 0;
static int            g_pulse_thread_has_handle = 0; /* 仅主线程读写，安全 */

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
/* XNU 黄金警告带语义：rule->target_mb 仅作后台触发门槛（fp > target 才入队/执行）；
 * 实际写入内核的非致命水位取 L = ceil(fp0 / 0.85)，使当前占用精准落在 ~85%×L，
 * 命中 XNU ledger [80%×L, 100%×L) 警告区间（PHYS_FOOTPRINT_WARNING_LEVEL=80）：
 * 1) !limit_exceeded 分支向 UIKit 投递 NOTE_MEMORYSTATUS_PRESSURE_WARN/CRITICAL，
 *    真正触发 applicationDidReceiveMemoryWarning 释放可回收缓存；
 * 2) fp0 < L 确保 memorystatus_kill_hiwat_proc 跳过该进程，杜绝 highwater 误杀保活 App。 */
static inline uint64_t calc_hard_guard_limit(uint64_t fp_mb) {
    return (fp_mb * 11 + 9) / 10;   /* ceil(fp * 1.10) */
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

    /* INT1：原 lstat(CONFIG_DIR) 检查并入 jp_secure_precheck——同时校验目标
     * symlink/类型/root 属主/组或其他可写；异常一律拒绝写入，不回退 fopen。 */
    if (jp_secure_precheck(PULSE_STATE_TMP_PATH) != 0) return -1;

    /* M1 原子替换保持 O_NOFOLLOW；0600 保持远端原权限位 */
    int fd = open(PULSE_STATE_TMP_PATH, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (jp_secure_fd_check(fd, 0600) != 0) {
        close(fd);
        unlink(PULSE_STATE_TMP_PATH);
        return -1;
    }

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
    fprintf(f, "fmt_magic=%u\n", st->fmt_magic);
    fprintf(f, "self_written=%d\n", st->self_written);
    /* M1: 统一原子提交——原实现缺 fsync（数据与目录）；O_NOFOLLOW/fchmod 保留 */
    if (jp_atomic_commit(f, PULSE_STATE_TMP_PATH, PULSE_STATE_PATH) != 0) {
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

    int fd = open(PULSE_STATE_PATH, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;

    struct stat fd_st;
    if (fstat(fd, &fd_st) != 0 || !S_ISREG(fd_st.st_mode) ||
        fd_st.st_uid != geteuid() || (fd_st.st_mode & 0777) != 0600 ||
        fd_st.st_ino != file_st.st_ino || fd_st.st_dev != file_st.st_dev) {
        close(fd);
        return -1;
    }

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
    int has_fmt_magic = 0;
    int has_self_written = 0;

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
        } else if (strncmp(line, "fmt_magic=", 10) == 0) {
            st_out->fmt_magic = (uint32_t)strtoul(line + 10, NULL, 0);
            has_fmt_magic = 1;
        } else if (strncmp(line, "self_written=", 13) == 0) {
            st_out->self_written = atoi(line + 13);
            has_self_written = 1;
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
    /* P2: 显式格式魔数 + 合法值校验；旧格式/魔数不符/非法值一律降级为不可信镜像 */
    if (!has_fmt_magic || st_out->fmt_magic != PULSE_STATE_FMT_MAGIC) {
        st_out->self_written = 0;
    }
    if (!has_self_written || (st_out->self_written != 0 && st_out->self_written != 1)) {
        st_out->self_written = 0;
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
    /* Ltarget: 此快照由本进程在写限额前落盘，身份校验通过即视为可信写前值，
     * 60s 窗口结束必须原样恢复（不再用当前 fp 推断"不可信"）。 */
    st_out->fmt_magic = PULSE_STATE_FMT_MAGIC;
    st_out->self_written = 1;
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

/* P1-3: 按 bid 维度的恢复失败计数与当日熔断。覆盖 pulse_do_restore 内所有
 * REFUSED/FAILED 与 worker 写限额前的全部早退。熔断仅停止为该 App 写新水位，
 * 不停 daemon；成功恢复清零 streak，但当日熔断状态保持到跨日 reset。 */
static void pulse_note_restore_failed(const char *bid, pid_t pid, const char *reason) {
    if (!bid || bid[0] == '\0') return;
    char today_str[16] = {0};
    pulse_get_local_date_str(today_str, sizeof(today_str));

    pthread_mutex_lock(&s_pulse_mutex);
    pulse_app_state_t *st = pulse_get_app_state(bid, today_str);
    if (st && !st->restore_tripped_today) {
        st->restore_fail_streak++;
        if (st->restore_fail_streak >= PULSE_RESTORE_CIRCUIT_N) {
            st->restore_tripped_today = 1;
            JPLOG(JP_WARN, "%-6s RESTORE_CIRCUIT_OPEN app=%s pid=%d streak=%d last=%s -> no new watermark today",
                  "PLS", bid, (int)pid, st->restore_fail_streak, reason ? reason : "");
            DIAGLOG(DIAG_BASIC,
                    "DIAG event=PLS_CIRCUIT bid=%s reason=open streak=%d dedup_key=%s|-|circuit_open diag_dropped=%d diag_latch=%d diag_budget=%d",
                    bid, st->restore_fail_streak, bid, diag_dropped_now(), diag_latch_now(), diag_budget_now());
        } else {
            JPLOG(JP_WARN, "%-6s RESTORE_WARN_STREAK app=%s pid=%d streak=%d last=%s",
                  "PLS", bid, (int)pid, st->restore_fail_streak, reason ? reason : "");
            DIAGLOG(DIAG_BASIC,
                    "DIAG event=PLS_CIRCUIT bid=%s reason=warn_streak streak=%d dedup_key=%s|-|circuit_warn diag_dropped=%d diag_latch=%d diag_budget=%d",
                    bid, st->restore_fail_streak, bid, diag_dropped_now(), diag_latch_now(), diag_budget_now());
        }
    }
    pthread_mutex_unlock(&s_pulse_mutex);
}

static void pulse_note_restore_ok(const char *bid) {
    if (!bid || bid[0] == '\0') return;
    char today_str[16] = {0};
    pulse_get_local_date_str(today_str, sizeof(today_str));

    pthread_mutex_lock(&s_pulse_mutex);
    pulse_app_state_t *st = pulse_get_app_state(bid, today_str);
    if (st && st->restore_fail_streak != 0) {
        st->restore_fail_streak = 0;
        JPLOG(JP_INFO, "%-6s RESTORE_WARN_STREAK cleared app=%s", "PLS", bid);
        DIAGLOG(DIAG_BASIC,
                "DIAG event=PLS_CIRCUIT bid=%s reason=cleared streak=0 dedup_key=%s|-|circuit_cleared diag_dropped=%d diag_latch=%d diag_budget=%d",
                bid, bid, diag_dropped_now(), diag_latch_now(), diag_budget_now());
    }
    pthread_mutex_unlock(&s_pulse_mutex);
}

/* 统一 Restore 实现函数 (C2-3) */
static int pulse_do_restore(pid_t pid, const char *bid, char *verdict_out, size_t verdict_sz) {
    if (verdict_out && verdict_sz > 0) verdict_out[0] = '\0';

    char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
    if (proc_pidpath(pid, path, sizeof(path)) <= 0 || !pulse_is_supported_app(pid, path, bid)) {
        if (verdict_out) snprintf(verdict_out, verdict_sz, "RESTORE_REFUSED");
        JPLOG(JP_INFO, "RESTORE_REFUSED app=%s pid=%d reason=validate", bid, (int)pid);
        pulse_note_restore_failed(bid, pid, "validate");
        return 1;
    }

    uint64_t fp_mb = 0;
    int fp_err = pulse_get_footprint_mb(pid, &fp_mb);
    if (fp_err != 0) {
        if (verdict_out) snprintf(verdict_out, verdict_sz, "RESTORE_REFUSED");
        JPLOG(JP_INFO, "RESTORE_REFUSED app=%s pid=%d reason=fp_unreadable", bid, (int)pid);
        pulse_note_restore_failed(bid, pid, "fp_unreadable");
        return 1;
    }

    proc_identity_t ident;
    if (pulse_get_proc_identity(pid, &ident) != 0) {
        if (verdict_out) snprintf(verdict_out, verdict_sz, "RESTORE_REFUSED");
        JPLOG(JP_INFO, "RESTORE_REFUSED app=%s pid=%d reason=ident", bid, (int)pid);
        pulse_note_restore_failed(bid, pid, "ident");
        return 1;
    }

    memorystatus_memlimit_properties_t cur_props;
    int cur_err = pulse_get_memlimit_props(pid, &cur_props);
    if (cur_err != 0) {
        if (verdict_out) snprintf(verdict_out, verdict_sz, "RESTORE_REFUSED");
        JPLOG(JP_INFO, "RESTORE_REFUSED app=%s pid=%d reason=get", bid, (int)pid);
        pulse_note_restore_failed(bid, pid, "get");
        return 1;
    }

    pulse_saved_state_t state;
    const char *mirror_reason = NULL;
    int trusted_self = 0;
    if (pulse_load_trusted_state_file(pid, &ident, &state) != 0) {
        mirror_reason = "untrusted";
    } else if (!state.self_written || state.orig_inactive < 0) {
        /* 旧格式/魔数不符/非法值/非本进程快照：沿用原有可信判定 */
        if (state.orig_inactive <= 0) {
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
    } else {
        /* Ltarget: 本进程写前快照 + 身份校验通过 = 可信。L=target<fp 时
         * "orig_inactive < ceil(fp*1.2)" 必然命中，旧判定会把写前值误判不可信，
         * 回退 active 后常见 RESTORE_REFUSED → 永久遗留 target 水位。
         * 这里强制原样恢复，保证窗口结束回到写前值。 */
        trusted_self = 1;
    }

    int32_t target_inactive = 0;
    uint32_t target_inactive_attr = 0;

    if (mirror_reason != NULL) {
        uint64_t floor_1_2 = calc_restore_floor(fp_mb);
        if (cur_props.memlimit_active <= 0 ||
            (uint64_t)cur_props.memlimit_active <= floor_1_2) {
            if (verdict_out) snprintf(verdict_out, verdict_sz, "RESTORE_REFUSED");
            JPLOG(JP_INFO, "RESTORE_REFUSED app=%s pid=%d reason=active_low", bid, (int)pid);
            pulse_note_restore_failed(bid, pid, "active_low");
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
            if (verdict_out) snprintf(verdict_out, verdict_sz, "RESTORE_REFUSED");
            JPLOG(JP_INFO, "RESTORE_REFUSED app=%s pid=%d reason=%s", bid, (int)pid, refused_reason);
            pulse_note_restore_failed(bid, pid, refused_reason);
            return 1;
        }
        if (readback_mismatch) {
            if (verdict_out) snprintf(verdict_out, verdict_sz, "RESTORE_FAILED");
            JPLOG(JP_INFO, "RESTORE_FAILED app=%s pid=%d reason=readback", bid, (int)pid);
            pulse_note_restore_failed(bid, pid, "readback");
            return 1;
        } else {
            if (verdict_out) snprintf(verdict_out, verdict_sz, "RESTORE_FAILED");
            JPLOG(JP_INFO, "RESTORE_FAILED app=%s pid=%d reason=retry", bid, (int)pid);
            pulse_note_restore_failed(bid, pid, "retry");
            return 1;
        }
    }

    pulse_clear_state_file(pid);
    pulse_note_restore_ok(bid);

    if (mirror_reason != NULL) {
        if (verdict_out) snprintf(verdict_out, verdict_sz, "RESTORE_MIRROR");
        JPLOG(JP_INFO, "RESTORE_MIRROR app=%s pid=%d reason=%s", bid, (int)pid, mirror_reason);
        return 0;
    }

    if (verdict_out) snprintf(verdict_out, verdict_sz, "RESTORE_OK");
    JPLOG(JP_INFO, "RESTORE_OK app=%s pid=%d self=%d", bid, (int)pid, trusted_self);
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

/* 读取进程累计 CPU 时间 (pti_total_user + pti_total_system) (C2-T) */
static int pulse_get_cpu_time(pid_t pid, uint64_t *cpu_total_out) {
    if (!cpu_total_out || pid <= 0) return -1;
    struct proc_taskinfo ti;
    memset(&ti, 0, sizeof(ti));
    if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &ti, sizeof(ti)) == sizeof(ti)) {
        *cpu_total_out = ti.pti_total_user + ti.pti_total_system;
        return 0;
    }
    return -1;
}

/* 计算 CPU 时间增量 (ms) (C2-T) */
static uint64_t pulse_calc_cpu_ms(uint64_t cpu_start, uint64_t cpu_end) {
    if (cpu_end <= cpu_start) return 0;
    uint64_t raw_diff = cpu_end - cpu_start;
    static struct mach_timebase_info tb = {0, 0};
    if (tb.denom == 0) {
        mach_timebase_info(&tb);
    }
    uint64_t ms = 0;
    if (tb.denom > 0) {
        ms = (uint64_t)(((double)raw_diff * (double)tb.numer) / ((double)tb.denom * 1000000.0));
    } else {
        ms = raw_diff / 1000000ULL;
    }
    if (raw_diff > 0 && ms == 0) {
        ms = 1;
    }
    return ms;
}

/* 崩溃恢复状态与启动校验 (v0.7.8 C2-5 / Q5) */
static void pulse_startup_recovery(void) {
    struct stat dir_st;
    if (lstat(CONFIG_DIR, &dir_st) != 0 || S_ISLNK(dir_st.st_mode) || !S_ISDIR(dir_st.st_mode)) {
        JPLOG(JP_WARN, "%-6s state_invalid reason=dir_symlink path=%s", "PLS", PULSE_STATE_PATH);
        DIAGLOG(DIAG_BASIC, "DIAG event=PLS_STATE reason=dir_symlink dedup_key=-|-|dir_symlink diag_dropped=%d diag_latch=%d diag_budget=%d", diag_dropped_now(), diag_latch_now(), diag_budget_now());
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
        DIAGLOG(DIAG_BASIC, "DIAG event=PLS_STATE reason=file_attr dedup_key=-|-|file_attr diag_dropped=%d diag_latch=%d diag_budget=%d", diag_dropped_now(), diag_latch_now(), diag_budget_now());
        g_pulse_corrupt_state = 1;
        g_pulse_corrupt_bid[0] = '\0';
        pulse_log_today_pulsed_bids();
        return;
    }

    pulse_saved_state_t st;
    if (pulse_load_trusted_state_file(0, NULL, &st) != 0 || st.pid <= 0) {
        unlink(PULSE_STATE_PATH);
        JPLOG(JP_WARN, "%-6s state_invalid reason=parse_fail path=%s", "PLS", PULSE_STATE_PATH);
        DIAGLOG(DIAG_BASIC, "DIAG event=PLS_STATE reason=parse_fail dedup_key=-|-|parse_fail diag_dropped=%d diag_latch=%d diag_budget=%d", diag_dropped_now(), diag_latch_now(), diag_budget_now());
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
        DIAGLOG(DIAG_BASIC, "DIAG event=PLS_STATE reason=process_gone dedup_key=-|-|process_gone diag_dropped=%d diag_latch=%d diag_budget=%d", diag_dropped_now(), diag_latch_now(), diag_budget_now());
        return;
    }

    if (curr_ident.start_sec != st.start_sec || curr_ident.start_usec != st.start_usec) {
        /* pid 对不上 (pid 已被其他进程复用) */
        unlink(PULSE_STATE_PATH);
        JPLOG(JP_INFO, "%-6s state_cleanup pid=%d reason=pid_reused", "PLS", st.pid);
        DIAGLOG(DIAG_BASIC, "DIAG event=PLS_STATE reason=pid_reused dedup_key=-|-|pid_reused diag_dropped=%d diag_latch=%d diag_budget=%d", diag_dropped_now(), diag_latch_now(), diag_budget_now());
        return;
    }

    /* 校验通过，并且 pid 和启动时间都对得上：先 restore，再进入正常流程 */
    char verdict_str[64] = {0};
    const char *bid = (st.bid[0] != '\0') ? st.bid : "com.tencent.xin";
    pulse_do_restore(st.pid, bid, verdict_str, sizeof(verdict_str));
    JPLOG(JP_INFO, "%-6s crash_recovered pid=%d bid=%s verdict=%s",
          "PLS", st.pid, bid, verdict_str[0] ? verdict_str : "RESTORE_OK");
    DIAGLOG(DIAG_BASIC, "DIAG event=PLS_STATE reason=crash_recovered bid=%s dedup_key=%s|-|crash_recovered diag_dropped=%d diag_latch=%d diag_budget=%d",
            bid, bid, diag_dropped_now(), diag_latch_now(), diag_budget_now());
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
    /* 复审 P3：判空失败也要走收尾——直接 return 会绕过 thread_done，
     * running 恒 1 使 spawn 守卫永久 defer（free(NULL) 安全） */
    if (!target) goto thread_done;

    pid_t pid = target->pid;
    const char *bid = target->bid;
    int was_locked = target->locked;

    /* DIAG2: worker 局部观测快照（纯内存，绝不写 pulse.state 磁盘结构） */
    int diag_bg_seq = -1, diag_round = 0, diag_band_in = -1;
    time_t diag_first_obs_s = -1, diag_gate_t_s = -1;
    int diag_arm_t_s = -1;
    time_t diag_bg_start_mono = 0;
    int diag_ovr_seen = 0, diag_ovr_t = -1, diag_ovr_val = -1, diag_ovr_attr = 0;
    int diag_ovr_band = -1, diag_reassert_ok = -1;
    uint64_t diag_ovr_fp = 0;
    int fg_interrupted = 0;

    /* 截止时间检查 (Place 3 worker 开头)：bg_start + 25 秒之前才能真正开始 (C2-T / D1-a) */
    char today_str[16] = {0};
    pulse_get_local_date_str(today_str, sizeof(today_str));

    struct timespec ts_start;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    time_t now_mono = ts_start.tv_sec;

    pthread_mutex_lock(&s_pulse_mutex);
    pulse_app_state_t *st_init = pulse_get_app_state(bid, today_str);
    if (!st_init || !st_init->in_bg || st_init->tracked_pid != pid || st_init->bg_start_mono <= 0) {
        diag_skip_note(bid, -1, DSK_BG_NOT_TRACKED, 0, -1, -1);
        pthread_mutex_unlock(&s_pulse_mutex);
        goto thread_done;
    }
    /* DIAG2: 锁内快照本段观测字段（含 diag_bg_seq，避免收尾 GONE 归因串到新会话） */
    diag_bg_seq = st_init->diag_bg_seq;
    diag_round = st_init->diag_round;
    diag_band_in = st_init->diag_band_in;
    diag_first_obs_s = st_init->diag_first_observe_s;
    diag_gate_t_s = st_init->diag_gate_t_s;
    diag_bg_start_mono = st_init->bg_start_mono;
    time_t elapsed_start = now_mono - st_init->bg_start_mono;
    if (elapsed_start >= PULSE_DEADLINE_BG_SEC) {
        if (!st_init->pulse_late_this_bg) {
            st_init->pulse_late_this_bg = 1;
            JPLOG(JP_INFO, "PULSE_LATE app=%s pid=%d elapsed=%ld locked=%d", bid, (int)pid, (long)elapsed_start, g_locked);
        }
        diag_skip_too_late(bid, diag_bg_seq, diag_round, elapsed_start, -1);
        pthread_mutex_unlock(&s_pulse_mutex);
        goto thread_done;
    }
    pthread_mutex_unlock(&s_pulse_mutex);

    /* P1-3: 恢复熔断二次防线——worker 侧现读，当日已熔断则本次不写任何水位 */
    pthread_mutex_lock(&s_pulse_mutex);
    char today_w_str[16] = {0};
    pulse_get_local_date_str(today_w_str, sizeof(today_w_str));
    pulse_app_state_t *rst_chk = pulse_get_app_state(bid, today_w_str);
    int restore_tripped = (rst_chk && rst_chk->restore_tripped_today);
    int diag_rst_streak = rst_chk ? rst_chk->restore_fail_streak : 0;
    pthread_mutex_unlock(&s_pulse_mutex);
    if (restore_tripped) {
        JPLOG(JP_INFO, "PULSE_CIRCUIT_SKIP app=%s pid=%d reason=restore_streak", bid, (int)pid);
        DIAGLOG(DIAG_BASIC,
                "DIAG event=PLS_CIRCUIT bid=%s reason=skip streak=%d dedup_key=%s|-|circuit_skip diag_dropped=%d diag_latch=%d diag_budget=%d",
                bid, diag_rst_streak, bid, diag_dropped_now(), diag_latch_now(), diag_budget_now());
        diag_skip_note(bid, diag_bg_seq, DSK_RESTORE_TRIPPED, diag_round, -1, diag_band_in);
        goto thread_done;
    }

    /* 线程开始执行时重新检查条件 (M2) */
    uint64_t reval_fp = 0;
    if (!pulse_revalidate_conditions(pid, bid, &reval_fp)) {
        goto thread_done;
    }

    proc_identity_t ident;
    if (pulse_get_proc_identity(pid, &ident) != 0) {
        JPLOG(JP_INFO, "RESTORE_REFUSED app=%s pid=%d reason=ident", bid, (int)pid);
        pulse_note_restore_failed(bid, pid, "ident");
        goto thread_done;
    }

    pulse_saved_state_t state;
    int state_rc = pulse_get_or_record_initial_state(pid, &ident, bid, &state);
    if (state_rc != 0) {
        const char *ref_reason = (state_rc == -1) ? "get" : "state";
        JPLOG(JP_INFO, "RESTORE_REFUSED app=%s pid=%d reason=%s", bid, (int)pid, ref_reason);
        pulse_note_restore_failed(bid, pid, ref_reason);
        goto thread_done;
    }

    uint64_t fp0 = 0;
    if (pulse_get_footprint_mb(pid, &fp0) != 0) {
        JPLOG(JP_INFO, "RESTORE_REFUSED app=%s pid=%d reason=fp_unreadable", bid, (int)pid);
        pulse_note_restore_failed(bid, pid, "fp_unreadable");
        pulse_clear_state_file(pid);
        goto thread_done;
    }

    /* target_mb 作后台触发门槛，实际水位 L = ceil(fp0 / 0.85) 命中 XNU [80%,100%) 警告带 */
    pulse_rule_t pulse_rule_snap;
    uint64_t pulse_target_mb = 0;
    if (pulse_find_rule(bid, &pulse_rule_snap) && pulse_rule_snap.enabled) {
        pulse_target_mb = (uint64_t)pulse_rule_snap.target_mb;
    }
    if (pulse_target_mb == 0) {
        /* 规则在 worker 期间被删除/关闭：不写任何水位，清理快照后收尾 */
        JPLOG(JP_INFO, "PULSE_RULE_GONE app=%s pid=%d", bid, (int)pid);
        pulse_clear_state_file(pid);
        goto thread_done;
    }
    if (fp0 <= pulse_target_mb) {
        /* 二次读 fp0 已自行回落至 target 以下：无需写水位，清理快照安全退出 */
        diag_skip_note(bid, diag_bg_seq, DSK_FP_BELOW_TARGET, diag_round, -1, diag_band_in);
        pulse_clear_state_file(pid);
        goto thread_done;
    }
    uint64_t L = calc_pulse_limit(fp0);
    uint64_t hard_guard = calc_hard_guard_limit(fp0);
    if (L < hard_guard) L = hard_guard;

    memorystatus_memlimit_properties_t cur_props;
    if (pulse_get_memlimit_props(pid, &cur_props) != 0) {
        JPLOG(JP_INFO, "RESTORE_REFUSED app=%s pid=%d reason=get", bid, (int)pid);
        pulse_note_restore_failed(bid, pid, "get");
        pulse_clear_state_file(pid);
        goto thread_done;
    }
    if (cur_props.memlimit_active > 0 && L > (uint64_t)cur_props.memlimit_active) {
        L = (uint64_t)cur_props.memlimit_active;
    }
    if (fp0 * 100 >= (uint64_t)PULSE_GUARD_PCT * L) {
        JPLOG(JP_INFO, "PULSE_SKIP_NEAR_CAP app=%s pid=%d fp=%llu active=%d",
              bid, (int)pid, (unsigned long long)fp0, cur_props.memlimit_active);
        pulse_clear_state_file(pid);
        goto thread_done;
    }

    memorystatus_memlimit_properties_t new_props = cur_props;
    new_props.memlimit_inactive = (int32_t)L;
    new_props.memlimit_inactive_attr &= ~MEMORYSTATUS_MEMLIMIT_ATTR_FATAL;

    proc_identity_t check_ident;
    if (pulse_get_proc_identity(pid, &check_ident) != 0 || !pulse_compare_proc_identity(&ident, &check_ident)) {
        JPLOG(JP_INFO, "RESTORE_REFUSED app=%s pid=%d reason=ident", bid, (int)pid);
        pulse_note_restore_failed(bid, pid, "ident");
        pulse_clear_state_file(pid);
        goto thread_done;
    }

    if (pulse_set_memlimit_props(pid, &new_props) != 0) {
        JPLOG(JP_INFO, "RESTORE_REFUSED app=%s pid=%d reason=set", bid, (int)pid);
        pulse_note_restore_failed(bid, pid, "set");
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
    /* DIAG2: arm_t 相对本段 bg_start_mono 的非负秒（未 arm 保持 -1=NA） */
    if (diag_bg_start_mono > 0 && now_mono >= diag_bg_start_mono) {
        diag_arm_t_s = (int)(now_mono - diag_bg_start_mono);
    }

    /* 挂起检测：PULSE_ARMED 时读取累计 CPU 时间 (C2-T / M1) */
    uint64_t cpu_start = 0;
    int cpu_start_ok = (pulse_get_cpu_time(pid, &cpu_start) == 0);

    const char *end_reason = "WINDOW_END";
    int should_restore = 1;
    int t_reach = -1;
    uint64_t fp15 = 0, fp30 = 0, fp60 = 0, fp1 = 0;
    int has_fp15 = 0, has_fp30 = 0, has_fp60 = 0, has_fp1 = 0;

    /* 卡 P：OVERRIDDEN 跟踪与重新设限状态 */
    int ovr_t = -1;
    int32_t ovr_val = -1;
    uint32_t ovr_attr = 0;
    uint64_t fp_ovr = 0;
    int ovr_read_fail = 0;
    int reassert_count = 0;
    int reassert_val = -1; /* -1: NA, 0: ok=0, 1: ok=1 */
    int32_t expected_inactive = (int32_t)L;
    uint64_t last_fp = fp0;
    int last_sec = 0;

    for (int sec = 1; sec <= 60; sec++) {
        usleep(1000 * 1000);
        if (g_locked) was_locked = 1;

        if (g_pulse_stop_requested) {
            end_reason = "WINDOW_END";
            should_restore = 1;
            break;
        }

        proc_identity_t loop_ident;
        if (pulse_get_proc_identity(pid, &loop_ident) != 0) {
            struct proc_bsdinfo bi;
            if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof(bi)) != sizeof(bi)) {
                JPLOG(JP_INFO, "PULSE_GONE app=%s pid=%d", bid, (int)pid);
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
                JPLOG(JP_INFO, "PULSE_GONE app=%s pid=%d", bid, (int)pid);
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

        last_fp = cur_fp;
        last_sec = sec;

        if (sec == 1) { fp1 = cur_fp; has_fp1 = 1; }  /* DIAG2: full 级采样用，无 DIAGLOG */
        if (sec == 15) { fp15 = cur_fp; has_fp15 = 1; }
        else if (sec == 30) { fp30 = cur_fp; has_fp30 = 1; }
        else if (sec == 60) { fp60 = cur_fp; has_fp60 = 1; }

        JPLOG(JP_DEBUG, "t=%ds fp=%llu L=%llu (%.1f%%)",
              sec, (unsigned long long)cur_fp, (unsigned long long)L,
              (double)cur_fp * 100.0 / (double)L);

        /* 卡 P-1 / 卡 P-2: 每秒在读 fp 之后调用一次 pulse_get_memlimit_props(pid) */
        memorystatus_memlimit_properties_t loop_props;
        if (pulse_get_memlimit_props(pid, &loop_props) != 0) {
            ovr_read_fail++;
        } else if (loop_props.memlimit_inactive != expected_inactive) {
            if (reassert_count == 0 && ovr_t < 0) {
                /* 第一次发现 inactive ≠ 当前期望值 (初始为 L) */
                ovr_t = sec;
                ovr_val = loop_props.memlimit_inactive;
                ovr_attr = loop_props.memlimit_inactive_attr;
                fp_ovr = cur_fp;

                int32_t band = -1;
                pulse_get_cur_priority(pid, &band);

                JPLOG(JP_INFO, "PULSE_OVERRIDE app=%s pid=%d t=%d inactive=%d/0x%x fp=%llu band=%d",
                      bid, (int)pid, ovr_t, ovr_val, ovr_attr,
                      (unsigned long long)fp_ovr, (int)band);
                /* DIAG2: 只记本地快照，PLS_OVERRIDE 行在 worker 收尾一次性 flush（循环内零 DIAGLOG） */
                diag_ovr_seen = 1;
                diag_ovr_t = ovr_t;
                diag_ovr_val = ovr_val;
                diag_ovr_attr = (int)ovr_attr;
                diag_ovr_fp = fp_ovr;
                diag_ovr_band = (int)band;

                /* 计算从窗口开始到现在的 cpu_ms */
                uint64_t now_cpu = 0;
                int now_cpu_ok = (pulse_get_cpu_time(pid, &now_cpu) == 0);
                uint64_t cur_cpu_ms = (cpu_start_ok && now_cpu_ok) ? pulse_calc_cpu_ms(cpu_start, now_cpu) : 0;

                /* 卡 P-5: SUSPENDED 优先：cpu_ms == 0 时不重新设限、不恢复、结论仍记 SUSPENDED */
                if (cur_cpu_ms == 0) {
                    end_reason = "SUSPENDED";
                    should_restore = 0;
                    break;
                }

                /* 卡 P-2: 重新设限，最多 1 次
                 * 条件：ovr_t <= PULSE_REASSERT_MAX_SEC (30s)，现读档位 < BAND_PROTECT_DEFAULT，
                 * fp_ovr > target，从窗口开始到现在的 cpu_ms > 0，且没重新设过 */
                /* H2: 读锁内取规则快照，锁外用快照判定 */
                pulse_rule_t rule_snap;
                int have_rule = pulse_find_rule(bid, &rule_snap);
                uint64_t target_mb = have_rule ? (uint64_t)rule_snap.target_mb : 0;

                if (ovr_t <= PULSE_REASSERT_MAX_SEC && band >= BAND_IDLE &&
                    band < BAND_PROTECT_DEFAULT && fp_ovr > target_mb) {
                    reassert_count = 1;
                    proc_identity_t check_reassert_ident;
                    int reassert_ok = 0;
                    uint64_t r_L = calc_pulse_limit(fp_ovr);
                    uint64_t r_guard = calc_hard_guard_limit(fp_ovr);
                    if (r_L < r_guard) r_L = r_guard;
                    if (loop_props.memlimit_active > 0 && r_L > (uint64_t)loop_props.memlimit_active) {
                        r_L = (uint64_t)loop_props.memlimit_active;
                    }
                    if (fp_ovr * 100 < (uint64_t)PULSE_GUARD_PCT * r_L &&
                        pulse_get_proc_identity(pid, &check_reassert_ident) == 0 &&
                        pulse_compare_proc_identity(&ident, &check_reassert_ident)) {
                        memorystatus_memlimit_properties_t r_props = loop_props;
                        r_props.memlimit_inactive = (int32_t)r_L;
                        r_props.memlimit_inactive_attr &= ~MEMORYSTATUS_MEMLIMIT_ATTR_FATAL;
                        if (pulse_set_memlimit_props(pid, &r_props) == 0) {
                            memorystatus_memlimit_properties_t rb;
                            if (pulse_get_memlimit_props(pid, &rb) == 0) {
                                int rb_act_fatal = (rb.memlimit_active_attr & MEMORYSTATUS_MEMLIMIT_ATTR_FATAL) ? 1 : 0;
                                int exp_act_fatal = (r_props.memlimit_active_attr & MEMORYSTATUS_MEMLIMIT_ATTR_FATAL) ? 1 : 0;
                                int rb_inact_fatal = (rb.memlimit_inactive_attr & MEMORYSTATUS_MEMLIMIT_ATTR_FATAL) ? 1 : 0;
                                if (rb.memlimit_active == r_props.memlimit_active &&
                                    rb_act_fatal == exp_act_fatal &&
                                    rb.memlimit_inactive == (int32_t)r_L &&
                                    rb_inact_fatal == 0) {
                                    reassert_ok = 1;
                                }
                            }
                        }
                    }
                    JPLOG(JP_INFO, "PULSE_REASSERT ok=%d t=%d", reassert_ok, sec);
                    diag_reassert_ok = reassert_ok;  /* DIAG2: 收尾统一输出 */
                    if (reassert_ok) {
                        reassert_val = 1;
                        L = r_L;
                        expected_inactive = (int32_t)r_L;
                        state.orig_inactive = ovr_val;
                        state.orig_inactive_attr = ovr_attr;
                        pulse_save_state_file(&state);
                        /* 继续这个窗口 */
                    } else {
                        reassert_val = 0;
                        end_reason = "OVERRIDDEN";
                        should_restore = 0;
                        break;
                    }
                } else {
                    /* 不满足条件：结束窗口，end_reason="OVERRIDDEN"，不恢复 */
                    if (jp_is_foreground(band)) fg_interrupted = 1;
                    end_reason = "OVERRIDDEN";
                    should_restore = 0;
                    break;
                }
            } else {
                /* 重新设限之后又被改：结束窗口，end_reason="OVERRIDDEN"，不恢复，不再重新设限 */
                int32_t band2 = -1;
                if (pulse_get_cur_priority(pid, &band2) == 0 && jp_is_foreground(band2)) {
                    fg_interrupted = 1;
                }
                end_reason = "OVERRIDDEN";
                should_restore = 0;
                break;
            }
        }

        /* 95%×L 越界前保护：L = ceil(fp/0.85) 使起始占用落在 ~85%×L（处于 XNU [80%,100%)
         * UIKit 警告带）。一旦后台异常暴涨达到 95%×L，立即在触碰 100%×L 之前恢复原限额，
         * 从数学上杜绝越界触发 memorystatus_kill_hiwat_proc (highwater) 误杀保活 App。 */
        if (cur_fp * 100 >= (uint64_t)PULSE_GUARD_PCT * L) {
            if (sec <= 1) {
                JPLOG(JP_WARN, "PULSE_ASSERT early_abort app=%s pid=%d fp=%llu fp0=%llu L=%llu",
                      bid, (int)pid, (unsigned long long)cur_fp,
                      (unsigned long long)fp0, (unsigned long long)L);
            }
            JPLOG(JP_INFO, "ABORT guard95 fp=%llu L=%llu",
                  (unsigned long long)cur_fp, (unsigned long long)L);
            end_reason = "ABORT";
            t_reach = sec;
            should_restore = 1;
            break;
        }

        /* TARGET_REACHED：占用已回落至规则 target_mb 以下，提前恢复并继续只读采样至 60s */
        if (cur_fp <= pulse_target_mb) {
            JPLOG(JP_INFO, "TARGET_REACHED fp=%llu <= target=%llu",
                  (unsigned long long)cur_fp, (unsigned long long)pulse_target_mb);
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

    /* 挂起检测：窗口结束时读取累计 CPU 时间并计算差值 (C2-T / M1) */
    uint64_t cpu_end = 0;
    int cpu_end_ok = (pulse_get_cpu_time(pid, &cpu_end) == 0);
    uint64_t cpu_ms = 0;
    int cpu_calc_ok = 0;
    if (cpu_start_ok && cpu_end_ok) {
        cpu_ms = pulse_calc_cpu_ms(cpu_start, cpu_end);
        cpu_calc_ok = 1;
    }
    if (strcmp(end_reason, "GONE") != 0 && strcmp(end_reason, "PID_REUSED") != 0) {
        if (!cpu_calc_ok) {
            end_reason = "CPU_FAIL";
        } else if (cpu_ms == 0) {
            end_reason = "SUSPENDED";
            if (ovr_t > 0) {
                should_restore = 0;
            }
        }
    }

    char restore_verdict_str[64] = "NONE";
    if (should_restore) {
        memorystatus_memlimit_properties_t pre_restore;
        if (pulse_get_memlimit_props(pid, &pre_restore) == 0 &&
            pre_restore.memlimit_inactive != expected_inactive) {
            if (ovr_t < 0) {
                ovr_t = (last_sec > 0) ? last_sec : 1;
                ovr_val = pre_restore.memlimit_inactive;
                ovr_attr = pre_restore.memlimit_inactive_attr;
                fp_ovr = last_fp;
                int32_t band = -1;
                pulse_get_cur_priority(pid, &band);
                JPLOG(JP_INFO, "PULSE_OVERRIDE app=%s pid=%d t=%d inactive=%d/0x%x fp=%llu band=%d",
                      bid, (int)pid, ovr_t, ovr_val, ovr_attr,
                      (unsigned long long)fp_ovr, (int)band);
                /* DIAG2: 收尾统一输出 PLS_OVERRIDE（此处不在 60s 循环内） */
                diag_ovr_seen = 1;
                diag_ovr_t = ovr_t;
                diag_ovr_val = ovr_val;
                diag_ovr_attr = (int)ovr_attr;
                diag_ovr_fp = fp_ovr;
                diag_ovr_band = (int)band;
            }
            if (cpu_calc_ok && cpu_ms == 0) {
                end_reason = "SUSPENDED";
            } else {
                if (jp_is_foreground(diag_ovr_band)) fg_interrupted = 1;
                end_reason = "OVERRIDDEN";
            }
            snprintf(restore_verdict_str, sizeof(restore_verdict_str), "%s", end_reason);
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
        } else if (strcmp(end_reason, "OVERRIDDEN") == 0) {
            snprintf(restore_verdict_str, sizeof(restore_verdict_str), "OVERRIDDEN");
        } else if (strcmp(end_reason, "SUSPENDED") == 0) {
            snprintf(restore_verdict_str, sizeof(restore_verdict_str), "SUSPENDED");
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
            if (pulse_get_cur_priority(pid, &cur_prio) == 0 && cur_prio == BAND_FOREGROUND) {
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

    /* 汇总行输出 (C2-4 / M1 / D1-a / 卡 P) */
    char t_reach_str[32], fp15_str[32], fp30_str[32], fp60_str[32], cpu_ms_str[32], bg_lag_str[32], bg_lag_max_str[32];
    char ovr_t_str[32], fp_ovr_str[32], reassert_str[32];

    if (t_reach > 0) snprintf(t_reach_str, sizeof(t_reach_str), "%d", t_reach);
    else snprintf(t_reach_str, sizeof(t_reach_str), "NA");

    if (has_fp15) snprintf(fp15_str, sizeof(fp15_str), "%llu", (unsigned long long)fp15);
    else snprintf(fp15_str, sizeof(fp15_str), "NA");

    if (has_fp30) snprintf(fp30_str, sizeof(fp30_str), "%llu", (unsigned long long)fp30);
    else snprintf(fp30_str, sizeof(fp30_str), "NA");

    if (has_fp60) snprintf(fp60_str, sizeof(fp60_str), "%llu", (unsigned long long)fp60);
    else snprintf(fp60_str, sizeof(fp60_str), "NA");

    if (cpu_calc_ok) snprintf(cpu_ms_str, sizeof(cpu_ms_str), "%llu", (unsigned long long)cpu_ms);
    else snprintf(cpu_ms_str, sizeof(cpu_ms_str), "NA");

    if (target->bg_lag >= 0) snprintf(bg_lag_str, sizeof(bg_lag_str), "%ld", (long)target->bg_lag);
    else snprintf(bg_lag_str, sizeof(bg_lag_str), "NA");
    /* v0.8.6 B0: 本段最大滞后秒数（含 0 为具体值），NA 仅表示无前台参照 */
    if (target->bg_lag_max >= 0) snprintf(bg_lag_max_str, sizeof(bg_lag_max_str), "%ld", (long)target->bg_lag_max);
    else snprintf(bg_lag_max_str, sizeof(bg_lag_max_str), "NA");

    if (ovr_t > 0) snprintf(ovr_t_str, sizeof(ovr_t_str), "%d", ovr_t);
    else snprintf(ovr_t_str, sizeof(ovr_t_str), "NA");

    if (ovr_t > 0) snprintf(fp_ovr_str, sizeof(fp_ovr_str), "%llu", (unsigned long long)fp_ovr);
    else snprintf(fp_ovr_str, sizeof(fp_ovr_str), "NA");

    if (reassert_val >= 0) snprintf(reassert_str, sizeof(reassert_str), "%d", reassert_val);
    else snprintf(reassert_str, sizeof(reassert_str), "NA");

    if (g_locked) was_locked = 1;

    /* DIAG2: off 保留 v=1 行逐字不变；basic 及以上改写 v=2（v=1 不再重复输出） */
    int cur_diag_lv = diag_level_now();
    if (cur_diag_lv >= DIAG_BASIC) {
        char s_first[16], s_gate[16], s_arm[16], s_band[16];
        diag_sec_str(s_first, sizeof(s_first), diag_first_obs_s);
        diag_sec_str(s_gate, sizeof(s_gate), diag_gate_t_s);
        diag_sec_str(s_arm, sizeof(s_arm), (time_t)diag_arm_t_s);
        diag_sec_str(s_band, sizeof(s_band), (diag_band_in >= 0) ? (time_t)diag_band_in : (time_t)-1);
        DIAGLOG(DIAG_BASIC,
                "DIAG event=PULSE v=2 bid=%s round=%d band_in=%s bg_lag=%s bg_lag_max=%s first_observe=%s gate_t=%s arm_t=%s L=%llu t_reach=%s cpu_ms=%s end=%s restore=%s diag_dropped=%d diag_latch=%d diag_budget=%d",
                bid, diag_round, s_band, bg_lag_str, bg_lag_max_str, s_first, s_gate, s_arm,
                (unsigned long long)L, t_reach_str, cpu_ms_str, end_reason,
                restore_verdict_str, diag_dropped_now(), diag_latch_now(), diag_budget_now());
    } else {
        JPLOG(JP_INFO, "PULSE app=%s pid=%d fp0=%llu L=%llu t_reach=%s fp15=%s fp30=%s fp60=%s cpu_ms=%s end=%s restore=%s bg_lag=%s locked=%d ovr_t=%s fp_ovr=%s reassert=%s ovr_rf=%d",
              bid, (int)pid, (unsigned long long)fp0, (unsigned long long)L,
              t_reach_str, fp15_str, fp30_str, fp60_str, cpu_ms_str, end_reason, restore_verdict_str,
              bg_lag_str, was_locked,
              ovr_t_str, fp_ovr_str, reassert_str, ovr_read_fail);
    }

    /* DIAG2: full 级 PLS_OVERRIDE / PLS_STAGE / PLS_SAMPLE 在 worker 收尾一次性
     * flush —— :3825+ 与 :4309+ 的 60s 循环内 DIAGLOG 调用恒为 0。 */
    if (diag_ovr_seen) {
        DIAGLOG(DIAG_BASIC,
                "DIAG event=PLS_OVERRIDE bid=%s t=%d inactive=%d/0x%x fp=%llu band=%d reassert=%d dedup_key=%s|-|override diag_dropped=%d diag_latch=%d diag_budget=%d",
                bid, diag_ovr_t, diag_ovr_val, (unsigned)diag_ovr_attr,
                (unsigned long long)diag_ovr_fp, diag_ovr_band, diag_reassert_ok,
                bid, diag_dropped_now(), diag_latch_now(), diag_budget_now());
    }
    if (cur_diag_lv >= DIAG_FULL) {
        char s_fp1[24], s_fp15[24], s_fp30[24], s_fp60[24];
        char s_first2[16], s_gate2[16], s_arm2[16];
        if (has_fp1)  snprintf(s_fp1,  sizeof(s_fp1),  "%llu", (unsigned long long)fp1);  else snprintf(s_fp1,  sizeof(s_fp1),  "NA");
        if (has_fp15) snprintf(s_fp15, sizeof(s_fp15), "%llu", (unsigned long long)fp15); else snprintf(s_fp15, sizeof(s_fp15), "NA");
        if (has_fp30) snprintf(s_fp30, sizeof(s_fp30), "%llu", (unsigned long long)fp30); else snprintf(s_fp30, sizeof(s_fp30), "NA");
        if (has_fp60) snprintf(s_fp60, sizeof(s_fp60), "%llu", (unsigned long long)fp60); else snprintf(s_fp60, sizeof(s_fp60), "NA");
        diag_sec_str(s_first2, sizeof(s_first2), diag_first_obs_s);
        diag_sec_str(s_gate2,  sizeof(s_gate2),  diag_gate_t_s);
        diag_sec_str(s_arm2,   sizeof(s_arm2),   (time_t)diag_arm_t_s);
        DIAGLOG(DIAG_FULL,
                "DIAG event=PLS_STAGE bid=%s round=%d first=%s gate=%s arm=%s set=%s window=%s restore=%s end=%s dedup_key=%s|-|stage diag_dropped=%d diag_latch=%d diag_budget=%d",
                bid, diag_round, s_first2, s_gate2, s_arm2, s_arm2, t_reach_str, t_reach_str,
                end_reason, bid, diag_dropped_now(), diag_latch_now(), diag_budget_now());
        DIAGLOG(DIAG_FULL,
                "DIAG event=PLS_SAMPLE bid=%s t1=%s t15=%s t30=%s t60=%s cpu_ms=%s dedup_key=%s|-|sample diag_dropped=%d diag_latch=%d diag_budget=%d",
                bid, s_fp1, s_fp15, s_fp30, s_fp60, cpu_ms_str, bid, diag_dropped_now(), diag_latch_now(), diag_budget_now());
    }

    /* 计次与状态更新 (M6: 持锁保护) */
    pulse_get_local_date_str(today_str, sizeof(today_str));
    pthread_mutex_lock(&s_pulse_mutex);
    pulse_app_state_t *st = pulse_get_app_state(bid, today_str);
    if (st) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        st->last_pulse_mono = ts.tv_sec;
        st->day_count++;
        int fg_ovr = (strcmp(end_reason, "OVERRIDDEN") == 0 && fg_interrupted);
        if (strcmp(end_reason, "SUSPENDED") != 0 && strcmp(end_reason, "CPU_FAIL") != 0 && !fg_ovr) {
            st->total_pulses++;
        }

        /* 熔断判定 (C2-6 / BL-010): 窗口内或 restore 后 10 秒内 GONE；PID_REUSED 不计入熔断 (S7) */
        if ((strcmp(end_reason, "GONE") == 0 || gone_post_restore) && strcmp(end_reason, "PID_REUSED") != 0) {
            /* BL-010 状态机落地：唯一权威迁移逻辑在 pulse_circuit_on_gone（便于测试/审计）。 */
            time_t now_wall = time(NULL);
            struct timespec ts_cir;
            clock_gettime(CLOCK_MONOTONIC, &ts_cir);
            const char *cir_reason = pulse_circuit_on_gone(st, now_wall, ts_cir.tv_sec);
            const char *dedup_tail = (st->circuit_level == CIRCUIT_ESCALATED)
                                     ? "circuit_escalated" : "circuit_cooldown";
            const char *log_tag = (st->circuit_level == CIRCUIT_ESCALATED) ? "escalated" : "cooldown";
            JPLOG(JP_WARN, "%-6s circuit_%s app=%s pid=%d reason=gone", "PLS", log_tag, bid, (int)pid);
            DIAGLOG(DIAG_BASIC,
                    "DIAG event=PLS_CIRCUIT bid=%s reason=%s streak=%d dedup_key=%s|-|%s diag_dropped=%d diag_latch=%d diag_budget=%d",
                    bid, cir_reason, st->restore_fail_streak, bid, dedup_tail, diag_dropped_now(), diag_latch_now(), diag_budget_now());

            /* BL-003: GONE 作为终态原因写入双槽终态槽（使用 worker 启动时快照的 diag_bg_seq，
             * 防止 60s 窗口内前后台切换后归因串到新会话）；走既有 diag_skip_note 三闸/去重。 */
            diag_skip_note(bid, diag_bg_seq, DSK_GONE, diag_round, -1, diag_band_in);
        }

        /* 无响应判定 (C2-6): 若已完成 >= 2 次 pulse 且最近连续 2 次降幅均 <= 5MB，标记为无响应 (只标记，不停用) */
        /* PID_REUSED、GONE 均不计入「没降」统计 (S7) */
        /* SUSPENDED 也不计入 total 和 no_drop，也不触发 unresponsive (C2-T) */
        /* CPU_FAIL (读 CPU 失败) 也不计入 total 和 no_drop，不触发 unresponsive (M1) */
        /* 卡 P-4: 计算 fp_final: 有 ovr_t 且这次以 OVERRIDDEN 结束，fp_final = fp_ovr; 其他情况维持原取法 */
        uint64_t fp_final = (ovr_t > 0 && strcmp(end_reason, "OVERRIDDEN") == 0) ? fp_ovr
                            : (has_fp60 ? fp60 : (has_fp30 ? fp30 : (has_fp15 ? fp15 : fp0)));
        /* 卡 P-4: 后台系统改写 OVERRIDDEN 照常计入 total_pulses 并参与 no_drop/unresponsive 判定；
         * 但用户切回前台 (band>=100) 引发的前台打断 fg_ovr 不属于后台释放无响应，予以排除 */
        if (strcmp(end_reason, "GONE") != 0 && !gone_post_restore &&
            strcmp(end_reason, "PID_REUSED") != 0 &&
            strcmp(end_reason, "SUSPENDED") != 0 &&
            strcmp(end_reason, "CPU_FAIL") != 0 &&
            !fg_ovr) {
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
    /* H1: 先清逻辑态，最后清 running；running=0(release) 是 worker 完成的
     * 唯一权威信号，此后本线程不再写任何共享 pulse 状态 */
    atomic_store_explicit(&g_pulse_in_progress, 0, memory_order_relaxed);
    atomic_store_explicit(&g_pulse_current_pid, 0, memory_order_relaxed);
    g_pulse_current_bid[0] = '\0';
    atomic_store_explicit(&g_pulse_thread_running, 0, memory_order_release);
    free(target);
    return NULL;
}

/* 对照组工作线程参数 (M5 / Q2) */
typedef struct {
    char     bid[128];
    pid_t    pid;
    time_t   bg_lag;
    time_t   bg_lag_max;   /* v0.8.6 B0: 纯内存快照 */
    int      locked;
} pulse_ctrl_arg_t;

/* 对照组异步只读采样线程 (M5 / Q2)
 * 从头到尾只读，不写任何档位/内存限制。秒级时钟采样，误差超过 +-2s 记 NA */
static void *pulse_ctrl_worker_thread(void *arg) {
    pulse_ctrl_arg_t *carg = (pulse_ctrl_arg_t *)arg;
    /* 复审 P3：同 pulse worker——判空失败也要走 ctrl_done 收尾（不 free） */
    if (!carg) goto ctrl_done;

    pid_t pid = carg->pid;
    char bid[128];
    rst_str_copy(bid, sizeof(bid), carg->bid);
    time_t carg_bg_lag = carg->bg_lag;
    time_t carg_bg_lag_max = carg->bg_lag_max;   /* v0.8.6 B0 */
    int was_locked = carg->locked;
    free(carg);

    /* 截止时间检查 (对照组同等截止时间：bg_start + 25 秒之前才能开始采样) (C2-T / D1-a) */
    char today_str[16] = {0};
    pulse_get_local_date_str(today_str, sizeof(today_str));

    struct timespec ts_ctrl;
    clock_gettime(CLOCK_MONOTONIC, &ts_ctrl);
    time_t now_mono = ts_ctrl.tv_sec;

    pthread_mutex_lock(&s_pulse_mutex);
    pulse_app_state_t *st_ctrl_chk = pulse_get_app_state(bid, today_str);
    if (!st_ctrl_chk || !st_ctrl_chk->in_bg || st_ctrl_chk->tracked_pid != pid || st_ctrl_chk->bg_start_mono <= 0) {
        diag_skip_note(bid, -1, DSK_BG_NOT_TRACKED, 0, -1, -1);
        pthread_mutex_unlock(&s_pulse_mutex);
        goto ctrl_done;
    }
    time_t elapsed_ctrl = now_mono - st_ctrl_chk->bg_start_mono;
    if (elapsed_ctrl >= PULSE_DEADLINE_BG_SEC) {
        if (!st_ctrl_chk->pulse_late_this_bg) {
            st_ctrl_chk->pulse_late_this_bg = 1;
            JPLOG(JP_INFO, "PULSE_LATE app=%s pid=%d elapsed=%ld locked=%d", bid, (int)pid, (long)elapsed_ctrl, g_locked);
        }
        diag_skip_too_late(bid, st_ctrl_chk->diag_bg_seq, st_ctrl_chk->diag_round, elapsed_ctrl, -1);
        pthread_mutex_unlock(&s_pulse_mutex);
        goto ctrl_done;
    }
    pthread_mutex_unlock(&s_pulse_mutex);

    /* 对照线程开始时现读 fp0；读失败就放弃这组、不记录 (C2F2-N1) */
    uint64_t fp0 = 0;
    if (pulse_get_footprint_mb(pid, &fp0) != 0) {
        diag_skip_note(bid, -1, DSK_FP_UNREADABLE, 0, -1, -1);
        goto ctrl_done;
    }

    /* 挂起检测：对照组采样开始时读取 CPU 时间 (C2-T / M1) */
    uint64_t ctrl_cpu_start = 0;
    int ctrl_cpu_start_ok = (pulse_get_cpu_time(pid, &ctrl_cpu_start) == 0);

    uint64_t fp15 = 0, fp30 = 0, fp60 = 0;
    int has_fp15 = 0, has_fp30 = 0, has_fp60 = 0;

    /* 两组都从这一刻开始计时 (C2F2-N1) */
    struct timespec t_start;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    for (int sec = 1; sec <= 60; sec++) {
        usleep(1000 * 1000);
        if (g_locked) was_locked = 1;
        if (g_got_sigterm || g_pulse_stop_requested) break;

        struct proc_bsdinfo bi;
        if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof(bi)) != sizeof(bi)) {
            break; /* 进程消失，停止采样 */
        }

        int32_t cur_prio = -1;
        if (pulse_get_cur_priority(pid, &cur_prio) == 0 &&
            (cur_prio == BAND_FOREGROUND || cur_prio == BAND_SYSTEM_ELEVATED)) {
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

    /* 挂起检测：对照组采样结束时计算 CPU 差值 (C2-T / M1) */
    uint64_t ctrl_cpu_end = 0;
    int ctrl_cpu_end_ok = (pulse_get_cpu_time(pid, &ctrl_cpu_end) == 0);
    uint64_t ctrl_cpu_ms = 0;
    int ctrl_cpu_calc_ok = 0;
    if (ctrl_cpu_start_ok && ctrl_cpu_end_ok) {
        ctrl_cpu_ms = pulse_calc_cpu_ms(ctrl_cpu_start, ctrl_cpu_end);
        ctrl_cpu_calc_ok = 1;
    }

    char s_fp15[32], s_fp30[32], s_fp60[32], s_ctrl_cpu_ms[32], s_bg_lag[32], s_bg_lag_max[32];
    if (has_fp15) snprintf(s_fp15, sizeof(s_fp15), "%llu", (unsigned long long)fp15);
    else snprintf(s_fp15, sizeof(s_fp15), "NA");

    if (has_fp30) snprintf(s_fp30, sizeof(s_fp30), "%llu", (unsigned long long)fp30);
    else snprintf(s_fp30, sizeof(s_fp30), "NA");

    if (has_fp60) snprintf(s_fp60, sizeof(s_fp60), "%llu", (unsigned long long)fp60);
    else snprintf(s_fp60, sizeof(s_fp60), "NA");

    if (ctrl_cpu_calc_ok) snprintf(s_ctrl_cpu_ms, sizeof(s_ctrl_cpu_ms), "%llu", (unsigned long long)ctrl_cpu_ms);
    else snprintf(s_ctrl_cpu_ms, sizeof(s_ctrl_cpu_ms), "NA");

    if (carg_bg_lag >= 0) snprintf(s_bg_lag, sizeof(s_bg_lag), "%ld", (long)carg_bg_lag);
    else snprintf(s_bg_lag, sizeof(s_bg_lag), "NA");
    if (carg_bg_lag_max >= 0) snprintf(s_bg_lag_max, sizeof(s_bg_lag_max), "%ld", (long)carg_bg_lag_max);
    else snprintf(s_bg_lag_max, sizeof(s_bg_lag_max), "NA");

    if (g_locked) was_locked = 1;

    JPLOG(JP_INFO, "PULSE_SKIP_CTRL app=%s pid=%d fp0=%llu fp15=%s fp30=%s fp60=%s cpu_ms=%s bg_lag=%s bg_lag_max=%s locked=%d",
          bid, (int)pid, (unsigned long long)fp0, s_fp15, s_fp30, s_fp60, s_ctrl_cpu_ms,
          s_bg_lag, s_bg_lag_max, was_locked);

    pthread_mutex_lock(&s_pulse_mutex);
    pulse_app_state_t *st = pulse_get_app_state(bid, today_str);
    if (st) {
        st->ctrl_day_count++;
    }
    g_pulse_ctrl_global_today_count++;
    pthread_mutex_unlock(&s_pulse_mutex);

ctrl_done:
    /* H1: 同 pulse worker——先清逻辑态，最后 running=0(release) */
    atomic_store_explicit(&g_pulse_in_progress, 0, memory_order_relaxed);
    atomic_store_explicit(&g_pulse_current_pid, 0, memory_order_relaxed);
    g_pulse_current_bid[0] = '\0';
    atomic_store_explicit(&g_pulse_thread_running, 0, memory_order_release);
    return NULL;
}

/* =========================================================================
 * App 进程列表单轮遍历缓存 (供 restart 与 pulse 共同复用，N5-a)
 * ========================================================================= */
#define RST_MAX_TRACKED_APPS 256
typedef struct {
    pid_t pid;
    int32_t priority;
    uint32_t state;             /* v0.8.6: JETSAM entry.state（供 A2 观测与 75 档冷冻室核验） */
    char path[PROC_PIDPATHINFO_MAXSIZE];
    char bid[128];
} rst_app_proc_t;

static rst_app_proc_t s_app_procs[RST_MAX_TRACKED_APPS];
static int s_napp_procs = -1;
/* v0.8.6 C3: 共享缓存带轮次 epoch。缓存有效当且仅当 s_napp_procs>=0 且
 * s_app_proc_epoch==s_round_epoch（主线程每轮 +1）；门控快轮显式置 0 失效。 */
static unsigned long s_app_proc_epoch = 0;   /* 0 = 无效 token */
static unsigned long s_round_epoch = 0;

static void rst_ensure_app_procs(memorystatus_priority_entry_t *list, int count);
static void rst_app_procs_invalidate(void);
static void rst_cache_app_proc_from_scan(pid_t pid, int32_t priority,
                                         uint32_t state, const char *path);
static void rst_update_cached_priority(pid_t pid, int32_t priority);

/* v0.8.6 C3: 显式失效（保留 P0 门控快轮强制失效语义），bump epoch + 清 token */
static void rst_app_procs_invalidate(void) {
    s_round_epoch++;
    s_app_proc_epoch = 0;
    s_napp_procs = -1;
}

/* 慢轮主扫描已拿到 path，顺手填本轮共享缓存（与 rst_ensure_app_procs 同过滤条件：
 * main-app 二进制 + 有效 bid；fail-closed，不降低判定严格度）。 */
static void rst_cache_app_proc_from_scan(pid_t pid, int32_t priority,
                                         uint32_t state, const char *path) {
    if (s_napp_procs < 0 || s_napp_procs >= RST_MAX_TRACKED_APPS) return;
    if (!memp_is_main_app_binary(path)) return;
    const char *bid = app_bid_for_path(path);
    if (!bid || bid[0] == '\0') return;
    for (int j = 0; j < s_napp_procs; j++) {
        if (s_app_procs[j].pid == pid) return;   /* 同 pid 不重复入缓存 */
    }
    rst_app_proc_t *e = &s_app_procs[s_napp_procs++];
    e->pid = pid;
    e->priority = priority;
    e->state = state;
    rst_str_copy(e->path, sizeof(e->path), path);
    rst_str_copy(e->bid, sizeof(e->bid), bid);
}

/* 慢轮改档成功后同步更新同轮共享缓存中的 priority，防止后续 restart/bgkill/pulse 读到改档前旧值 */
static void rst_update_cached_priority(pid_t pid, int32_t priority) {
    if (s_napp_procs <= 0 || s_app_proc_epoch != s_round_epoch) return;
    if (s_app_procs[s_napp_procs - 1].pid == pid) {
        s_app_procs[s_napp_procs - 1].priority = priority;
        return;
    }
    for (int j = 0; j < s_napp_procs; j++) {
        if (s_app_procs[j].pid == pid) {
            s_app_procs[j].priority = priority;
            return;
        }
    }
}

static void bgkill_observe_foreground_apps(void);

static void pulse_round(memorystatus_priority_entry_t *list, int count) {
    if (g_npulse_rules == 0 || g_pulse_cfg_file_absent) return;

    /* 回收已结束的 pulse 工作线程 (M4)；H1: running 用 acquire 读，
     * 为 0 时 worker 的收尾写（清逻辑态）对主线程全部可见，join 安全 */
    if (g_pulse_thread_has_handle &&
        !atomic_load_explicit(&g_pulse_thread_running, memory_order_acquire)) {
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
    bgkill_observe_foreground_apps();

    pid_t best_ctrl_pid = 0;
    char best_ctrl_bid[128] = {0};
    uint64_t best_ctrl_fp = 0;
    time_t best_ctrl_lag = -1;
    time_t best_ctrl_lag_max = -1;   /* v0.8.6 B0 */

    /* 1. 维护队列：若队列中的 App 已回前台 (BAND_FOREGROUND)、
     * 处于系统抬升档或进程消失，移出队列 */
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
        if (!found || qcur == BAND_FOREGROUND || qcur == BAND_SYSTEM_ELEVATED) {
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

        /* v0.8.6 A4: off 规则在规则循环开头就清状态并退出，不再走状态推进、
         * 不产出 PULSE_BG_EST / PLS_SKIP reason=rule_off 噪声。
         * pulse_check_candidate 的 !enabled 防御与 worker 出队重检均保留，
         * 已入队的历史条目仍会被拦下；持锁位置不变（s_pulse_mutex）。 */
        if (!rule->enabled) {
            st->tracked_pid = 0;
            st->in_bg = 0;
            st->bg_start_mono = 0;
            st->last_fg_seen_mono = 0;
            st->first_bg_seen_mono = 0;
            st->bg_lag = -1;
            st->bg_lag_max = -1;
            st->pulsed_this_bg = 0;
            st->ctrl_this_bg = 0;
            st->pulse_late_this_bg = 0;
            st->diag_round = 0;
            st->diag_first_observe_s = -1;
            st->diag_gate_t_s = -1;
            st->appmon_in_fg = 0;
            st->appmon_bg_pending_mono = 0;
            continue;
        }

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
            st->last_fg_seen_mono = 0;
            st->first_bg_seen_mono = 0;  /* v0.8.6 B0 */
            st->bg_lag = -1;
            st->bg_lag_max = -1;         /* v0.8.6 B0 */
            st->pulsed_this_bg = 0;
            st->ctrl_this_bg = 0;
            st->pulse_late_this_bg = 0;
            st->diag_round = 0;             /* DIAG2: 内存观测字段清零 */
            st->diag_first_observe_s = -1;
            st->diag_gate_t_s = -1;
            st->appmon_in_fg = 0;
            st->appmon_bg_pending_mono = 0;
            continue;
        }

        /* 档位状态推进与 delay 计时 (Q2 / delay 裁定 / D1-a) */
        /* v0.8.6 B0: "最后前台"口径放宽到 jp_is_foreground(>=100，含 120)。
         * 只读地记入 delay 起点并清本段后台取证字段；下面各分支的 in_bg /
         * pulsed_this_bg / 门控语义完全保持原样（120 仍不进候选窗口）。 */
        if (jp_is_foreground(main_cur)) {
            st->last_fg_seen_mono = now_mono;
            st->first_bg_seen_mono = 0;
            st->bg_lag_max = -1;
            if (st->appmon_bg_pending_mono > 0 &&
                (now_mono < st->appmon_bg_pending_mono ||
                 (now_mono - st->appmon_bg_pending_mono) >= APPMON_BG_PENDING_MAX_SEC)) {
                st->appmon_bg_pending_mono = 0;
            }
        }

        if (main_cur == BAND_FOREGROUND) {
            st->in_bg = 0;
            st->bg_start_mono = 0;
            st->last_fg_seen_mono = now_mono;
            st->first_bg_seen_mono = 0;  /* v0.8.6 B0 */
            st->bg_lag = -1;
            st->bg_lag_max = -1;         /* v0.8.6 B0 */
            st->pulsed_this_bg = 0;
            st->ctrl_this_bg = 0;
            st->pulse_late_this_bg = 0;
            st->tracked_pid = main_pid;
            st->diag_round = 0;             /* DIAG2 */
            st->diag_first_observe_s = -1;
            st->diag_gate_t_s = -1;
        } else if (main_cur == BAND_SYSTEM_ELEVATED) {
            st->in_bg = 0;
            st->bg_start_mono = 0;
            /* v0.8.6 B0: 120 也是"最后 >=100"，只用于 delay 起点；
             * in_bg/pulsed/ctrl 的原有清零集合不变（系统抬升档不是 pulse 窗口）。 */
            st->last_fg_seen_mono = now_mono;
            st->first_bg_seen_mono = 0;  /* v0.8.6 B0 */
            st->bg_lag = -1;
            st->bg_lag_max = -1;         /* v0.8.6 B0 */
            st->ctrl_this_bg = 0;
            st->pulse_late_this_bg = 0;
            st->tracked_pid = main_pid;
            st->diag_round = 0;             /* DIAG2 */
            st->diag_first_observe_s = -1;
            st->diag_gate_t_s = -1;
        } else if (jp_is_candidate(main_cur)) {
            if (st->tracked_pid != main_pid) {
                st->tracked_pid = main_pid;
                st->in_bg = 1;
                st->bg_start_mono = now_mono;   /* 25s 死线起点不回推 */
                st->last_fg_seen_mono = 0;      /* 新 pid 不继承旧 fg 起点 */
                st->first_bg_seen_mono = now_mono;  /* v0.8.6 B0 */
                st->bg_lag = -1;               /* 新 pid 无前台参照，NA */
                st->bg_lag_max = -1;           /* v0.8.6 B0 */
                st->pulsed_this_bg = 0;
                st->ctrl_this_bg = 0;
                st->pulse_late_this_bg = 0;
                st->appmon_in_fg = 0;
                st->appmon_bg_pending_mono = 0;
                /* DIAG2: 新后台段：段序号+1，round 归零，band_in 快照，首观测=0 */
                st->diag_bg_seq++;
                st->diag_round = 0;
                st->diag_band_in = main_cur;
                st->diag_first_observe_s = 0;
                st->diag_gate_t_s = -1;
                JPLOG(JP_INFO, "PULSE_BG_EST app=%s src=now", rule->bid);
            } else if (!st->in_bg || st->bg_start_mono == 0) {
                st->in_bg = 1;
                st->pulse_late_this_bg = 0;
                st->appmon_in_fg = 0;
                st->appmon_bg_pending_mono = 0;
                /* v0.8.6 B0: bg_lag 由 first_bg_seen-last_fg_seen 得出，不再受 15s
                 * 截断；bg_start_mono 仍=首次观测，25s 死线语义原样。 */
                st->bg_start_mono = now_mono;
                if (st->first_bg_seen_mono <= 0) st->first_bg_seen_mono = now_mono;
                if (st->last_fg_seen_mono > 0 &&
                    st->first_bg_seen_mono >= st->last_fg_seen_mono) {
                    st->bg_lag = st->first_bg_seen_mono - st->last_fg_seen_mono;
                } else {
                    st->bg_lag = -1;
                }
                if (st->bg_lag >= 0 &&
                    (st->bg_lag_max < 0 || st->bg_lag > st->bg_lag_max)) {
                    st->bg_lag_max = st->bg_lag;
                }
                if (st->bg_lag >= 0) {
                    JPLOG(JP_INFO, "PULSE_BG_EST app=%s src=fg lag=%ld",
                          rule->bid, (long)st->bg_lag);
                } else {
                    JPLOG(JP_INFO, "PULSE_BG_EST app=%s src=now", rule->bid);
                }
                /* DIAG2: 恢复后台段（首观测在 bg_start 之后算出，非负秒） */
                st->diag_bg_seq++;
                st->diag_round = 0;
                st->diag_band_in = main_cur;
                st->diag_first_observe_s = (st->bg_start_mono > 0 && now_mono >= st->bg_start_mono)
                                           ? (time_t)(now_mono - st->bg_start_mono) : (time_t)-1;
                st->diag_gate_t_s = -1;
            }
        } else {
            /* 其他值一律不处理：不在候选档区间，不累计进入后台 delay 计时 (M3) */
            st->in_bg = 0;
            st->bg_start_mono = 0;
            st->last_fg_seen_mono = 0;
            st->first_bg_seen_mono = 0;  /* v0.8.6 B0 */
            st->bg_lag = -1;
            st->bg_lag_max = -1;         /* v0.8.6 B0 */
            st->pulse_late_this_bg = 0;
            st->tracked_pid = main_pid;
            st->diag_round = 0;             /* DIAG2 */
            st->diag_first_observe_s = -1;
            st->diag_gate_t_s = -1;
            st->appmon_in_fg = 0;
            st->appmon_bg_pending_mono = 0;
        }

        /* 检查 7 项触发条件 */
        uint64_t cand_fp = 0;
        int skip_ctrl = 0;
        st->diag_round++;  /* DIAG2: 本段候选评估次数（新增内存字段，不改判定语义） */
        int eligible = pulse_check_candidate(rule, st, main_pid, main_cur, main_path,
                                             now_mono, &cand_fp, &skip_ctrl);
        if (eligible) {
            if (atomic_load_explicit(&g_pulse_in_progress, memory_order_acquire) &&
                atomic_load_explicit(&g_pulse_current_pid, memory_order_acquire) == main_pid) {
                diag_skip_note(rule->bid, st->diag_bg_seq, DSK_ALREADY_DONE,
                               st->diag_round, now_mono - st->bg_start_mono, (int)main_cur);
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
                qe->bg_lag = st->bg_lag;
                qe->bg_lag_max = st->bg_lag_max;   /* v0.8.6 B0 */
                qe->locked = g_locked;
            } else {
                /* DIAG2: 已在队列或队列满；只加日志，入队行为不变 */
                diag_skip_note(rule->bid, st->diag_bg_seq, DSK_QUEUE_FULL,
                               st->diag_round, now_mono - st->bg_start_mono, (int)main_cur);
            }
        } else if (skip_ctrl) {
            /* 满 delay 时因 n 或 daymax 跳过，记录对照组候选 (M5 / Q2) */
            if (best_ctrl_pid == 0 || cand_fp > best_ctrl_fp) {
                best_ctrl_pid = main_pid;
                rst_str_copy(best_ctrl_bid, sizeof(best_ctrl_bid), rule->bid);
                best_ctrl_fp = cand_fp;
                best_ctrl_lag = st->bg_lag;
                best_ctrl_lag_max = st->bg_lag_max;   /* v0.8.6 B0 */
            }
        }
    }
    pthread_mutex_unlock(&s_pulse_mutex);

    /* 3. 对队列按 fp 从大到小排序 */
    pulse_sort_queue();

    /* 4. 若全局无 pulse 正在进行，且队列非空，弹出最大者并启动异步线程 */
    while (atomic_load_explicit(&g_pulse_in_progress, memory_order_acquire) == 0 &&
           g_npulse_queue > 0) {
        /* H1 spawn/join 守卫：旧 worker 仍在收尾（has_handle && running==1）→
         * 本轮不 spawn，条目保留在队列下轮重试，绝不 pthread_create 覆写未
         * join 的旧句柄（否则旧 joinable 线程泄漏，且旧线程最后的 running=0
         * 会清掉新线程的状态）。running==0（acquire）时 worker 收尾写已全部
         * 可见，join 安全；此后主循环内再无其他写者，create 时 running 必为
         * 0，spawn 路径永不与未 join 的旧 joinable 线程并存。 */
        if (g_pulse_thread_has_handle) {
            if (atomic_load_explicit(&g_pulse_thread_running, memory_order_acquire)) {
                break; /* 旧 worker 仍在收尾：下轮重试 */
            }
            pthread_join(g_pulse_thread, NULL);
            g_pulse_thread_has_handle = 0;
        }
        pulse_queue_entry_t target = g_pulse_queue[0];
        pulse_queue_remove_at(0);

        /* 出队启动线程时重新检查条件 (M2) */
        uint64_t reval_fp = 0;
        if (!pulse_revalidate_conditions(target.pid, target.bid, &reval_fp)) {
            continue; /* 条件不满足，放弃这次 pulse，尝试队列中下一个 */
        }
        target.fp_mb = reval_fp;

        if (g_got_sigterm || g_pulse_stop_requested) {
            return;
        }

        pulse_queue_entry_t *thread_arg = (pulse_queue_entry_t *)malloc(sizeof(pulse_queue_entry_t));
        if (!thread_arg) {
            break;
        }
        *thread_arg = target;

        pthread_mutex_lock(&s_pulse_mutex);
        pulse_app_state_t *st = pulse_get_app_state(target.bid, today_str);
        if (st) {
            st->pulsed_this_bg = 1; /* 每次进入后台最多 1 次 */
        }
        pthread_mutex_unlock(&s_pulse_mutex);

        atomic_store_explicit(&g_pulse_in_progress, 1, memory_order_relaxed);
        atomic_store_explicit(&g_pulse_current_pid, target.pid, memory_order_relaxed);
        rst_str_copy(g_pulse_current_bid, sizeof(g_pulse_current_bid), target.bid);

        /* H1: 先置 running(release) 再 create，作为新 worker 状态的权威置位 */
        atomic_store_explicit(&g_pulse_thread_running, 1, memory_order_release);
        if (pthread_create(&g_pulse_thread, NULL, pulse_worker_thread, thread_arg) == 0) {
            g_pulse_thread_has_handle = 1;
        } else {
            atomic_store_explicit(&g_pulse_thread_running, 0, memory_order_relaxed);
            free(thread_arg);
            atomic_store_explicit(&g_pulse_in_progress, 0, memory_order_relaxed);
            atomic_store_explicit(&g_pulse_current_pid, 0, memory_order_relaxed);
            g_pulse_current_bid[0] = '\0';
            pthread_mutex_lock(&s_pulse_mutex);
            pulse_app_state_t *st_rb = pulse_get_app_state(target.bid, today_str);
            if (st_rb) {
                st_rb->pulsed_this_bg = 0;
            }
            pthread_mutex_unlock(&s_pulse_mutex);
        }
        break;
    }

    /* 5. 若全局无 pulse 正在进行且队列为空，可启动对照组只读采样线程 (M5 / Q2)
     * 跟 pulse 共用「全局同时只做 1 个」名额，排队时 pulse 优先 */
    if (atomic_load_explicit(&g_pulse_in_progress, memory_order_acquire) == 0 &&
        g_npulse_queue == 0 && best_ctrl_pid > 0) {
        if (g_got_sigterm || g_pulse_stop_requested) {
            return;
        }

        /* H1: 同 pulse 的 spawn/join 守卫——旧 worker 仍在收尾（has_handle &&
         * running==1）则本轮不启动对照采样（best_ctrl 下轮会重新评估），绝不
         * pthread_create 覆写未 join 的旧句柄 */
        if (g_pulse_thread_has_handle) {
            if (atomic_load_explicit(&g_pulse_thread_running, memory_order_acquire)) {
                return; /* 旧 worker 仍在收尾：下轮重试 */
            }
            pthread_join(g_pulse_thread, NULL);
            g_pulse_thread_has_handle = 0;
        }

        pulse_ctrl_arg_t *carg = (pulse_ctrl_arg_t *)malloc(sizeof(pulse_ctrl_arg_t));
        if (carg) {
            rst_str_copy(carg->bid, sizeof(carg->bid), best_ctrl_bid);
            carg->pid = best_ctrl_pid;
            carg->bg_lag = best_ctrl_lag;
            carg->bg_lag_max = best_ctrl_lag_max;   /* v0.8.6 B0 */
            carg->locked = g_locked;

            atomic_store_explicit(&g_pulse_in_progress, 1, memory_order_relaxed);
            atomic_store_explicit(&g_pulse_current_pid, best_ctrl_pid, memory_order_relaxed);
            rst_str_copy(g_pulse_current_bid, sizeof(g_pulse_current_bid), best_ctrl_bid);

            /* H1: 先置 running(release) 再 create，作为新 worker 状态的权威置位 */
            atomic_store_explicit(&g_pulse_thread_running, 1, memory_order_release);
            if (pthread_create(&g_pulse_thread, NULL, pulse_ctrl_worker_thread, carg) == 0) {
                g_pulse_thread_has_handle = 1;
                pthread_mutex_lock(&s_pulse_mutex);
                pulse_app_state_t *cst = pulse_get_app_state(best_ctrl_bid, today_str);
                if (cst) {
                    cst->ctrl_this_bg = 1;
                }
                pthread_mutex_unlock(&s_pulse_mutex);
            } else {
                atomic_store_explicit(&g_pulse_thread_running, 0, memory_order_relaxed);
                free(carg);
                atomic_store_explicit(&g_pulse_in_progress, 0, memory_order_relaxed);
                atomic_store_explicit(&g_pulse_current_pid, 0, memory_order_relaxed);
                g_pulse_current_bid[0] = '\0';
            }
        }
    }
}

/* D-003 + v0.8.7: 门控评估与精确休眠时长计算（毫秒级）。
 * 1. 若 APPMON 已收到切后台推送但 XNU 档位尚处 >=100 过渡期（<3s），开门并以 250ms 快检紧盯跌落；
 * 2. 若候选已进后台且未满 delay_s，精确睡剩余秒数（<=5s），到点即发信号；
 * 3. 若已满 delay_s 但仍在 25s 死线内，维持 <=5s 门控巡检。 */
static int pulse_next_wait_ms(time_t now_mono, int default_secs, int *gate_open_out) {
    int wait_ms = (default_secs > 0) ? (default_secs * 1000) : 1000;
    if (gate_open_out) *gate_open_out = 0;
    if (g_npulse_rules <= 0 || g_pulse_cfg_file_absent) return wait_ms;

    int open = 0;
    time_t wall_now = time(NULL);
    pthread_mutex_lock(&s_pulse_mutex);
    for (int i = 0; i < g_npulse_app_states; i++) {
        pulse_app_state_t *st = &g_pulse_app_states[i];
        if (st->pulsed_this_bg || st->ctrl_this_bg || st->pulse_late_this_bg ||
            st->restore_tripped_today) {
            st->appmon_bg_pending_mono = 0;
            continue;
        }

        const pulse_rule_t *rule = NULL;
        for (int r = 0; r < g_npulse_rules; r++) {
            if (g_pulse_rules[r].enabled && strcmp(g_pulse_rules[r].bid, st->bid) == 0) {
                rule = &g_pulse_rules[r];
                break;
            }
        }
        if (!rule) {
            st->appmon_bg_pending_mono = 0;
            continue;
        }
        if (pulse_circuit_skip_reason(st, wall_now + PULSE_DEADLINE_BG_SEC,
                                      now_mono + PULSE_DEADLINE_BG_SEC) != NULL) {
            st->appmon_bg_pending_mono = 0;
            continue;
        }

        /* 分支 A: APPMON 已收到退后台通知，但 XNU 档位尚未跌下 100（过渡动画 <3s） */
        if (!st->in_bg && st->appmon_bg_pending_mono > 0) {
            if (now_mono >= st->appmon_bg_pending_mono &&
                (now_mono - st->appmon_bg_pending_mono) < APPMON_BG_PENDING_MAX_SEC) {
                open = 1;
                if (wait_ms > 250) wait_ms = 250;
            } else {
                st->appmon_bg_pending_mono = 0;
            }
            continue;
        }

        /* 分支 B: 已处于候选档区间且在 25s 死线内 */
        if (!st->in_bg || st->tracked_pid <= 0 || st->bg_start_mono <= 0) continue;

        time_t elapsed = now_mono - st->bg_start_mono;
        /* elapsed<0 视为时钟异常回拨，保守不打开门控 */
        if (elapsed >= 0 && elapsed < (time_t)PULSE_DEADLINE_BG_SEC) {
            if (st->diag_gate_t_s < 0) {
                st->diag_gate_t_s = elapsed;
            }
            open = 1;
            int gate_cap_ms = PULSE_FAST_GATE_POLL_SEC * 1000;
            if (wait_ms > gate_cap_ms) wait_ms = gate_cap_ms;

            time_t delay_elapsed = elapsed;
            if (st->last_fg_seen_mono > 0 && st->first_bg_seen_mono >= st->last_fg_seen_mono) {
                delay_elapsed = now_mono - st->last_fg_seen_mono;
            }
            if (delay_elapsed >= 0 && delay_elapsed < (time_t)rule->delay_s) {
                time_t rem_s = (time_t)rule->delay_s - delay_elapsed;
                int rem_ms = (int)(rem_s * 1000);
                if (rem_ms > 0 && rem_ms < wait_ms) {
                    wait_ms = rem_ms;
                }
            }
        }
    }
    pthread_mutex_unlock(&s_pulse_mutex);
    if (gate_open_out) *gate_open_out = open;
    return wait_ms;
}

static int pulse_fast_gate_open(time_t now_mono) {
    int open = 0;
    (void)pulse_next_wait_ms(now_mono, 60, &open);
    return open;
}

/* P1-1: 是否存在启用的 pulse 规则；仅主线程调用（与 pulse 规则数组访问约定一致） */
static int pulse_any_enabled_rule(void) {
    for (int i = 0; i < g_npulse_rules; i++) {
        if (g_pulse_rules[i].enabled) return 1;
    }
    return 0;
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
    /* M1: O_NOFOLLOW 打开 tmp（原 fopen("w") 缺 O_NOFOLLOW） */
    /* INT1：lstat 前置校验（symlink/类型/root 属主/组或其他可写），异常拒绝写入 */
    if (jp_secure_precheck(RESTART_STATE_TMP_PATH) != 0) return;
    int fd = open(RESTART_STATE_TMP_PATH, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return;
    if (jp_secure_fd_check(fd, 0644) != 0) { close(fd); unlink(RESTART_STATE_TMP_PATH); return; }
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(RESTART_STATE_TMP_PATH); return; }

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

    /* M1: 统一原子提交——fsync 数据 → rename → fsync 目录；失败保留旧状态文件 */
    if (jp_atomic_commit(f, RESTART_STATE_TMP_PATH, RESTART_STATE_PATH) != 0) {
        return;
    }
    struct stat st;
    if (stat(RESTART_STATE_PATH, &st) == 0) {
        g_rst_state_mtime = st.st_mtime;
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


/* C4: 读取 band_settled（在 WAIT_SETTLE 读：距 t_launch >= 10 秒后的第一轮）
 * T3-IMP 3.3: orig 快照过 TTL 视为未知 → 现读兜底 → 再退回 cur。 */
static int32_t rst_c4_read_band_settled(pid_t new_pid, int32_t cur_band) {
    applied_t *a = applied_find(new_pid);
    if (a && a->orig_known &&
        jp_snapshot_fresh(a->orig_seen_mono, applied_mono_now())) {
        return a->orig;
    }
    int32_t live = cur_band;
    if (jp_read_current_band(new_pid, &live) == 0) return live;
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
    /* v0.8.6 C3: 同 epoch 内复用慢轮主扫描缓存，不再对同一 list 重复 proc_pidpath；
     * 跨轮 / 快轮 / 显式失效（epoch=0）一律重建，fail-closed。 */
    if (s_napp_procs >= 0 && s_app_proc_epoch == s_round_epoch) return;
    s_napp_procs = 0;
    s_app_proc_epoch = s_round_epoch;
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
            s_app_procs[s_napp_procs].state = list[i].state;  /* v0.8.6 C3 */
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

    /* v0.8.6 C3: 删除原无条件 s_napp_procs=-1；慢轮主扫描已按本轮 epoch
     * 填充共享缓存，这里直接复用；跨轮/快轮由 rst_ensure_app_procs 自行重建。 */

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
        uint32_t main_state = 0;
        int main_state_known = 0;
        if (!restart_is_blocked_bid(rule->bid)) {
            for (int j = 0; j < s_napp_procs; j++) {
                if (strcmp(s_app_procs[j].bid, rule->bid) == 0) {
                    main_pid = s_app_procs[j].pid;
                    main_cur = s_app_procs[j].priority;
                    main_state = s_app_procs[j].state;
                    main_state_known = 1;
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
        /* H1: 原子读，消除与 worker 收尾竞态下的 UB */
        if (atomic_load_explicit(&g_pulse_in_progress, memory_order_acquire) &&
            atomic_load_explicit(&g_pulse_current_pid, memory_order_acquire) == main_pid) {
            restart_log_skip(rule, r_state, "pulse_in_progress", main_cur, now_mono);
            continue;
        }

        if (main_pid != r_state->tracked_pid) {
            r_state->tracked_pid = main_pid;
            r_state->in_bg = (main_cur < BAND_PROTECT_DEFAULT) ? 1 : 0;
            r_state->bg_start_mono = (main_cur < BAND_PROTECT_DEFAULT) ? now_mono : 0;
            r_state->sample_count = 0;
        }

        int bg_sec_elapsed = 0;
        const char *skip_reason = NULL;

        /* 判定 1: 后台计时 (任何一轮档位 >= BAND_PROTECT_DEFAULT，后台计时清零；连续低于该档满 bg 秒) */
        if (main_cur >= BAND_PROTECT_DEFAULT) {
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

        /* 判定 2: 系统自然档位（<= BAND_BACKGROUND 或 BAND_FREEZER 挂起冷冻态允许杀） */
        int32_t sys_band = jp_natural_band(main_pid, main_cur);

        if (!skip_reason && !jp_kill_allowed_with_state(sys_band, main_state, main_state_known)) {
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

        /* 动手前实时复核一次档位与冷冻状态，杜绝解冻/回前台瞬间误杀 */
        int32_t rechk_band = -1;
        uint32_t rechk_state = 0;
        if (jp_read_current_entry(main_pid, &rechk_band, &rechk_state) == 0) {
            if (rechk_band >= BAND_PROTECT_DEFAULT) {
                r_state->in_bg = 0;
                r_state->bg_start_mono = 0;
                r_state->sample_count = 0;
                restart_log_skip(rule, r_state, "fg", rechk_band, now_mono);
                continue;
            }
            int32_t rechk_nat = jp_natural_band(main_pid, rechk_band);
            if (!jp_kill_allowed_with_state(rechk_nat, rechk_state, 1)) {
                r_state->sample_count = 0;
                restart_log_skip(rule, r_state, "band", rechk_nat, now_mono);
                continue;
            }
            sys_band = rechk_nat;
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

/* =========================================================================
 * 后台定时清理：判定与执行 (v0.8.0 卡 K)
 * ========================================================================= */

static int bgkill_rule_cfg_equal(const bgkill_rule_t *a, const bgkill_rule_t *b) {
    if (!a || !b) return 0;
    if (strcmp(a->bid, b->bid) != 0) return 0;
    if (a->enabled != b->enabled) return 0;
    if (a->min != b->min) return 0;
    if (a->max != b->max) return 0;
    return 1;
}

static bgkill_rule_state_t *bgkill_get_rule_state(const char *bid) {
    if (!bid || bid[0] == '\0') return NULL;
    for (int i = 0; i < g_nbgkill_rule_states; i++) {
        if (strcmp(g_bgkill_rule_states[i].bid, bid) == 0) {
            return &g_bgkill_rule_states[i];
        }
    }
    if (g_nbgkill_rule_states >= BGKILL_MAX_RULES) return NULL;
    bgkill_rule_state_t *st = &g_bgkill_rule_states[g_nbgkill_rule_states++];
    memset(st, 0, sizeof(*st));
    strncpy(st->bid, bid, sizeof(st->bid) - 1);
    st->bid[sizeof(st->bid) - 1] = '\0';
    strncpy(st->last_kill_hhmm, "-", sizeof(st->last_kill_hhmm) - 1);
    return st;
}

static void bgkill_check_date_reset(bgkill_rule_state_t *st, const struct tm *tm_now) {
    int y = tm_now->tm_year + 1900;
    int m = tm_now->tm_mon + 1;
    int d = tm_now->tm_mday;
    if (st->today_y != y || st->today_m != m || st->today_d != d) {
        st->today_count = 0;
        st->today_y = y;
        st->today_m = m;
        st->today_d = d;
        strncpy(st->last_kill_hhmm, "-", sizeof(st->last_kill_hhmm) - 1);
    }
}

static void bgkill_write_status(FILE *m) {
    if (!m) return;
    fprintf(m, "bgkill_support=1\n");
    fprintf(m, "bgkill_master=%s\n", g_bgkill_master ? "on" : "off");
    fprintf(m, "bgkill_bandgate=%s\n", g_bgkill_bandgate ? "on" : "off");
    fprintf(m, "bgkill_cfg_bad=%d\n", g_bgkill_cfg_bad_count);
    for (int i = 0; i < g_nbgkill_rules; i++) {
        bgkill_rule_t *rule = &g_bgkill_rules[i];
        bgkill_rule_state_t *st = bgkill_get_rule_state(rule->bid);
        int today = st ? st->today_count : 0;
        const char *last = (st && st->last_kill_hhmm[0] != '\0') ? st->last_kill_hhmm : "-";
        fprintf(m, "bgkill %s today=%d last=%s\n", rule->bid, today, last);
    }
}

static int bgkill_load_config(void) {
    int file_absent = 0;
    FILE *f = NULL;
    int rc = jp_config_open(BGKILL_CONFIG_PATH, &s_bgkill_cfg_stamp, "BGK", &file_absent, &f);

    if (rc == 0) {
        return g_bgkill_cfg_file_absent ? -1 : 0;
    }
    if (rc < 0) {
        return g_bgkill_cfg_file_absent ? -1 : 0;
    }

    if (file_absent || !f) {
        g_bgkill_cfg_file_absent = 1;
        pthread_rwlock_wrlock(&s_rule_lock);
        g_nbgkill_rules = 0;
        pthread_rwlock_unlock(&s_rule_lock);
        g_bgkill_cfg_bad_count = 0;
        g_bgkill_bandgate = 1;   /* 配置消失时回到安全的默认门禁 */
        return -1;
    }

    bgkill_rule_t parsed_rules[BGKILL_MAX_RULES];
    int nparsed = 0;
    int bad_line_count = 0;
    int parsed_master = 1; /* default on */
    int parsed_bandgate = 1;
    int global_count = 0;
    int bandgate_count = 0;

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
            JPLOG(JP_ERR, "%-6s cfg_bad line=%d text=%s", "BGK", line_no, raw_text);
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

        if (ntokens >= 1 && strcmp(tokens[0], "global") == 0) {
            global_count++;
            if (global_count > 1 || ntokens != 2 || (strcmp(tokens[1], "on") != 0 && strcmp(tokens[1], "off") != 0)) {
                /* global 写了两次，或值不是 on 和 off，都按坏行处理，总开关保持 on */
                bad_line_count++;
                JPLOG(JP_ERR, "%-6s cfg_bad line=%d text=%s", "BGK", line_no, raw_text);
                parsed_master = 1;
                continue;
            }
            parsed_master = (strcmp(tokens[1], "on") == 0) ? 1 : 0;
            continue;
        }

        /* T3-IMP 3.2: 回滚开关 bandgate=0/1（单行，控制字，非规则行） */
        if (ntokens >= 1 && strncmp(tokens[0], "bandgate=", 9) == 0) {
            bandgate_count++;
            const char *bv = tokens[0] + 9;
            if (bandgate_count > 1 || ntokens != 1 ||
                (strcmp(bv, "0") != 0 && strcmp(bv, "1") != 0)) {
                bad_line_count++;
                JPLOG(JP_ERR, "%-6s cfg_bad line=%d text=%s", "BGK", line_no, raw_text);
                parsed_bandgate = 1;   /* 非法值按安全默认处理 */
                continue;
            }
            parsed_bandgate = (strcmp(bv, "0") == 0) ? 0 : 1;
            continue;
        }

        /* 规则行格式: bid:<bid> on|off min=<分钟> max=<每天次数> (4 个 token) */
        if (ntokens != 4) {
            bad_line_count++;
            JPLOG(JP_ERR, "%-6s cfg_bad line=%d text=%s", "BGK", line_no, raw_text);
            continue;
        }

        if (strncmp(tokens[0], "bid:", 4) != 0) {
            bad_line_count++;
            JPLOG(JP_ERR, "%-6s cfg_bad line=%d text=%s", "BGK", line_no, raw_text);
            continue;
        }

        const char *bid = tokens[0] + 4;
        if (bid[0] == '\0' || strlen(bid) >= sizeof(parsed_rules[0].bid) || restart_is_blocked_bid(bid)) {
            bad_line_count++;
            JPLOG(JP_ERR, "%-6s cfg_bad line=%d text=%s", "BGK", line_no, raw_text);
            continue;
        }

        int enabled = -1;
        if (strcmp(tokens[1], "on") == 0) {
            enabled = 1;
        } else if (strcmp(tokens[1], "off") == 0) {
            enabled = 0;
        } else {
            bad_line_count++;
            JPLOG(JP_ERR, "%-6s cfg_bad line=%d text=%s", "BGK", line_no, raw_text);
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
            JPLOG(JP_ERR, "%-6s cfg_bad line=%d text=%s", "BGK", line_no, raw_text);
            continue;
        }

        int has_min = 0, has_max = 0;
        int32_t val_min = 0, val_max = 0;
        int bad = 0;

        for (int k = 2; k < 4; k++) {
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

            if (strcmp(key, "min") == 0) {
                if (has_min) { bad = 1; break; }
                if (v < BGKILL_MIN_MIN || v > BGKILL_MAX_MIN) { bad = 1; break; }
                val_min = (int32_t)v;
                has_min = 1;
            } else if (strcmp(key, "max") == 0) {
                if (has_max) { bad = 1; break; }
                if (v < BGKILL_MIN_MAX || v > BGKILL_MAX_MAX) { bad = 1; break; }
                val_max = (int32_t)v;
                has_max = 1;
            } else {
                bad = 1;
                break;
            }
        }

        if (bad || !has_min || !has_max) {
            bad_line_count++;
            JPLOG(JP_ERR, "%-6s cfg_bad line=%d text=%s", "BGK", line_no, raw_text);
            continue;
        }

        if (nparsed >= BGKILL_MAX_RULES) {
            bad_line_count++;
            JPLOG(JP_ERR, "%-6s cfg_bad line=%d text=%s", "BGK", line_no, raw_text);
            continue;
        }

        bgkill_rule_t *br = &parsed_rules[nparsed++];
        strncpy(br->bid, bid, sizeof(br->bid) - 1);
        br->bid[sizeof(br->bid) - 1] = '\0';
        br->enabled = enabled;
        br->min = val_min;
        br->max = val_max;
    }
    fclose(f);

    g_bgkill_cfg_bad_count = bad_line_count;

    /* master 与 bandgate 都是单值控制字：配置变更即时生效，不必重启 */
    if (parsed_master != g_bgkill_master) {
        g_bgkill_master = parsed_master;
        JPLOG(JP_INFO, "%-6s master=%s", "BGK", g_bgkill_master ? "on" : "off");
    }
    if (parsed_bandgate != g_bgkill_bandgate) {
        g_bgkill_bandgate = parsed_bandgate;
        JPLOG(JP_INFO, "%-6s bandgate=%s", "BGK",
              g_bgkill_bandgate ? "on" : "off");
    }
    g_bgkill_first_load = 0;

    int changed = 0;
    if (file_absent != g_bgkill_cfg_file_absent) {
        changed = 1;
    } else if (nparsed != g_nbgkill_rules) {
        changed = 1;
    } else {
        for (int i = 0; i < nparsed; i++) {
            if (!bgkill_rule_cfg_equal(&parsed_rules[i], &g_bgkill_rules[i])) {
                changed = 1;
                break;
            }
        }
    }

    if (changed) {
        JPLOG(JP_INFO, "%-6s cfg rules=%d file=%s",
              "BGK", nparsed, file_absent ? "absent" : "ok");
        for (int i = 0; i < nparsed; i++) {
            JPLOG(JP_DEBUG, "%-6s cfg_rule i=%d bid=%s on=%d min=%d max=%d",
                  "BGK", i, parsed_rules[i].bid, parsed_rules[i].enabled,
                  (int)parsed_rules[i].min, (int)parsed_rules[i].max);
        }
        /* H2: 整块替换规则数组必须在写锁内完成（worker 可能正在读） */
        pthread_rwlock_wrlock(&s_rule_lock);
        memcpy(g_bgkill_rules, parsed_rules, sizeof(bgkill_rule_t) * nparsed);
        g_nbgkill_rules = nparsed;
        pthread_rwlock_unlock(&s_rule_lock);
        g_bgkill_cfg_file_absent = file_absent;

        int new_nbgkill_rule_states = 0;
        for (int i = 0; i < g_nbgkill_rule_states; i++) {
            int found = 0;
            for (int j = 0; j < g_nbgkill_rules; j++) {
                if (strcmp(g_bgkill_rule_states[i].bid, g_bgkill_rules[j].bid) == 0) {
                    found = 1;
                    break;
                }
            }
            if (found) {
                if (new_nbgkill_rule_states != i) {
                    g_bgkill_rule_states[new_nbgkill_rule_states] = g_bgkill_rule_states[i];
                }
                new_nbgkill_rule_states++;
            }
        }
        g_nbgkill_rule_states = new_nbgkill_rule_states;
    }

    return 0;
}

static int bgkill_read_current_entry(pid_t pid, int32_t *band_out,
                                     uint32_t *state_out, int *state_known_out) {
    if (state_known_out) *state_known_out = 0;
    if (pid <= 0 || !band_out) return -1;
    uint32_t st_val = 0;
    if (jp_read_current_entry(pid, band_out, &st_val) == 0) {
        if (state_out) *state_out = st_val;
        if (state_known_out) *state_known_out = 1;
        return 0;
    }
    if (errno == ESRCH) return -1;
    int count = 0;
    memorystatus_priority_entry_t *list = get_list(&count);
    if (!list) return -1;
    int found = 0;
    for (int i = 0; i < count; i++) {
        if (list[i].pid == pid) {
            *band_out = list[i].priority;
            if (state_out) *state_out = list[i].state;
            if (state_known_out) *state_known_out = 1;
            found = 1;
            break;
        }
    }
    free(list);
    return found ? 0 : -1;
}

static void bgkill_clear_pid_tracking(bgkill_rule_state_t *st) {
    if (!st) return;
    st->tracked_pid = 0;
    st->in_bg = 0;
    st->bg_start_mono = 0;
    st->bg_rounds = 0;
    st->ever_fg = 0;
    st->prewarm_hold = 0;
    st->prewarm_logged = 0;
}

/* 同步观察 s_app_procs 中的前台出现记录（供门控快轮 pulse_round 顺带复用）：
 * 一旦某规则主进程上过前台 (jp_is_foreground)，该 pid 生命周期内闩锁 ever_fg=1 并重置后台计时。 */
static void bgkill_observe_foreground_apps(void) {
    if (g_bgkill_cfg_file_absent || !g_bgkill_master || s_napp_procs <= 0) return;
    for (int i = 0; i < g_nbgkill_rules; i++) {
        bgkill_rule_t *rule = &g_bgkill_rules[i];
        if (!rule->enabled) continue;
        bgkill_rule_state_t *st = bgkill_get_rule_state(rule->bid);
        if (!st) continue;
        for (int j = 0; j < s_napp_procs; j++) {
            if (strcmp(s_app_procs[j].bid, rule->bid) == 0) {
                pid_t pid = s_app_procs[j].pid;
                int32_t cur = s_app_procs[j].priority;
                if (jp_is_foreground(cur)) {
                    st->tracked_pid = pid;
                    st->in_bg = 0;
                    st->bg_start_mono = 0;
                    st->bg_rounds = 0;
                    st->ever_fg = 1;
                    st->prewarm_hold = 0;
                    st->prewarm_logged = 0;
                }
                break;
            }
        }
    }
}

/* v0.8.6 C1: band 的详细档位并入同一行输出；仍共用 60s/原因变更限流。
 * sys_band/cur_band 仅 band 原因有效，其他原因传 -1（不输出）。 */
static void bgkill_log_skip(const bgkill_rule_t *rule, bgkill_rule_state_t *st,
                            const char *reason, time_t now_mono,
                            int32_t sys_band, int32_t cur_band) {
    int should_log = 0;
    if (st->last_skip_mono == 0 || now_mono - st->last_skip_mono >= 60) {
        should_log = 1;
    } else if (strcmp(st->last_skip_reason, reason) != 0) {
        should_log = 1;
    }

    if (should_log) {
        st->last_skip_mono = now_mono;
        strncpy(st->last_skip_reason, reason, sizeof(st->last_skip_reason) - 1);
        st->last_skip_reason[sizeof(st->last_skip_reason) - 1] = '\0';
        if (strcmp(reason, "band") == 0) {
            JPLOG(JP_DEBUG, "%-6s skip=%s bid=%s sys_band=%d cur_band=%d",
                  "BGK", reason, rule->bid, (int)sys_band, (int)cur_band);
        } else {
            JPLOG(JP_DEBUG, "%-6s skip=%s bid=%s", "BGK", reason, rule->bid);
        }
    }
}

static void bgkill_round(memorystatus_priority_entry_t *list, int count) {
    if (g_bgkill_cfg_file_absent || !g_bgkill_master) {
        for (int i = 0; i < g_nbgkill_rule_states; i++) {
            bgkill_clear_pid_tracking(&g_bgkill_rule_states[i]);
        }
        return;
    }

    /* 取不到优先级列表时不要清空已有状态，跳过本轮即可 */
    if (!list || count <= 0) {
        return;
    }

    rst_ensure_app_procs(list, count);

    struct timespec ts_mono;
    clock_gettime(CLOCK_MONOTONIC, &ts_mono);
    time_t now_mono = ts_mono.tv_sec;

    time_t now_wall = time(NULL);
    struct tm tm_now;
    localtime_r(&now_wall, &tm_now);

    for (int i = 0; i < g_nbgkill_rules; i++) {
        bgkill_rule_t *rule = &g_bgkill_rules[i];
        bgkill_rule_state_t *st = bgkill_get_rule_state(rule->bid);
        if (!st) continue;

        bgkill_check_date_reset(st, &tm_now);

        if (!rule->enabled) {
            bgkill_clear_pid_tracking(st);
            continue;
        }

        /* 查找该 bid 的用户 App 主进程 */
        pid_t main_pid = 0;
        int32_t main_cur = 0;
        for (int j = 0; j < s_napp_procs; j++) {
            if (strcmp(s_app_procs[j].bid, rule->bid) == 0) {
                main_pid = s_app_procs[j].pid;
                main_cur = s_app_procs[j].priority;
                break;
            }
        }

        if (main_pid <= 0) {
            bgkill_clear_pid_tracking(st);
            continue;
        }

        /* pid 变了：清零，按新进程重新建账 */
        if (main_pid != st->tracked_pid) {
            st->tracked_pid = main_pid;
            st->ever_fg = jp_is_foreground(main_cur) ? 1 : 0;
            st->prewarm_logged = 0;
            if (main_cur < BAND_PROTECT_DEFAULT) {
                st->in_bg = 1;
                st->bg_start_mono = now_mono;
                st->bg_rounds = 1;
                uint64_t init_fp_bytes = 0;
                if (!st->ever_fg && memp_get_phys_footprint(main_pid, &init_fp_bytes) == 0 &&
                    (init_fp_bytes / (1024ULL * 1024ULL)) < (uint64_t)BGKILL_PREWARM_MAX_FP_MB) {
                    st->prewarm_hold = 1;
                } else {
                    st->prewarm_hold = 0;
                }
            } else {
                st->in_bg = 0;
                st->bg_start_mono = 0;
                st->bg_rounds = 0;
                st->prewarm_hold = 0;
            }
        } else {
            if (jp_is_foreground(main_cur)) {
                st->ever_fg = 1;
                st->prewarm_hold = 0;
                st->prewarm_logged = 0;
            }
            /* 档位 >= BAND_PROTECT_DEFAULT 清零; 低于该档进入记 bg_start_mono, 之后每轮 bg_rounds++ */
            if (main_cur >= BAND_PROTECT_DEFAULT) {
                st->in_bg = 0;
                st->bg_start_mono = 0;
                st->bg_rounds = 0;
            } else {
                if (!st->in_bg) {
                    st->in_bg = 1;
                    st->bg_start_mono = now_mono;
                    st->bg_rounds = 1;
                } else {
                    st->bg_rounds++;
                }
                /* 预热暂缓中的进程若在两次慢轮间隙被用户短暂打开又切回后台（错过前台采样，
                 * 但 UIKit 已初始化使内存跨过 40MB），立刻解除暂缓并从当前时刻起重新倒计时 */
                if (st->prewarm_hold) {
                    uint64_t chk_fp_bytes = 0;
                    if (memp_get_phys_footprint(main_pid, &chk_fp_bytes) == 0 &&
                        (chk_fp_bytes / (1024ULL * 1024ULL)) >= (uint64_t)BGKILL_PREWARM_MAX_FP_MB) {
                        st->prewarm_hold = 0;
                        st->ever_fg = 1;
                        st->prewarm_logged = 0;
                        st->bg_start_mono = now_mono;
                        st->bg_rounds = 1;
                    }
                }
            }
        }

        /* 如果在前台快筛区，不触发也不打日志（真门槛在条件 6 的 jp_kill_allowed） */
        if (main_cur >= BAND_PROTECT_DEFAULT || !st->in_bg) {
            continue;
        }

        /* 触发条件 2: 时间未满不打日志 */
        int bg_sec_elapsed = (int)(now_mono - st->bg_start_mono);
        if (rule->min > 0) {
            if (bg_sec_elapsed < rule->min * 60) {
                continue;
            }
        } else {
            /* min == 0: bg_rounds >= 2 并且 now - bg_start_mono >= 5 秒 */
            if (st->bg_rounds < 2 || bg_sec_elapsed < 5) {
                continue;
            }
        }

        /* 触发条件 3: pulse 进行中跳过该 pid */
        /* H1: 原子读，消除与 worker 收尾竞态下的 UB */
        if (atomic_load_explicit(&g_pulse_in_progress, memory_order_acquire) &&
            atomic_load_explicit(&g_pulse_current_pid, memory_order_acquire) == main_pid) {
            bgkill_log_skip(rule, st, "pulse", now_mono, -1, -1);
            continue;
        }

        /* 触发条件 4: 重启状态机正在处理该 bid */
        if (g_rst_sm.state != RST_STATE_IDLE && strcmp(g_rst_sm.bid, rule->bid) == 0) {
            bgkill_log_skip(rule, st, "rst_busy", now_mono, -1, -1);
            continue;
        }

        /* 触发条件 5: 当天次数 < max */
        if (st->today_count >= rule->max) {
            bgkill_log_skip(rule, st, "daymax", now_mono, -1, -1);
            continue;
        }

        /* 触发条件 6: 现读一次档位与状态位（前台快筛 + 冷冻室实时核验） */
        int32_t cur_band = -1;
        uint32_t cur_state = 0;
        int cur_state_known = 0;
        if (bgkill_read_current_entry(main_pid, &cur_band, &cur_state, &cur_state_known) != 0 ||
            cur_band >= BAND_PROTECT_DEFAULT) {
            if (cur_band >= 0 && jp_is_foreground(cur_band)) {
                st->ever_fg = 1;
                st->prewarm_hold = 0;
                st->prewarm_logged = 0;
            }
            st->in_bg = 0;
            st->bg_start_mono = 0;
            st->bg_rounds = 0;
            bgkill_log_skip(rule, st, "recheck", now_mono, -1, -1);
            continue;
        }

        /* 触发条件 7 (T3-IMP 3.2 + v0.8.6): 系统自然档位安全门槛，与 restart 共用回退链。
         * <= BAND_BACKGROUND(30) 或经实时 state 确认为 XNU 冷冻室闲置冻结态 (BAND_FREEZER=75,
         * Suspended|Frozen 且无 Assertion/Active) 放行；真正后台活跃播放/通话/任务仍拦截。
         * bandgate=0 时整段短路，行为回到 v0.8.1（无需重编包）。 */
        int32_t sys_band = cur_band;
        if (g_bgkill_bandgate) {
            sys_band = jp_natural_band(main_pid, cur_band);
            if (!jp_kill_allowed_with_state(sys_band, cur_state, cur_state_known)) {
                st->in_bg = 0;
                st->bg_start_mono = 0;
                st->bg_rounds = 0;
                /* v0.8.6 C1: 与 bgkill_log_skip 合并为一行，保留 60s/原因变更限流 */
                bgkill_log_skip(rule, st, "band", now_mono, sys_band, cur_band);
                continue;
            }
        }

        char fp_str[32] = "NA";
        uint64_t fp_bytes = 0;
        uint64_t fp_mb = 0;
        int fp_ok = (memp_get_phys_footprint(main_pid, &fp_bytes) == 0);
        if (fp_ok) {
            fp_mb = fp_bytes / (1024ULL * 1024ULL);
            snprintf(fp_str, sizeof(fp_str), "%llu", (unsigned long long)fp_mb);
        }

        /* 触发条件 8: 跳过系统 dasd 应用预热空壳（从未进过前台且物理内存 < 40MB）。
         * 只要进过前台 (ever_fg=1)，无论内存多小均照常按时清理，保持「不看内存、用过必清」性质；
         * <40MB 兜底仅用于放行守护进程重启前已打开用过的后台存量进程。 */
        if (!st->ever_fg) {
            if (fp_ok && fp_mb < (uint64_t)BGKILL_PREWARM_MAX_FP_MB) {
                st->prewarm_hold = 1;
                if (!st->prewarm_logged) {
                    st->prewarm_logged = 1;
                    JPLOG(JP_INFO, "%-6s prewarm_skip pid=%d bid=%s bg_s=%d fp=%s",
                          "BGK", (int)main_pid, rule->bid, bg_sec_elapsed, fp_str);
                }
                /* v0.8.6 C1: 删除原 bgkill_log_skip(..., "prewarm") 重复行；
                 * prewarm_skip 每 tracked pid 一次已足够，last_skip_* 不再被它更新。 */
                continue;
            }
            if (st->prewarm_hold && fp_ok && fp_mb >= (uint64_t)BGKILL_PREWARM_MAX_FP_MB) {
                st->prewarm_hold = 0;
                st->ever_fg = 1;
                st->prewarm_logged = 0;
                st->bg_start_mono = now_mono;
                st->bg_rounds = 1;
                continue;
            }
        }

        /* 全部满足，执行 kill */
        int krc = kill(main_pid, SIGKILL);
        int kill_errno = (krc == 0) ? 0 : errno;
        if (kill_errno == 0 || kill_errno == ESRCH) {
            st->today_count++;
            snprintf(st->last_kill_hhmm, sizeof(st->last_kill_hhmm), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
        }
        int today_n = st->today_count;

        JPLOG(JP_INFO, "%-6s kill pid=%d bid=%s bg_s=%d min=%d band=%d sys_band=%d fp=%s n=%d rc=%d",
              "BGK", (int)main_pid, rule->bid, bg_sec_elapsed, (int)rule->min,
              (int)cur_band, (int)sys_band, fp_str, today_n, kill_errno);

        bgkill_clear_pid_tracking(st);
    }
}

static void appmon_sync_rules(void);

static void mempressure_round(memorystatus_priority_entry_t *list, int count) {
    /* 自动重启与后台释放配置及判定 (v0.7.7 C2, v0.7.8 C2, v0.8.0 K, v0.8.7 APPMON) */
    restart_load_config();
    pulse_load_config();
    diag_load_config();   /* DIAG2: 慢轮感知 diag.txt 变更（stat 去重，未变零开销） */
    bgkill_load_config();
    appmon_sync_rules();
    restart_round(list, count);
    bgkill_round(list, count);
    pulse_round(list, count);
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

    bool responds = ((bool (*)(jp_objc_id, jp_objc_SEL, jp_objc_SEL))fn_objc_msgSend)(
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

/* ========================================================================= */
/* --- v0.8.7 APPMON: 零轮询前后台事件推送监听 (BKSApplicationStateMonitor) --- */
/* ========================================================================= */
static jp_objc_id  s_appmon_obj = NULL;
static void       *s_appmon_msgSend = NULL;
static jp_objc_SEL s_sel_app_info = NULL;
static jp_objc_SEL s_sel_update_bids = NULL;
static jp_objc_SEL s_sel_invalidate = NULL;
static void      *(*s_appmon_poolPush)(void) = NULL;
static void       (*s_appmon_poolPop)(void *) = NULL;

static CFStringRef s_cf_key_bid = NULL;
static CFStringRef s_cf_key_pid = NULL;
static CFStringRef s_cf_key_state = NULL;
static CFStringRef s_cf_key_front = NULL;
static CFStringRef s_cf_key_ext = NULL;
static CFStringRef s_cf_sb_bid = NULL;

static char s_appmon_bids[APPMON_MAX_BIDS][128];
static int  s_nappmon_bids = -1; /* -1: 尚未首次同步 */

static int appmon_parse_info_dict(jp_objc_id info, char *bid_out, size_t bid_sz,
                                  pid_t *pid_out, uint32_t *state_out) {
    if (!info || !bid_out || bid_sz == 0 || !s_cf_key_bid || !s_cf_key_pid || !s_cf_key_state) {
        return -1;
    }
    if (CFGetTypeID((CFTypeRef)info) != CFDictionaryGetTypeID()) {
        return -1;
    }
    CFDictionaryRef dict = (CFDictionaryRef)info;

    /* 过滤 .appex XPC 扩展进程，绝不允许插件状态冒充主应用前后台切换 */
    if (s_cf_key_ext) {
        const void *ext_v = CFDictionaryGetValue(dict, s_cf_key_ext);
        if (ext_v) {
            CFTypeID tid = CFGetTypeID(ext_v);
            if (tid == CFBooleanGetTypeID()) {
                if (CFBooleanGetValue((CFBooleanRef)ext_v)) return -1;
            } else if (tid == CFNumberGetTypeID()) {
                int ext_i = 0;
                if (CFNumberGetValue((CFNumberRef)ext_v, kCFNumberIntType, &ext_i) && ext_i != 0) {
                    return -1;
                }
            }
        }
    }

    const void *bid_v = CFDictionaryGetValue(dict, s_cf_key_bid);
    if (!bid_v || CFGetTypeID(bid_v) != CFStringGetTypeID()) return -1;
    bid_out[0] = '\0';
    if (!CFStringGetCString((CFStringRef)bid_v, bid_out, (CFIndex)bid_sz, kCFStringEncodingUTF8) ||
        bid_out[0] == '\0') {
        return -1;
    }

    int pid_i = 0;
    const void *pid_v = CFDictionaryGetValue(dict, s_cf_key_pid);
    if (pid_v && CFGetTypeID(pid_v) == CFNumberGetTypeID()) {
        (void)CFNumberGetValue((CFNumberRef)pid_v, kCFNumberIntType, &pid_i);
    }

    int st_i = 0;
    const void *st_v = CFDictionaryGetValue(dict, s_cf_key_state);
    if (st_v && CFGetTypeID(st_v) == CFNumberGetTypeID()) {
        (void)CFNumberGetValue((CFNumberRef)st_v, kCFNumberIntType, &st_i);
    }
    uint32_t st_u = (st_i > 0) ? (uint32_t)st_i : 0U;

    if (s_cf_key_front) {
        const void *fr_v = CFDictionaryGetValue(dict, s_cf_key_front);
        if (fr_v) {
            CFTypeID ftid = CFGetTypeID(fr_v);
            if ((ftid == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)fr_v)) ||
                (ftid == CFNumberGetTypeID() && ({
                    int fi = 0;
                    CFNumberGetValue((CFNumberRef)fr_v, kCFNumberIntType, &fi) && fi != 0;
                }))) {
                st_u = JP_BKS_STATE_FOREGROUND;
            }
        }
    }

    if (pid_out) *pid_out = (pid_i > 0) ? (pid_t)pid_i : 0;
    if (state_out) *state_out = st_u;
    return 0;
}

static void appmon_on_bks_info(jp_objc_id info) {
    void *pool = s_appmon_poolPush ? s_appmon_poolPush() : NULL;
    char bid[128];
    pid_t pid = 0;
    uint32_t state = 0;
    int rc = appmon_parse_info_dict(info, bid, sizeof(bid), &pid, &state);
    if (pool && s_appmon_poolPop) s_appmon_poolPop(pool);
    if (rc != 0 || state == 0) return;

    time_t ev_mono = mono_now();
    int need_write = 0;

    pthread_mutex_lock(&s_appmon_mutex);
    if (s_appmon_count > 0) {
        unsigned last_idx = (s_appmon_head + s_appmon_count - 1U) % APPMON_RING_CAP;
        appmon_event_t *last = &s_appmon_ring[last_idx];
        if (last->pid == pid && last->state == state && strcmp(last->bid, bid) == 0) {
            last->mono = ev_mono;
            pthread_mutex_unlock(&s_appmon_mutex);
            return;
        }
    }
    unsigned idx;
    if (s_appmon_count < APPMON_RING_CAP) {
        idx = (s_appmon_head + s_appmon_count) % APPMON_RING_CAP;
        s_appmon_count++;
    } else {
        /* 环满时覆盖最老条目，确保最新前后台状态绝不丢失 */
        idx = s_appmon_head;
        s_appmon_head = (s_appmon_head + 1U) % APPMON_RING_CAP;
        s_appmon_dropped++;
    }
    rst_str_copy(s_appmon_ring[idx].bid, sizeof(s_appmon_ring[idx].bid), bid);
    s_appmon_ring[idx].pid = pid;
    s_appmon_ring[idx].state = state;
    s_appmon_ring[idx].mono = ev_mono;
    if (!s_appmon_pipe_pending) {
        s_appmon_pipe_pending = 1;
        need_write = 1;
    }
    pthread_mutex_unlock(&s_appmon_mutex);

    if (need_write && g_appmon_pipe[1] >= 0) {
        char ch = 'A';
        ssize_t wr = write(g_appmon_pipe[1], &ch, 1);
        (void)wr;
    }
}

#ifdef __BLOCKS__
static void (^s_appmon_block)(jp_objc_id) = ^(jp_objc_id info) {
    appmon_on_bks_info(info);
};
#endif

static void appmon_close_pipe(void) {
    if (g_appmon_pipe[0] >= 0) { close(g_appmon_pipe[0]); g_appmon_pipe[0] = -1; }
    if (g_appmon_pipe[1] >= 0) { close(g_appmon_pipe[1]); g_appmon_pipe[1] = -1; }
}

static void appmon_init(void) {
    g_appmon_active = 0;
#ifndef __BLOCKS__
    (void)appmon_on_bks_info;
    return;
#else
    if (pipe(g_appmon_pipe) != 0) {
        g_appmon_pipe[0] = g_appmon_pipe[1] = -1;
        JPLOG(JP_WARN, "%-6s fallback reason=pipe errno=%d", "APPMON", errno);
        return;
    }
    for (int e = 0; e < 2; e++) {
        int fd_flags = fcntl(g_appmon_pipe[e], F_GETFD, 0);
        if (fd_flags >= 0) (void)fcntl(g_appmon_pipe[e], F_SETFD, fd_flags | FD_CLOEXEC);
        int fl_flags = fcntl(g_appmon_pipe[e], F_GETFL, 0);
        if (fl_flags >= 0) (void)fcntl(g_appmon_pipe[e], F_SETFL, fl_flags | O_NONBLOCK);
    }

    void *h_objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_LAZY);
    void *h_as = dlopen("/System/Library/PrivateFrameworks/AssertionServices.framework/AssertionServices", RTLD_LAZY);
    if (!h_as) {
        h_as = dlopen("/System/Library/PrivateFrameworks/BackBoardServices.framework/BackBoardServices", RTLD_LAZY);
    }
    if (!h_objc || !h_as) {
        appmon_close_pipe();
        JPLOG(JP_WARN, "%-6s fallback reason=dlopen", "APPMON");
        return;
    }

    jp_objc_Class (*fn_getClass)(const char *) =
        (jp_objc_Class (*)(const char *))dlsym(h_objc, "objc_getClass");
    jp_objc_SEL (*fn_sel)(const char *) =
        (jp_objc_SEL (*)(const char *))dlsym(h_objc, "sel_registerName");
    s_appmon_msgSend = dlsym(h_objc, "objc_msgSend");
    s_appmon_poolPush = (void *(*)(void))dlsym(h_objc, "objc_autoreleasePoolPush");
    s_appmon_poolPop = (void (*)(void *))dlsym(h_objc, "objc_autoreleasePoolPop");
    if (!fn_getClass || !fn_sel || !s_appmon_msgSend) {
        appmon_close_pipe();
        JPLOG(JP_WARN, "%-6s fallback reason=nosym", "APPMON");
        return;
    }

    jp_objc_Class clsBKS = fn_getClass("BKSApplicationStateMonitor");
    if (!clsBKS) {
        appmon_close_pipe();
        JPLOG(JP_WARN, "%-6s fallback reason=noclass", "APPMON");
        return;
    }

    jp_objc_SEL selAlloc = fn_sel("alloc");
    jp_objc_SEL selInitBids = fn_sel("initWithBundleIDs:states:");
    jp_objc_SEL selResponds = fn_sel("respondsToSelector:");
    jp_objc_SEL selInstancesRespond = fn_sel("instancesRespondToSelector:");
    jp_objc_SEL selSetHandler = fn_sel("setHandler:");
    s_sel_app_info = fn_sel("applicationInfoForApplication:");
    s_sel_update_bids = fn_sel("updateInterestedBundleIDs:");
    s_sel_invalidate = fn_sel("invalidate");
    if (!selAlloc || !selInitBids || !selResponds || !selInstancesRespond ||
        !selSetHandler || !s_sel_app_info || !s_sel_update_bids || !s_sel_invalidate) {
        appmon_close_pipe();
        JPLOG(JP_WARN, "%-6s fallback reason=noselector", "APPMON");
        return;
    }

    bool ok_init = ((bool (*)(jp_objc_id, jp_objc_SEL, jp_objc_SEL))s_appmon_msgSend)(
        (jp_objc_id)clsBKS, selInstancesRespond, selInitBids);
    bool ok_upd = ((bool (*)(jp_objc_id, jp_objc_SEL, jp_objc_SEL))s_appmon_msgSend)(
        (jp_objc_id)clsBKS, selInstancesRespond, s_sel_update_bids);
    bool ok_hdl = ((bool (*)(jp_objc_id, jp_objc_SEL, jp_objc_SEL))s_appmon_msgSend)(
        (jp_objc_id)clsBKS, selInstancesRespond, selSetHandler);
    bool ok_info = ((bool (*)(jp_objc_id, jp_objc_SEL, jp_objc_SEL))s_appmon_msgSend)(
        (jp_objc_id)clsBKS, selInstancesRespond, s_sel_app_info);
    if (!ok_init || !ok_upd || !ok_hdl || !ok_info) {
        appmon_close_pipe();
        JPLOG(JP_WARN, "%-6s fallback reason=abi_mismatch", "APPMON");
        return;
    }

    if (!s_cf_key_bid)   s_cf_key_bid   = CFStringCreateWithCString(kCFAllocatorDefault, "SBApplicationStateDisplayIDKey", kCFStringEncodingUTF8);
    if (!s_cf_key_pid)   s_cf_key_pid   = CFStringCreateWithCString(kCFAllocatorDefault, "SBApplicationStateProcessIDKey", kCFStringEncodingUTF8);
    if (!s_cf_key_state) s_cf_key_state = CFStringCreateWithCString(kCFAllocatorDefault, "SBApplicationStateKey", kCFStringEncodingUTF8);
    if (!s_cf_key_front) s_cf_key_front = CFStringCreateWithCString(kCFAllocatorDefault, "BKSApplicationStateAppIsFrontmost", kCFStringEncodingUTF8);
    if (!s_cf_key_ext)   s_cf_key_ext   = CFStringCreateWithCString(kCFAllocatorDefault, "BKSApplicationStateExtensionKey", kCFStringEncodingUTF8);
    if (!s_cf_sb_bid)    s_cf_sb_bid    = CFStringCreateWithCString(kCFAllocatorDefault, "com.apple.springboard", kCFStringEncodingUTF8);
    if (!s_cf_key_bid || !s_cf_key_pid || !s_cf_key_state || !s_cf_sb_bid) {
        appmon_close_pipe();
        JPLOG(JP_WARN, "%-6s fallback reason=cfstr", "APPMON");
        return;
    }

    /* 初始传空数组 @[]（注意：传 nil 会订阅全机所有进程，空数组则订阅 0 个进程） */
    CFArrayRef empty_arr = CFArrayCreate(kCFAllocatorDefault, NULL, 0, &kCFTypeArrayCallBacks);
    if (!empty_arr) {
        appmon_close_pipe();
        JPLOG(JP_WARN, "%-6s fallback reason=cfarr", "APPMON");
        return;
    }

    void *pool = s_appmon_poolPush ? s_appmon_poolPush() : NULL;
    jp_objc_id raw = ((jp_objc_id (*)(jp_objc_id, jp_objc_SEL))s_appmon_msgSend)(
        (jp_objc_id)clsBKS, selAlloc);
    jp_objc_id mon = NULL;
    if (raw) {
        mon = ((jp_objc_id (*)(jp_objc_id, jp_objc_SEL, jp_objc_id, unsigned int))s_appmon_msgSend)(
            raw, selInitBids, (jp_objc_id)empty_arr, JP_BKS_STATE_ALL);
    }
    CFRelease(empty_arr);
    if (!mon) {
        if (pool && s_appmon_poolPop) s_appmon_poolPop(pool);
        appmon_close_pipe();
        JPLOG(JP_WARN, "%-6s fallback reason=init_nil", "APPMON");
        return;
    }

    /* 启动自检：向 runningboardd 试查 com.apple.springboard。
     * 若权限缺失或服务不可达，返回 nil / pid<=0，立即销毁并回退 20s 慢轮兜底。 */
    jp_objc_id sb_info = ((jp_objc_id (*)(jp_objc_id, jp_objc_SEL, jp_objc_id))s_appmon_msgSend)(
        mon, s_sel_app_info, (jp_objc_id)s_cf_sb_bid);
    char sb_bid[128] = {0};
    pid_t sb_pid = 0;
    uint32_t sb_state = 0;
    int probe_ok = (appmon_parse_info_dict(sb_info, sb_bid, sizeof(sb_bid), &sb_pid, &sb_state) == 0 &&
                    sb_pid > 0);
    if (!probe_ok) {
        ((void (*)(jp_objc_id, jp_objc_SEL))s_appmon_msgSend)(mon, s_sel_invalidate);
        CFRelease((CFTypeRef)mon);
        if (pool && s_appmon_poolPop) s_appmon_poolPop(pool);
        appmon_close_pipe();
        JPLOG(JP_WARN, "%-6s fallback reason=probe_denied", "APPMON");
        return;
    }

    ((void (*)(jp_objc_id, jp_objc_SEL, jp_objc_id))s_appmon_msgSend)(
        mon, selSetHandler, (jp_objc_id)s_appmon_block);
    if (pool && s_appmon_poolPop) s_appmon_poolPop(pool);

    s_appmon_obj = mon;
    g_appmon_active = 1;
    JPLOG(JP_INFO, "%-6s init ok sb_pid=%d sb_state=%u", "APPMON", (int)sb_pid, (unsigned)sb_state);
#endif
}

static void appmon_apply_single_event(const appmon_event_t *ev, const char *today_str,
                                      int from_seed, int *need_bg_scan_io) {
    if (!ev || ev->bid[0] == '\0') return;

    if (!from_seed) {
        DIAGLOG(DIAG_DETAILED, "DIAG event=APPMON bid=%s pid=%d bks_state=%u",
                ev->bid, (int)ev->pid, (unsigned)ev->state);
    }

    if (ev->state == JP_BKS_STATE_FOREGROUND && ev->pid > 0) {
        /* 1. bgkill: 毫秒级闩锁 ever_fg=1，即使点开 0.3 秒划走也不漏记 */
        if (!g_bgkill_cfg_file_absent && g_bgkill_master) {
            for (int i = 0; i < g_nbgkill_rules; i++) {
                if (g_bgkill_rules[i].enabled && strcmp(g_bgkill_rules[i].bid, ev->bid) == 0) {
                    bgkill_rule_state_t *bst = bgkill_get_rule_state(ev->bid);
                    if (bst) {
                        bst->tracked_pid = ev->pid;
                        bst->in_bg = 0;
                        bst->bg_start_mono = 0;
                        bst->bg_rounds = 0;
                        bst->ever_fg = 1;
                        bst->prewarm_hold = 0;
                        bst->prewarm_logged = 0;
                    }
                    break;
                }
            }
        }
        /* 2. pulse: 移出待处理队列并标记实时前台态 */
        for (int q = g_npulse_queue - 1; q >= 0; q--) {
            if (g_pulse_queue[q].pid == ev->pid ||
                strcmp(g_pulse_queue[q].bid, ev->bid) == 0) {
                pulse_queue_remove_at(q);
            }
        }
        if (!g_pulse_cfg_file_absent && g_npulse_rules > 0) {
            pthread_mutex_lock(&s_pulse_mutex);
            for (int i = 0; i < g_npulse_rules; i++) {
                if (g_pulse_rules[i].enabled && strcmp(g_pulse_rules[i].bid, ev->bid) == 0) {
                    pulse_app_state_t *pst = pulse_get_app_state(ev->bid, today_str);
                    if (pst) {
                        pst->tracked_pid = ev->pid;
                        pst->in_bg = 0;
                        pst->bg_start_mono = 0;
                        pst->last_fg_seen_mono = ev->mono;
                        pst->first_bg_seen_mono = 0;
                        pst->bg_lag = -1;
                        pst->bg_lag_max = -1;
                        pst->pulsed_this_bg = 0;
                        pst->ctrl_this_bg = 0;
                        pst->pulse_late_this_bg = 0;
                        pst->diag_round = 0;
                        pst->diag_first_observe_s = -1;
                        pst->diag_gate_t_s = -1;
                        pst->appmon_in_fg = 1;
                        pst->appmon_bg_pending_mono = 0;
                    }
                    break;
                }
            }
            pthread_mutex_unlock(&s_pulse_mutex);
        }
    } else if ((ev->state == JP_BKS_STATE_BG_RUNNING ||
                ev->state == JP_BKS_STATE_BG_SUSPENDED) && ev->pid > 0) {
        if (!from_seed && need_bg_scan_io) {
            *need_bg_scan_io = 1;
        }
        if (!g_pulse_cfg_file_absent && g_npulse_rules > 0 && !from_seed) {
            pthread_mutex_lock(&s_pulse_mutex);
            for (int i = 0; i < g_npulse_rules; i++) {
                if (g_pulse_rules[i].enabled && strcmp(g_pulse_rules[i].bid, ev->bid) == 0) {
                    pulse_app_state_t *pst = pulse_get_app_state(ev->bid, today_str);
                    if (pst) {
                        if (pst->tracked_pid == ev->pid && pst->appmon_in_fg) {
                            /* 零滞后真切换时刻：刷新前台终点与后台起点为同一退后台瞬间 */
                            pst->last_fg_seen_mono = ev->mono;
                            pst->first_bg_seen_mono = ev->mono;
                            pst->appmon_bg_pending_mono = ev->mono;
                        } else if (!pst->in_bg) {
                            if (pst->tracked_pid == ev->pid && pst->first_bg_seen_mono <= 0) {
                                pst->first_bg_seen_mono = ev->mono;
                            }
                            pst->appmon_bg_pending_mono = ev->mono;
                        }
                        pst->appmon_in_fg = 0;
                    }
                    break;
                }
            }
            pthread_mutex_unlock(&s_pulse_mutex);
        }
    } else if (ev->state == JP_BKS_STATE_TERMINATED) {
        if (!g_pulse_cfg_file_absent && g_npulse_rules > 0) {
            pthread_mutex_lock(&s_pulse_mutex);
            for (int i = 0; i < g_npulse_app_states; i++) {
                if (strcmp(g_pulse_app_states[i].bid, ev->bid) == 0) {
                    g_pulse_app_states[i].appmon_in_fg = 0;
                    g_pulse_app_states[i].appmon_bg_pending_mono = 0;
                    break;
                }
            }
            pthread_mutex_unlock(&s_pulse_mutex);
        }
    }
}

static int appmon_drain_events(int *need_bg_scan_out) {
    if (need_bg_scan_out) *need_bg_scan_out = 0;
    if (!g_appmon_active) return 0;

    appmon_event_t batch[APPMON_RING_CAP];
    unsigned n = 0;

    pthread_mutex_lock(&s_appmon_mutex);
    while (s_appmon_count > 0 && n < APPMON_RING_CAP) {
        batch[n++] = s_appmon_ring[s_appmon_head];
        s_appmon_head = (s_appmon_head + 1U) % APPMON_RING_CAP;
        s_appmon_count--;
    }
    pthread_mutex_unlock(&s_appmon_mutex);

    if (n == 0) return 0;

    char today_str[16] = {0};
    pulse_get_local_date_str(today_str, sizeof(today_str));
    pulse_check_date_reset(today_str);

    int need_bg = 0;
    for (unsigned i = 0; i < n; i++) {
        appmon_apply_single_event(&batch[i], today_str, 0, &need_bg);
    }
    if (need_bg_scan_out) *need_bg_scan_out = need_bg;
    return (int)n;
}

static void appmon_add_unique_bid(char list[][128], int *cnt_io, const char *bid) {
    if (!bid || bid[0] == '\0' || *cnt_io >= APPMON_MAX_BIDS) return;
    for (int i = 0; i < *cnt_io; i++) {
        if (strcmp(list[i], bid) == 0) return;
    }
    rst_str_copy(list[*cnt_io], 128, bid);
    (*cnt_io)++;
}

static int appmon_bid_cmp(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

static int appmon_bid_in_prev(const char *bid) {
    if (s_nappmon_bids <= 0) return 0;
    for (int i = 0; i < s_nappmon_bids; i++) {
        if (strcmp(s_appmon_bids[i], bid) == 0) return 1;
    }
    return 0;
}

static void appmon_sync_rules(void) {
    if (!g_appmon_active || !s_appmon_obj || !s_appmon_msgSend || !s_sel_update_bids) return;

    char next_bids[APPMON_MAX_BIDS][128];
    int n_next = 0;

    if (!g_pulse_cfg_file_absent) {
        for (int i = 0; i < g_npulse_rules; i++) {
            if (g_pulse_rules[i].enabled) {
                appmon_add_unique_bid(next_bids, &n_next, g_pulse_rules[i].bid);
            }
        }
    }
    if (!g_bgkill_cfg_file_absent && g_bgkill_master) {
        for (int i = 0; i < g_nbgkill_rules; i++) {
            if (g_bgkill_rules[i].enabled) {
                appmon_add_unique_bid(next_bids, &n_next, g_bgkill_rules[i].bid);
            }
        }
    }
    if (!g_restart_cfg_file_absent) {
        for (int i = 0; i < g_nrestart_rules; i++) {
            appmon_add_unique_bid(next_bids, &n_next, g_restart_rules[i].bid);
        }
    }
    for (int i = 0; i < g_nprotect; i++) {
        if (g_protect[i].is_bid && strncmp(g_protect[i].frag, "bid:", 4) == 0) {
            appmon_add_unique_bid(next_bids, &n_next, g_protect[i].frag + 4);
        }
    }
    for (int i = 0; i < g_ndemote; i++) {
        if (g_demote[i].is_bid && strncmp(g_demote[i].frag, "bid:", 4) == 0) {
            appmon_add_unique_bid(next_bids, &n_next, g_demote[i].frag + 4);
        }
    }

    if (n_next > 1) {
        qsort(next_bids, (size_t)n_next, sizeof(next_bids[0]), appmon_bid_cmp);
    }

    if (s_nappmon_bids == n_next) {
        int same = 1;
        for (int i = 0; i < n_next; i++) {
            if (strcmp(s_appmon_bids[i], next_bids[i]) != 0) {
                same = 0;
                break;
            }
        }
        if (same) return; /* 订阅集合未变，零 XPC 开销 */
    }

    CFStringRef cf_strs[APPMON_MAX_BIDS];
    int n_cf = 0;
    for (int i = 0; i < n_next; i++) {
        CFStringRef s = CFStringCreateWithCString(kCFAllocatorDefault, next_bids[i], kCFStringEncodingUTF8);
        if (s) cf_strs[n_cf++] = s;
    }

    CFArrayRef arr = CFArrayCreate(kCFAllocatorDefault,
                                   n_cf > 0 ? (const void **)cf_strs : NULL,
                                   (CFIndex)n_cf, &kCFTypeArrayCallBacks);
    if (arr) {
        void *pool = s_appmon_poolPush ? s_appmon_poolPush() : NULL;
        ((void (*)(jp_objc_id, jp_objc_SEL, jp_objc_id))s_appmon_msgSend)(
            s_appmon_obj, s_sel_update_bids, (jp_objc_id)arr);

        /* 对新加入订阅的 bid 做一次即时前台态快照，防止配置新增瞬间该 App 已在前台而漏记 */
        if (s_sel_app_info && n_cf > 0) {
            char today_str[16] = {0};
            pulse_get_local_date_str(today_str, sizeof(today_str));
            pulse_check_date_reset(today_str);
            time_t now_m = mono_now();
            for (int i = 0; i < n_cf; i++) {
                char chk_bid[128] = {0};
                if (!CFStringGetCString(cf_strs[i], chk_bid, sizeof(chk_bid), kCFStringEncodingUTF8) ||
                    appmon_bid_in_prev(chk_bid)) {
                    continue;
                }
                jp_objc_id info = ((jp_objc_id (*)(jp_objc_id, jp_objc_SEL, jp_objc_id))s_appmon_msgSend)(
                    s_appmon_obj, s_sel_app_info, (jp_objc_id)cf_strs[i]);
                appmon_event_t seed_ev;
                memset(&seed_ev, 0, sizeof(seed_ev));
                if (appmon_parse_info_dict(info, seed_ev.bid, sizeof(seed_ev.bid),
                                           &seed_ev.pid, &seed_ev.state) == 0 &&
                    seed_ev.state == JP_BKS_STATE_FOREGROUND && seed_ev.pid > 0) {
                    seed_ev.mono = now_m;
                    appmon_apply_single_event(&seed_ev, today_str, 1, NULL);
                }
            }
        }
        if (pool && s_appmon_poolPop) s_appmon_poolPop(pool);
        CFRelease(arr);
    }
    for (int i = 0; i < n_cf; i++) {
        CFRelease(cf_strs[i]);
    }

    s_nappmon_bids = n_next;
    for (int i = 0; i < n_next; i++) {
        rst_str_copy(s_appmon_bids[i], sizeof(s_appmon_bids[i]), next_bids[i]);
    }
    JPLOG(JP_INFO, "%-6s sync bids=%d", "APPMON", n_next);
}

int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (argv[i] && strcmp(argv[i], "--probe") == 0) {
            exit(0);
        }
    }

    if (geteuid() != 0) {
        fprintf(stderr, "[JetPilot] 必须以 root 运行\n");
        return 1;
    }
    /* 单实例锁 (C1)：通过 flock 确保互斥，避免读写 pid 文件的竞态及 pid 复用误判 */
    /* INT1：CONFIG_DIR + lock 路径 lstat 校验（symlink/类型/root 属主/组或其他可写） */
    if (jp_secure_precheck(LOCK_PATH) != 0) {
        JPLOG(JP_WARN, "LOCK_FAIL reason=dir");
        return 1;
    }
    int lock_fd = open(LOCK_PATH, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock_fd < 0) {
        JPLOG(JP_WARN, "LOCK_FAIL errno=%d", errno);
        return 1;
    }
    /* INT1：fstat 复核（防 lstat→open 竞态）并 fchmod 回 0600，异常直接拒绝启动 */
    if (jp_secure_fd_check(lock_fd, 0600) != 0) {
        close(lock_fd);
        JPLOG(JP_WARN, "LOCK_FAIL reason=stat");
        return 1;
    }
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            int file_pid = -1;   /* 读不到就是 -1 */
            FILE *rpf = fopen(PID_PATH, "r");
            if (rpf) { if (fscanf(rpf, "%d", &file_pid) != 1 || file_pid <= 0) file_pid = -1; fclose(rpf); }
            JPLOG(JP_INFO, "DUP_INSTANCE pid=%d running=%d", (int)getpid(), file_pid);
            if (file_pid > 0) fprintf(stderr, "[JetPilot] 已在运行 (pid %d)\n", file_pid);
            else              fprintf(stderr, "[JetPilot] 已在运行\n");
            return 0;
        }
        JPLOG(JP_WARN, "LOCK_FAIL errno=%d", errno);
        return 1;
    }
    (void)lock_fd; /* 锁在进程退出（含 SIGKILL）时由内核自动释放，生命周期内保持打开，不删除 lock 文件 */

    /* M1: pid 文件改为 tmp+rename 原子替换（原直接 fopen("w") 存在 torn write
     * 窗口）+ O_NOFOLLOW + fsync，App 存活探测不再读到半截 pid */
    char pid_tmp[512];
    snprintf(pid_tmp, sizeof(pid_tmp), "%s.tmp", PID_PATH);
    /* INT1：lstat 前置校验 + O_NOFOLLOW + fstat 复核（0644 保持远端权限位）；
     * 异常拒绝写入、清掉 tmp，不回退 fopen。 */
    if (jp_secure_precheck(pid_tmp) == 0) {
        int pid_fd = open(pid_tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (pid_fd >= 0) {
            FILE *pf = NULL;
            if (jp_secure_fd_check(pid_fd, 0644) == 0) {
                pf = fdopen(pid_fd, "w");
            }
            if (pf) {
                fprintf(pf, "%d\n", getpid());
                (void)jp_atomic_commit(pf, pid_tmp, PID_PATH);
            } else {
                close(pid_fd);
                unlink(pid_tmp);
            }
        }
    }

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
    appmon_init();   /* v0.8.7: 前后台事件推送监听（失败自动回退 20s 慢轮兜底） */
    load_old_targets();  /* 启动时导入旧 status.txt 记录供首轮认领 */
    restart_state_load();  /* 启动时载入重启计数状态 (v0.7.7 C2) */
    pulse_startup_recovery();  /* 启动时脉冲状态恢复 (v0.7.8 C2-5) */
    JPLOG(JP_INFO, "%-6s v%s commit=%s pid=%d old_records=%d locked=%d appmon=%d",
           "START", JP_VERSION, JP_COMMIT, (int)getpid(), g_nold_targets, g_locked, g_appmon_active);
    mempressure_init();  /* 前台 App 内存压力触发初始化 (v0.7.0) */

    /* E4: 启动探测定向查询命令，决定快轮初始模式（三级兜底见 fast_scan_probe） */
    fast_scan_probe();

    int lock_changed_prev = 0;  /* E1: 休眠期间锁屏状态是否变化（触发下一轮慢轮） */
    time_t last_appmon_slow_mono = 0;

    while (!should_stop()) {
        /* v0.8.6 C3: 每轮推进 epoch；上轮缓存自动失效，rst_ensure_app_procs
         * 只在同轮内复用主扫描缓存，杜绝陈旧 pid→bid 映射跨轮使用。 */
        s_round_epoch++;
        if (load_config() != 0) { sleep(BACKOFF_BASE_SECS); continue; }  /* E2: 基础间隔 */
        pulse_load_config();
        bgkill_load_config();

        int appmon_need_bg_scan = 0;
        (void)appmon_drain_events(&appmon_need_bg_scan);

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
            /* M1（Chief 裁定纳入）：open(O_NOFOLLOW)+fdopen 替换 fopen("w")，
             * 堵符号链接写穿；末尾统一走 jp_atomic_commit（fsync+rename+目录 fsync） */
            /* INT1：lstat 前置校验 + O_NOFOLLOW + fstat 复核 + fchmod 回 0644；
             * 异常一律拒绝写入、清理 tmp，不回退 fopen。 */
            if (jp_secure_precheck(BANDS_TMP_PATH) == 0) {
                int bfd = open(BANDS_TMP_PATH, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
                if (bfd >= 0) {
                    if (jp_secure_fd_check(bfd, 0644) == 0) {
                        bf = fdopen(bfd, "w");
                    }
                    if (!bf) { close(bfd); unlink(BANDS_TMP_PATH); }
                }
            }
            if (bf) {
                time_t bt = time(NULL);
                struct tm btm;
                localtime_r(&bt, &btm);
                char bts[64];
                strftime(bts, sizeof(bts), "%Y-%m-%d %H:%M:%S", &btm);
                fprintf(bf, "req=%s\nupdated=%s\n", bands_token, bts);
            }
        }

        /* ---- E1 + v0.8.7: 慢轮判定（任一满足即本轮走慢轮）----
         * 首轮 / 周期到期 / 锁屏状态变化 / config 变化 / bands.req / APPMON 退后台事件（2s 节流）；
         * E4 最坏兜底 FAST_MODE_LEGACY 时每轮都是慢轮（回退现状） */
        time_t now_mono_start = mono_now();
        int config_changed = jp_file_stat_changed(CONFIG_PATH, &s_config_stamp, NULL);
        int appmon_force_slow = 0;
        if (appmon_need_bg_scan && (last_appmon_slow_mono == 0 ||
            (now_mono_start - last_appmon_slow_mono) >= 2)) {
            appmon_force_slow = 1;
            last_appmon_slow_mono = now_mono_start;
        }
        int slow_round_now =
            (g_fast_mode == FAST_MODE_LEGACY) ||
            (g_slow_due_mono == 0) ||
            (now_mono_start >= g_slow_due_mono) ||
            lock_changed_prev || config_changed || handle_bands_req ||
            appmon_force_slow;

        int slow_actions = 0;              /* E2: 本轮 RAISE/LOWER/DEMOTE/RETURN 成功次数 */
        unsigned long creates_before = g_applied_creates;

        int protected_now = 0;   /* P3-2: demote_actions 存量死变量已删（只增不读，批次1 存量） */
        protected_entry_t plist[MAX_PROTECTED_LIST];
        int nplist = 0;
        protected_entry_t dlist[MAX_PROTECTED_LIST];
        int ndlist = 0;
        int fast_round_actions = 0;
        int pulse_gate_active = 0;   /* D-003: 本轮门控是否打开（用于约束下一次唤醒间隔） */

        if (!slow_round_now) {
            /* E1 快轮：不调 proc_pidpath / mempressure_round / write_status */
            fast_round_actions = fast_round();
            appmon_sync_rules();
            diag_load_config();   /* DIAG2: 快轮仅 stat 感知，未变零日志/零写盘，≤5s 生效 */
            /* D-003 (F1): 门控打开才临时恢复全表，仅跑 pulse 状态推进/候选评估。
             * 拿不到全表时按 S12 用 NULL 调用（pulse_round 安全跳过且不清状态）；
             * 连续 3 次失败回退 LEGACY（每轮慢轮），与 E4 慢轮兜底一致。 */
            pulse_gate_active = pulse_fast_gate_open(now_mono_start);
            if (pulse_gate_active) {
                int pulse_count = 0;
                memorystatus_priority_entry_t *pulse_list = get_list(&pulse_count);
                if (pulse_list) {
                    /* P0 修复（保留）+ v0.8.6 C3: 门控快轮不经过 restart_round，
                     * 必须显式 bump epoch 使上轮缓存失效，否则会把已回前台 App
                     * 误判后台并触发 pulse。 */
                    rst_app_procs_invalidate();
                    pulse_round(pulse_list, pulse_count);
                    rst_app_procs_invalidate();
                    free(pulse_list);
                    g_fast_pulse_list_fail_streak = 0;
                } else {
                    pulse_round(NULL, 0);
                    if (++g_fast_pulse_list_fail_streak >= 3 && !g_fast_mode_env_forced &&
                        g_fast_mode != FAST_MODE_LEGACY) {
                        g_fast_mode = FAST_MODE_LEGACY;
                        JPLOG(JP_WARN, "%-6s fast_pulse_list_fail x3 -> legacy full-scan", "E4");
                    }
                }
            }
        } else {
        /* DIAG2: 小时级 DIAG_STATS 只在慢轮发出（慢轮最长 240s，足以覆盖整点）；
         * 快轮分支内绝不写任何 DIAG 行。 */
        {
            time_t diag_t = time(NULL);
            struct tm diag_tm;
            localtime_r(&diag_t, &diag_tm);
            int emit_stats = 0;
            pthread_mutex_lock(&s_diag_mutex);
            if (g_diag_stats_hour != diag_tm.tm_hour) {
                g_diag_stats_hour = diag_tm.tm_hour;
                emit_stats = 1;
            }
            pthread_mutex_unlock(&s_diag_mutex);
            if (emit_stats) {
                diag_stats_emit();
            }
        }
        int count = 0;
        memorystatus_priority_entry_t *list = get_list(&count);

        if (list) {
            /* v0.8.6 C3: 慢轮主扫描顺手填共享缓存并打本轮 epoch；
             * restart/bgkill/pulse 的 rst_ensure_app_procs 同轮直接复用。 */
            s_napp_procs = 0;
            s_app_proc_epoch = s_round_epoch;
            for (int i = 0; i < count; i++) {
                pid_t pid = list[i].pid;
                int32_t cur = list[i].priority;
                if (pid <= 0) continue;

                char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
                if (proc_pidpath(pid, path, sizeof(path)) <= 0) continue;
                /* 只碰用户 App，绝不动系统进程 */
                if (!strstr(path, "/var/containers/Bundle/Application/")) continue;
                /* v0.8.6 C3: 同一次 proc_pidpath 结果填缓存（含 state） */
                rst_cache_app_proc_from_scan(pid, cur, list[i].state, path);

                int32_t target = 0;
                const char *frag = NULL;
                const char *demote_frag = NULL;
                int32_t demote_target = BAND_BACKGROUND;
                int skip_this_round = 0;   /* 本轮是否走了 SKIP：非 SKIP 路径要清去重项 */
                applied_t *prev = applied_find(pid);
                if (prev) {
                    prev->seen = 1;
                    applied_observe_band(prev, cur);
                }
                /* 首轮认领：上个实例记录过、且当前档位仍等于它设的值 */
                if (!prev && g_nold_targets > 0 && !jp_is_foreground(cur)) {
                    int32_t ot = 0, oorig = 0;
                    int ok = 0;
                    if (old_target_find(pid, &ot, &oorig, &ok) && cur == ot) {
                        applied_set(pid, cur);
                        prev = applied_find(pid);   /* 表满时为 NULL，安全 */
                        if (prev) {
                            if (ok) {
                                prev->orig = oorig;
                                prev->orig_known = 1;
                                prev->orig_seen_mono = applied_mono_now();
                            } else {
                                prev->orig_known = 0;
                            }
                            prev->sys_band = 0;
                            prev->sys_band_known = 0;
                            prev->sys_band_seen_mono = 0;
                            prev->claim_origin = 1;   /* 旧实例认领：来源可信，归属判定放行 */
                            /* E1/P3-1: CLAIM 条目不填身份缓存（has_identity=0）——旧 target
                             * 可能来自 demote 规则，按 protect 填 kind=0 会让快轮在 cur<30
                             * 时抬档，比慢轮（对 demote 条目永不抬档）更激进。快轮只确认
                             * 存活；身份缓存由慢轮首个真实动作（RAISE/LOWER/DEMOTE）填充，
                             * 之前由慢轮周期兜底 */
                        }
                        JPLOG(JP_INFO, "%-6s pid=%d band=%d", "CLAIM", (int)pid, (int)cur);
                    }
                }
                const char *app_bid = NULL;
                if (g_has_bid_rules) {
                    app_bid = app_bid_for_path(path);
                }
                /* v0.8.6 A2: 仅追踪主应用进程 (band,state) 状态跳变，避免 .appex 插件海量噪音 */
                if (diag_level_now() >= DIAG_DETAILED && memp_is_main_app_binary(path)) {
                    const char *diag_bid = app_bid ? app_bid : app_bid_for_path(path);
                    diag_jetsam_state_note(pid, diag_bid, cur, list[i].state, s_round_epoch);
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
                            slow_actions++;   /* E2: 空转判定 */
                            g_reassert_count++;
                            applied_set_ex(pid, target, cur);
                            /* E1: 慢轮填充/刷新身份缓存（新建或已有条目） */
                            applied_fill_identity(applied_find(pid), path, frag, target, 0, app_bid);
                            act_log_success(ACT_KIND_RAISE, pid, path, frag, cur, target);
                            cur = target;
                            rst_update_cached_priority(pid, cur);
                        } else {
                            /* errno 已被 set_priority 失败分支的日志调用污染，用 g_last_err_no */
                            JPLOG(JP_ERR, "%-6s pid=%d %s %s %d->%d FAIL errno=%d %s",
                                   "RAISE", (int)pid, proc_label_log(path), frag, (int)cur, (int)target,
                                   g_last_err_no, strerror(g_last_err_no));
                        }
                    } else if (cur > target && !jp_is_foreground(cur)) {
                        /* T3-IMP 3.3: 需可信来源 + 新鲜观察，禁止纯数值相等误归属 */
                        if (jp_attribution_trusted(prev, applied_mono_now()) && prev->band == cur) {
                            /* 高于目标且是我们（或旧实例）设的：跟着降 */
                            int rc = set_priority(pid, target);
                            if (rc == 0) {
                                slow_actions++;   /* E2: 空转判定 */
                                g_reassert_count++;
                                applied_set_ex(pid, target, cur);
                                /* E1: 慢轮填充/刷新身份缓存（protect 规则命中，kind=0） */
                                applied_fill_identity(applied_find(pid), path, frag, target, 0, app_bid);
                                JPLOG(JP_INFO, "%-6s pid=%d %s %s %d->%d ok",
                                       "LOWER", (int)pid, proc_label_log(path), frag, (int)cur, (int)target);
                                cur = target;
                                rst_update_cached_priority(pid, cur);
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
                    if (!jp_is_foreground(cur) && cur > demote_target) {
                        int rc = set_priority(pid, demote_target);
                        if (rc == 0) {
                            slow_actions++;   /* E2: 空转判定 */
                            if (de) de->cur = demote_target;
                            applied_set_ex(pid, demote_target, cur);
                            /* E1: 慢轮填充/刷新身份缓存（demote 规则命中，kind=1） */
                            applied_fill_identity(applied_find(pid), path, demote_frag, demote_target, 1, app_bid);
                            act_log_success(ACT_KIND_DEMOTE, pid, path, demote_frag, cur, demote_target);
                            cur = demote_target;
                            rst_update_cached_priority(pid, cur);
                        } else {
                            /* errno 已被 set_priority 失败分支的日志调用污染，用 g_last_err_no */
                            JPLOG(JP_ERR, "%-6s pid=%d %s %s %d->%d FAIL errno=%d %s",
                                   "DEMOTE", (int)pid, proc_label_log(path), demote_frag, (int)cur, (int)demote_target,
                                   g_last_err_no, strerror(g_last_err_no));
                        }
                    }
                } else if (prev && prev->band == cur && !jp_is_foreground(cur) &&
                           jp_attribution_trusted(prev, applied_mono_now())) {
                    /* T3-IMP 3.3: 同上，数值相等不足以推断归属 */
                    if (cur > BAND_BACKGROUND) {
                        /* 原逻辑：protect 抬过的档位还回普通后台 (BAND_BACKGROUND) */
                        int rc = set_priority(pid, BAND_BACKGROUND);
                        if (rc == 0) {
                            slow_actions++;   /* E2: 空转判定（RETURN 也是动作） */
                            applied_remove(pid);   /* 还回后不再是我们的档位，内含 act_dedup_clear */
                            prev = NULL;           /* 指针已失效 */
                            int32_t old_cur = cur;
                            cur = BAND_BACKGROUND;
                            rst_update_cached_priority(pid, cur);
                            JPLOG(JP_INFO, "%-6s pid=%d %s %d->%d ok",
                                   "RETURN", (int)pid, proc_label_log(path), (int)old_cur, BAND_BACKGROUND);
                        } else {
                            /* errno 已被 set_priority 失败分支的日志调用污染，用 g_last_err_no */
                            JPLOG(JP_ERR, "%-6s pid=%d %s %d->%d FAIL errno=%d %s",
                                   "RETURN", (int)pid, proc_label_log(path), (int)cur, BAND_BACKGROUND,
                                   g_last_err_no, strerror(g_last_err_no));
                        }
                    } else {
                        /* cur <= BAND_BACKGROUND：demote 压过的，或已是后台档；不动档位，只把记录忘掉，交还系统 */
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
            g_slow_list_fail_streak = 0;   /* E4: 全表恢复，清连续失败计数 */
            fast_mode_recover_on_slow_success();   /* E4/P2: SLEEP/失败 LEGACY 回升 LISTSCAN（env 强制除外） */
        } else {
            mempressure_round(NULL, 0);
            /* E4 最坏兜底：慢轮也连续取不到全表（×3）→ 回退现状（每轮全表扫） */
            if (++g_slow_list_fail_streak >= 3 && g_fast_mode != FAST_MODE_LEGACY) {
                g_fast_mode = FAST_MODE_LEGACY;
                JPLOG(JP_WARN, "%-6s get_list_fail x3 -> legacy full-scan every round", "E4");
            }
        }
        }  /* end if (slow_round_now) */

        if (bf) {
            /* M1: 统一原子提交——ferror 检查、fsync、fclose、rename、目录 fsync
             * 都在助手内；失败清理 tmp，旧 bands.txt 保持完整（瞬态快照，下轮再生） */
            (void)jp_atomic_commit(bf, BANDS_TMP_PATH, BANDS_PATH);
        }

        int demoted_pids = 0;
        for (int i = 0; i < ndlist; i++) {
            if (dlist[i].cur <= dlist[i].target) {
                demoted_pids++;
            }
        }

        /* E3: status 只在慢轮写（快轮不重建 plist/dlist，避免用空列表写出错误 body）；
         * force=handle_bands_req：用户在看档位页时即使内容没变也刷新 updated= */
        if (slow_round_now) {
            write_status(protected_now, demoted_pids, plist, nplist, dlist, ndlist, handle_bands_req);
            mempressure_write_state();
        }

        /* ---- E2: 自适应退避（快/慢轮都统计；锁屏 10/60s 独立，不参与退避） ---- */
        {
            int idle;
            if (slow_round_now) {
                /* 慢轮空转：无动作 + config 未变 + 无 bands.req +
                 * 无新受保护进程（protected_now 与上轮同、无新建 applied 条目） */
                idle = (slow_actions == 0 && !config_changed && !handle_bands_req &&
                        protected_now == g_last_protected_now &&
                        g_applied_creates == creates_before);
                g_last_protected_now = protected_now;
            } else {
                /* 快轮半组判定：config/bands/锁屏变化已在慢轮判定中拦截；
                 * fast_round 返回 -1（异常降级未 prune）视为非空转 */
                idle = (fast_round_actions == 0);
            }
            if (idle) {
                if (g_idle_rounds < 8) g_idle_rounds++;   /* 封顶防移位溢出 */
                int iv = BACKOFF_BASE_SECS << g_idle_rounds;
                int sp = SLOW_BASE_SECS << g_idle_rounds;
                g_interval    = (iv > BACKOFF_MAX_SECS) ? BACKOFF_MAX_SECS : iv;
                g_slow_period = (sp > SLOW_MAX_SECS) ? SLOW_MAX_SECS : sp;
            } else {
                g_idle_rounds = 0;
                g_interval    = BACKOFF_BASE_SECS;
                g_slow_period = SLOW_BASE_SECS;
            }
            /* P1-1 + v0.8.7: APPMON 启用时前后台切换由系统事件毫秒级直推，无需再把慢轮锁在 <=20s，
             * 恢复 30~240s 自适应长退避；仅当 APPMON 未启用时保留 <=20s 慢轮兜底。 */
            if (!g_appmon_active && pulse_any_enabled_rule() &&
                g_slow_period > PULSE_FIRST_OBSERVE_MAX_SLOW) {
                g_slow_period = PULSE_FIRST_OBSERVE_MAX_SLOW;
            }
            /* 慢轮到期时间只在慢轮收尾推进：空转时慢轮仍按 g_slow_period 兜底 */
            if (slow_round_now) g_slow_due_mono = mono_now() + g_slow_period;
        }

        /* v0.8.6 A3 + v0.8.7: 统一重算门控与精确等待毫秒数（含 250ms 档位跌落快检与 delay_s 到点唤醒） */
        int base_secs = g_locked ? ((g_locked_passes < 2) ? 10 : 60) : g_interval;
        int wait_ms = pulse_next_wait_ms(mono_now(), base_secs, &pulse_gate_active);

        if (should_stop() || g_got_sigterm) break;

        if (g_locked) {
            g_locked_passes++;
        } else {
            g_locked_passes = 0;
        }
        int appmon_woke = 0;
        wait_events_ms(wait_ms, &lock_changed_prev, &appmon_woke);
        (void)appmon_woke;
        /* 注意：锁屏时最长 60 秒才发现 running.flag 被删（停止变慢），
         * 但点「停止」时人在用手机、是亮屏状态，实际不影响。 */
    }

    /* H1: 关停路径改原子读 running；为 0 时旧 worker 必已收尾，join 立即返回 */
    if (atomic_load_explicit(&g_pulse_thread_running, memory_order_acquire)) {
        g_pulse_stop_requested = 1;
        pthread_join(g_pulse_thread, NULL);
        atomic_store_explicit(&g_pulse_thread_running, 0, memory_order_relaxed);
        g_pulse_thread_has_handle = 0;
    } else if (g_pulse_thread_has_handle) {
        pthread_join(g_pulse_thread, NULL);
        g_pulse_thread_has_handle = 0;
    }

    appmon_close_pipe();
    act_dedup_flush_all();
    JPLOG(JP_INFO, "STOP");
    FILE *pf = fopen(PID_PATH, "r");
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
