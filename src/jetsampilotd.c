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
        /* EPERM 多半是缺权限；记录一次方便排查 */
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

static int path_matches(const char *path, rule_t *rules, int n, int32_t *target_out) {
    for (int i = 0; i < n; i++) {
        if (strstr(path, rules[i].frag)) {
            if (target_out) *target_out = rules[i].target;
            return 1;
        }
    }
    return 0;
}

static void write_status(int protected_now, int demoted_now) {
    FILE *f = fopen(STATUS_PATH, "w");
    if (!f) return;
    time_t t = time(NULL);
    char ts[64]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));
    fprintf(f, "updated=%s\nprotect_rules=%d\ndemote_rules=%d\n"
               "protected_pids=%d\ndemoted_pids=%d\nreasserts_total=%lu\n",
            ts, g_nprotect, g_ndemote, protected_now, demoted_now, g_reassert_count);
    fclose(f);
}

static int screen_locked(void) {
    /* 没有可靠的公开方法判断锁屏；这里恒返回 0，循环间隔用固定值。
     * 如需省电，可改为读取 backboardd 状态（需额外权限，暂不做）。 */
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

    while (!should_stop()) {
        if (load_config() != 0) { sleep(5); continue; }

        int count = 0;
        memorystatus_priority_entry_t *list = get_list(&count);
        int protected_now = 0, demoted_now = 0;

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
                if (path_matches(path, g_protect, g_nprotect, &target)) {
                    protected_now++;
                    if (cur < target) {
                        if (set_priority(pid, target) == 0) g_reassert_count++;
                    }
                } else if (path_matches(path, g_demote, g_ndemote, NULL)) {
                    /* 只在非前台时降，且只降到 BACKGROUND，避免误杀正在用的 App */
                    if (cur < BAND_FOREGROUND && cur > BAND_BACKGROUND) {
                        demoted_now++;
                        set_priority(pid, BAND_BACKGROUND);
                    }
                }
            }
            free(list);
        }

        write_status(protected_now, demoted_now);

        /* 5~10 秒随机抖动；锁屏时放长到 20~30 秒 */
        int base = screen_locked() ? 20 : 5;
        int jitter = rand() % 6;
        sleep(base + jitter);
    }

    unlink(PID_PATH);
    fprintf(stderr, "[JetsamPilot] 收到停止信号，退出\n");
    return 0;
}
