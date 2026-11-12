#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <sys/resource.h>

#if defined(__has_include)
  #if __has_include(<libproc.h>)
    #include <libproc.h>
  #endif
#endif

#if defined(__APPLE__)
  #include <CoreFoundation/CoreFoundation.h>
#endif

/* ---- XNU memorystatus 相关定义 ---- */
#define MEMORYSTATUS_CMD_GET_PRIORITY_LIST        1
#define MEMORYSTATUS_CMD_SET_PRIORITY_PROPERTIES  2
#define MEMORYSTATUS_CMD_SET_MEMLIMIT_PROPERTIES  7
#define MEMORYSTATUS_CMD_GET_MEMLIMIT_PROPERTIES  8

#define MEMORYSTATUS_MEMLIMIT_ATTR_FATAL          0x1

typedef struct memorystatus_priority_entry {
    pid_t    pid;
    int32_t  priority;
    uint64_t user_data;
    int32_t  limit;        /* MB */
    uint32_t state;
} memorystatus_priority_entry_t;

typedef struct memorystatus_memlimit_properties {
    int32_t  memlimit_active;        /* 前台限额 MB */
    uint32_t memlimit_active_attr;   /* 前台属性 (0x1=FATAL) */
    int32_t  memlimit_inactive;      /* 后台限额 MB */
    uint32_t memlimit_inactive_attr; /* 后台属性 (0x1=FATAL) */
} memorystatus_memlimit_properties_t;

/* N11: 结构体大小与字段布局静态断言（与 xnu-8796.101.5 kern_memorystatus.h 严格一致） */
_Static_assert(sizeof(memorystatus_memlimit_properties_t) == 16,
               "sizeof(memorystatus_memlimit_properties_t) must be 16");
_Static_assert(sizeof(memorystatus_priority_entry_t) == 24,
               "sizeof(memorystatus_priority_entry_t) must be 24");

extern int memorystatus_control(uint32_t command, int32_t pid, uint32_t flags,
                                void *buffer, size_t buffersize);

/* ---- XNU libproc / bsdinfo 补齐声明（若 SDK 未自带 libproc.h） ---- */
#ifndef PROC_PIDPATHINFO_MAXSIZE
#define PROC_PIDPATHINFO_MAXSIZE (4 * 1024)
#endif

#ifndef PROC_PIDTBSDINFO
#define PROC_PIDTBSDINFO 3
struct proc_bsdinfo {
    uint32_t                pbi_flags;
    uint32_t                pbi_status;
    uint32_t                pbi_xstatus;
    uint32_t                pbi_pid;
    uint32_t                pbi_ppid;
    uid_t                   pbi_uid;
    gid_t                   pbi_gid;
    uid_t                   pbi_ruid;
    gid_t                   pbi_rgid;
    uid_t                   pbi_svuid;
    gid_t                   pbi_svgid;
    uint32_t                rfu_1;
    char                    pbi_comm[16];
    char                    pbi_name[32];
    uint32_t                pbi_nfiles;
    uint32_t                pbi_pgid;
    uint32_t                pbi_pjobc;
    uint32_t                rfu_2;
    uint64_t                pbi_start_tvsec;
    uint64_t                pbi_start_tvusec;
    uint64_t                pbi_uniqueid;
};
#endif

#if !defined(__has_include) || !__has_include(<libproc.h>)
extern int proc_pidpath(int pid, void *buffer, uint32_t buffersize);
extern int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);
#endif

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

#if !defined(__has_include) || !__has_include(<libproc.h>)
extern int proc_pid_rusage(int pid, int flavor, rusage_info_t *buffer);
#endif

/* ---- 进程身份标识（防 PID 复用） ---- */
typedef struct proc_identity {
    pid_t    pid;
    uint64_t uniqueid;
    uint64_t start_sec;
    uint64_t start_usec;
    char     path[PROC_PIDPATHINFO_MAXSIZE];
} proc_identity_t;

static int get_proc_identity(pid_t pid, proc_identity_t *ident) {
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
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bsdinfo, sizeof(bsdinfo)) == sizeof(bsdinfo)) {
        ident->uniqueid = bsdinfo.pbi_uniqueid;
        ident->start_sec = bsdinfo.pbi_start_tvsec;
        ident->start_usec = bsdinfo.pbi_start_tvusec;
        return 0;
    }
    return -1;
}

static int compare_proc_identity(const proc_identity_t *a, const proc_identity_t *b) {
    if (!a || !b) return 0;
    if (a->pid != b->pid) return 0;
    if (a->uniqueid != 0 && b->uniqueid != 0 && a->uniqueid != b->uniqueid) return 0;
    if (a->start_sec != 0 && b->start_sec != 0 &&
        (a->start_sec != b->start_sec || a->start_usec != b->start_usec)) return 0;
    if (strcmp(a->path, b->path) != 0) return 0;
    return 1;
}

/* ---- 状态文件管理（Q1, N5, N7, D1） ---- */
#define JPLIMIT_STATE_DIR "/var/mobile/Library/jplimit"

static const char *get_state_dir(void) {
    return JPLIMIT_STATE_DIR;
}

static void get_state_path(pid_t pid, char *out, size_t out_sz) {
    snprintf(out, out_sz, "%s/state-%d.txt", get_state_dir(), pid);
}

static int ensure_state_dir(void) {
    const char *dir = get_state_dir();
    struct stat st;
    if (lstat(dir, &st) != 0) {
        char tmp[PROC_PIDPATHINFO_MAXSIZE];
        snprintf(tmp, sizeof(tmp), "%s", dir);
        for (char *p = tmp + 1; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                mkdir(tmp, 0700);
                *p = '/';
            }
        }
        if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
            return -1;
        }
        if (lstat(dir, &st) != 0) {
            return -1;
        }
    }
    if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) {
        return -1;
    }
    if (st.st_uid != geteuid()) {
        return -1;
    }
    if ((st.st_mode & 0777) != 0700) {
        chmod(dir, 0700);
    }
    return 0;
}

static void cleanup_stale_state_files(void) {
    const char *dir = get_state_dir();
    struct stat dir_st;
    if (lstat(dir, &dir_st) != 0) return;
    if (S_ISLNK(dir_st.st_mode) || !S_ISDIR(dir_st.st_mode)) return;
    if (dir_st.st_uid != geteuid()) return;
    if ((dir_st.st_mode & 0777) != 0700) return;

    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        int pid = 0;
        char extra = '\0';
        if (sscanf(ent->d_name, "state-%d.txt%c", &pid, &extra) == 1 && pid > 0) {
            char full_path[PROC_PIDPATHINFO_MAXSIZE];
            snprintf(full_path, sizeof(full_path), "%s/%s", dir, ent->d_name);

            struct stat file_st;
            if (lstat(full_path, &file_st) != 0) continue;
            if (S_ISLNK(file_st.st_mode) || !S_ISREG(file_st.st_mode)) continue;
            if (file_st.st_uid != geteuid()) continue;
            if ((file_st.st_mode & 0777) != 0600) continue;

            int fd = open(full_path, O_RDONLY | O_NOFOLLOW);
            if (fd < 0) continue;
            FILE *f = fdopen(fd, "r");
            if (!f) {
                close(fd);
                continue;
            }
            char line[PROC_PIDPATHINFO_MAXSIZE + 64];
            int file_pid = -1;
            int has_pid_field = 0, has_sec_field = 0;
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "pid=", 4) == 0) {
                    file_pid = atoi(line + 4);
                    has_pid_field = 1;
                } else if (strncmp(line, "start_sec=", 10) == 0) {
                    has_sec_field = 1;
                }
            }
            fclose(f);

            if (!has_pid_field || !has_sec_field || file_pid != pid) {
                continue;
            }

            if (kill(pid, 0) == -1 && errno == ESRCH) {
                unlink(full_path);
            }
        }
    }
    closedir(d);
}

typedef struct jplimit_saved_state {
    pid_t    pid;
    uint64_t start_sec;
    uint64_t start_usec;
    int32_t  orig_active;
    uint32_t orig_active_attr;
    int32_t  orig_inactive;
    uint32_t orig_inactive_attr;
    int      suspect;
    uint64_t uniqueid;
    char     path[PROC_PIDPATHINFO_MAXSIZE];
} jplimit_saved_state_t;

static int load_trusted_state_file(pid_t pid, const proc_identity_t *ident, jplimit_saved_state_t *st_out) {
    if (!st_out || pid <= 0) return -1;
    memset(st_out, 0, sizeof(*st_out));

    const char *dir = get_state_dir();
    struct stat dir_st;
    if (lstat(dir, &dir_st) != 0) return -1;
    if (S_ISLNK(dir_st.st_mode) || !S_ISDIR(dir_st.st_mode)) return -1;
    if (dir_st.st_uid != geteuid()) return -1;
    if ((dir_st.st_mode & 0777) != 0700) return -1;

    char path[PROC_PIDPATHINFO_MAXSIZE];
    get_state_path(pid, path, sizeof(path));

    struct stat file_st;
    if (lstat(path, &file_st) != 0) return -1;
    if (S_ISLNK(file_st.st_mode) || !S_ISREG(file_st.st_mode)) return -1;
    if (file_st.st_uid != geteuid()) return -1;
    if ((file_st.st_mode & 0777) != 0600) return -1;

    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -1;

    FILE *f = fdopen(fd, "r");
    if (!f) {
        close(fd);
        return -1;
    }

    char line[PROC_PIDPATHINFO_MAXSIZE + 64];
    int has_pid = 0, has_sec = 0, has_usec = 0;
    int has_act = 0, has_act_attr = 0, has_inact = 0, has_inact_attr = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "pid=", 4) == 0) {
            st_out->pid = (pid_t)atoi(line + 4);
            has_pid = 1;
        } else if (strncmp(line, "start_sec=", 10) == 0) {
            st_out->start_sec = strtoull(line + 10, NULL, 10);
            has_sec = 1;
        } else if (strncmp(line, "start_usec=", 11) == 0) {
            st_out->start_usec = strtoull(line + 11, NULL, 10);
            has_usec = 1;
        } else if (strncmp(line, "orig_active=", 12) == 0) {
            st_out->orig_active = (int32_t)atoi(line + 12);
            has_act = 1;
        } else if (strncmp(line, "orig_active_attr=", 17) == 0) {
            st_out->orig_active_attr = (uint32_t)strtoul(line + 17, NULL, 0);
            has_act_attr = 1;
        } else if (strncmp(line, "orig_active_fatal=", 18) == 0) {
            if (!has_act_attr) {
                st_out->orig_active_attr = atoi(line + 18) ? MEMORYSTATUS_MEMLIMIT_ATTR_FATAL : 0;
            }
            has_act_attr = 1;
        } else if (strncmp(line, "orig_inactive=", 14) == 0) {
            st_out->orig_inactive = (int32_t)atoi(line + 14);
            has_inact = 1;
        } else if (strncmp(line, "orig_inactive_attr=", 19) == 0) {
            st_out->orig_inactive_attr = (uint32_t)strtoul(line + 19, NULL, 0);
            has_inact_attr = 1;
        } else if (strncmp(line, "orig_inactive_fatal=", 20) == 0) {
            if (!has_inact_attr) {
                st_out->orig_inactive_attr = atoi(line + 20) ? MEMORYSTATUS_MEMLIMIT_ATTR_FATAL : 0;
            }
            has_inact_attr = 1;
        } else if (strncmp(line, "suspect=", 8) == 0) {
            st_out->suspect = atoi(line + 8);
        } else if (strncmp(line, "uniqueid=", 9) == 0) {
            st_out->uniqueid = strtoull(line + 9, NULL, 10);
        } else if (strncmp(line, "path=", 5) == 0) {
            size_t len = strlen(line + 5);
            if (len > 0 && line[5 + len - 1] == '\n') line[5 + len - 1] = '\0';
            snprintf(st_out->path, sizeof(st_out->path), "%s", line + 5);
        }
    }
    fclose(f);

    if (!has_pid || !has_sec || !has_usec || !has_act || !has_act_attr || !has_inact || !has_inact_attr) {
        return -1;
    }

    if (ident != NULL) {
        if (st_out->pid != ident->pid ||
            st_out->start_sec != ident->start_sec ||
            st_out->start_usec != ident->start_usec) {
            return -1;
        }
    } else if (st_out->pid != pid) {
        return -1;
    }

    return 0;
}

static int save_state_file(const jplimit_saved_state_t *st) {
    if (!st || st->pid <= 0) return -1;
    if (ensure_state_dir() != 0) return -1;

    char path[PROC_PIDPATHINFO_MAXSIZE];
    get_state_path(st->pid, path, sizeof(path));

    struct stat file_st;
    if (lstat(path, &file_st) == 0) {
        if (S_ISLNK(file_st.st_mode) || !S_ISREG(file_st.st_mode) || file_st.st_uid != geteuid()) {
            return -1;
        }
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    fchmod(fd, 0600);

    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        return -1;
    }
    fprintf(f, "pid=%d\n", st->pid);
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
    return 0;
}

static void clear_state_file(pid_t pid) {
    char path[PROC_PIDPATHINFO_MAXSIZE];
    get_state_path(pid, path, sizeof(path));
    struct stat file_st;
    if (lstat(path, &file_st) == 0) {
        if (!S_ISLNK(file_st.st_mode) && S_ISREG(file_st.st_mode) && file_st.st_uid == geteuid()) {
            unlink(path);
        }
    }
}

/* 前向声明 */
static int get_memlimit_props(pid_t pid, memorystatus_memlimit_properties_t *props_out);

/* Q1 & N5: 首次写入前先 GET 存入状态文件，同 pid 再写不覆盖原值 */
static int get_or_record_initial_state(pid_t pid, const proc_identity_t *ident,
                                       jplimit_saved_state_t *st_out) {
    if (!ident || pid <= 0 || !st_out) return -1;

    jplimit_saved_state_t existing;
    if (load_trusted_state_file(pid, ident, &existing) == 0) {
        if (existing.pid == ident->pid &&
            existing.start_sec == ident->start_sec &&
            existing.start_usec == ident->start_usec) {
            /* 同一个 pid 再写不覆盖原值 */
            *st_out = existing;
            return 0;
        }
    }

    /* 第一次写入前先 GET */
    memorystatus_memlimit_properties_t props;
    int err = get_memlimit_props(pid, &props);
    if (err != 0) {
        fprintf(stderr, "Error: initial GET failed: errno=%d (%s)\n", err, strerror(err));
        return -1;
    }

    memset(st_out, 0, sizeof(*st_out));
    st_out->pid = ident->pid;
    st_out->start_sec = ident->start_sec;
    st_out->start_usec = ident->start_usec;
    st_out->uniqueid = ident->uniqueid;
    snprintf(st_out->path, sizeof(st_out->path), "%s", ident->path);
    st_out->orig_active = props.memlimit_active;
    st_out->orig_active_attr = props.memlimit_active_attr;
    st_out->orig_inactive = props.memlimit_inactive;
    st_out->orig_inactive_attr = props.memlimit_inactive_attr;

    /* R4: 比较当前 GET 到的 inactive 和 active：值不一样，或者 FATAL 位不一样，就记 suspect=1 */
    int act_fatal = (props.memlimit_active_attr & MEMORYSTATUS_MEMLIMIT_ATTR_FATAL) ? 1 : 0;
    int inact_fatal = (props.memlimit_inactive_attr & MEMORYSTATUS_MEMLIMIT_ATTR_FATAL) ? 1 : 0;
    if (props.memlimit_inactive != props.memlimit_active || inact_fatal != act_fatal) {
        st_out->suspect = 1;
        printf("ORIG_SUSPECT inactive=%d/%u active=%d/%u\n",
               props.memlimit_inactive, props.memlimit_inactive_attr,
               props.memlimit_active, props.memlimit_active_attr);
        fflush(stdout);
    } else {
        st_out->suspect = 0;
    }

    if (save_state_file(st_out) != 0) {
        fprintf(stderr, "Error: failed to save state file for pid %d: errno=%d (%s)\n",
                pid, errno, strerror(errno));
        return -1;
    }
    return 0;
}

/* 读取 App 的 Info.plist 元数据（CFBundleExecutable 与 CFBundleIdentifier） */
static int read_info_plist_meta(const char *app_dir, char *exe_out, size_t exe_sz,
                                char *bid_out, size_t bid_sz) {
    char plist_path[PROC_PIDPATHINFO_MAXSIZE];
    snprintf(plist_path, sizeof(plist_path), "%s/Info.plist", app_dir);

    FILE *f = fopen(plist_path, "rb");
    if (!f) {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 10 * 1024 * 1024) {
        fclose(f);
        return -1;
    }
    unsigned char *buf = (unsigned char *)malloc(sz);
    if (!buf) {
        fclose(f);
        return -1;
    }
    if (fread(buf, 1, sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

#if defined(__APPLE__)
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, buf, sz);
    free(buf);
    if (!data) return -1;

    CFErrorRef err = NULL;
    CFPropertyListRef plist = CFPropertyListCreateWithData(kCFAllocatorDefault, data,
                                                           kCFPropertyListImmutable, NULL, &err);
    CFRelease(data);
    if (!plist) {
        if (err) CFRelease(err);
        return -1;
    }
    if (CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
        CFRelease(plist);
        return -1;
    }

    CFDictionaryRef dict = (CFDictionaryRef)plist;
    if (exe_out && exe_sz > 0) {
        CFStringRef cf_exe = (CFStringRef)CFDictionaryGetValue(dict, CFSTR("CFBundleExecutable"));
        if (cf_exe && CFGetTypeID(cf_exe) == CFStringGetTypeID()) {
            CFStringGetCString(cf_exe, exe_out, exe_sz, kCFStringEncodingUTF8);
        }
    }
    if (bid_out && bid_sz > 0) {
        CFStringRef cf_bid = (CFStringRef)CFDictionaryGetValue(dict, CFSTR("CFBundleIdentifier"));
        if (cf_bid && CFGetTypeID(cf_bid) == CFStringGetTypeID()) {
            CFStringGetCString(cf_bid, bid_out, bid_sz, kCFStringEncodingUTF8);
        }
    }
    CFRelease(plist);
    return 0;
#else
    char *text = (char *)buf;
    char *k = strstr(text, "CFBundleExecutable");
    if (k) {
        char *str_start = strstr(k, "<string>");
        if (str_start) {
            str_start += 8;
            char *str_end = strstr(str_start, "</string>");
            if (str_end && exe_out && exe_sz > 0) {
                size_t len = str_end - str_start;
                if (len >= exe_sz) len = exe_sz - 1;
                memcpy(exe_out, str_start, len);
                exe_out[len] = '\0';
            }
        }
    }
    k = strstr(text, "CFBundleIdentifier");
    if (k) {
        char *str_start = strstr(k, "<string>");
        if (str_start) {
            str_start += 8;
            char *str_end = strstr(str_start, "</string>");
            if (str_end && bid_out && bid_sz > 0) {
                size_t len = str_end - str_start;
                if (len >= bid_sz) len = bid_sz - 1;
                memcpy(bid_out, str_start, len);
                bid_out[len] = '\0';
            }
        }
    }
    free(buf);
    return 0;
#endif
}

/* 检查是否为合法的用户 App 主程序（N2, N3, N4 白名单机制）：
 * 1. 约束路径结构为 /var/containers/Bundle/Application/<UUID>/<AppName>.app/<ExecutableName>，无多余子层级
 * 2. 检查进程 UID/RUID，排除 root 进程及 root helper（uid=0 / ruid=0）
 * 3. 校验 Info.plist 必须存在且可读（N2: 读不到就拒绝）
 * 4. 白名单校验：bundle id 必须精确等于 com.tencent.xin（N3+N4）
 * 5. 校验 Info.plist 中的 CFBundleExecutable 确实为当前二进制
 * 返回值：
 *   1: 是合法的微信 App 主程序
 *   0: 不是微信 App 主程序（已被拒绝）
 *  -1: 进程不存在或无法获取信息
 */
static int check_user_app_binary(pid_t pid, char *path_out, size_t path_sz, char *app_dir_out, size_t app_dir_sz) {
    if (pid <= 0) return -1;

    char path[PROC_PIDPATHINFO_MAXSIZE];
    memset(path, 0, sizeof(path));
    int ret = proc_pidpath(pid, path, sizeof(path));
    if (ret <= 0) {
        return -1;
    }

    struct proc_bsdinfo bsdinfo;
    memset(&bsdinfo, 0, sizeof(bsdinfo));
    ret = proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bsdinfo, sizeof(bsdinfo));
    if (ret != sizeof(bsdinfo)) {
        return -1;
    }

    /* 拒绝 root 运行的 helper 与系统进程 */
    if (bsdinfo.pbi_uid == 0 || bsdinfo.pbi_ruid == 0) {
        fprintf(stderr, "Error: pid %d is running as root (uid=%u ruid=%u); rejecting root process\n",
                pid, bsdinfo.pbi_uid, bsdinfo.pbi_ruid);
        return 0;
    }

    /* 校验路径前缀：要求可执行文件在 /var/containers/Bundle/Application/ 下 */
    static const char kPrefix1[] = "/private/var/containers/Bundle/Application/";
    static const char kPrefix2[] = "/var/containers/Bundle/Application/";
    size_t prefix_len = 0;
    if (strncmp(path, kPrefix1, sizeof(kPrefix1) - 1) == 0) {
        prefix_len = sizeof(kPrefix1) - 1;
    } else if (strncmp(path, kPrefix2, sizeof(kPrefix2) - 1) == 0) {
        prefix_len = sizeof(kPrefix2) - 1;
    } else {
        fprintf(stderr, "Error: pid %d path '%s' does not start with /var/containers/Bundle/Application/\n", pid, path);
        return 0;
    }

    /* 严格约束 UUID 紧跟 *.app 且下层为主可执行文件，无多余子目录 */
    const char *rel = path + prefix_len;
    const char *s1 = strchr(rel, '/');
    if (!s1 || s1 == rel) {
        fprintf(stderr, "Error: pid %d path '%s' missing UUID component\n", pid, path);
        return 0;
    }
    const char *s2 = strchr(s1 + 1, '/');
    if (!s2) {
        fprintf(stderr, "Error: pid %d path '%s' missing .app component\n", pid, path);
        return 0;
    }
    size_t app_name_len = s2 - (s1 + 1);
    if (app_name_len <= 4 || strncmp(s2 - 4, ".app", 4) != 0) {
        fprintf(stderr, "Error: pid %d path '%s' app component does not end with .app\n", pid, path);
        return 0;
    }

    /* 严禁存在第三个斜杠（防止嵌套子目录或子 App，如 Watch/W.app/W） */
    const char *s3 = strchr(s2 + 1, '/');
    if (s3 != NULL) {
        fprintf(stderr, "Error: pid %d path '%s' contains nested directory beyond .app\n", pid, path);
        return 0;
    }
    const char *exe_name = s2 + 1;
    if (*exe_name == '\0') {
        fprintf(stderr, "Error: pid %d path '%s' has empty executable name\n", pid, path);
        return 0;
    }

    char app_dir[PROC_PIDPATHINFO_MAXSIZE];
    snprintf(app_dir, sizeof(app_dir), "%.*s", (int)(s2 - path), path);

    /* N2 + N3: 读取 Info.plist，读不到直接拒绝；只认 bundle id 精确等于 com.tencent.xin */
    char expected_exe[256] = {0};
    char bundle_id[256] = {0};
    int plist_rc = read_info_plist_meta(app_dir, expected_exe, sizeof(expected_exe),
                                        bundle_id, sizeof(bundle_id));
    if (plist_rc != 0) {
        fprintf(stderr, "Error: pid %d: could not read Info.plist at %s: errno=%d (%s)\n",
                pid, app_dir, errno, strerror(errno));
        return 0;
    }

    if (strcmp(bundle_id, "com.tencent.xin") != 0) {
        fprintf(stderr, "Error: pid %d bundle id '%s' is not allowed (whitelist: com.tencent.xin only)\n",
                pid, bundle_id);
        return 0;
    }

    if (expected_exe[0] == '\0' || strcmp(expected_exe, exe_name) != 0) {
        fprintf(stderr, "Error: pid %d binary '%s' does not match Info.plist CFBundleExecutable '%s'\n",
                pid, exe_name, expected_exe);
        return 0;
    }

    if (path_out && path_sz > 0) {
        snprintf(path_out, path_sz, "%s", path);
    }
    if (app_dir_out && app_dir_sz > 0) {
        snprintf(app_dir_out, app_dir_sz, "%s", app_dir);
    }
    return 1;
}

/* 读取进程物理内存 footprint (MB) */
static int get_phys_footprint_mb(pid_t pid, uint64_t *fp_mb_out) {
    struct rusage_info_v4 ri;
    memset(&ri, 0, sizeof(ri));
    int rc = proc_pid_rusage(pid, RUSAGE_INFO_V4, (rusage_info_t *)&ri);
    if (rc == 0) {
        if (fp_mb_out) {
            *fp_mb_out = ri.ri_phys_footprint / (1024 * 1024);
        }
        return 0;
    }
    return errno != 0 ? errno : -1;
}

/* 从 priority list 中读取进程当前内核 effective priority 与生效 limit (MB) */
static int get_process_priority_entry(pid_t pid, int32_t *prio_out, int32_t *limit_out) {
    int size = memorystatus_control(MEMORYSTATUS_CMD_GET_PRIORITY_LIST, 0, 0, NULL, 0);
    if (size <= 0) {
        return errno != 0 ? errno : -1;
    }
    size += (int)(sizeof(memorystatus_priority_entry_t) * 32);
    memorystatus_priority_entry_t *buf = (memorystatus_priority_entry_t *)malloc(size);
    if (!buf) {
        return ENOMEM;
    }
    int ret = memorystatus_control(MEMORYSTATUS_CMD_GET_PRIORITY_LIST, 0, 0, buf, size);
    if (ret <= 0) {
        int err = errno != 0 ? errno : -1;
        free(buf);
        return err;
    }
    int count = ret / (int)sizeof(memorystatus_priority_entry_t);
    int found = 0;
    for (int i = 0; i < count; i++) {
        if (buf[i].pid == pid) {
            if (prio_out) {
                *prio_out = buf[i].priority;
            }
            if (limit_out) {
                *limit_out = buf[i].limit;
            }
            found = 1;
            break;
        }
    }
    free(buf);
    return found ? 0 : ESRCH;
}

/* 读取内存限额属性 */
static int get_memlimit_props(pid_t pid, memorystatus_memlimit_properties_t *props_out) {
    if (!props_out) return EINVAL;
    memset(props_out, 0, sizeof(*props_out));
    int rc = memorystatus_control(MEMORYSTATUS_CMD_GET_MEMLIMIT_PROPERTIES, pid, 0,
                                  props_out, sizeof(*props_out));
    if (rc != 0) {
        return errno != 0 ? errno : -1;
    }
    return 0;
}

/* 写入内存限额属性 */
static int set_memlimit_props(pid_t pid, const memorystatus_memlimit_properties_t *props) {
    if (!props) return EINVAL;
    int rc = memorystatus_control(MEMORYSTATUS_CMD_SET_MEMLIMIT_PROPERTIES, pid, 0,
                                  (void *)props, sizeof(*props));
    if (rc != 0) {
        return errno != 0 ? errno : -1;
    }
    return 0;
}

/* 向文件追加一行日志 */
static void append_log_line(const char *path, const char *line) {
    if (!path || !line) return;
    FILE *f = fopen(path, "a");
    if (!f) {
        fprintf(stderr, "Warning: failed to open output file %s: errno=%d (%s)\n",
                path, errno, strerror(errno));
        return;
    }
    fchmod(fileno(f), 0644);
    fprintf(f, "%s\n", line);
    fclose(f);
}

/* 参数解析与严格校验工具 */
static int parse_positive_int(const char *str, int *out_val) {
    if (!str || !*str) return -1;
    char *endptr = NULL;
    errno = 0;
    long val = strtol(str, &endptr, 10);
    if (errno != 0 || *endptr != '\0' || val <= 0 || val > INT32_MAX) {
        return -1;
    }
    *out_val = (int)val;
    return 0;
}

static int parse_nonnegative_int(const char *str, int *out_val) {
    if (!str || !*str) return -1;
    char *endptr = NULL;
    errno = 0;
    long val = strtol(str, &endptr, 10);
    if (errno != 0 || *endptr != '\0' || val < 0 || val > INT32_MAX) {
        return -1;
    }
    *out_val = (int)val;
    return 0;
}

/* 打印使用方法 */
static void print_usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s get <pid>\n", prog);
    fprintf(stderr, "  %s set-inactive <pid> <MB>\n", prog);
    fprintf(stderr, "  %s restore <pid>\n", prog);
    fprintf(stderr, "  %s pulse <pid> <seconds>\n", prog);
    fprintf(stderr, "  %s watch <pid> <seconds> <output_file>\n", prog);
    fprintf(stderr, "\nNotes:\n");
    fprintf(stderr, "  - Target process must be WeChat (bundle id 'com.tencent.xin') under /var/containers/Bundle/Application/.\n");
    fprintf(stderr, "  - 'restore <pid>' restores inactive limit to original system value recorded in state file.\n");
    fprintf(stderr, "  - 'prio=NA' in watch indicates kernel priority was not available from priority list (e.g. process not in priority table).\n");
    fprintf(stderr, "  - GONE line in watch and pulse: GONE last_fp=<MB> last_eff_limit=<MB> last_prio=<n|NA> over_limit=<0|1> last_ts=<timestamp>\n");
}

/* N1 向上取整公式，杜绝 IEEE 754 浮点误差 */
static inline uint64_t calc_hard_guard_limit(uint64_t fp_mb) {
    return (fp_mb * 11 + 9) / 10;   /* ceil(fp * 1.1) */
}

static inline uint64_t calc_set_inactive_limit(uint64_t fp_mb) {
    return (fp_mb * 15 + 9) / 10;   /* ceil(fp * 1.5) */
}

static inline uint64_t calc_pulse_limit(uint64_t fp_mb) {
    return (fp_mb * 100 + 84) / 85; /* ceil(fp / 0.85) */
}

/* D1 restore 合理性下限：ceil(fp * 1.2) */
static inline uint64_t calc_restore_floor(uint64_t fp_mb) {
    return (fp_mb * 12 + 9) / 10;   /* ceil(fp * 1.2) */
}

/* 校验 pid 是否为合法用户 App 主程序 */
static int validate_user_app(pid_t pid) {
    char path[PROC_PIDPATHINFO_MAXSIZE];
    int chk = check_user_app_binary(pid, path, sizeof(path), NULL, 0);
    if (chk < 0) {
        fprintf(stderr, "Error: pid %d not found or cannot get path: errno=%d (%s)\n",
                pid, errno, strerror(errno));
        return -1;
    }
    if (chk == 0) {
        return 0;
    }
    return 1;
}

/* ---- 子命令实现 ---- */

/* N8: GET 如实打印：limit <= 0 时打印 active=none、inactive=none，不算错误 */
static int cmd_get(pid_t pid) {
    if (validate_user_app(pid) <= 0) {
        return 1;
    }
    memorystatus_memlimit_properties_t props;
    int err = get_memlimit_props(pid, &props);
    if (err != 0) {
        fprintf(stderr, "Error: memorystatus_control GET failed: errno=%d (%s)\n",
                err, strerror(err));
        return 1;
    }

    char act_str[64], inact_str[64];
    if (props.memlimit_active <= 0) {
        snprintf(act_str, sizeof(act_str), "active=none");
    } else {
        int fatal = (props.memlimit_active_attr & MEMORYSTATUS_MEMLIMIT_ATTR_FATAL) ? 1 : 0;
        snprintf(act_str, sizeof(act_str), "active=%d active_fatal=%d",
                 props.memlimit_active, fatal);
    }
    if (props.memlimit_inactive <= 0) {
        snprintf(inact_str, sizeof(inact_str), "inactive=none");
    } else {
        int fatal = (props.memlimit_inactive_attr & MEMORYSTATUS_MEMLIMIT_ATTR_FATAL) ? 1 : 0;
        snprintf(inact_str, sizeof(inact_str), "inactive=%d inactive_fatal=%d",
                 props.memlimit_inactive, fatal);
    }
    printf("%s %s\n", act_str, inact_str);
    return 0;
}

static int cmd_set_inactive(pid_t pid, int target_mb) {
    if (validate_user_app(pid) <= 0) {
        return 1;
    }

    proc_identity_t ident;
    if (get_proc_identity(pid, &ident) != 0) {
        fprintf(stderr, "Error: cannot get identity for pid %d\n", pid);
        return 1;
    }

    /* N1: 写入的 inactive 值必须 >= ceil(fp * 1.5)，fp 读失败则拒绝 */
    uint64_t fp_mb = 0;
    int err = get_phys_footprint_mb(pid, &fp_mb);
    if (err != 0) {
        fprintf(stderr, "Error: failed to get footprint for pid %d: errno=%d (%s)\n",
                pid, err, strerror(err));
        return 1;
    }
    uint64_t min_limit = calc_set_inactive_limit(fp_mb);
    if ((uint64_t)target_mb < min_limit) {
        fprintf(stderr, "Error: hard guard violation: target inactive %d MB is below ceil(fp %llu MB * 1.5) = %llu MB\n",
                target_mb, (unsigned long long)fp_mb, (unsigned long long)min_limit);
        return 1;
    }

    /* Q1 & N5: 首次写入存入状态文件，同一 pid 再写不覆盖原值 */
    jplimit_saved_state_t state;
    if (get_or_record_initial_state(pid, &ident, &state) != 0) {
        return 1;
    }

    /* 打印设置前的原值 */
    char orig_act_str[64], orig_inact_str[64];
    if (state.orig_active <= 0) {
        snprintf(orig_act_str, sizeof(orig_act_str), "active=none");
    } else {
        int fatal = (state.orig_active_attr & MEMORYSTATUS_MEMLIMIT_ATTR_FATAL) ? 1 : 0;
        snprintf(orig_act_str, sizeof(orig_act_str), "active=%d active_fatal=%d",
                 state.orig_active, fatal);
    }
    if (state.orig_inactive <= 0) {
        snprintf(orig_inact_str, sizeof(orig_inact_str), "inactive=none");
    } else {
        int fatal = (state.orig_inactive_attr & MEMORYSTATUS_MEMLIMIT_ATTR_FATAL) ? 1 : 0;
        snprintf(orig_inact_str, sizeof(orig_inact_str), "inactive=%d inactive_fatal=%d",
                 state.orig_inactive, fatal);
    }
    printf("orig: %s %s\n", orig_act_str, orig_inact_str);

    /* 总原则 1: 工具只改 inactive 一侧。active 的值和 attr 永远原样照抄当前 GET 的结果 */
    memorystatus_memlimit_properties_t cur_props;
    err = get_memlimit_props(pid, &cur_props);
    if (err != 0) {
        fprintf(stderr, "Error: GET current properties failed: errno=%d (%s)\n",
                err, strerror(err));
        return 1;
    }

    memorystatus_memlimit_properties_t target_props = cur_props;
    target_props.memlimit_inactive = (int32_t)target_mb;
    target_props.memlimit_inactive_attr &= ~MEMORYSTATUS_MEMLIMIT_ATTR_FATAL;

    /* SET 前复查进程身份防 PID 复用 */
    proc_identity_t check_ident;
    if (get_proc_identity(pid, &check_ident) != 0 || !compare_proc_identity(&ident, &check_ident)) {
        fprintf(stderr, "Error: PID reuse detected immediately before SET\n");
        return 1;
    }

    err = set_memlimit_props(pid, &target_props);
    if (err != 0) {
        fprintf(stderr, "Error: SET failed: errno=%d (%s)\n",
                err, strerror(err));
        return 1;
    }

    memorystatus_memlimit_properties_t readback;
    memset(&readback, 0, sizeof(readback));
    err = get_memlimit_props(pid, &readback);
    if (err != 0) {
        fprintf(stderr, "Error: verify GET failed: errno=%d (%s)\n",
                err, strerror(err));
        return 1;
    }

    /* M2: 每次回读都打一行原始 attr，十六进制：READBACK active=<MB>/0x<attr> inactive=<MB>/0x<attr> */
    printf("READBACK active=%d/0x%x inactive=%d/0x%x\n",
           readback.memlimit_active, readback.memlimit_active_attr,
           readback.memlimit_inactive, readback.memlimit_inactive_attr);
    fflush(stdout);

    int rb_act_fatal = (readback.memlimit_active_attr & MEMORYSTATUS_MEMLIMIT_ATTR_FATAL) ? 1 : 0;
    int rb_inact_fatal = (readback.memlimit_inactive_attr & MEMORYSTATUS_MEMLIMIT_ATTR_FATAL) ? 1 : 0;
    int exp_act_fatal = (target_props.memlimit_active_attr & MEMORYSTATUS_MEMLIMIT_ATTR_FATAL) ? 1 : 0;

    if (readback.memlimit_active != target_props.memlimit_active ||
        rb_act_fatal != exp_act_fatal ||
        readback.memlimit_inactive != target_mb ||
        rb_inact_fatal != 0) {
        fprintf(stderr, "Error: verify failed: expected inactive=%d inactive_fatal=0, got inactive=%d inactive_fatal=%d\n",
                target_mb, readback.memlimit_inactive, rb_inact_fatal);
        return 1;
    }

    printf("active=%d active_fatal=%d inactive=%d inactive_fatal=%d\n",
           readback.memlimit_active, rb_act_fatal,
           readback.memlimit_inactive, rb_inact_fatal);
    return 0;
}

/* Q2 & M3: 格式化输出 watch 和 pulse 共用的 GONE 行 */
static void emit_gone(const char *now_ts, uint64_t last_fp, int32_t last_eff_limit,
                      int32_t last_prio, int last_prio_valid,
                      const char *last_sample_ts, int has_sample,
                      const char *out_path) {
    int over_limit = 0;
    if (has_sample && last_eff_limit > 0 && last_fp > (uint64_t)last_eff_limit) {
        over_limit = 1;
    }
    char prio_str[16];
    if (has_sample && last_prio_valid) {
        snprintf(prio_str, sizeof(prio_str), "%d", last_prio);
    } else {
        snprintf(prio_str, sizeof(prio_str), "NA");
    }
    const char *ts_to_use = (has_sample && last_sample_ts[0] != '\0') ? last_sample_ts : now_ts;

    char line[256];
    snprintf(line, sizeof(line), "%s GONE last_fp=%llu last_eff_limit=%d last_prio=%s over_limit=%d last_ts=%s",
             now_ts, (unsigned long long)last_fp, last_eff_limit, prio_str, over_limit, ts_to_use);
    if (out_path) {
        append_log_line(out_path, line);
    }
    printf("%s\n", line);
    fflush(stdout);
}

/* R2: 统一 restore 结论标记打印函数（四种标记全部统一输出到 stdout） */
static void restore_verdict(const char *kind, const char *reason) {
    if (strcmp(kind, "RESTORE_REFUSED") == 0 || strcmp(kind, "RESTORE_FAILED") == 0) {
        printf("提示：强制关闭微信即可恢复系统默认值\n");
    }
    if (reason && reason[0]) {
        printf("%s reason=%s\n", kind, reason);
    } else {
        printf("%s\n", kind);
    }
    fflush(stdout);
}

/* D1 & D2 & D3 & R2 & R4 & M2 & 对齐要求：统一 restore 实现函数（cmd_restore 与 pulse 自动 restore 共享调用） */
static int do_restore(pid_t pid, const char *ts_prefix) {
    (void)ts_prefix;
    if (validate_user_app(pid) <= 0) {
        restore_verdict("RESTORE_REFUSED", "validate");
        return 1;
    }

    /* D3: fp 读不到，任何写入都不做，打 RESTORE_REFUSED fp_unreadable，退出码不为 0 */
    uint64_t fp_mb = 0;
    int fp_err = get_phys_footprint_mb(pid, &fp_mb);
    if (fp_err != 0) {
        fprintf(stderr, "Error: failed to read footprint for pid %d (errno=%d)\n", pid, fp_err);
        restore_verdict("RESTORE_REFUSED", "fp_unreadable");
        return 1;
    }

    proc_identity_t ident;
    if (get_proc_identity(pid, &ident) != 0) {
        fprintf(stderr, "Error: cannot get identity for pid %d\n", pid);
        restore_verdict("RESTORE_REFUSED", "ident");
        return 1;
    }

    memorystatus_memlimit_properties_t cur_props;
    int cur_err = get_memlimit_props(pid, &cur_props);
    if (cur_err != 0) {
        fprintf(stderr, "Error: GET current properties failed for pid %d: errno=%d (%s)\n",
                pid, cur_err, strerror(cur_err));
        restore_verdict("RESTORE_REFUSED", "get");
        return 1;
    }

    /* D1: 检查状态文件可信度 */
    jplimit_saved_state_t state;
    const char *mirror_reason = NULL;
    if (load_trusted_state_file(pid, &ident, &state) != 0) {
        mirror_reason = "untrusted";
    } else if (state.orig_inactive <= 0) {
        /* D2: orig <= 0 (none) 时走镜像 active，记 reason=none */
        mirror_reason = "none";
    } else if (state.suspect == 1) {
        /* R4: restore 读到 suspect=1，不用文件里的原值，直接走镜像 active，并打 RESTORE_MIRROR reason=suspect */
        mirror_reason = "suspect";
    } else {
        /* D1 第 3 点: 原值合理性检查
         * 写回的 inactive 值必须 >= ceil(fp * 1.2)，并且 >= 当前 active 值的一半
         */
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
        /* 走「镜像 active」兜底：
         * 镜像之前还要检查 active > ceil(fp * 1.2)，不满足就拒绝，提示强制关闭微信
         */
        uint64_t floor_1_2 = calc_restore_floor(fp_mb);
        if (cur_props.memlimit_active <= 0 ||
            (uint64_t)cur_props.memlimit_active <= floor_1_2) {
            fprintf(stderr, "Error: active limit %d MB is <= ceil(fp %llu MB * 1.2) = %llu MB\n",
                    cur_props.memlimit_active, (unsigned long long)fp_mb, (unsigned long long)floor_1_2);
            restore_verdict("RESTORE_REFUSED", "active_low");
            return 1;
        }
        /* M2: inactive 的值和 attr 都等于当前 GET 到的 active 原始整字段值和 attr */
        target_inactive = cur_props.memlimit_active;
        target_inactive_attr = cur_props.memlimit_active_attr;
    } else {
        /* M2: 状态文件存 attr 的原始整字段，restore 时原样写回，不再自己拼 0 或 FATAL */
        target_inactive = state.orig_inactive;
        target_inactive_attr = state.orig_inactive_attr;
    }

    /* 写入时重试 3 次（共 4 次尝试，每次间隔 100 ms） */
    int success = 0;
    int write_attempted = 0;
    int readback_mismatch = 0;

    for (int attempt = 0; attempt < 4; attempt++) {
        if (attempt > 0) {
            usleep(100 * 1000); /* 100 ms */
        }

        /* 写入前身份校验防复用 */
        proc_identity_t check_ident;
        if (get_proc_identity(pid, &check_ident) != 0 || !compare_proc_identity(&ident, &check_ident)) {
            fprintf(stderr, "Error: PID reuse detected immediately before restore SET\n");
            break;
        }

        /* active 取当前 GET 的值，不取状态文件 */
        memorystatus_memlimit_properties_t cur_active;
        if (get_memlimit_props(pid, &cur_active) != 0) {
            continue;
        }

        memorystatus_memlimit_properties_t target_props;
        target_props.memlimit_active = cur_active.memlimit_active;
        target_props.memlimit_active_attr = cur_active.memlimit_active_attr;
        target_props.memlimit_inactive = target_inactive;
        target_props.memlimit_inactive_attr = target_inactive_attr;

        write_attempted = 1;
        if (set_memlimit_props(pid, &target_props) != 0) {
            continue;
        }

        /* 写完回读 */
        memorystatus_memlimit_properties_t readback;
        memset(&readback, 0, sizeof(readback));
        if (get_memlimit_props(pid, &readback) != 0) {
            readback_mismatch = 1;
            continue;
        }

        /* M2: 每次回读都打一行原始 attr，十六进制：READBACK active=<MB>/0x<attr> inactive=<MB>/0x<attr> */
        printf("READBACK active=%d/0x%x inactive=%d/0x%x\n",
               readback.memlimit_active, readback.memlimit_active_attr,
               readback.memlimit_inactive, readback.memlimit_inactive_attr);
        fflush(stdout);

        /* M2: 回读比对：MB 值必须相等，FATAL 位必须相等；其他位不比 */
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
            restore_verdict("RESTORE_REFUSED", "ident");
            return 1;
        }
        if (readback_mismatch) {
            restore_verdict("RESTORE_FAILED", "readback");
            return 1;
        } else {
            restore_verdict("RESTORE_FAILED", "retry");
            return 1;
        }
    }

    clear_state_file(pid);

    if (mirror_reason != NULL) {
        restore_verdict("RESTORE_MIRROR", mirror_reason);
        return 0;
    }

    restore_verdict("RESTORE_OK", NULL);
    return 0;
}

/* Q1 & N1 & N5 & N6 & N7 & D1 & D2 & D3 & R2: restore <pid>（无数值参数） */
static int cmd_restore(pid_t pid) {
    return do_restore(pid, NULL);
}

static int cmd_pulse(pid_t pid, int duration_sec) {
    if (validate_user_app(pid) <= 0) {
        return 1;
    }
    /* Q2: pulse 窗口上限改成 60 秒（参数上限也改成 60） */
    if (duration_sec <= 0 || duration_sec > 60) {
        fprintf(stderr, "Error: duration_sec must be between 1 and 60 seconds (got %d)\n", duration_sec);
        return 1;
    }

    proc_identity_t ident;
    if (get_proc_identity(pid, &ident) != 0) {
        fprintf(stderr, "Error: cannot get identity for pid %d\n", pid);
        return 1;
    }

    /* 1. 先记录或读取状态文件中的原值 */
    jplimit_saved_state_t state;
    if (get_or_record_initial_state(pid, &ident, &state) != 0) {
        return 1;
    }

    char orig_act_str[64], orig_inact_str[64];
    if (state.orig_active <= 0) {
        snprintf(orig_act_str, sizeof(orig_act_str), "active=none");
    } else {
        int fatal = (state.orig_active_attr & MEMORYSTATUS_MEMLIMIT_ATTR_FATAL) ? 1 : 0;
        snprintf(orig_act_str, sizeof(orig_act_str), "active=%d active_fatal=%d",
                 state.orig_active, fatal);
    }
    if (state.orig_inactive <= 0) {
        snprintf(orig_inact_str, sizeof(orig_inact_str), "inactive=none");
    } else {
        int fatal = (state.orig_inactive_attr & MEMORYSTATUS_MEMLIMIT_ATTR_FATAL) ? 1 : 0;
        snprintf(orig_inact_str, sizeof(orig_inact_str), "inactive=%d inactive_fatal=%d",
                 state.orig_inactive, fatal);
    }
    printf("orig: %s %s\n", orig_act_str, orig_inact_str);

    /* 2. 读 fp，N1: fp 读失败则 pulse 拒绝；L = ceil(fp/0.85)，还要 >= ceil(fp*1.1) */
    uint64_t fp_mb = 0;
    int err = get_phys_footprint_mb(pid, &fp_mb);
    if (err != 0) {
        fprintf(stderr, "Error: failed to read footprint: errno=%d (%s)\n", err, strerror(err));
        return 1;
    }

    uint64_t L = calc_pulse_limit(fp_mb);
    uint64_t hard_guard = calc_hard_guard_limit(fp_mb);
    if (L < hard_guard) {
        L = hard_guard;
    }

    /* 取当前 GET 值，只改 inactive，attr 非致命 */
    memorystatus_memlimit_properties_t cur_props;
    err = get_memlimit_props(pid, &cur_props);
    if (err != 0) {
        fprintf(stderr, "Error: get current props failed: errno=%d (%s)\n", err, strerror(err));
        return 1;
    }

    memorystatus_memlimit_properties_t new_props = cur_props;
    new_props.memlimit_inactive = (int32_t)L;
    new_props.memlimit_inactive_attr &= ~MEMORYSTATUS_MEMLIMIT_ATTR_FATAL;

    proc_identity_t check_ident;
    if (get_proc_identity(pid, &check_ident) != 0 || !compare_proc_identity(&ident, &check_ident)) {
        fprintf(stderr, "Error: PID reuse detected before pulse SET\n");
        return 1;
    }

    err = set_memlimit_props(pid, &new_props);
    if (err != 0) {
        fprintf(stderr, "Error: pulse SET failed: errno=%d (%s)\n", err, strerror(err));
        return 1;
    }

    memorystatus_memlimit_properties_t readback;
    err = get_memlimit_props(pid, &readback);
    if (err != 0) {
        fprintf(stderr, "Error: pulse verify GET failed: errno=%d (%s)\n", err, strerror(err));
        return 1;
    }

    /* M2: 每次回读都打一行原始 attr，十六进制：READBACK active=<MB>/0x<attr> inactive=<MB>/0x<attr> */
    printf("READBACK active=%d/0x%x inactive=%d/0x%x\n",
           readback.memlimit_active, readback.memlimit_active_attr,
           readback.memlimit_inactive, readback.memlimit_inactive_attr);
    fflush(stdout);

    int rb_inactive_fatal = (readback.memlimit_inactive_attr & MEMORYSTATUS_MEMLIMIT_ATTR_FATAL) ? 1 : 0;
    if (readback.memlimit_inactive != (int32_t)L || rb_inactive_fatal != 0) {
        fprintf(stderr, "Error: pulse readback mismatch: expected inactive=%llu inactive_fatal=0, got inactive=%d inactive_fatal=%d\n",
                (unsigned long long)L, readback.memlimit_inactive, rb_inactive_fatal);
        return 1;
    }

    time_t start_time = time(NULL);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&start_time));
    printf("%s PULSE_ARMED pid=%d fp=%llu L=%llu window=%ds\n",
           ts, pid, (unsigned long long)fp_mb, (unsigned long long)L, duration_sec);
    fflush(stdout);

    /* M3: 初始化有效采样供 GONE 行输出 */
    int32_t init_prio = -1;
    int32_t init_eff_limit = -1;
    int init_prio_err = get_process_priority_entry(pid, &init_prio, &init_eff_limit);

    uint64_t last_fp = fp_mb;
    int32_t last_eff_limit = init_eff_limit;
    int32_t last_prio = init_prio;
    int last_prio_valid = (init_prio_err == 0);
    char last_sample_ts[32] = {0};
    snprintf(last_sample_ts, sizeof(last_sample_ts), "%s", ts);
    int has_sample = 1;

    /* 3. 窗口内每 1 秒记一次 fp 与 eff_limit / prio */
    int should_restore = 1;
    for (int sec = 1; sec <= duration_sec; sec++) {
        sleep(1);

        time_t now = time(NULL);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));

        proc_identity_t loop_ident;
        if (get_proc_identity(pid, &loop_ident) != 0) {
            emit_gone(ts, last_fp, last_eff_limit, last_prio, last_prio_valid,
                      last_sample_ts, has_sample, NULL);
            printf("%s PULSE_GONE L=%llu t=%d\n", ts, (unsigned long long)L, sec);
            fflush(stdout);
            should_restore = 0;
            break;
        }
        if (!compare_proc_identity(&ident, &loop_ident)) {
            printf("%s PID_REUSED\n", ts);
            fflush(stdout);
            should_restore = 0;
            break;
        }

        uint64_t cur_fp = 0;
        err = get_phys_footprint_mb(pid, &cur_fp);
        if (err != 0) {
            if (err == ESRCH || kill(pid, 0) == -1) {
                emit_gone(ts, last_fp, last_eff_limit, last_prio, last_prio_valid,
                          last_sample_ts, has_sample, NULL);
                printf("%s PULSE_GONE L=%llu t=%d\n", ts, (unsigned long long)L, sec);
                fflush(stdout);
                should_restore = 0;
                break;
            }
            printf("%s fp_read_err errno=%d\n", ts, err);
            fflush(stdout);
            /* B2: 进程仍在、仅读取失败，必须尝试按原值 restore，不得静默留下探测限额 L */
            should_restore = 1;
            break;
        }

        /* M3: 循环每秒采样时必须同时采 eff_limit 和 prio */
        int32_t prio = -1;
        int32_t eff_limit = -1;
        int prio_err = get_process_priority_entry(pid, &prio, &eff_limit);
        if (prio_err != 0) {
            if (prio_err == ESRCH || kill(pid, 0) == -1) {
                emit_gone(ts, last_fp, last_eff_limit, last_prio, last_prio_valid,
                          last_sample_ts, has_sample, NULL);
                printf("%s PULSE_GONE L=%llu t=%d\n", ts, (unsigned long long)L, sec);
                fflush(stdout);
                should_restore = 0;
                break;
            }
        }

        has_sample = 1;
        last_fp = cur_fp;
        last_eff_limit = eff_limit;
        last_prio = prio;
        last_prio_valid = (prio_err == 0);
        snprintf(last_sample_ts, sizeof(last_sample_ts), "%s", ts);

        printf("%s t=%ds fp=%llu L=%llu (%.1f%%)\n",
               ts, sec, (unsigned long long)cur_fp, (unsigned long long)L,
               (double)cur_fp * 100.0 / (double)L);
        fflush(stdout);

        /* 5. 窗口内一旦 fp >= 0.95*L，马上 restore，记 ABORT near_limit */
        if (cur_fp * 100 >= 95 * L) {
            printf("%s ABORT near_limit fp=%llu limit=%llu\n", ts, (unsigned long long)cur_fp, (unsigned long long)L);
            fflush(stdout);
            should_restore = 1;
            break;
        }

        /* 4. 窗口到时间，或者 fp 降到 0.8*L 以下，就按原值 restore */
        if (cur_fp * 100 < 80 * L) {
            printf("%s TARGET_REACHED fp=%llu < 0.8*L (%llu)\n",
                   ts, (unsigned long long)cur_fp, (unsigned long long)((80 * L) / 100));
            fflush(stdout);
            should_restore = 1;
            break;
        }
    }

    if (!should_restore) {
        return 0;
    }

    /* 6. restore 前再 GET 一次：如果 inactive 已经不是工具写进去的 L，说明被别人改了，记 OVERRIDDEN old=… now=…，不 restore */
    time_t now = time(NULL);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));

    memorystatus_memlimit_properties_t pre_restore;
    err = get_memlimit_props(pid, &pre_restore);
    if (err != 0) {
        fprintf(stderr, "Error: pre-restore GET failed for pid %d: errno=%d (%s)\n",
                pid, err, strerror(err));
        restore_verdict("RESTORE_REFUSED", "get");
        return 1;
    }

    if (pre_restore.memlimit_inactive != (int32_t)L) {
        printf("%s OVERRIDDEN old=%llu now=%d\n", ts, (unsigned long long)L, pre_restore.memlimit_inactive);
        fflush(stdout);
        return 0;
    }

    /* 对齐要求：pulse 自动 restore 走统一 do_restore 实现 */
    return do_restore(pid, ts);
}

static int cmd_watch(pid_t pid, int duration_sec, const char *out_path) {
    if (validate_user_app(pid) <= 0) {
        return 1;
    }

    proc_identity_t watch_ident;
    if (get_proc_identity(pid, &watch_ident) != 0) {
        fprintf(stderr, "Error: cannot get initial identity for pid %d\n", pid);
        return 1;
    }

    time_t start_time = time(NULL);
    int has_prev = 0;
    int prev_active = 0;
    int prev_active_fatal = 0;
    int prev_inactive = 0;
    int prev_inactive_fatal = 0;

    /* Q2: 保存最后一次有效采样数据供 GONE 行输出 */
    uint64_t last_fp = 0;
    int32_t last_eff_limit = -1;
    int32_t last_prio = -1;
    int last_prio_valid = 0;
    char last_sample_ts[32] = {0};
    int has_sample = 0;

    while (1) {
        time_t now = time(NULL);
        if (duration_sec > 0 && (now - start_time) >= duration_sec) {
            break;
        }

        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));

        /* 1. 存活性与 PID 复用检查 */
        proc_identity_t cur_ident;
        int id_err = get_proc_identity(pid, &cur_ident);
        if (id_err != 0) {
            if (errno == EPERM) {
                char line[256];
                snprintf(line, sizeof(line), "%s EPERM (permission denied)", ts);
                append_log_line(out_path, line);
                printf("%s\n", line);
                fflush(stdout);
                return 1;
            }
            emit_gone(ts, last_fp, last_eff_limit, last_prio, last_prio_valid,
                      last_sample_ts, has_sample, out_path);
            return 0;
        }

        if (!compare_proc_identity(&watch_ident, &cur_ident)) {
            char line[PROC_PIDPATHINFO_MAXSIZE * 2 + 128];
            snprintf(line, sizeof(line), "%s PID_REUSED orig_path=%s new_path=%s",
                     ts, watch_ident.path, cur_ident.path);
            append_log_line(out_path, line);
            printf("%s\n", line);
            fflush(stdout);
            return 1;
        }

        /* 2. 物理内存 footprint 读取 */
        uint64_t fp_mb = 0;
        int fp_err = get_phys_footprint_mb(pid, &fp_mb);
        if (fp_err != 0) {
            if (fp_err == ESRCH || kill(pid, 0) == -1) {
                emit_gone(ts, last_fp, last_eff_limit, last_prio, last_prio_valid,
                          last_sample_ts, has_sample, out_path);
                return 0;
            }
            if (fp_err == EPERM) {
                char line[128];
                snprintf(line, sizeof(line), "%s EPERM", ts);
                append_log_line(out_path, line);
                printf("%s\n", line);
                fflush(stdout);
                return 1;
            }
            char line[128];
            snprintf(line, sizeof(line), "%s RUSAGE_FAIL errno=%d", ts, fp_err);
            append_log_line(out_path, line);
            printf("%s\n", line);
            fflush(stdout);
            return 1;
        }

        /* 3. 内核 priority 与 effective limit */
        int32_t prio = -1;
        int32_t eff_limit = -1;
        int prio_err = get_process_priority_entry(pid, &prio, &eff_limit);
        if (prio_err != 0) {
            if (prio_err == ESRCH || kill(pid, 0) == -1) {
                emit_gone(ts, last_fp, last_eff_limit, last_prio, last_prio_valid,
                          last_sample_ts, has_sample, out_path);
                return 0;
            }
        }

        /* 4. 限额属性获取 */
        memorystatus_memlimit_properties_t props;
        int prop_err = get_memlimit_props(pid, &props);
        if (prop_err != 0) {
            if (prop_err == ESRCH || kill(pid, 0) == -1) {
                emit_gone(ts, last_fp, last_eff_limit, last_prio, last_prio_valid,
                          last_sample_ts, has_sample, out_path);
                return 0;
            }
            if (prop_err == EPERM) {
                char line[128];
                snprintf(line, sizeof(line), "%s EPERM", ts);
                append_log_line(out_path, line);
                printf("%s\n", line);
                fflush(stdout);
                return 1;
            }
            char line[128];
            snprintf(line, sizeof(line), "%s GET_FAIL errno=%d", ts, prop_err);
            append_log_line(out_path, line);
            printf("%s\n", line);
            fflush(stdout);
            return 1;
        }

        /* 采样成功，记录用于 GONE 行输出 */
        has_sample = 1;
        last_fp = fp_mb;
        last_eff_limit = eff_limit;
        last_prio = prio;
        last_prio_valid = (prio_err == 0);
        snprintf(last_sample_ts, sizeof(last_sample_ts), "%s", ts);

        int active_fatal = (props.memlimit_active_attr & MEMORYSTATUS_MEMLIMIT_ATTR_FATAL) ? 1 : 0;
        int inactive_fatal = (props.memlimit_inactive_attr & MEMORYSTATUS_MEMLIMIT_ATTR_FATAL) ? 1 : 0;

        /* 字段变动时单独输出 CHG 行 */
        if (has_prev) {
            if (props.memlimit_active != prev_active) {
                char chg[256];
                snprintf(chg, sizeof(chg), "%s CHG active %d->%d", ts, prev_active, props.memlimit_active);
                append_log_line(out_path, chg);
                printf("%s\n", chg);
            }
            if (active_fatal != prev_active_fatal) {
                char chg[256];
                snprintf(chg, sizeof(chg), "%s CHG active_fatal %d->%d", ts, prev_active_fatal, active_fatal);
                append_log_line(out_path, chg);
                printf("%s\n", chg);
            }
            if (props.memlimit_inactive != prev_inactive) {
                char chg[256];
                snprintf(chg, sizeof(chg), "%s CHG inactive %d->%d", ts, prev_inactive, props.memlimit_inactive);
                append_log_line(out_path, chg);
                printf("%s\n", chg);
            }
            if (inactive_fatal != prev_inactive_fatal) {
                char chg[256];
                snprintf(chg, sizeof(chg), "%s CHG inactive_fatal %d->%d", ts, prev_inactive_fatal, inactive_fatal);
                append_log_line(out_path, chg);
                printf("%s\n", chg);
            }
        }
        has_prev = 1;
        prev_active = props.memlimit_active;
        prev_active_fatal = active_fatal;
        prev_inactive = props.memlimit_inactive;
        prev_inactive_fatal = inactive_fatal;

        /* N9: prio 读不到时显示 prio=NA */
        char prio_str[16];
        if (prio_err == 0) {
            snprintf(prio_str, sizeof(prio_str), "%d", prio);
        } else {
            snprintf(prio_str, sizeof(prio_str), "NA");
        }

        char line[512];
        snprintf(line, sizeof(line), "%s fp=%llu prio=%s eff_limit=%d active=%d active_fatal=%d inactive=%d inactive_fatal=%d",
                 ts, (unsigned long long)fp_mb, prio_str, eff_limit,
                 props.memlimit_active, active_fatal,
                 props.memlimit_inactive, inactive_fatal);
        append_log_line(out_path, line);
        printf("%s\n", line);
        fflush(stdout);

        if (duration_sec > 0 && (time(NULL) - start_time) >= duration_sec) {
            break;
        }

        sleep(5);
    }
    return 0;
}

int main(int argc, char *argv[]) {
    /* N7: 启动时顺便清掉 pid 已经不在的旧文件 */
    cleanup_stale_state_files();

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *subcmd = argv[1];

    if (strcmp(subcmd, "get") == 0) {
        if (argc != 3) {
            print_usage(argv[0]);
            return 1;
        }
        int pid = 0;
        if (parse_positive_int(argv[2], &pid) != 0) {
            fprintf(stderr, "Error: invalid pid '%s': must be a positive integer\n", argv[2]);
            return 1;
        }
        return cmd_get((pid_t)pid);
    }

    if (strcmp(subcmd, "set-inactive") == 0) {
        if (argc != 4) {
            print_usage(argv[0]);
            return 1;
        }
        int pid = 0, mb = 0;
        if (parse_positive_int(argv[2], &pid) != 0) {
            fprintf(stderr, "Error: invalid pid '%s': must be a positive integer\n", argv[2]);
            return 1;
        }
        if (parse_positive_int(argv[3], &mb) != 0) {
            fprintf(stderr, "Error: invalid MB '%s': must be a positive integer\n", argv[3]);
            return 1;
        }
        return cmd_set_inactive((pid_t)pid, mb);
    }

    /* Q1: restore <pid>（删掉数值参数形式，多传或少传参一律拒绝） */
    if (strcmp(subcmd, "restore") == 0) {
        if (argc != 3) {
            print_usage(argv[0]);
            return 1;
        }
        int pid = 0;
        if (parse_positive_int(argv[2], &pid) != 0) {
            fprintf(stderr, "Error: invalid pid '%s': must be a positive integer\n", argv[2]);
            return 1;
        }
        return cmd_restore((pid_t)pid);
    }

    if (strcmp(subcmd, "pulse") == 0) {
        if (argc != 4) {
            print_usage(argv[0]);
            return 1;
        }
        int pid = 0, sec = 0;
        if (parse_positive_int(argv[2], &pid) != 0) {
            fprintf(stderr, "Error: invalid pid '%s': must be a positive integer\n", argv[2]);
            return 1;
        }
        if (parse_positive_int(argv[3], &sec) != 0) {
            fprintf(stderr, "Error: invalid seconds '%s': must be a positive integer\n", argv[3]);
            return 1;
        }
        return cmd_pulse((pid_t)pid, sec);
    }

    if (strcmp(subcmd, "watch") == 0) {
        if (argc != 5) {
            print_usage(argv[0]);
            return 1;
        }
        int pid = 0, sec = 0;
        if (parse_positive_int(argv[2], &pid) != 0) {
            fprintf(stderr, "Error: invalid pid '%s': must be a positive integer\n", argv[2]);
            return 1;
        }
        if (parse_nonnegative_int(argv[3], &sec) != 0) {
            fprintf(stderr, "Error: invalid seconds '%s': must be a non-negative integer\n", argv[3]);
            return 1;
        }
        const char *out_path = argv[4];
        if (!out_path || strlen(out_path) == 0) {
            fprintf(stderr, "Error: invalid output file path\n");
            return 1;
        }
        return cmd_watch((pid_t)pid, sec, out_path);
    }

    fprintf(stderr, "Error: unknown subcommand '%s'\n", subcmd);
    print_usage(argv[0]);
    return 1;
}
