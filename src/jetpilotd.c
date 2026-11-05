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
/* iOS SDK 没有 libproc.h，手动声明 */
#define PROC_PIDPATHINFO_MAXSIZE (4 * 1024)
extern int proc_pidpath(int pid, void *buffer, uint32_t buffersize);
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <notify.h>   /* 锁屏状态：com.apple.springboard.lockstate（libSystem 自带） */
#include <sys/select.h>
#include <sys/time.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>

#ifndef RTLD_DEFAULT
#define RTLD_DEFAULT ((void *) -2)
#endif

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
#define JP_VERSION   "0.6.0"
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

/* ---- 阶段 A 诊断：Safari 网页进程与相关宿主进程日志 ---- */
#define PROC_PIDCOALITIONINFO 20
struct proc_pidcoalitioninfo {
    uint64_t coalition_id[2];
    uint64_t reserved1, reserved2, reserved3;
}; /* [0]=resource [1]=jetsam */

#define PROC_PIDTASKINFO 4
struct proc_taskinfo {
    uint64_t pti_virtual_size, pti_resident_size, pti_total_user, pti_total_system,
             pti_threads_user, pti_threads_system;
    int32_t  pti_policy, pti_faults, pti_pageins, pti_cow_faults, pti_messages_sent,
             pti_messages_received, pti_syscalls_mach, pti_syscalls_unix, pti_csw,
             pti_threadnum, pti_numrunning, pti_priority;
};

extern int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);

#define PROC_PIDTBSDINFO 3
#define PROC_PIDTBSDINFO_MAXCOMLEN 16
struct proc_bsdinfo {
    uint32_t pbi_flags;
    uint32_t pbi_status;
    uint32_t pbi_xstatus;
    uint32_t pbi_pid;
    uint32_t pbi_ppid;
    uid_t    pbi_uid;
    gid_t    pbi_gid;
    uid_t    pbi_ruid;
    gid_t    pbi_rgid;
    uid_t    pbi_svuid;
    gid_t    pbi_svgid;
    uint32_t rfu_1;
    char     pbi_comm[PROC_PIDTBSDINFO_MAXCOMLEN];
    char     pbi_name[2 * PROC_PIDTBSDINFO_MAXCOMLEN];
    uint32_t pbi_nfiles;
    uint32_t pbi_pgid;
    uint32_t pbi_pjobc;
    uint32_t e_tdev;
    uint32_t e_tpgid;
    int32_t  pbi_nice;
    uint64_t pbi_start_tvsec;
    uint64_t pbi_start_tvusec;
};

#define MAX_WEBDIAG 256
typedef struct {
    pid_t    pid;
    char     kind[16];
    int32_t  band;
    uint64_t coal_jet;
    int      coal_jet_err;
    int64_t  rss_mb;
    int      rss_err;
    pid_t    resp;
} webdiag_entry_t;

static webdiag_entry_t g_webdiag[MAX_WEBDIAG];
static int g_webdiag_count = 0;
static unsigned long long g_webdiag_dropped_total = 0;

typedef int (*jp_resp_fn)(pid_t pid, pid_t *out);
static jp_resp_fn g_resp_fn = NULL;

static void webdiag_init(void) {
    g_resp_fn = (jp_resp_fn)dlsym(RTLD_DEFAULT, "responsibility_get_pid_responsible_for_pid");
    if (g_resp_fn) {
        pid_t out = -1;
        int r = g_resp_fn(getpid(), &out);
        /* 解读：out 被改写（!= -1）→ 双参形式，out 为结果、r 为状态；
                 out 仍为 -1 → 按单参形式理解，r 本身即结果 */
        pid_t resp;
        if (out != -1) {
            resp = out;
        } else {
            resp = (r > 0) ? (pid_t)r : -1;
        }
        if (resp <= 0) {
            jp_log("WEBDIAG resp_self=NA resp_r=%d", r);
        } else {
            jp_log("WEBDIAG resp_self=%d resp_r=%d", (int)resp, r);
        }
    } else {
        jp_log("WEBDIAG resp_self=NA resp_r=NA");
    }
}

static const char *get_web_proc_kind(const char *path) {
    if (!path) return NULL;
    size_t len = strlen(path);

    /* 1. 路径以 /com.apple.WebKit.WebContent 结尾 */
    const char s1[] = "/com.apple.WebKit.WebContent";
    size_t s1_len = sizeof(s1) - 1;
    if (len >= s1_len && strcmp(path + len - s1_len, s1) == 0) return "web";

    /* 2. 路径以 /MobileSafari 结尾 */
    const char s2[] = "/MobileSafari";
    size_t s2_len = sizeof(s2) - 1;
    if (len >= s2_len && strcmp(path + len - s2_len, s2) == 0) return "safari";

    /* 3. 路径以 /SafariViewService 结尾 */
    const char s3[] = "/SafariViewService";
    size_t s3_len = sizeof(s3) - 1;
    if (len >= s3_len && strcmp(path + len - s3_len, s3) == 0) return "svs";

    return NULL;
}

static int is_web_proc(const char *path) {
    return get_web_proc_kind(path) != NULL;
}

static void webdiag(pid_t pid, const char *path, int32_t cur) {
    if (!is_web_proc(path)) return;

    struct proc_pidcoalitioninfo coal;
    memset(&coal, 0, sizeof(coal));
    int coal_ret = proc_pidinfo(pid, PROC_PIDCOALITIONINFO, 0, &coal, sizeof(coal));
    int coal_errno = errno;
    int coal_err = (coal_ret != (int)sizeof(coal));

    char coal_res_str[64];
    char coal_jet_str[64];
    if (coal_err) {
        int err = (coal_ret < 0) ? coal_errno : 0;
        snprintf(coal_res_str, sizeof(coal_res_str), "coal_res=ERR errno=%d", err);
        snprintf(coal_jet_str, sizeof(coal_jet_str), "coal_jet=ERR errno=%d", err);
    } else {
        snprintf(coal_res_str, sizeof(coal_res_str), "coal_res=%llu", (unsigned long long)coal.coalition_id[0]);
        snprintf(coal_jet_str, sizeof(coal_jet_str), "coal_jet=%llu", (unsigned long long)coal.coalition_id[1]);
    }

    struct proc_taskinfo taskinfo;
    memset(&taskinfo, 0, sizeof(taskinfo));
    int task_ret = proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &taskinfo, sizeof(taskinfo));
    int rss_err = (task_ret != (int)sizeof(taskinfo));
    int64_t rss_mb = 0;
    char rss_str[32];
    if (rss_err) {
        snprintf(rss_str, sizeof(rss_str), "ERR");
    } else {
        rss_mb = (int64_t)(taskinfo.pti_resident_size / 1048576);
        snprintf(rss_str, sizeof(rss_str), "%lld", (long long)rss_mb);
    }

    char resp_str[32];
    char resp_r_str[32];
    pid_t resp_val = -1;   /* 上次写出的 resp 数值，-1 = NA（去重键，v6 §三） */
    if (!g_resp_fn) {
        snprintf(resp_str, sizeof(resp_str), "NA");
        snprintf(resp_r_str, sizeof(resp_r_str), "NA");
    } else {
        pid_t out = -1;
        int r = g_resp_fn(pid, &out);
        /* 解读：out 被改写（!= -1）→ 双参形式，out 为结果、r 为状态；
                 out 仍为 -1 → 按单参形式理解，r 本身即结果 */
        pid_t resp;
        if (out != -1) {
            resp = out;
        } else {
            resp = (r > 0) ? (pid_t)r : -1;
        }
        resp_val = (resp > 0) ? resp : -1;
        if (resp <= 0) {
            snprintf(resp_str, sizeof(resp_str), "NA");
        } else {
            snprintf(resp_str, sizeof(resp_str), "%d", (int)resp);
        }
        snprintf(resp_r_str, sizeof(resp_r_str), "%d", r);
    }

    webdiag_entry_t *entry = NULL;
    for (int i = 0; i < g_webdiag_count; i++) {
        if (g_webdiag[i].pid == pid) {
            entry = &g_webdiag[i];
            break;
        }
    }

    if (!entry) {
        if (g_webdiag_count >= MAX_WEBDIAG) {
            g_webdiag_dropped_total++;
            return;
        }
        entry = &g_webdiag[g_webdiag_count++];
        memset(entry, 0, sizeof(*entry));
        entry->pid = pid;
        const char *k = get_web_proc_kind(path);
        if (k) {
            strncpy(entry->kind, k, sizeof(entry->kind) - 1);
            entry->kind[sizeof(entry->kind) - 1] = '\0';
        }
    } else {
        const char *k = get_web_proc_kind(path);
        if (k && entry->kind[0] == '\0') {
            strncpy(entry->kind, k, sizeof(entry->kind) - 1);
            entry->kind[sizeof(entry->kind) - 1] = '\0';
        }
        int band_changed = (cur != entry->band);
        int coal_jet_changed = (coal_err != entry->coal_jet_err) ||
                               (!coal_err && coal.coalition_id[1] != entry->coal_jet);
        int resp_changed = (resp_val != entry->resp);
        int rss_changed = 0;
        if (rss_err != entry->rss_err) {
            rss_changed = 1;
        } else if (!rss_err) {
            int64_t diff = rss_mb - entry->rss_mb;
            if (diff < 0) diff = -diff;
            if (diff >= 10) rss_changed = 1;
        }
        if (!band_changed && !coal_jet_changed && !resp_changed && !rss_changed) {
            return;
        }
    }

    entry->band = cur;
    entry->coal_jet = coal_err ? 0 : coal.coalition_id[1];
    entry->coal_jet_err = coal_err;
    entry->rss_mb = rss_mb;
    entry->rss_err = rss_err;
    entry->resp = resp_val;

    jp_log("WEBDIAG pid=%d band=%d %s %s resp=%s resp_r=%s rss_mb=%s path=%s",
           (int)pid, (int)cur, coal_res_str, coal_jet_str, resp_str, resp_r_str, rss_str, path);
}

static void webdiag_gone_scan(void) {
    for (int i = 0; i < g_webdiag_count; ) {
        if (kill(g_webdiag[i].pid, 0) == -1 && errno == ESRCH) {
            jp_log("WEBDIAG gone pid=%d kind=%s coal_jet=%llu last_band=%d last_rss_mb=%lld",
                   (int)g_webdiag[i].pid,
                   g_webdiag[i].kind[0] ? g_webdiag[i].kind : "unknown",
                   (unsigned long long)g_webdiag[i].coal_jet,
                   (int)g_webdiag[i].band,
                   (long long)g_webdiag[i].rss_mb);
            g_webdiag[i] = g_webdiag[--g_webdiag_count];
        } else {
            i++;
        }
    }
}

#define XRAISED_PATH     CONFIG_DIR "/webdiag_raised.txt"
#define XRAISED_TMP_PATH CONFIG_DIR "/webdiag_raised.txt.tmp"
#define XPROT_FLAG_PATH  CONFIG_DIR "/webdiag_protect"

#define MAX_XRAISED 64
typedef struct {
    pid_t    pid;
    char     kind[16];
    uint64_t start_tvsec;
    uint64_t start_tvusec;
    int32_t  orig;
} xraised_entry_t;

static xraised_entry_t g_xraised[MAX_XRAISED];
static int g_xraised_count = 0;
static int g_xfull = 0;

static int xraised_save(void) {
    FILE *f = fopen(XRAISED_TMP_PATH, "w");
    if (!f) return -1;
    for (int i = 0; i < g_xraised_count; i++) {
        fprintf(f, "%d %s %llu.%llu %d\n",
                (int)g_xraised[i].pid,
                g_xraised[i].kind,
                (unsigned long long)g_xraised[i].start_tvsec,
                (unsigned long long)g_xraised[i].start_tvusec,
                (int)g_xraised[i].orig);
    }
    if (fflush(f) != 0) {
        fclose(f);
        unlink(XRAISED_TMP_PATH);
        return -1;
    }
    int fd = fileno(f);
    if (fd < 0 || fsync(fd) != 0) {
        fclose(f);
        unlink(XRAISED_TMP_PATH);
        return -1;
    }
    if (fclose(f) != 0) {
        unlink(XRAISED_TMP_PATH);
        return -1;
    }
    if (rename(XRAISED_TMP_PATH, XRAISED_PATH) != 0) {
        unlink(XRAISED_TMP_PATH);
        return -1;
    }
    return 0;
}

static void xraised_restore(void) {
    int n = 0;
    int drop = 0;
    g_xraised_count = 0;

    FILE *f = fopen(XRAISED_PATH, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            pid_t pid = 0;
            char kind[16] = {0};
            uint64_t sec = 0, usec = 0;
            int32_t orig = 0;

            int matched = sscanf(line, "%d %15s %llu.%llu %d",
                                 &pid, kind, (unsigned long long *)&sec,
                                 (unsigned long long *)&usec, &orig);
            if (matched != 5) {
                matched = sscanf(line, "%d %15s %llu:%llu %d",
                                 &pid, kind, (unsigned long long *)&sec,
                                 (unsigned long long *)&usec, &orig);
            }
            if (matched != 5 || pid <= 0) {
                drop++;
                continue;
            }

            int exists = 0;
            for (int i = 0; i < g_xraised_count; i++) {
                if (g_xraised[i].pid == pid) { exists = 1; break; }
            }
            if (exists) {
                drop++;
                continue;
            }

            // 1. pid 存活
            if (kill(pid, 0) != 0 && errno == ESRCH) {
                drop++;
                continue;
            }

            // 2. 路径结尾与 kind 一致
            char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
            if (proc_pidpath(pid, path, sizeof(path)) <= 0) {
                drop++;
                continue;
            }
            const char *k = get_web_proc_kind(path);
            if (!k || strcmp(k, kind) != 0) {
                drop++;
                continue;
            }

            // 3. 启动时间与 start 一致
            struct proc_bsdinfo bsd;
            memset(&bsd, 0, sizeof(bsd));
            if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bsd, sizeof(bsd)) != (int)sizeof(bsd)) {
                drop++;
                continue;
            }
            if (bsd.pbi_start_tvsec != sec || bsd.pbi_start_tvusec != usec) {
                drop++;
                continue;
            }

            if (g_xraised_count < MAX_XRAISED) {
                g_xraised[g_xraised_count].pid = pid;
                strncpy(g_xraised[g_xraised_count].kind, kind, sizeof(g_xraised[g_xraised_count].kind) - 1);
                g_xraised[g_xraised_count].kind[sizeof(g_xraised[g_xraised_count].kind) - 1] = '\0';
                g_xraised[g_xraised_count].start_tvsec = sec;
                g_xraised[g_xraised_count].start_tvusec = usec;
                g_xraised[g_xraised_count].orig = orig;
                g_xraised_count++;
                n++;
            } else {
                drop++;
            }
        }
        fclose(f);
    }

    jp_log("WEBDIAG xrestore_load n=%d drop=%d", n, drop);
    xraised_save();
}

typedef struct {
    pid_t pid;
    int32_t cur;
    const char *kind;
    uint64_t coal_jet;
    int coal_err;
} diag_proc_t;

static void diag_round(void) {
    struct timespec diag_start;
    clock_gettime(CLOCK_MONOTONIC, &diag_start);

    static time_t s_last_diag_sec = 0;
    long dt = 0;
    if (s_last_diag_sec > 0) {
        dt = (long)(diag_start.tv_sec - s_last_diag_sec);
        if (dt < 0) dt = 0;
    }
    s_last_diag_sec = diag_start.tv_sec;

    // 1. gone 扫描
    webdiag_gone_scan();

    // 2. 读开关：access("/var/mobile/Library/JetPilot/webdiag_protect", F_OK) == 0 为打开
    int mode_on = (access(XPROT_FLAG_PATH, F_OK) == 0);

    // 3. get_list
    int count = 0;
    memorystatus_priority_entry_t *list = get_list(&count);
    if (!list) {
        struct timespec diag_end;
        clock_gettime(CLOCK_MONOTONIC, &diag_end);
        long proc_ms = (long)((diag_end.tv_sec - diag_start.tv_sec) * 1000 +
                              (diag_end.tv_nsec - diag_start.tv_nsec) / 1000000);
        char locked_str[8];
        if (g_lock_fd < 0) snprintf(locked_str, sizeof(locked_str), "NA");
        else snprintf(locked_str, sizeof(locked_str), "%d", g_locked ? 1 : 0);

        jp_log("WEBDIAG xsum mode=%s locked=%s list=fail S=NA set=- at90=- below90=- coal_err=- raised_left=%d xfull=%d dt=%ld proc_ms=%ld tracked=%d dropped_total=%llu",
               mode_on ? "on" : "off",
               locked_str,
               g_xraised_count,
               g_xfull,
               dt,
               proc_ms,
               g_webdiag_count,
               (unsigned long long)g_webdiag_dropped_total);
        return;
    }

    int alloc_count = (count > 0) ? count : 1;
    diag_proc_t *procs = calloc(alloc_count, sizeof(diag_proc_t));
    if (!procs) {
        free(list);
        return;
    }

    int safari_idx = -1;
    for (int i = 0; i < count; i++) {
        procs[i].pid = list[i].pid;
        procs[i].cur = list[i].priority;
        if (procs[i].pid <= 0) continue;

        char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
        if (proc_pidpath(procs[i].pid, path, sizeof(path)) <= 0) continue;

        procs[i].kind = get_web_proc_kind(path);
        if (procs[i].kind && strcmp(procs[i].kind, "safari") == 0) {
            if (safari_idx < 0) {
                safari_idx = i;
            }
        }
    }

    if (safari_idx >= 0) {
        struct proc_pidcoalitioninfo coal;
        memset(&coal, 0, sizeof(coal));
        int ret = proc_pidinfo(procs[safari_idx].pid, PROC_PIDCOALITIONINFO, 0, &coal, sizeof(coal));
        if (ret == (int)sizeof(coal)) {
            procs[safari_idx].coal_jet = coal.coalition_id[1];
            procs[safari_idx].coal_err = 0;
        } else {
            procs[safari_idx].coal_jet = 0;
            procs[safari_idx].coal_err = 1;
        }
    }

    static uint64_t s_last_S = 0;
    static int s_S_fail_count = 0;

    uint64_t S = 0;
    int S_valid = 0;
    int S_is_stale = 0;

    if (safari_idx >= 0) {
        if (!procs[safari_idx].coal_err) {
            S = procs[safari_idx].coal_jet;
            S_valid = 1;
            S_is_stale = 0;
            s_last_S = S;
            s_S_fail_count = 0;
        } else {
            s_S_fail_count++;
            if (s_S_fail_count == 1 && s_last_S != 0) {
                S = s_last_S;
                S_valid = 1;
                S_is_stale = 1;
            } else {
                S = 0;
                S_valid = 0;
                s_last_S = 0;
            }
        }
    } else {
        S = 0;
        S_valid = 0;
        s_last_S = 0;
        s_S_fail_count = 0;
    }

    char coal_err_str[256] = "";
    int n_coal_err = 0;

    for (int i = 0; i < count; i++) {
        if (procs[i].kind && strcmp(procs[i].kind, "web") == 0) {
            struct proc_pidcoalitioninfo coal;
            memset(&coal, 0, sizeof(coal));
            int ret = proc_pidinfo(procs[i].pid, PROC_PIDCOALITIONINFO, 0, &coal, sizeof(coal));
            if (ret == (int)sizeof(coal)) {
                procs[i].coal_jet = coal.coalition_id[1];
                procs[i].coal_err = 0;
            } else {
                procs[i].coal_jet = 0;
                procs[i].coal_err = 1;
                if (n_coal_err > 0) {
                    strncat(coal_err_str, ",", sizeof(coal_err_str) - strlen(coal_err_str) - 1);
                }
                char pbuf[16];
                snprintf(pbuf, sizeof(pbuf), "%d", (int)procs[i].pid);
                strncat(coal_err_str, pbuf, sizeof(coal_err_str) - strlen(coal_err_str) - 1);
                n_coal_err++;
            }
        }
    }
    if (n_coal_err == 0) {
        snprintf(coal_err_str, sizeof(coal_err_str), "-");
    }

    char set_str[16] = "-";
    char at90_str[16] = "-";
    char below90_str[256] = "-";

    if (mode_on) {
        int set_count = 0;
        for (int i = 0; i < count; i++) {
            int in_set = 0;
            if (procs[i].kind && strcmp(procs[i].kind, "safari") == 0) {
                in_set = 1;
            } else if (procs[i].kind && strcmp(procs[i].kind, "web") == 0 &&
                       S_valid && !procs[i].coal_err && procs[i].coal_jet == S) {
                in_set = 1;
            }
            if (!in_set) continue;

            set_count++;

            if (procs[i].cur < 90) {
                int idx = -1;
                for (int r = 0; r < g_xraised_count; r++) {
                    if (g_xraised[r].pid == procs[i].pid) {
                        idx = r;
                        break;
                    }
                }

                if (idx >= 0) {
                    int rc = set_priority(procs[i].pid, 90);
                    if (rc == 0) {
                        jp_log("WEBDIAG xprot pid=%d kind=%s %d->90 ok",
                               (int)procs[i].pid, procs[i].kind, (int)procs[i].cur);
                        procs[i].cur = 90;
                    } else {
                        int alive = (kill(procs[i].pid, 0) == 0 || errno != ESRCH) ? 1 : 0;
                        jp_log("WEBDIAG xprot pid=%d kind=%s %d->90 fail alive=%d",
                               (int)procs[i].pid, procs[i].kind, (int)procs[i].cur, alive);
                    }
                } else {
                    if (g_xraised_count >= MAX_XRAISED) {
                        g_xfull++;
                    } else {
                        struct proc_bsdinfo bsd;
                        memset(&bsd, 0, sizeof(bsd));
                        if (proc_pidinfo(procs[i].pid, PROC_PIDTBSDINFO, 0, &bsd, sizeof(bsd)) != (int)sizeof(bsd)) {
                            // proc_pidinfo 失败不抬
                        } else {
                            g_xraised[g_xraised_count].pid = procs[i].pid;
                            strncpy(g_xraised[g_xraised_count].kind, procs[i].kind, sizeof(g_xraised[g_xraised_count].kind) - 1);
                            g_xraised[g_xraised_count].kind[sizeof(g_xraised[g_xraised_count].kind) - 1] = '\0';
                            g_xraised[g_xraised_count].start_tvsec = bsd.pbi_start_tvsec;
                            g_xraised[g_xraised_count].start_tvusec = bsd.pbi_start_tvusec;
                            g_xraised[g_xraised_count].orig = procs[i].cur;
                            g_xraised_count++;

                            if (xraised_save() != 0) {
                                g_xraised_count--;
                                jp_log("WEBDIAG xpersist fail pid=%d", (int)procs[i].pid);
                            } else {
                                int rc = set_priority(procs[i].pid, 90);
                                if (rc == 0) {
                                    jp_log("WEBDIAG xprot pid=%d kind=%s %d->90 ok",
                                           (int)procs[i].pid, procs[i].kind, (int)procs[i].cur);
                                    procs[i].cur = 90;
                                } else {
                                    int alive = (kill(procs[i].pid, 0) == 0 || errno != ESRCH) ? 1 : 0;
                                    jp_log("WEBDIAG xprot pid=%d kind=%s %d->90 fail alive=%d",
                                           (int)procs[i].pid, procs[i].kind, (int)procs[i].cur, alive);
                                }
                            }
                        }
                    }
                }
            }
        }

        int at90_count = 0;
        int n_below90 = 0;
        below90_str[0] = '\0';

        for (int i = 0; i < count; i++) {
            int in_set = 0;
            if (procs[i].kind && strcmp(procs[i].kind, "safari") == 0) {
                in_set = 1;
            } else if (procs[i].kind && strcmp(procs[i].kind, "web") == 0 &&
                       S_valid && !procs[i].coal_err && procs[i].coal_jet == S) {
                in_set = 1;
            }
            if (!in_set) continue;

            if (procs[i].cur >= 90) {
                at90_count++;
            } else {
                int alive = (kill(procs[i].pid, 0) == 0 || errno != ESRCH);
                if (alive) {
                    if (n_below90 > 0) {
                        strncat(below90_str, ",", sizeof(below90_str) - strlen(below90_str) - 1);
                    }
                    char pbuf[16];
                    snprintf(pbuf, sizeof(pbuf), "%d", (int)procs[i].pid);
                    strncat(below90_str, pbuf, sizeof(below90_str) - strlen(below90_str) - 1);
                    n_below90++;
                }
            }
        }
        if (n_below90 == 0) {
            snprintf(below90_str, sizeof(below90_str), "-");
        }
        snprintf(set_str, sizeof(set_str), "%d", set_count);
        snprintf(at90_str, sizeof(at90_str), "%d", at90_count);

    } else {
        int modified = 0;
        for (int r = 0; r < g_xraised_count; ) {
            pid_t rpid = g_xraised[r].pid;
            if (kill(rpid, 0) == -1 && errno == ESRCH) {
                g_xraised[r] = g_xraised[--g_xraised_count];
                modified = 1;
                continue;
            }

            int found = 0;
            int32_t rcur = 0;
            for (int i = 0; i < count; i++) {
                if (procs[i].pid == rpid) {
                    rcur = procs[i].cur;
                    found = 1;
                    break;
                }
            }

            if (!found) {
                if (kill(rpid, 0) == -1 && errno == ESRCH) {
                    g_xraised[r] = g_xraised[--g_xraised_count];
                    modified = 1;
                    continue;
                }
                r++;
                continue;
            }

            if (rcur != 90) {
                g_xraised[r] = g_xraised[--g_xraised_count];
                modified = 1;
                continue;
            }

            int rc = set_priority(rpid, g_xraised[r].orig);
            if (rc == 0) {
                jp_log("WEBDIAG xprot_off pid=%d kind=%s 90->%d ok",
                       (int)rpid, g_xraised[r].kind, (int)g_xraised[r].orig);
                g_xraised[r] = g_xraised[--g_xraised_count];
                modified = 1;
            } else {
                int alive = (kill(rpid, 0) == 0 || errno != ESRCH) ? 1 : 0;
                jp_log("WEBDIAG xprot_off pid=%d kind=%s 90->%d fail alive=%d",
                       (int)rpid, g_xraised[r].kind, (int)g_xraised[r].orig, alive);
                r++;
            }
        }
        if (modified) {
            xraised_save();
        }
    }

    struct timespec diag_end;
    clock_gettime(CLOCK_MONOTONIC, &diag_end);
    long proc_ms = (long)((diag_end.tv_sec - diag_start.tv_sec) * 1000 +
                          (diag_end.tv_nsec - diag_start.tv_nsec) / 1000000);

    char locked_str[8];
    if (g_lock_fd < 0) snprintf(locked_str, sizeof(locked_str), "NA");
    else snprintf(locked_str, sizeof(locked_str), "%d", g_locked ? 1 : 0);

    char S_str[32];
    if (S_valid) {
        if (S_is_stale) {
            snprintf(S_str, sizeof(S_str), "%llu(stale)", (unsigned long long)S);
        } else {
            snprintf(S_str, sizeof(S_str), "%llu", (unsigned long long)S);
        }
    } else {
        snprintf(S_str, sizeof(S_str), "NA");
    }

    jp_log("WEBDIAG xsum mode=%s locked=%s list=ok S=%s set=%s at90=%s below90=%s coal_err=%s raised_left=%d xfull=%d dt=%ld proc_ms=%ld tracked=%d dropped_total=%llu",
           mode_on ? "on" : "off",
           locked_str,
           S_str,
           set_str,
           at90_str,
           below90_str,
           coal_err_str,
           g_xraised_count,
           g_xfull,
           dt,
           proc_ms,
           g_webdiag_count,
           (unsigned long long)g_webdiag_dropped_total);

    free(procs);
    free(list);
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
    webdiag_init();
    jp_log("WEBDIAG xstart");
    xraised_restore();

    while (!should_stop()) {
        diag_round();

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

                webdiag(pid, path, cur);

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
            free(list);
            applied_prune();
            g_nold_targets = 0;  /* 首轮结束，清空旧记录 */
        }

        write_status(protected_now, demoted_now, plist, nplist);

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
