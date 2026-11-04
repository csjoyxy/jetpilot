/*
 * jetsampilotd.c — JetsamPilot 常驻进程
 *
 * 作用：周期性读取全系统 jetsam 档位，把「受保护」App（按 bundle 路径片段匹配）
 *       的后台档位抬到目标值，让系统在内存紧张时先杀别的进程。
 *
 * 适配：iOS 16.x（档位数值自 iOS 16 起整体 ×10）。iPhone 11 / 16.4.1 主目标。
 * 前提：必须以 root 运行，且二进制带 com.apple.private.memorystatus
 *       与 com.apple.private.jetsam.modify-priority 权限（见 entitlements-daemon.plist）。
 *       这些权限要求按 XNU 一般规则推断，未在 16.4.1 上逐行核实。
 *
 * 编译（GitHub Actions macos-latest，见 .github/workflows/build.yml）：
 *   xcrun -sdk iphoneos clang -arch arm64 -O2 -Wall \
 *     -miphoneos-version-min=15.0 jetsampilotd.c -o jetsampilotd
 *   ldid -Sentitlements-daemon.plist jetsampilotd
 *
 * 配置：/var/mobile/Library/JetsamPilot/config.txt
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
/* iOS SDK 没有 libproc.h，手动声明 */
#define PROC_PIDPATHINFO_MAXSIZE (4 * 1024)
extern int proc_pidpath(int pid, void *buffer, uint32_t buffersize);
#include <sys/sysctl.h>
#include <sys/types.h>
#include <signal.h>
#include <notify.h>   /* 锁屏状态：com.apple.springboard.lockstate（libSystem 自带） */
#include <sys/select.h>
#include <sys/time.h>

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
#define CONFIG_DIR  "/var/mobile/Library/JetsamPilot"
#define CONFIG_PATH CONFIG_DIR "/config.txt"
#define FLAG_PATH   CONFIG_DIR "/running.flag"
#define STATUS_PATH CONFIG_DIR "/status.txt"
#define PID_PATH    CONFIG_DIR "/daemon.pid"

#define MAX_RULES 32
typedef struct {
    char    frag[256];
    int32_t target;   /* protect 用；demote 规则 target = -1 */
} rule_t;

static rule_t g_protect[MAX_RULES];  static int g_nprotect = 0;
static rule_t g_demote[MAX_RULES];   static int g_ndemote  = 0;
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
static int g_first_round_done = 0;  /* 首轮结束后置 1，此后无记录 pid 不再认领 */

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

static int load_config(void) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return -1;
    g_nprotect = g_ndemote = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        char verb[32], frag[256]; int band = 0;
        int n = sscanf(p, "%31s %255s %d", verb, frag, &band);
        if (n < 2) continue;
        if (strcmp(verb, "protect") == 0 && g_nprotect < MAX_RULES) {
            if (n < 3 || band <= 0) band = BAND_FOREGROUND - 10; /* 默认 90 */
            if (band > BAND_MAX_PROTECT) band = BAND_MAX_PROTECT;
            strncpy(g_protect[g_nprotect].frag, frag, 255);
            g_protect[g_nprotect].target = band;
            g_nprotect++;
        } else if (strcmp(verb, "demote") == 0 && g_ndemote < MAX_RULES) {
            strncpy(g_demote[g_ndemote].frag, frag, 255);
            g_demote[g_ndemote].target = -1;
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
        fprintf(stderr, "[JetsamPilot] set pid %d -> %d 失败: %s\n",
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

static int path_matches(const char *path, rule_t *rules, int n,
                        int32_t *target_out, const char **frag_out) {
    for (int i = 0; i < n; i++) {
        if (strstr(path, rules[i].frag)) {
            if (target_out) *target_out = rules[i].target;
            if (frag_out) *frag_out = rules[i].frag;
            return 1;
        }
    }
    return 0;
}

/* status.txt 格式（App 端解析）：
 *   原有 key=value 行保持不变；新增 protected_app 行（可多行）：
 *     protected_app=<路径片段>|<pid>|<当前档位>|<目标档位>
 *   旧版 App 忽略不认识的行；新版 App 读不到这些行时显示"暂无数据"。 */
#define MAX_PROTECTED_LIST 64
typedef struct {
    const char *frag;   /* 指向 g_protect 规则的 frag，本轮 scan 内有效 */
    pid_t       pid;
    int32_t     cur;
    int32_t     target;
} protected_entry_t;

static void write_status(int protected_now, int demoted_now,
                         protected_entry_t *plist, int nplist) {
    /* 先写临时文件再 rename，原子替换：App 永远读不到写一半的 status.txt */
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", STATUS_PATH);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    time_t t = time(NULL);
    char ts[64]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));
    fprintf(f, "updated=%s\nprotect_rules=%d\ndemote_rules=%d\n"
               "protected_pids=%d\ndemoted_pids=%d\nreasserts_total=%lu\n",
            ts, g_nprotect, g_ndemote, protected_now, demoted_now, g_reassert_count);
    /* 失败记录：无失败时只写总数 0，App 端隐藏红色区域 */
    fprintf(f, "set_fail_total=%lu\n", g_set_fail_total);
    if (g_set_fail_total > 0)
        fprintf(f, "last_error=%d|%s|%d|%d|%s\n",
                g_last_err_no, g_last_err_text,
                (int)g_last_err_pid, g_last_err_target, g_last_err_time);
    for (int i = 0; i < nplist; i++)
        fprintf(f, "protected_app=%s|%d|%d|%d\n",
                plist[i].frag, (int)plist[i].pid, plist[i].cur, plist[i].target);
    int werr = ferror(f);  /* 必须在 fclose 之前查 */
    fclose(f);
    if (werr) { unlink(tmp); return; }  /* 写坏了就不转正，保留旧 status.txt */
    rename(tmp, STATUS_PATH);
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

int main(void) {
    if (geteuid() != 0) {
        fprintf(stderr, "[JetsamPilot] 必须以 root 运行\n");
        return 1;
    }
    /* 防止重复启动：已有实例在跑就直接退出 */
    FILE *pf = fopen(PID_PATH, "r");
    if (pf) {
        int old = 0;
        if (fscanf(pf, "%d", &old) == 1 && old > 0 && kill(old, 0) == 0) {
            fclose(pf);
            fprintf(stderr, "[JetsamPilot] 已在运行 (pid %d)\n", old);
            return 0;
        }
        fclose(pf);
    }
    pf = fopen(PID_PATH, "w");
    if (pf) { fprintf(pf, "%d\n", getpid()); fclose(pf); }

    setsid();  /* 脱离拉起它的 App 会话 */
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    lockmon_init();  /* 锁屏监听（失败则保持常速轮询，不影响保护） */

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
                applied_t *prev = applied_find(pid);
                if (prev) prev->seen = 1;
                if (path_matches(path, g_protect, g_nprotect, &target, &frag)) {
                    protected_now++;
                    if (nplist < MAX_PROTECTED_LIST)
                        plist[nplist++] = (protected_entry_t){frag, pid, cur, target};
                    if (cur < target) {
                        /* 低于目标：抬高 */
                        if (set_priority(pid, target) == 0) {
                            g_reassert_count++;
                            applied_set(pid, target);
                        }
                    } else if (cur > target && cur < BAND_FOREGROUND) {
                        int is_plugin = (strstr(path, "/PlugIns/") != NULL);
                        if (!prev && !g_first_round_done && !is_plugin) {
                            /* 首轮认领旧档位 */
                            applied_set(pid, cur);
                            prev = applied_find(pid);
                        }
                        if (prev && prev->band == cur) {
                            /* 高于目标且是我们（或旧实例）设的：跟着降；
                             * 插件类进程不认领 */
                            if (set_priority(pid, target) == 0) {
                                g_reassert_count++;
                                applied_set(pid, target);
                            }
                        }
                    }
                } else if (path_matches(path, g_demote, g_ndemote, NULL, NULL)) {
                    /* 只在非前台时降，且只降到 BACKGROUND，避免误杀正在用的 App */
                    if (cur < BAND_FOREGROUND && cur > BAND_BACKGROUND) {
                        demoted_now++;
                        set_priority(pid, BAND_BACKGROUND);
                    }
                } else if (prev && prev->band == cur && cur > BAND_BACKGROUND &&
                           cur < BAND_FOREGROUND) {
                    /* 规则被删：把我们抬过的档位还回普通后台 */
                    if (set_priority(pid, BAND_BACKGROUND) == 0)
                        prev->band = BAND_BACKGROUND;
                }
            }
            free(list);
            applied_prune();
            g_first_round_done = 1;  /* 首轮结束 */
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

    unlink(PID_PATH);
    fprintf(stderr, "[JetsamPilot] 收到停止信号，退出\n");
    return 0;
}
