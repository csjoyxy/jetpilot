/*
 * jetwebctl.c — JetWeb 瞬时 root 提档助手（非常驻进程，执行完毫秒级即退，零后台耗电）
 *
 * 职责：
 *   1. --probe
 *      验证 TrollStore persona(99/uid=0) 拉起与基础权限是否可用。
 *   2. --query <pid1> [pid2 ...]
 *      只读查询目标进程的当前 Jetsam 档位与物理内存占用（MB）。
 *   3. --apply <band> <max_mb> <pid1> [pid2 ...]
 *      将 JetWeb 主进程及其关联的 WebKit 进程（WebContent / Networking / GPU）抬到目标档位：
 *      - 安全红线 1：路径严格白名单（仅限容器内 .../JetWeb.app/JetWeb 与系统 WebKit.framework 下的 XPC 进程），绝不碰其他进程；
 *      - 安全红线 2：目标档位硬钳制在 [30, 95]，绝不触碰前台档 (>=100)；
 *      - 安全红线 3：WebContent 单进程物理内存超过 max_mb 时拒绝保活（若处于抬升态则还回 30 档），保护 4GB 内存。
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/resource.h>

#define PROC_PIDPATHINFO_MAXSIZE (4 * 1024)
extern int proc_pidpath(int pid, void *buffer, uint32_t buffersize);

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

#define MEMORYSTATUS_CMD_GET_PRIORITY_LIST            1
#define MEMORYSTATUS_CMD_SET_PRIORITY_PROPERTIES      2
#define MEMORYSTATUS_CMD_GET_PRIORITY_PROPERTIES      3

#define JW_BAND_BACKGROUND  30
#define JW_BAND_MAX_PROTECT 95
#define JW_BAND_FOREGROUND  100

typedef struct memorystatus_priority_entry {
    pid_t    pid;
    int32_t  priority;
    uint64_t user_data;
    int32_t  limit;
    uint32_t state;
} memorystatus_priority_entry_t;

typedef struct memorystatus_priority_properties {
    int32_t  priority;
    uint64_t user_data;
} memorystatus_priority_properties_t;

extern int memorystatus_control(uint32_t command, int32_t pid, uint32_t flags,
                                void *buffer, size_t buffersize);

typedef enum {
    JW_TARGET_INVALID = 0,
    JW_TARGET_HOST_APP = 1,
    JW_TARGET_WEBCONTENT = 2,
    JW_TARGET_WEBKIT_AUX = 3
} jw_target_kind_t;

static int has_suffix(const char *str, const char *suffix) {
    size_t slen = strlen(str);
    size_t xlen = strlen(suffix);
    return (slen >= xlen) && (strcmp(str + slen - xlen, suffix) == 0);
}

static jw_target_kind_t classify_allowed_path(const char *path) {
    if (!path || path[0] == '\0') return JW_TARGET_INVALID;
    if ((strncmp(path, "/var/containers/Bundle/Application/", 35) == 0 ||
         strncmp(path, "/private/var/containers/Bundle/Application/", 43) == 0) &&
        has_suffix(path, "/JetWeb.app/JetWeb")) {
        return JW_TARGET_HOST_APP;
    }
    if (strstr(path, "/System/Library/Frameworks/WebKit.framework/") != NULL) {
        if (strstr(path, "com.apple.WebKit.WebContent") != NULL) {
            return JW_TARGET_WEBCONTENT;
        }
        if (strstr(path, "com.apple.WebKit.Networking") != NULL ||
            strstr(path, "com.apple.WebKit.GPU") != NULL) {
            return JW_TARGET_WEBKIT_AUX;
        }
    }
    return JW_TARGET_INVALID;
}

static uint64_t read_phys_footprint_mb(pid_t pid) {
    struct rusage_info_v4 ri;
    memset(&ri, 0, sizeof(ri));
    if (proc_pid_rusage((int)pid, RUSAGE_INFO_V4, (rusage_info_t *)&ri) == 0) {
        return ri.ri_phys_footprint / (1024ULL * 1024ULL);
    }
    return 0;
}

static int read_priority(pid_t pid, int32_t *out_prio) {
    if (pid <= 0 || !out_prio) {
        errno = EINVAL;
        return -1;
    }
    memorystatus_priority_properties_t props;
    memset(&props, 0, sizeof(props));
    if (memorystatus_control(MEMORYSTATUS_CMD_GET_PRIORITY_PROPERTIES,
                             (int32_t)pid, 0, &props, sizeof(props)) == 0) {
        *out_prio = props.priority;
        return 0;
    }

    int sz = memorystatus_control(MEMORYSTATUS_CMD_GET_PRIORITY_LIST, 0, 0, NULL, 0);
    if (sz <= 0) return -1;
    size_t alloc_sz = (size_t)sz + sizeof(memorystatus_priority_entry_t) * 16;
    memorystatus_priority_entry_t *list = (memorystatus_priority_entry_t *)malloc(alloc_sz);
    if (!list) return -1;
    int got = memorystatus_control(MEMORYSTATUS_CMD_GET_PRIORITY_LIST, 0, 0, list, alloc_sz);
    if (got <= 0) {
        int saved_err = errno;
        free(list);
        errno = saved_err ? saved_err : EIO;
        return -1;
    }
    int count = got / (int)sizeof(memorystatus_priority_entry_t);
    int found = 0;
    for (int i = 0; i < count; i++) {
        if (list[i].pid == pid) {
            *out_prio = list[i].priority;
            found = 1;
            break;
        }
    }
    free(list);
    if (!found) {
        errno = ESRCH;
        return -1;
    }
    return 0;
}

static int set_priority_band(pid_t pid, int32_t band) {
    if (band < 0) band = 0;
    if (band > JW_BAND_MAX_PROTECT) band = JW_BAND_MAX_PROTECT;
    memorystatus_priority_properties_t sp;
    memset(&sp, 0, sizeof(sp));
    sp.priority = band;
    return memorystatus_control(MEMORYSTATUS_CMD_SET_PRIORITY_PROPERTIES,
                                (int32_t)pid, 0, &sp, sizeof(sp));
}

static int parse_positive_int(const char *s, int *out_val) {
    if (!s || !*s || !out_val) return -1;
    char *endp = NULL;
    errno = 0;
    long v = strtol(s, &endp, 10);
    if (errno != 0 || !endp || *endp != '\0' || v <= 0 || v > 2147483647L) return -1;
    *out_val = (int)v;
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc >= 2 && strcmp(argv[1], "--probe") == 0) {
        return (geteuid() == 0) ? 0 : 2;
    }

    if (geteuid() != 0) {
        fprintf(stderr, "ERR not_root euid=%u\n", (unsigned)geteuid());
        return 2;
    }

    if (argc >= 3 && strcmp(argv[1], "--query") == 0) {
        for (int i = 2; i < argc; i++) {
            int pid_i = 0;
            if (parse_positive_int(argv[i], &pid_i) != 0) continue;
            pid_t pid = (pid_t)pid_i;
            char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
            if (proc_pidpath((int)pid, path, sizeof(path)) <= 0) {
                printf("GONE pid=%d\n", pid_i);
                continue;
            }
            jw_target_kind_t kind = classify_allowed_path(path);
            if (kind == JW_TARGET_INVALID) {
                printf("SKIP_PATH pid=%d\n", pid_i);
                continue;
            }
            int32_t cur = -1;
            (void)read_priority(pid, &cur);
            uint64_t fp = read_phys_footprint_mb(pid);
            printf("STAT pid=%d kind=%d cur=%d fp=%llu\n",
                   pid_i, (int)kind, (int)cur, (unsigned long long)fp);
        }
        return 0;
    }

    if (argc >= 5 && strcmp(argv[1], "--apply") == 0) {
        int band = 85;
        int max_mb = 450;
        if (parse_positive_int(argv[2], &band) != 0) band = 85;
        if (parse_positive_int(argv[3], &max_mb) != 0) max_mb = 450;
        if (band < JW_BAND_BACKGROUND) band = JW_BAND_BACKGROUND;
        if (band > JW_BAND_MAX_PROTECT) band = JW_BAND_MAX_PROTECT;
        if (max_mb < 100) max_mb = 100;
        if (max_mb > 1500) max_mb = 1500;

        for (int i = 4; i < argc; i++) {
            int pid_i = 0;
            if (parse_positive_int(argv[i], &pid_i) != 0) continue;
            pid_t pid = (pid_t)pid_i;

            char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
            if (proc_pidpath((int)pid, path, sizeof(path)) <= 0) {
                printf("GONE pid=%d\n", pid_i);
                continue;
            }
            jw_target_kind_t kind = classify_allowed_path(path);
            if (kind == JW_TARGET_INVALID) {
                printf("SKIP_PATH pid=%d\n", pid_i);
                continue;
            }

            int32_t cur = -1;
            if (read_priority(pid, &cur) != 0) {
                printf("ERR_READ pid=%d errno=%d\n", pid_i, errno);
                continue;
            }
            uint64_t fp = read_phys_footprint_mb(pid);

            /* 绝不触碰前台进程 (>=100) */
            if (cur >= JW_BAND_FOREGROUND) {
                printf("FG pid=%d cur=%d fp=%llu\n", pid_i, (int)cur, (unsigned long long)fp);
                continue;
            }

            /* 单标签 WebContent 内存安全阀：超限拒绝保活，若已被抬升则还回普通后台 30 档 */
            if (kind == JW_TARGET_WEBCONTENT && fp > (uint64_t)max_mb) {
                int32_t after = cur;
                if (cur > JW_BAND_BACKGROUND) {
                    if (set_priority_band(pid, JW_BAND_BACKGROUND) == 0) {
                        after = JW_BAND_BACKGROUND;
                        (void)read_priority(pid, &after);
                    }
                }
                printf("OVER_MEM pid=%d before=%d after=%d fp=%llu max=%d\n",
                       pid_i, (int)cur, (int)after, (unsigned long long)fp, max_mb);
                continue;
            }

            if (cur == band) {
                printf("OK pid=%d before=%d after=%d fp=%llu\n",
                       pid_i, (int)cur, (int)cur, (unsigned long long)fp);
                continue;
            }

            if (set_priority_band(pid, band) == 0) {
                int32_t after = band;
                (void)read_priority(pid, &after);
                printf("OK pid=%d before=%d after=%d fp=%llu\n",
                       pid_i, (int)cur, (int)after, (unsigned long long)fp);
            } else {
                printf("ERR_SET pid=%d before=%d errno=%d\n", pid_i, (int)cur, errno);
            }
        }
        return 0;
    }

    fprintf(stderr, "Usage: jetwebctl --probe | --query <pids...> | --apply <band> <max_mb> <pids...>\n");
    return 1;
}
