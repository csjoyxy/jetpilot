/*
 * JetPilot v0.7.8 — iOS 15+ TrollStore 守护进程管理与规则编辑
 * 单文件 UIKit 纯代码实现，深色模式自适应，Auto Layout 布局。
 *
 * 编译命令：
 *   xcrun -sdk iphoneos clang -arch arm64 -O2 -Wall -fobjc-arc \
 *     -miphoneos-version-min=15.0 -framework UIKit -framework Foundation \
 *     app/main.m -o JetPilot
 *   ldid -Sentitlements-app.plist JetPilot
 */

#import <UIKit/UIKit.h>
#include <spawn.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <dlfcn.h>
#include <sys/wait.h>

#define POSIX_SPAWN_PERSONA_FLAGS_OVERRIDE 1
extern int posix_spawnattr_set_persona_np(const posix_spawnattr_t *, uid_t, uint32_t);
extern int posix_spawnattr_set_persona_uid_np(const posix_spawnattr_t *, uid_t);
extern int posix_spawnattr_set_persona_gid_np(const posix_spawnattr_t *, uid_t);
extern char **environ;

#if __has_include(<libproc.h>)
#include <libproc.h>
#endif

#ifndef PROC_PIDTBSDINFO
#define PROC_PIDTBSDINFO 3
#endif

#ifndef _SYS_PROC_INFO_H
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
    char     pbi_comm[16];
    char     pbi_name[32];
    uint32_t pbi_nfiles;
    uint32_t pbi_pgid;
    uint32_t pbi_pjobc;
    uint32_t e_tdev;
    uint32_t e_tpgid;
    int32_t  pbi_nice;
    uint64_t pbi_start_tvsec;
    uint64_t pbi_start_tvusec;
};
#endif
extern int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);

#ifndef PROC_PIDPATHINFO_MAXSIZE
#define PROC_PIDPATHINFO_MAXSIZE (4 * 1024)
#endif
extern int proc_pidpath(int pid, void *buffer, uint32_t buffersize);

#ifndef JP_COMMIT
#define JP_COMMIT "dev"
#endif

static NSString *const kDir        = @"/var/mobile/Library/JetPilot";
static NSString *const kConfig     = @"/var/mobile/Library/JetPilot/config.txt";
static NSString *const kConfigBackup = @"/var/mobile/Library/JetPilot/config.txt.pre-0.5.6";
static NSString *const kMigrationDeclined = @"/var/mobile/Library/JetPilot/migration.declined";
static NSString *const kFlag       = @"/var/mobile/Library/JetPilot/running.flag";
static NSString *const kStatus     = @"/var/mobile/Library/JetPilot/status.txt";
static NSString *const kPidFile    = @"/var/mobile/Library/JetPilot/daemon.pid";
static NSString *const kLogFile    = @"/var/mobile/Library/JetPilot/daemon.log";
static NSString *const kLogOldFile = @"/var/mobile/Library/JetPilot/daemon.log.1";
static NSString *const kExportDir  = @"/var/mobile/Library/JetPilot/export";

static NSString *const kMemPressureConfig    = @"/var/mobile/Library/JetPilot/mempressure.txt";
static NSString *const kMemPressureConfigOff = @"/var/mobile/Library/JetPilot/mempressure.txt.off";
static NSString *const kMemPressureState     = @"/var/mobile/Library/JetPilot/mempressure_state.txt";
static NSString *const kMemPressureFloor     = @"/var/mobile/Library/JetPilot/mempressure_floor.txt";

static NSString *const kRestartConfig        = @"/var/mobile/Library/JetPilot/restart.txt";
static NSString *const kRestartState         = @"/var/mobile/Library/JetPilot/restart_state.txt";

static NSString *const kPulseConfig          = @"/var/mobile/Library/JetPilot/pulse.txt";
static NSString *const kPulseState           = @"/var/mobile/Library/JetPilot/pulse.state";
static NSString *const kPulseMigrated        = @"/var/mobile/Library/JetPilot/pulse_migrated";

static NSString *const kBandsReqPath    = @"/var/mobile/Library/JetPilot/bands.req";
static NSString *const kBandsReqTmpPath = @"/var/mobile/Library/JetPilot/bands.req.tmp";
static NSString *const kBandsPath       = @"/var/mobile/Library/JetPilot/bands.txt";

@interface JPBandsProcess : NSObject
@property (nonatomic, copy) NSString *bundleId;
@property (nonatomic, copy) NSString *procLabel;
@property (nonatomic, assign) pid_t pid;
@property (nonatomic, assign) NSInteger curBand;
@property (nonatomic, assign) BOOL isOurs;
@property (nonatomic, assign) BOOL hasOrig;
@property (nonatomic, assign) NSInteger origBand;
@property (nonatomic, assign) BOOL hasFp;
@property (nonatomic, assign) NSInteger fpMb;
- (NSString *)bandDescription;
@end

@implementation JPBandsProcess
- (NSString *)bandDescription {
    if (self.isOurs) {
        if (self.hasOrig) {
            return [NSString stringWithFormat:@"原 %ld → 现 %ld", (long)self.origBand, (long)self.curBand];
        } else {
            return [NSString stringWithFormat:@"%ld · 原档位未知", (long)self.curBand];
        }
    } else {
        if (self.curBand == 100) {
            return @"前台 100";
        } else {
            return [NSString stringWithFormat:@"后台 %ld", (long)self.curBand];
        }
    }
}
@end

@interface JPBandsSnapshot : NSObject
@property (nonatomic, copy) NSString *reqToken;
@property (nonatomic, copy) NSString *updatedTime;
@property (nonatomic, copy) NSArray<JPBandsProcess *> *processes;
@property (nonatomic, assign) BOOL timedOut;
+ (void)requestSnapshotWithCompletion:(void (^)(JPBandsSnapshot *snapshot))completion;
- (JPBandsProcess *)bestProcessForBundleId:(NSString *)bundleId;
@end

@implementation JPBandsSnapshot

+ (JPBandsSnapshot *)parseSnapshotContent:(NSString *)content expectedToken:(NSString *)expectedToken {
    if (!content || content.length == 0) return nil;
    NSArray<NSString *> *lines = [content componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]];
    if (lines.count < 2) return nil;
    NSString *line0 = lines[0];
    if (![line0 hasPrefix:@"req="]) return nil;
    NSString *token = [line0 substringFromIndex:4];
    if (expectedToken.length > 0 && ![token isEqualToString:expectedToken]) return nil;

    JPBandsSnapshot *snap = [JPBandsSnapshot new];
    snap.reqToken = token;
    NSString *line1 = lines[1];
    if ([line1 hasPrefix:@"updated="]) {
        snap.updatedTime = [line1 substringFromIndex:8];
    }

    NSMutableArray<JPBandsProcess *> *procs = [NSMutableArray array];
    for (NSUInteger i = 2; i < lines.count; i++) {
        NSString *line = lines[i];
        if (line.length == 0) continue;
        NSArray<NSString *> *parts = [line componentsSeparatedByString:@"|"];
        if (parts.count < 7) continue;

        JPBandsProcess *bp = [JPBandsProcess new];
        bp.bundleId = parts[0];
        bp.procLabel = parts[1];
        bp.pid = (pid_t)[parts[2] intValue];
        bp.curBand = [parts[3] integerValue];
        bp.isOurs = ([parts[4] intValue] == 1);
        if ([parts[5] isEqualToString:@"NA"]) {
            bp.hasOrig = NO;
        } else {
            bp.hasOrig = YES;
            bp.origBand = [parts[5] integerValue];
        }
        if ([parts[6] isEqualToString:@"NA"]) {
            bp.hasFp = NO;
        } else {
            bp.hasFp = YES;
            bp.fpMb = [parts[6] integerValue];
        }
        [procs addObject:bp];
    }
    snap.processes = [procs copy];
    return snap;
}

+ (void)requestSnapshotWithCompletion:(void (^)(JPBandsSnapshot *snapshot))completion {
    NSString *token = [NSString stringWithFormat:@"%lld_%u",
        (long long)([[NSDate date] timeIntervalSince1970] * 1000.0),
        arc4random_uniform(100000)];

    NSError *err = nil;
    [token writeToFile:kBandsReqTmpPath atomically:YES encoding:NSUTF8StringEncoding error:&err];
    if (err == nil) {
        rename([kBandsReqTmpPath UTF8String], [kBandsReqPath UTF8String]);
    }

    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        int attempts = 0;
        JPBandsSnapshot *result = nil;
        while (attempts < 15) {
            sleep(1);
            attempts++;
            NSString *content = [NSString stringWithContentsOfFile:kBandsPath encoding:NSUTF8StringEncoding error:nil];
            if (content.length > 0) {
                JPBandsSnapshot *snap = [self parseSnapshotContent:content expectedToken:token];
                if (snap) {
                    result = snap;
                    break;
                }
            }
        }
        if (!result) {
            unlink([kBandsReqPath UTF8String]);
            unlink([kBandsReqTmpPath UTF8String]);
            result = [JPBandsSnapshot new];
            result.reqToken = token;
            result.timedOut = YES;
            result.processes = @[];
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            if (completion) completion(result);
        });
    });
}

- (JPBandsProcess *)bestProcessForBundleId:(NSString *)bundleId {
    if (!bundleId || bundleId.length == 0 || self.processes.count == 0) return nil;
    JPBandsProcess *firstMatch = nil;
    for (JPBandsProcess *bp in self.processes) {
        if ([bp.bundleId isEqualToString:bundleId]) {
            if ([bp.procLabel isEqualToString:@"main"]) {
                return bp;
            }
            if (!firstMatch) {
                firstMatch = bp;
            }
        }
    }
    return firstMatch;
}

@end

typedef NS_ENUM(NSInteger, JPDaemonState) {
    JPDaemonStateRunning,
    JPDaemonStateStopped,   /* 用户主动停止：running.flag 不在 */
    JPDaemonStateDead,      /* flag 在，但开机后没跑过 / 进程已退出 */
};

/* 本次开机时间（秒）；取不到返回 0 */
static time_t JPBootTime(void) {
    struct timeval bt = {0};
    size_t len = sizeof(bt);
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (sysctl(mib, 2, &bt, &len, NULL, 0) != 0) return 0;
    return bt.tv_sec;
}

static NSString *DefaultConfig(void) {
    return @"# JetPilot 规则（iOS 16 档位：后台 30，前台 100）\n"
           @"# protect <路径片段> <档位，最高 95>\n"
           @"# demote  <路径片段> [档位] （降到指定档位，默认 30）\n"
           @"protect Reynard.app/ 90\n";
}

static NSString *BandDescription(NSInteger band) {
    if (band >= 95) return [NSString stringWithFormat:@"%ld · 最高保护", (long)band];
    if (band >= 90) return [NSString stringWithFormat:@"%ld · 接近前台", (long)band];
    if (band >= 80) return [NSString stringWithFormat:@"%ld · 高优先级后台", (long)band];
    if (band >= 60) return [NSString stringWithFormat:@"%ld · 中优先级后台", (long)band];
    if (band >= 40) return [NSString stringWithFormat:@"%ld · 低优先级后台", (long)band];
    if (band > 30)  return [NSString stringWithFormat:@"%ld · 普通后台", (long)band];
    if (band == 30) return @"30 · 系统默认后台";
    if (band >= 20) return [NSString stringWithFormat:@"%ld · 次级后台", (long)band];
    if (band >= 10) return [NSString stringWithFormat:@"%ld · 极低后台", (long)band];
    if (band > 0)   return [NSString stringWithFormat:@"%ld · 濒危后台", (long)band];
    return @"0 · 最先被杀";
}

static NSString *FormatShortTimestamp(NSString *raw) {
    if (!raw || raw.length == 0) return @"";
    NSString *trimmed = [raw stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (trimmed.length >= 16 && [trimmed characterAtIndex:4] == '-' && [trimmed characterAtIndex:7] == '-') {
        return [trimmed substringWithRange:NSMakeRange(5, 11)];
    }
    return trimmed;
}

static NSString *ErrnoName(int err) {
    switch (err) {
        case 1:  return @"EPERM";
        case 3:  return @"ESRCH";
        case 22: return @"EINVAL";
        default: return nil;
    }
}

static NSString *const kCrashReporterDir = @"/var/mobile/Library/Logs/CrashReporter";

#pragma mark - Models

typedef NS_ENUM(NSInteger, JPRuleType) {
    JPRuleTypeProtect,
    JPRuleTypeDemote
};

@interface JPRule : NSObject
@property (nonatomic, assign) JPRuleType type;
@property (nonatomic, copy) NSString *pathFragment;
@property (nonatomic, assign) NSInteger targetBand;
@property (nonatomic, copy) NSString *displayName;
@property (nonatomic, assign) BOOL isConflict;
@property (nonatomic, copy) NSArray<NSString *> *conflictAppNames;
@property (nonatomic, assign) BOOL isDuplicate;
@property (nonatomic, assign) BOOL isConflictWithDemote;
@property (nonatomic, assign) BOOL hasSameBand;
@end

@implementation JPRule
@end

@interface JPProtectedApp : NSObject
@property (nonatomic, copy) NSString *pathFragment;
@property (nonatomic, assign) pid_t pid;
@property (nonatomic, assign) NSInteger currentBand;
@property (nonatomic, assign) NSInteger targetBand;
@property (nonatomic, copy) NSString *procLabel;   /* nil = 旧守护进程未提供 */
@property (nonatomic, copy) NSString *bundleId;    /* 主 App Bundle ID，nil = 读失败或旧 daemon */
@end

@implementation JPProtectedApp
@end

@interface JPPulseAppState : NSObject
@property (nonatomic, copy) NSString *bid;
@property (nonatomic, assign) NSInteger dayCount;
@property (nonatomic, assign) BOOL trippedToday;
@property (nonatomic, assign) BOOL unresponsive;
@end

@implementation JPPulseAppState
@end

@interface JPStatusInfo : NSObject
@property (nonatomic, copy) NSString *updated;
@property (nonatomic, assign) NSInteger protectRules;
@property (nonatomic, assign) NSInteger demoteRules;
@property (nonatomic, assign) NSInteger protectedPids;
@property (nonatomic, assign) NSInteger demotedPids;
@property (nonatomic, assign) unsigned long reassertsTotal;
@property (nonatomic, copy) NSArray<JPProtectedApp *> *protectedApps;
@property (nonatomic, assign) BOOL hasProtectedAppLines;
@property (nonatomic, copy) NSArray<JPProtectedApp *> *demotedApps;
@property (nonatomic, assign) BOOL hasDemotedAppLines;
@property (nonatomic, assign) BOOL bidSupport;      /* 守护进程是否支持 bid: 规则 */
@property (nonatomic, assign) BOOL restartSupport;  /* 守护进程是否支持超内存重启 (restart.txt) */
@property (nonatomic, assign) BOOL pulseSupport;    /* 守护进程是否支持后台释放 (pulse.txt) */
@property (nonatomic, assign) NSInteger pulseToday; /* 当天全局 pulse 次数 (v0.7.8 C2-7) */
@property (nonatomic, assign) NSInteger pulseCfgBad;/* pulse.txt 无效行数 (v0.7.8 C2-7) */
@property (nonatomic, assign) BOOL pulseCorrupt;    /* pulse 状态文件恢复异常 (v0.7.8 C2-5/Q5) */
@property (nonatomic, copy) NSString *pulseCorruptBid;
@property (nonatomic, copy) NSDictionary<NSString *, JPPulseAppState *> *pulseAppStates;
@property (nonatomic, copy) NSString *version;       /* 守护进程版本号 (v0.7.8 C1-V) */
@property (nonatomic, copy) NSString *commit;        /* 守护进程 commit 哈希 (v0.7.8 C1-V) */
@property (nonatomic, assign) NSInteger cfgBad;      /* restart.txt 无效行数 (v0.7.8 C1-N6) */
@property (nonatomic, assign) unsigned long setFailTotal;
@property (nonatomic, assign) BOOL hasSetFailTotal;
@property (nonatomic, assign) int lastErrorErrno;
@property (nonatomic, copy) NSString *lastErrorText;
@property (nonatomic, assign) pid_t lastErrorPid;
@property (nonatomic, assign) int lastErrorTarget;
@property (nonatomic, copy) NSString *lastErrorTime;
@property (nonatomic, assign) BOOL hasLastError;
+ (JPStatusInfo *)loadCurrentStatus;
+ (JPProtectedApp *)parseProtectedAppLine:(NSString *)line;
+ (JPProtectedApp *)parseProtectedAppLine:(NSString *)line prefix:(NSString *)prefix;
@end

@implementation JPStatusInfo

+ (JPProtectedApp *)parseProtectedAppLine:(NSString *)line prefix:(NSString *)prefix {
    if (!line || !prefix || ![line hasPrefix:prefix]) return nil;
    NSString *raw = [line substringFromIndex:prefix.length];
    NSArray<NSString *> *parts = [raw componentsSeparatedByString:@"|"];
    if (parts.count != 4) return nil;

    NSString *frag = [parts[0] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (frag.length == 0) return nil;

    NSString *pidStr = [parts[1] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (pidStr.length == 0) return nil;
    NSScanner *pidScanner = [NSScanner scannerWithString:pidStr];
    int pidVal = 0;
    if (![pidScanner scanInt:&pidVal] || ![pidScanner isAtEnd] || pidVal <= 0) {
        return nil;
    }

    NSString *curStr = [parts[2] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (curStr.length == 0) return nil;
    NSScanner *curScanner = [NSScanner scannerWithString:curStr];
    int curVal = 0;
    if (![curScanner scanInt:&curVal] || ![curScanner isAtEnd]) {
        return nil;
    }

    NSString *targetStr = [parts[3] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (targetStr.length == 0) return nil;
    NSScanner *targetScanner = [NSScanner scannerWithString:targetStr];
    int targetVal = 0;
    if (![targetScanner scanInt:&targetVal] || ![targetScanner isAtEnd]) {
        return nil;
    }

    JPProtectedApp *app = [JPProtectedApp new];
    app.pathFragment = frag;
    app.pid = (pid_t)pidVal;
    app.currentBand = curVal;
    app.targetBand = targetVal;
    return app;
}

+ (JPProtectedApp *)parseProtectedAppLine:(NSString *)line {
    return [self parseProtectedAppLine:line prefix:@"protected_app="];
}

+ (JPStatusInfo *)loadCurrentStatus {
    JPStatusInfo *st = [JPStatusInfo new];
    st.updated = @"";
    st.protectedApps = @[];
    st.hasProtectedAppLines = NO;
    st.demotedApps = @[];
    st.hasDemotedAppLines = NO;
    st.bidSupport = NO;
    st.restartSupport = NO;
    st.setFailTotal = 0;
    st.hasSetFailTotal = NO;
    st.lastErrorErrno = 0;
    st.lastErrorText = @"";
    st.lastErrorPid = 0;
    st.lastErrorTarget = 0;
    st.lastErrorTime = @"";
    st.hasLastError = NO;
    NSString *raw = [NSString stringWithContentsOfFile:kStatus encoding:NSUTF8StringEncoding error:nil];
    if (!raw || raw.length == 0) return st;

    NSMutableArray<JPProtectedApp *> *apps = [NSMutableArray array];
    NSMutableArray<JPProtectedApp *> *demotedApps = [NSMutableArray array];
    NSMutableDictionary<NSNumber *, NSString *> *labels = [NSMutableDictionary dictionary];
    NSMutableDictionary<NSNumber *, NSString *> *bids = [NSMutableDictionary dictionary];
    NSMutableDictionary<NSString *, JPPulseAppState *> *pulseAppStates = [NSMutableDictionary dictionary];
    NSArray<NSString *> *lines = [raw componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]];
    for (NSString *line in lines) {
        if ([line hasPrefix:@"updated="]) {
            st.updated = [line substringFromIndex:8];
        } else if ([line hasPrefix:@"version="]) {
            st.version = [[line substringFromIndex:8] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        } else if ([line hasPrefix:@"commit="]) {
            st.commit = [[line substringFromIndex:7] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        } else if ([line hasPrefix:@"cfg_bad="]) {
            st.cfgBad = [[line substringFromIndex:8] integerValue];
        } else if ([line hasPrefix:@"bid_support="]) {
            st.bidSupport = ([[line substringFromIndex:12] intValue] == 1);
        } else if ([line hasPrefix:@"restart_support="]) {
            st.restartSupport = ([[line substringFromIndex:16] intValue] == 1);
        } else if ([line hasPrefix:@"pulse_support="]) {
            st.pulseSupport = ([[line substringFromIndex:14] intValue] == 1);
        } else if ([line hasPrefix:@"pulse_today="]) {
            st.pulseToday = [[line substringFromIndex:12] integerValue];
        } else if ([line hasPrefix:@"pulse_cfg_bad="]) {
            st.pulseCfgBad = [[line substringFromIndex:14] integerValue];
        } else if ([line hasPrefix:@"pulse_corrupt="]) {
            st.pulseCorrupt = ([[line substringFromIndex:14] intValue] == 1);
        } else if ([line hasPrefix:@"pulse_corrupt_bid="]) {
            st.pulseCorruptBid = [[line substringFromIndex:18] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        } else if ([line hasPrefix:@"pulse_app="]) {
            NSString *val = [line substringFromIndex:10];
            NSArray<NSString *> *parts = [val componentsSeparatedByString:@"|"];
            if (parts.count >= 4) {
                JPPulseAppState *ast = [JPPulseAppState new];
                ast.bid = parts[0];
                ast.dayCount = [parts[1] integerValue];
                ast.trippedToday = ([parts[2] intValue] == 1);
                ast.unresponsive = ([parts[3] intValue] == 1);
                pulseAppStates[ast.bid] = ast;
            }
        } else if ([line hasPrefix:@"protect_rules="]) {
            st.protectRules = [[line substringFromIndex:14] integerValue];
        } else if ([line hasPrefix:@"demote_rules="]) {
            st.demoteRules = [[line substringFromIndex:13] integerValue];
        } else if ([line hasPrefix:@"protected_pids="]) {
            st.protectedPids = [[line substringFromIndex:15] integerValue];
        } else if ([line hasPrefix:@"demoted_pids="]) {
            st.demotedPids = [[line substringFromIndex:13] integerValue];
        } else if ([line hasPrefix:@"reasserts_total="]) {
            st.reassertsTotal = (unsigned long)[[line substringFromIndex:16] longLongValue];
        } else if ([line hasPrefix:@"protected_app="]) {
            st.hasProtectedAppLines = YES;
            JPProtectedApp *app = [self parseProtectedAppLine:line prefix:@"protected_app="];
            if (app) {
                [apps addObject:app];
            }
        } else if ([line hasPrefix:@"demoted_app="]) {
            st.hasDemotedAppLines = YES;
            JPProtectedApp *app = [self parseProtectedAppLine:line prefix:@"demoted_app="];
            if (app) {
                [demotedApps addObject:app];
            }
        } else if ([line hasPrefix:@"protected_proc="] || [line hasPrefix:@"demoted_proc="]) {
            NSString *prefix = [line hasPrefix:@"protected_proc="] ? @"protected_proc=" : @"demoted_proc=";
            NSString *rest = [line substringFromIndex:prefix.length];
            NSArray<NSString *> *parts = [rest componentsSeparatedByString:@"|"];
            if (parts.count >= 2) {
                int pidVal = [parts[0] intValue];
                NSString *lab = parts[1];
                if (pidVal > 0 && lab.length > 0) labels[@(pidVal)] = lab;
                if (parts.count >= 3) {
                    NSString *bidVal = [parts[2] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
                    if (pidVal > 0 && bidVal.length > 0) bids[@(pidVal)] = bidVal;
                }
            }
        } else if ([line hasPrefix:@"set_fail_total="]) {
            @try {
                st.hasSetFailTotal = YES;
                NSString *valStr = [[line substringFromIndex:15] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
                NSScanner *scanner = [NSScanner scannerWithString:valStr];
                long long val = 0;
                if ([scanner scanLongLong:&val] && [scanner isAtEnd] && val >= 0) {
                    st.setFailTotal = (unsigned long)val;
                }
            } @catch (NSException *ex) {
                // Ignore dirty line, never crash
            }
        } else if ([line hasPrefix:@"last_error="]) {
            @try {
                NSString *rawErr = [line substringFromIndex:11];
                NSArray<NSString *> *parts = [rawErr componentsSeparatedByString:@"|"];
                // 5 字段：errno|errtext|pid|target|time（v0.5.1+ daemon）；4 字段为旧版（无 errtext）
                if (parts.count >= 4) {
                    NSCharacterSet *ws = [NSCharacterSet whitespaceCharacterSet];
                    NSString *eStr = [parts[0] stringByTrimmingCharactersInSet:ws];
                    NSString *errText = @"";
                    NSString *pStr, *tStr, *timeStr;
                    if (parts.count >= 5) {
                        errText = [parts[1] stringByTrimmingCharactersInSet:ws];
                        pStr = [parts[2] stringByTrimmingCharactersInSet:ws];
                        tStr = [parts[3] stringByTrimmingCharactersInSet:ws];
                        timeStr = [parts[4] stringByTrimmingCharactersInSet:ws];
                    } else {
                        pStr = [parts[1] stringByTrimmingCharactersInSet:ws];
                        tStr = [parts[2] stringByTrimmingCharactersInSet:ws];
                        timeStr = [parts[3] stringByTrimmingCharactersInSet:ws];
                    }

                    int eVal = 0, pVal = 0, tVal = 0;
                    NSScanner *s1 = [NSScanner scannerWithString:eStr];
                    NSScanner *s2 = [NSScanner scannerWithString:pStr];
                    NSScanner *s3 = [NSScanner scannerWithString:tStr];

                    if ([s1 scanInt:&eVal] && [s1 isAtEnd] &&
                        [s2 scanInt:&pVal] && [s2 isAtEnd] &&
                        [s3 scanInt:&tVal] && [s3 isAtEnd] &&
                        timeStr.length > 0) {
                        st.hasLastError = YES;
                        st.lastErrorErrno = eVal;
                        st.lastErrorText = errText;
                        st.lastErrorPid = (pid_t)pVal;
                        st.lastErrorTarget = tVal;
                        st.lastErrorTime = timeStr;
                    }
                }
            } @catch (NSException *ex) {
                // Ignore dirty line, never crash
            }
        }
    }
    for (JPProtectedApp *a in apps) {
        a.procLabel = labels[@(a.pid)];
        a.bundleId = bids[@(a.pid)];
    }
    for (JPProtectedApp *a in demotedApps) {
        a.procLabel = labels[@(a.pid)];
        a.bundleId = bids[@(a.pid)];
    }
    st.protectedApps = [apps copy];
    st.demotedApps = [demotedApps copy];
    st.pulseAppStates = [pulseAppStates copy];
    return st;
}

@end

static BOOL JPCheckVersionOrCommitMismatch(JPStatusInfo *st, NSString *appVersion, NSString *appCommit) {
    if (!st || st.version.length == 0) return YES;
    if (![st.version isEqualToString:appVersion]) return YES;

    NSString *tcDaemon = [st.commit stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    NSString *tcApp = [appCommit stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];

    if (tcDaemon.length >= 7 && tcApp.length >= 7 && ![tcDaemon isEqualToString:@"dev"] && ![tcApp isEqualToString:@"dev"]) {
        NSString *subDaemon = [[tcDaemon substringToIndex:7] lowercaseString];
        NSString *subApp = [[tcApp substringToIndex:7] lowercaseString];
        if (![subDaemon isEqualToString:subApp]) {
            return YES;
        }
    }
    return NO;
}

@interface JPInstalledApp : NSObject
@property (nonatomic, copy) NSString *displayName;
@property (nonatomic, copy) NSString *bundleName;
@property (nonatomic, copy) NSString *bundleId;
@property (nonatomic, copy) NSString *pathFragment;
@property (nonatomic, copy) NSString *appDirPath;
- (UIImage *)icon;
+ (NSArray<JPInstalledApp *> *)scanInstalledApps;
@end

@implementation JPInstalledApp

- (UIImage *)icon {
    SEL sel = NSSelectorFromString(@"_applicationIconImageForBundleIdentifier:format:scale:");
    if ([UIImage respondsToSelector:sel]) {
        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Warc-performSelector-leaks"
        NSMethodSignature *sig = [UIImage methodSignatureForSelector:sel];
        if (sig) {
            NSInvocation *inv = [NSInvocation invocationWithMethodSignature:sig];
            [inv setSelector:sel];
            [inv setTarget:[UIImage class]];
            NSString *bid = self.bundleId;
            int format = 0;
            double scale = [UIScreen mainScreen].scale;
            [inv setArgument:&bid atIndex:2];
            [inv setArgument:&format atIndex:3];
            [inv setArgument:&scale atIndex:4];
            [inv invoke];
            CFTypeRef result;
            [inv getReturnValue:&result];
            if (result) {
                UIImage *img = (__bridge UIImage *)result;
                if (img && img.size.width > 0) return img;
            }
        }
        #pragma clang diagnostic pop
    }
    if (self.appDirPath.length > 0) {
        NSString *plistPath = [self.appDirPath stringByAppendingPathComponent:@"Info.plist"];
        NSDictionary *plist = [NSDictionary dictionaryWithContentsOfFile:plistPath];
        NSArray *iconFiles = plist[@"CFBundleIcons"][@"CFBundlePrimaryIcon"][@"CFBundleIconFiles"];
        if (iconFiles.count > 0) {
            for (NSString *fn in iconFiles.reverseObjectEnumerator) {
                NSString *imgPath = [self.appDirPath stringByAppendingPathComponent:fn];
                UIImage *img = [UIImage imageWithContentsOfFile:imgPath];
                if (!img) img = [UIImage imageWithContentsOfFile:[imgPath stringByAppendingString:@"@2x.png"]];
                if (!img) img = [UIImage imageWithContentsOfFile:[imgPath stringByAppendingString:@"@3x.png"]];
                if (!img) img = [UIImage imageWithContentsOfFile:[imgPath stringByAppendingPathExtension:@"png"]];
                if (img) return img;
            }
        }
    }
    return [UIImage systemImageNamed:@"app.fill"];
}

+ (NSArray<JPInstalledApp *> *)scanInstalledApps {
    NSMutableArray<JPInstalledApp *> *list = [NSMutableArray array];
    NSString *base = @"/var/containers/Bundle/Application";
    NSFileManager *fm = NSFileManager.defaultManager;
    NSError *err = nil;
    NSArray<NSString *> *uuids = [fm contentsOfDirectoryAtPath:base error:&err];
    if (!uuids) return list;

    for (NSString *uuid in uuids) {
        NSString *uDir = [base stringByAppendingPathComponent:uuid];
        BOOL isDir = NO;
        if (![fm fileExistsAtPath:uDir isDirectory:&isDir] || !isDir) continue;

        NSArray<NSString *> *subs = [fm contentsOfDirectoryAtPath:uDir error:nil];
        for (NSString *sub in subs) {
            if ([sub.pathExtension isEqualToString:@"app"]) {
                NSString *appDir = [uDir stringByAppendingPathComponent:sub];
                NSString *plistPath = [appDir stringByAppendingPathComponent:@"Info.plist"];
                NSDictionary *plist = [NSDictionary dictionaryWithContentsOfFile:plistPath];
                if (!plist) continue;

                NSString *displayName = plist[@"CFBundleDisplayName"];
                if (!displayName || displayName.length == 0) {
                    displayName = plist[@"CFBundleName"];
                }
                if (!displayName || displayName.length == 0) {
                    NSString *zhPath = [appDir stringByAppendingPathComponent:@"zh-Hans.lproj/InfoPlist.strings"];
                    NSDictionary *zh = [NSDictionary dictionaryWithContentsOfFile:zhPath];
                    displayName = zh[@"CFBundleDisplayName"] ?: zh[@"CFBundleName"];
                }
                if (!displayName || displayName.length == 0) {
                    displayName = [sub stringByDeletingPathExtension];
                }

                NSString *bid = plist[@"CFBundleIdentifier"] ?: @"";
                JPInstalledApp *item = [JPInstalledApp new];
                item.displayName = displayName;
                item.bundleName = sub;
                item.bundleId = bid;
                item.pathFragment = [sub stringByAppendingString:@"/"];
                item.appDirPath = appDir;
                [list addObject:item];
            }
        }
    }

    [list sortUsingComparator:^NSComparisonResult(JPInstalledApp *a, JPInstalledApp *b) {
        return [a.displayName localizedStandardCompare:b.displayName];
    }];
    return list;
}
@end

static UIImage *JPIconForFragment(NSString *frag, NSArray<JPInstalledApp *> *installedApps) {
    if (!frag || frag.length == 0) return [UIImage systemImageNamed:@"app.fill"];
    if (installedApps) {
        for (JPInstalledApp *app in installedApps) {
            if ([frag hasPrefix:@"bid:"]) {
                NSString *bid = [frag substringFromIndex:4];
                if ([app.bundleId isEqualToString:bid]) return [app icon];
            } else {
                if ([app.pathFragment isEqualToString:frag]) return [app icon];
            }
        }
    }
    return [UIImage systemImageNamed:@"app.fill"];
}

static BOOL JPIsAppInstalled(NSString *frag, NSArray<JPInstalledApp *> *installedApps) {
    if (!frag || frag.length == 0 || !installedApps) return NO;
    for (JPInstalledApp *app in installedApps) {
        if ([frag hasPrefix:@"bid:"]) {
            NSString *bid = [frag substringFromIndex:4];
            if ([app.bundleId isEqualToString:bid]) return YES;
        } else {
            if ([app.pathFragment isEqualToString:frag]) return YES;
        }
    }
    return NO;
}

static BOOL JPIsRestartBlockedBundleId(NSString *bid) {
    if (!bid || bid.length == 0) return YES;
    if ([bid caseInsensitiveCompare:@"SpringBoard"] == NSOrderedSame ||
        [bid caseInsensitiveCompare:@"com.apple.springboard"] == NSOrderedSame) return YES;
    if ([bid caseInsensitiveCompare:@"backboardd"] == NSOrderedSame ||
        [bid caseInsensitiveCompare:@"com.apple.backboardd"] == NSOrderedSame) return YES;
    if ([bid caseInsensitiveCompare:@"launchd"] == NSOrderedSame) return YES;
    if ([bid isEqualToString:@"com.lili.jetpilot"]) return YES;
    if ([bid containsString:@"/"]) return YES;
    return NO;
}

static uint64_t JPGetDeviceMemcapMB(void) {
    uint64_t memsize = 0;
    size_t len = sizeof(memsize);
    if (sysctlbyname("hw.memsize", &memsize, &len, NULL, 0) == 0 && memsize > 0) {
        uint64_t cap = (memsize / 2ULL) / (1024ULL * 1024ULL);
        if (cap > 0) return cap;
    }
    return 2048;
}

#pragma mark - MemPressure Models

@interface JPMemPressureFloorEntry : NSObject
@property (nonatomic, copy) NSString *frag;
@property (nonatomic, assign) time_t lastTime;
@property (nonatomic, assign) NSInteger sampleCount;
@property (nonatomic, copy) NSString *medianStr;
@property (nonatomic, assign) double median;
@property (nonatomic, copy) NSString *samplesStr;
+ (NSDictionary<NSString *, JPMemPressureFloorEntry *> *)loadFloorFile;
@end

@implementation JPMemPressureFloorEntry

+ (NSDictionary<NSString *, JPMemPressureFloorEntry *> *)loadFloorFile {
    NSMutableDictionary<NSString *, JPMemPressureFloorEntry *> *dict = [NSMutableDictionary dictionary];
    NSString *content = [NSString stringWithContentsOfFile:kMemPressureFloor encoding:NSUTF8StringEncoding error:nil];
    if (!content || content.length == 0) return dict;

    BOOL hasTrailing = [content hasSuffix:@"\n"];
    NSString *toSplit = hasTrailing ? [content substringToIndex:content.length - 1] : content;
    NSArray<NSString *> *lines = [toSplit componentsSeparatedByString:@"\n"];
    if (lines.count == 0) return dict;

    NSString *firstLine = [lines[0] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (![firstLine isEqualToString:@"# jetpilot floor v1"]) {
        return dict;
    }

    for (NSUInteger lidx = 1; lidx < lines.count; lidx++) {
        NSString *line = lines[lidx];
        NSString *trimmed = [line stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if (trimmed.length == 0 || [trimmed hasPrefix:@"#"]) continue;

        NSArray<NSString *> *tokens = [trimmed componentsSeparatedByCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if (tokens.count == 0) continue;

        JPMemPressureFloorEntry *entry = [JPMemPressureFloorEntry new];
        entry.frag = tokens[0];
        entry.medianStr = @"NA";
        entry.median = 0.0;

        for (NSUInteger i = 1; i < tokens.count; i++) {
            NSString *tok = tokens[i];
            if ([tok hasPrefix:@"t="]) {
                entry.lastTime = (time_t)[[tok substringFromIndex:2] longLongValue];
            } else if ([tok hasPrefix:@"n="]) {
                entry.sampleCount = [[tok substringFromIndex:2] integerValue];
            } else if ([tok hasPrefix:@"med="]) {
                entry.medianStr = [tok substringFromIndex:4];
                if (![entry.medianStr isEqualToString:@"NA"]) {
                    entry.median = [entry.medianStr doubleValue];
                }
            } else if ([tok hasPrefix:@"s="]) {
                entry.samplesStr = [tok substringFromIndex:2];
            }
        }

        dict[entry.frag] = entry;
        if ([entry.frag hasPrefix:@"bid:"]) {
            dict[[entry.frag substringFromIndex:4]] = entry;
        }
    }
    return dict;
}

@end

@interface JPMemPressureRule : NSObject
@property (nonatomic, copy) NSString *frag;
@property (nonatomic, copy) NSString *displayName;
@property (nonatomic, assign) NSInteger thresholdMB;
@property (nonatomic, assign) NSInteger cooldown;
@property (nonatomic, assign) BOOL hasExplicitThreshold;
@property (nonatomic, assign) BOOL isEnabled;
@property (nonatomic, assign) BOOL isDuplicate;
@property (nonatomic, assign) BOOL isDirty;
@property (nonatomic, copy) NSString *extraTokensStr;
@end

@implementation JPMemPressureRule
@end

typedef NS_ENUM(NSInteger, JPMemPressureLineType) {
    JPMemPressureLineTypeComment = 0,
    JPMemPressureLineTypeGlobalCooldown,
    JPMemPressureLineTypeRule,
    JPMemPressureLineTypeDisabledRule,
    JPMemPressureLineTypeUnknown
};

@interface JPMemPressureLine : NSObject
@property (nonatomic, assign) JPMemPressureLineType type;
@property (nonatomic, copy) NSString *rawText;
@property (nonatomic, strong) JPMemPressureRule *rule;
@property (nonatomic, assign) BOOL isDirty;
@end

@implementation JPMemPressureLine
@end

@interface JPMemPressureConfig : NSObject
@property (nonatomic, assign) NSInteger globalCooldown;
@property (nonatomic, assign) NSInteger originalGlobalCooldown;
@property (nonatomic, assign) BOOL hasExplicitGlobalCooldown;
@property (nonatomic, assign) BOOL hasTrailingNewline;
@property (nonatomic, strong) NSMutableArray<JPMemPressureLine *> *lines;
@property (nonatomic, strong) NSMutableArray<JPMemPressureRule *> *rules;
+ (instancetype)loadFromPath:(NSString *)path;
- (BOOL)saveToPath:(NSString *)path error:(NSError **)error;
- (void)updateDuplicateRules;
@end

@implementation JPMemPressureConfig

+ (instancetype)loadFromPath:(NSString *)path {
    if (!path || path.length == 0) return nil;

    NSFileManager *fm = [NSFileManager defaultManager];
    if (![fm fileExistsAtPath:path]) {
        JPMemPressureConfig *cfg = [JPMemPressureConfig new];
        cfg.lines = [NSMutableArray array];
        cfg.rules = [NSMutableArray array];
        cfg.globalCooldown = 600;
        cfg.originalGlobalCooldown = 600;
        cfg.hasExplicitGlobalCooldown = NO;
        cfg.hasTrailingNewline = YES;
        return cfg;
    }

    NSString *content = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    if (!content) return nil;

    JPMemPressureConfig *cfg = [JPMemPressureConfig new];
    cfg.lines = [NSMutableArray array];
    cfg.rules = [NSMutableArray array];
    cfg.globalCooldown = 600;
    cfg.originalGlobalCooldown = 600;
    cfg.hasExplicitGlobalCooldown = NO;
    cfg.hasTrailingNewline = YES;

    if (content.length == 0) return cfg;

    cfg.hasTrailingNewline = [content hasSuffix:@"\n"];
    NSString *toSplit = cfg.hasTrailingNewline ? [content substringToIndex:content.length - 1] : content;
    NSArray<NSString *> *rawLines = [toSplit componentsSeparatedByString:@"\n"];

    uint64_t memcap = JPGetDeviceMemcapMB();
    JPMemPressureLine *lastCooldownLine = nil;

    for (NSString *rawLine in rawLines) {
        if ([rawLine lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 510) {
            JPMemPressureLine *l = [JPMemPressureLine new];
            l.type = JPMemPressureLineTypeUnknown;
            l.rawText = rawLine;
            [cfg.lines addObject:l];
            continue;
        }

        NSString *trimmed = [rawLine stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if ([trimmed hasSuffix:@"\r"]) {
            trimmed = [trimmed substringToIndex:trimmed.length - 1];
            trimmed = [trimmed stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        }

        if (trimmed.length == 0 || ([trimmed hasPrefix:@"#"] && ![trimmed hasPrefix:@"#off "])) {
            JPMemPressureLine *l = [JPMemPressureLine new];
            l.type = JPMemPressureLineTypeComment;
            l.rawText = rawLine;
            [cfg.lines addObject:l];
            continue;
        }

        BOOL isOff = [trimmed hasPrefix:@"#off "];
        NSString *ruleStr = isOff ? [trimmed substringFromIndex:5] : trimmed;
        ruleStr = [ruleStr stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];

        NSArray<NSString *> *rawTokens = [ruleStr componentsSeparatedByCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@" \t\r"]];
        NSMutableArray<NSString *> *tokens = [NSMutableArray array];
        for (NSString *t in rawTokens) {
            if (t.length > 0) [tokens addObject:t];
        }

        if (tokens.count == 0) {
            JPMemPressureLine *l = [JPMemPressureLine new];
            l.type = JPMemPressureLineTypeComment;
            l.rawText = rawLine;
            [cfg.lines addObject:l];
            continue;
        }

        if (tokens.count > 8) {
            JPMemPressureLine *l = [JPMemPressureLine new];
            l.type = JPMemPressureLineTypeUnknown;
            l.rawText = rawLine;
            [cfg.lines addObject:l];
            continue;
        }

        if ([tokens[0] isEqualToString:@"cooldown"]) {
            if (isOff || tokens.count != 2) {
                JPMemPressureLine *l = [JPMemPressureLine new];
                l.type = JPMemPressureLineTypeUnknown;
                l.rawText = rawLine;
                [cfg.lines addObject:l];
                continue;
            }
            NSScanner *scanner = [NSScanner scannerWithString:tokens[1]];
            int cdVal = 0;
            if (![scanner scanInt:&cdVal] || ![scanner isAtEnd] || cdVal < 60 || cdVal > 86400) {
                JPMemPressureLine *l = [JPMemPressureLine new];
                l.type = JPMemPressureLineTypeUnknown;
                l.rawText = rawLine;
                [cfg.lines addObject:l];
                continue;
            }

            cfg.globalCooldown = cdVal;
            cfg.hasExplicitGlobalCooldown = YES;

            if (lastCooldownLine) {
                [cfg.lines removeObject:lastCooldownLine];
            }

            JPMemPressureLine *l = [JPMemPressureLine new];
            l.type = JPMemPressureLineTypeGlobalCooldown;
            l.rawText = rawLine;
            [cfg.lines addObject:l];
            lastCooldownLine = l;
            continue;
        }

        NSString *frag = tokens[0];
        if (frag.length == 0 || [frag containsString:@"="]) {
            JPMemPressureLine *l = [JPMemPressureLine new];
            l.type = JPMemPressureLineTypeUnknown;
            l.rawText = rawLine;
            [cfg.lines addObject:l];
            continue;
        }

        NSInteger thr = 600;
        BOOL hasExplicitThr = NO;
        NSUInteger idx = 1;
        BOOL isBad = NO;

        if (idx < tokens.count && ![tokens[idx] containsString:@"="]) {
            NSScanner *scanner = [NSScanner scannerWithString:tokens[idx]];
            int thrVal = 0;
            if (![scanner scanInt:&thrVal] || ![scanner isAtEnd] || thrVal < 100 || (uint64_t)thrVal > memcap) {
                isBad = YES;
            } else {
                thr = thrVal;
                hasExplicitThr = YES;
            }
            idx++;
        }

        if (isBad) {
            JPMemPressureLine *l = [JPMemPressureLine new];
            l.type = JPMemPressureLineTypeUnknown;
            l.rawText = rawLine;
            [cfg.lines addObject:l];
            continue;
        }

        NSInteger ruleCd = 0;
        BOOL hasRuleCd = NO;

        for (; idx < tokens.count; idx++) {
            NSString *tok = tokens[idx];
            NSRange eqRange = [tok rangeOfString:@"="];
            if (eqRange.location == NSNotFound || eqRange.location == 0) {
                isBad = YES;
                break;
            }
            NSString *key = [tok substringToIndex:eqRange.location];
            NSString *valStr = [tok substringFromIndex:eqRange.location + 1];
            if (![key isEqualToString:@"cooldown"]) {
                isBad = YES;
                break;
            }
            if (hasRuleCd) {
                isBad = YES;
                break;
            }
            NSScanner *scanner = [NSScanner scannerWithString:valStr];
            int cdVal = 0;
            if (![scanner scanInt:&cdVal] || ![scanner isAtEnd] || cdVal < 60 || cdVal > 86400) {
                isBad = YES;
                break;
            }
            ruleCd = cdVal;
            hasRuleCd = YES;
        }

        if (isBad) {
            JPMemPressureLine *l = [JPMemPressureLine new];
            l.type = JPMemPressureLineTypeUnknown;
            l.rawText = rawLine;
            [cfg.lines addObject:l];
            continue;
        }

        JPMemPressureRule *r = [JPMemPressureRule new];
        r.frag = frag;
        r.thresholdMB = thr;
        r.hasExplicitThreshold = hasExplicitThr;
        r.cooldown = ruleCd;
        r.isEnabled = !isOff;
        r.isDirty = NO;

        JPMemPressureLine *l = [JPMemPressureLine new];
        l.type = isOff ? JPMemPressureLineTypeDisabledRule : JPMemPressureLineTypeRule;
        l.rawText = rawLine;
        l.rule = r;
        l.isDirty = NO;

        [cfg.lines addObject:l];
        [cfg.rules addObject:r];
    }

    cfg.originalGlobalCooldown = cfg.globalCooldown;

    [cfg updateDuplicateRules];

    return cfg;
}

- (void)updateDuplicateRules {
    NSMutableSet<NSString *> *seenFrags = [NSMutableSet set];
    for (JPMemPressureRule *r in self.rules) {
        if (!r.isEnabled) {
            r.isDuplicate = NO;
            continue;
        }
        if ([seenFrags containsObject:r.frag]) {
            r.isDuplicate = YES;
        } else {
            r.isDuplicate = NO;
            [seenFrags addObject:r.frag];
        }
    }
}

- (BOOL)saveToPath:(NSString *)path error:(NSError **)error {
    if (!self.hasExplicitGlobalCooldown) {
        JPMemPressureLine *cdLine = [JPMemPressureLine new];
        cdLine.type = JPMemPressureLineTypeGlobalCooldown;
        cdLine.rawText = [NSString stringWithFormat:@"cooldown %ld", (long)self.globalCooldown];
        cdLine.isDirty = YES;

        NSUInteger insertIdx = 0;
        for (NSUInteger i = 0; i < self.lines.count; i++) {
            if (self.lines[i].type == JPMemPressureLineTypeRule || self.lines[i].type == JPMemPressureLineTypeDisabledRule) {
                insertIdx = i;
                break;
            }
        }
        [self.lines insertObject:cdLine atIndex:insertIdx];
        self.hasExplicitGlobalCooldown = YES;
        self.originalGlobalCooldown = self.globalCooldown;
    }

    NSMutableArray<NSString *> *outputLines = [NSMutableArray array];
    NSMutableArray<JPMemPressureLine *> *outputOwners = [NSMutableArray array];
    for (JPMemPressureLine *l in self.lines) {
        NSString *out = nil;
        if (l.type == JPMemPressureLineTypeGlobalCooldown) {
            if (self.globalCooldown != self.originalGlobalCooldown || l.isDirty || l.rawText == nil) {
                out = [NSString stringWithFormat:@"cooldown %ld", (long)self.globalCooldown];
            } else {
                out = l.rawText;
            }
        } else if (l.type == JPMemPressureLineTypeComment || l.type == JPMemPressureLineTypeUnknown) {
            out = l.rawText ?: @"";
        } else if (l.rule) {
            JPMemPressureRule *r = l.rule;
            if (!r.isDirty && !l.isDirty && l.rawText != nil) {
                out = l.rawText;
            } else {
                NSMutableString *rLine = [NSMutableString string];
                if (!r.isEnabled) [rLine appendString:@"#off "];
                [rLine appendString:r.frag];
                [rLine appendFormat:@" %ld", (long)r.thresholdMB];
                if (r.cooldown > 0) {
                    [rLine appendFormat:@" cooldown=%ld", (long)r.cooldown];
                }
                if (r.extraTokensStr.length > 0) {
                    [rLine appendFormat:@" %@", r.extraTokensStr];
                }
                out = rLine;
            }
        }
        if (out) {
            [outputLines addObject:out];
            [outputOwners addObject:l];
        }
    }

    NSMutableString *outStr = [NSMutableString string];
    [outStr appendString:[outputLines componentsJoinedByString:@"\n"]];
    if (self.hasTrailingNewline) {
        [outStr appendString:@"\n"];
    }

    BOOL ok = [outStr writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:error];
    if (ok) {
        self.originalGlobalCooldown = self.globalCooldown;
        for (NSUInteger i = 0; i < outputOwners.count; i++) {
            JPMemPressureLine *l = outputOwners[i];
            l.rawText = outputLines[i];
            l.isDirty = NO;
            if (l.rule) l.rule.isDirty = NO;
        }
    }
    return ok;
}

@end

#pragma mark - Pulse Models (v0.7.8 C2-7)

@interface JPPulseRule : NSObject
@property (nonatomic, copy) NSString *bid;
@property (nonatomic, assign) BOOL enabled;      // YES: on, NO: off
@property (nonatomic, assign) NSInteger target;  // MB: 300 ~ 1500
@property (nonatomic, assign) NSInteger n;       // min: 5 ~ 60
@property (nonatomic, assign) NSInteger daymax;  // times: 1 ~ 100
@property (nonatomic, assign) NSInteger delay;   // 秒: 3 ~ 20 (默认 10)
@property (nonatomic, copy) NSString *displayName;
@property (nonatomic, assign) NSInteger lineIndex;
@property (nonatomic, assign) BOOL isDuplicate;     // 重复规则标记
@end

@implementation JPPulseRule
- (instancetype)init {
    self = [super init];
    if (self) {
        _lineIndex = -1;
        _enabled = NO; // 手动新加规则默认关闭 (Q6 / C2-7)
        _target = 550;
        _n = 10;
        _daymax = 30;
        _delay = 10;
    }
    return self;
}
@end

@interface JPPulseLine : NSObject
@property (nonatomic, copy) NSString *rawText;
@property (nonatomic, strong) JPPulseRule *rule;
@property (nonatomic, assign) BOOL isUnknownOrComment;
- (BOOL)isCommentOrEmpty;
@end

@implementation JPPulseLine
- (BOOL)isCommentOrEmpty {
    if (!self.isUnknownOrComment) return NO;
    NSString *trimmed = [self.rawText stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    return (trimmed.length == 0 || [trimmed hasPrefix:@"#"]);
}
@end

@interface JPPulseConfig : NSObject
@property (nonatomic, assign) BOOL hasTrailingNewline;
@property (nonatomic, strong) NSMutableArray<JPPulseLine *> *lines;
@property (nonatomic, strong) NSMutableArray<JPPulseRule *> *rules;
+ (instancetype)loadFromPath:(NSString *)path;
- (BOOL)saveToPath:(NSString *)path error:(NSError **)error;
- (JPPulseRule *)ruleForBundleId:(NSString *)bid;
- (void)setRule:(JPPulseRule *)rule forBundleId:(NSString *)bid;
- (JPPulseLine *)lineForRule:(JPPulseRule *)rule;
- (void)removeLine:(JPPulseLine *)line;
- (void)updateDuplicateRules;
@end

@implementation JPPulseConfig

- (instancetype)init {
    self = [super init];
    if (self) {
        _lines = [NSMutableArray array];
        _rules = [NSMutableArray array];
        _hasTrailingNewline = YES;
    }
    return self;
}

+ (instancetype)loadFromPath:(NSString *)path {
    if (!path || path.length == 0) return nil;

    NSFileManager *fm = [NSFileManager defaultManager];
    if (![fm fileExistsAtPath:path]) {
        JPPulseConfig *cfg = [JPPulseConfig new];
        cfg.hasTrailingNewline = YES;
        return cfg;
    }

    NSString *raw = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    if (!raw) return nil;

    JPPulseConfig *cfg = [JPPulseConfig new];
    cfg.hasTrailingNewline = YES;
    if (raw.length == 0) return cfg;

    BOOL hasTrailingNewline = [raw hasSuffix:@"\n"] || [raw hasSuffix:@"\r"];
    NSString *normalized = [raw stringByReplacingOccurrencesOfString:@"\r\n" withString:@"\n"];
    normalized = [normalized stringByReplacingOccurrencesOfString:@"\r" withString:@"\n"];
    cfg.hasTrailingNewline = hasTrailingNewline;

    NSString *toSplit = hasTrailingNewline ? [normalized substringToIndex:normalized.length - 1] : normalized;
    NSArray<NSString *> *rawLines = [toSplit componentsSeparatedByString:@"\n"];

    for (NSString *rawLine in rawLines) {
        NSString *trimmed = [rawLine stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if ([trimmed hasSuffix:@"\r"]) {
            trimmed = [trimmed substringToIndex:trimmed.length - 1];
            trimmed = [trimmed stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        }

        if (trimmed.length == 0 || [trimmed hasPrefix:@"#"]) {
            JPPulseLine *l = [JPPulseLine new];
            l.isUnknownOrComment = YES;
            l.rawText = rawLine;
            [cfg.lines addObject:l];
            continue;
        }

        NSArray<NSString *> *rawTokens = [trimmed componentsSeparatedByCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@" \t\r"]];
        NSMutableArray<NSString *> *tokens = [NSMutableArray array];
        for (NSString *t in rawTokens) {
            if (t.length > 0) [tokens addObject:t];
        }

        // pulse.txt 格式: bid:<bid> on|off target=<MB> n=<分钟> daymax=<次> delay_s=<秒> (恰好 6 个 token)
        if (tokens.count != 6 || ![tokens[0] hasPrefix:@"bid:"]) {
            JPPulseLine *l = [JPPulseLine new];
            l.isUnknownOrComment = YES;
            l.rawText = rawLine;
            [cfg.lines addObject:l];
            continue;
        }

        NSString *bid = [tokens[0] substringFromIndex:4];
        if (bid.length == 0 || JPIsRestartBlockedBundleId(bid)) {
            JPPulseLine *l = [JPPulseLine new];
            l.isUnknownOrComment = YES;
            l.rawText = rawLine;
            [cfg.lines addObject:l];
            continue;
        }

        NSString *stateTok = tokens[1];
        BOOL isEnabled = NO;
        if ([stateTok isEqualToString:@"on"]) {
            isEnabled = YES;
        } else if ([stateTok isEqualToString:@"off"]) {
            isEnabled = NO;
        } else {
            JPPulseLine *l = [JPPulseLine new];
            l.isUnknownOrComment = YES;
            l.rawText = rawLine;
            [cfg.lines addObject:l];
            continue;
        }

        BOOL hasTarget = NO, hasN = NO, hasDaymax = NO, hasDelay = NO;
        NSInteger valTarget = 0, valN = 0, valDaymax = 0, valDelay = 0;
        BOOL bad = NO;

        for (int k = 2; k < 6; k++) {
            NSString *tok = tokens[k];
            NSRange eq = [tok rangeOfString:@"="];
            if (eq.location == NSNotFound || eq.location == 0 || eq.location == tok.length - 1) {
                bad = YES;
                break;
            }
            NSString *key = [tok substringToIndex:eq.location];
            NSString *valStr = [tok substringFromIndex:eq.location + 1];

            NSScanner *scanner = [NSScanner scannerWithString:valStr];
            long long v = 0;
            if (![scanner scanLongLong:&v] || ![scanner isAtEnd]) {
                bad = YES;
                break;
            }

            if ([key isEqualToString:@"target"]) {
                if (hasTarget || v < 300 || v > 1500) { bad = YES; break; }
                valTarget = (NSInteger)v;
                hasTarget = YES;
            } else if ([key isEqualToString:@"n"]) {
                if (hasN || v < 5 || v > 60) { bad = YES; break; }
                valN = (NSInteger)v;
                hasN = YES;
            } else if ([key isEqualToString:@"daymax"]) {
                if (hasDaymax || v < 1 || v > 100) { bad = YES; break; }
                valDaymax = (NSInteger)v;
                hasDaymax = YES;
            } else if ([key isEqualToString:@"delay_s"]) {
                if (hasDelay || v < 3 || v > 20) { bad = YES; break; }
                valDelay = (NSInteger)v;
                hasDelay = YES;
            } else if ([key isEqualToString:@"delay"]) {
                if (hasDelay) { bad = YES; break; }
                // 旧的 delay=<分钟> 照样能解析，但数值忽略，按 10 秒执行 (C2-T)
                valDelay = 10;
                hasDelay = YES;
            } else {
                bad = YES;
                break;
            }
        }

        if (bad || !hasTarget || !hasN || !hasDaymax || !hasDelay) {
            JPPulseLine *l = [JPPulseLine new];
            l.isUnknownOrComment = YES;
            l.rawText = rawLine;
            [cfg.lines addObject:l];
            continue;
        }

        JPPulseRule *r = [JPPulseRule new];
        r.bid = bid;
        r.enabled = isEnabled;
        r.target = valTarget;
        r.n = valN;
        r.daymax = valDaymax;
        r.delay = valDelay;
        r.lineIndex = (NSInteger)cfg.lines.count;

        JPPulseLine *l = [JPPulseLine new];
        l.rule = r;
        l.rawText = rawLine;
        l.isUnknownOrComment = NO;
        [cfg.lines addObject:l];
        [cfg.rules addObject:r];
    }

    [cfg updateDuplicateRules];
    return cfg;
}

- (void)updateDuplicateRules {
    NSMutableSet<NSString *> *seenBids = [NSMutableSet set];
    for (JPPulseLine *line in self.lines) {
        if (!line.isUnknownOrComment && line.rule) {
            NSString *b = line.rule.bid;
            if ([seenBids containsObject:b]) {
                line.rule.isDuplicate = YES;
            } else {
                line.rule.isDuplicate = NO;
                [seenBids addObject:b];
            }
        }
    }
}

- (JPPulseRule *)ruleForBundleId:(NSString *)bid {
    if (bid.length == 0) return nil;
    for (JPPulseLine *l in self.lines) {
        if (!l.isUnknownOrComment && l.rule && [l.rule.bid isEqualToString:bid]) {
            return l.rule;
        }
    }
    return nil;
}

- (void)setRule:(JPPulseRule *)rule forBundleId:(NSString *)bid {
    if (bid.length == 0 || !rule) return;
    rule.bid = bid;

    for (NSUInteger i = 0; i < self.lines.count; i++) {
        JPPulseLine *l = self.lines[i];
        if (!l.isUnknownOrComment && l.rule == rule) {
            rule.lineIndex = (NSInteger)i;
            l.rawText = nil;
            [self updateDuplicateRules];
            return;
        }
    }

    if (rule.lineIndex >= 0 && (NSUInteger)rule.lineIndex < self.lines.count) {
        JPPulseLine *l = self.lines[(NSUInteger)rule.lineIndex];
        if (!l.isUnknownOrComment && l.rule && [l.rule.bid isEqualToString:bid]) {
            NSUInteger idx = [self.rules indexOfObject:l.rule];
            if (idx != NSNotFound) {
                self.rules[idx] = rule;
            }
            l.rule = rule;
            l.rawText = nil;
            [self updateDuplicateRules];
            return;
        }
    }

    JPPulseLine *singleMatch = nil;
    int matchCount = 0;
    for (JPPulseLine *l in self.lines) {
        if (!l.isUnknownOrComment && l.rule && [l.rule.bid isEqualToString:bid]) {
            singleMatch = l;
            matchCount++;
        }
    }
    if (matchCount == 1 && singleMatch) {
        NSUInteger idx = [self.rules indexOfObject:singleMatch.rule];
        if (idx != NSNotFound) {
            self.rules[idx] = rule;
        }
        singleMatch.rule = rule;
        singleMatch.rawText = nil;
        rule.lineIndex = (NSInteger)[self.lines indexOfObject:singleMatch];
        [self updateDuplicateRules];
        return;
    }

    // 新建规则行
    JPPulseLine *newLine = [JPPulseLine new];
    newLine.rule = rule;
    newLine.rawText = nil;
    newLine.isUnknownOrComment = NO;
    rule.lineIndex = (NSInteger)self.lines.count;
    [self.lines addObject:newLine];
    [self.rules addObject:rule];
    [self updateDuplicateRules];
}

- (JPPulseLine *)lineForRule:(JPPulseRule *)rule {
    if (!rule) return nil;
    for (JPPulseLine *l in self.lines) {
        if (!l.isUnknownOrComment && l.rule == rule) return l;
    }
    if (rule.lineIndex >= 0 && (NSUInteger)rule.lineIndex < self.lines.count) {
        JPPulseLine *l = self.lines[(NSUInteger)rule.lineIndex];
        if (!l.isUnknownOrComment && l.rule && [l.rule.bid isEqualToString:rule.bid]) {
            return l;
        }
    }
    return nil;
}

- (void)removeLine:(JPPulseLine *)line {
    if (!line) return;
    if (line.rule) {
        [self.rules removeObject:line.rule];
    }
    [self.lines removeObject:line];
    for (NSUInteger i = 0; i < self.lines.count; i++) {
        JPPulseLine *l = self.lines[i];
        if (!l.isUnknownOrComment && l.rule) {
            l.rule.lineIndex = (NSInteger)i;
        }
    }
    [self updateDuplicateRules];
}

- (BOOL)saveToPath:(NSString *)path error:(NSError **)error {
    if (!path || path.length == 0) return NO;

    NSMutableArray<NSString *> *outputLines = [NSMutableArray array];
    for (JPPulseLine *l in self.lines) {
        if (l.isUnknownOrComment || !l.rule) {
            [outputLines addObject:l.rawText ?: @""];
        } else {
            JPPulseRule *r = l.rule;
            NSString *rLine = [NSString stringWithFormat:@"bid:%@ %@ target=%ld n=%ld daymax=%ld delay_s=%ld",
                               r.bid, r.enabled ? @"on" : @"off",
                               (long)r.target, (long)r.n, (long)r.daymax, (long)r.delay];
            [outputLines addObject:rLine];
            l.rawText = rLine;
        }
    }

    NSMutableString *outStr = [NSMutableString string];
    if (outputLines.count > 0) {
        [outStr appendString:[outputLines componentsJoinedByString:@"\n"]];
        if (self.hasTrailingNewline) {
            [outStr appendString:@"\n"];
        }
    }

    return [outStr writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:error];
}

@end


#pragma mark - Restart Models

@interface JPRestartRule : NSObject
@property (nonatomic, copy) NSString *bid;
@property (nonatomic, assign) NSInteger thr;        // MB, 200 ~ memcap
@property (nonatomic, assign) NSInteger bg;         // 秒, 60 ~ 7200
@property (nonatomic, assign) NSInteger max;        // 每天最多几次, 1 ~ 10
@property (nonatomic, assign) NSInteger relaunch;   // 0 或 1
@property (nonatomic, assign) BOOL isEnabled;       // YES=生效, NO=#off
@property (nonatomic, assign) BOOL isDuplicate;     // 重复规则标记
@property (nonatomic, copy) NSString *displayName;  // App 显示名称
@property (nonatomic, assign) NSInteger lineIndex;  // 行定位 (M1')
@end

@implementation JPRestartRule
- (instancetype)init {
    self = [super init];
    if (self) {
        _lineIndex = -1;
    }
    return self;
}
@end

@interface JPRestartLine : NSObject
@property (nonatomic, copy) NSString *rawText;
@property (nonatomic, strong) JPRestartRule *rule;
@property (nonatomic, assign) BOOL isUnknownOrComment;
- (BOOL)isCommentOrEmpty;
@end

@implementation JPRestartLine
- (BOOL)isCommentOrEmpty {
    if (!self.isUnknownOrComment) return NO;
    NSString *trimmed = [self.rawText stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    return (trimmed.length == 0 || ([trimmed hasPrefix:@"#"] && ![trimmed hasPrefix:@"#off "]));
}
@end

@interface JPRestartConfig : NSObject
@property (nonatomic, assign) BOOL hasTrailingNewline;
@property (nonatomic, strong) NSMutableArray<JPRestartLine *> *lines;
@property (nonatomic, strong) NSMutableArray<JPRestartRule *> *rules;
+ (instancetype)loadFromPath:(NSString *)path;
- (BOOL)saveToPath:(NSString *)path error:(NSError **)error;
- (JPRestartRule *)ruleForBundleId:(NSString *)bid;
- (void)setRule:(JPRestartRule *)rule forBundleId:(NSString *)bid;
- (JPRestartLine *)lineForRule:(JPRestartRule *)rule;
- (void)removeLine:(JPRestartLine *)line;
- (void)updateDuplicateRules;
+ (NSDictionary<NSString *, NSNumber *> *)loadAll7DayRestartCounts;
+ (NSInteger)load7DayRestartCountForBundleId:(NSString *)bid;
+ (NSInteger)loadWarnThresholdForBundleId:(NSString *)bid pathFragment:(NSString *)frag;
@end

@implementation JPRestartConfig

- (instancetype)init {
    self = [super init];
    if (self) {
        _lines = [NSMutableArray array];
        _rules = [NSMutableArray array];
        _hasTrailingNewline = YES;
    }
    return self;
}

+ (instancetype)loadFromPath:(NSString *)path {
    JPRestartConfig *cfg = [JPRestartConfig new];
    cfg.hasTrailingNewline = YES;
    if (!path || path.length == 0) return cfg;

    NSString *raw = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    if (!raw || raw.length == 0) return cfg;

    BOOL hasTrailingNewline = [raw hasSuffix:@"\n"] || [raw hasSuffix:@"\r"];
    NSString *normalized = [raw stringByReplacingOccurrencesOfString:@"\r\n" withString:@"\n"];
    normalized = [normalized stringByReplacingOccurrencesOfString:@"\r" withString:@"\n"];
    cfg.hasTrailingNewline = hasTrailingNewline;

    NSString *toSplit = hasTrailingNewline ? [normalized substringToIndex:normalized.length - 1] : normalized;
    NSArray<NSString *> *rawLines = [toSplit componentsSeparatedByString:@"\n"];
    uint64_t memcap = JPGetDeviceMemcapMB();

    for (NSString *rawLine in rawLines) {
        NSString *trimmed = [rawLine stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if ([trimmed hasSuffix:@"\r"]) {
            trimmed = [trimmed substringToIndex:trimmed.length - 1];
            trimmed = [trimmed stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        }

        if (trimmed.length == 0 || ([trimmed hasPrefix:@"#"] && ![trimmed hasPrefix:@"#off "])) {
            JPRestartLine *l = [JPRestartLine new];
            l.isUnknownOrComment = YES;
            l.rawText = rawLine;
            [cfg.lines addObject:l];
            continue;
        }

        BOOL isOff = [trimmed hasPrefix:@"#off "];
        NSString *ruleStr = isOff ? [trimmed substringFromIndex:5] : trimmed;
        ruleStr = [ruleStr stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];

        NSArray<NSString *> *rawTokens = [ruleStr componentsSeparatedByCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@" \t\r"]];
        NSMutableArray<NSString *> *tokens = [NSMutableArray array];
        for (NSString *t in rawTokens) {
            if (t.length > 0) [tokens addObject:t];
        }

        // C1 规范：每行 bid:<bid> thr=<MB> bg=<s> max=<n> relaunch=<0|1>，严格恰好 5 个 token
        if (tokens.count != 5 || ![tokens[0] hasPrefix:@"bid:"]) {
            JPRestartLine *l = [JPRestartLine new];
            l.isUnknownOrComment = YES;
            l.rawText = rawLine;
            [cfg.lines addObject:l];
            continue;
        }

        NSString *bid = [tokens[0] substringFromIndex:4];
        if (bid.length == 0 || JPIsRestartBlockedBundleId(bid)) {
            JPRestartLine *l = [JPRestartLine new];
            l.isUnknownOrComment = YES;
            l.rawText = rawLine;
            [cfg.lines addObject:l];
            continue;
        }

        BOOL hasThr = NO, hasBg = NO, hasMax = NO, hasRelaunch = NO;
        NSInteger valThr = 0, valBg = 0, valMax = 0, valRelaunch = 0;
        BOOL bad = NO;

        for (int k = 1; k < 5; k++) {
            NSString *tok = tokens[k];
            NSRange eq = [tok rangeOfString:@"="];
            if (eq.location == NSNotFound || eq.location == 0 || eq.location == tok.length - 1) {
                bad = YES;
                break;
            }
            NSString *key = [tok substringToIndex:eq.location];
            NSString *valStr = [tok substringFromIndex:eq.location + 1];

            NSScanner *scanner = [NSScanner scannerWithString:valStr];
            long long v = 0;
            if (![scanner scanLongLong:&v] || ![scanner isAtEnd]) {
                bad = YES;
                break;
            }

            if ([key isEqualToString:@"thr"]) {
                if (hasThr || v < 200 || (uint64_t)v > memcap) { bad = YES; break; }
                valThr = (NSInteger)v;
                hasThr = YES;
            } else if ([key isEqualToString:@"bg"]) {
                if (hasBg || v < 60 || v > 7200) { bad = YES; break; }
                valBg = (NSInteger)v;
                hasBg = YES;
            } else if ([key isEqualToString:@"max"]) {
                if (hasMax || v < 1 || v > 10) { bad = YES; break; }
                valMax = (NSInteger)v;
                hasMax = YES;
            } else if ([key isEqualToString:@"relaunch"]) {
                if (hasRelaunch || (v != 0 && v != 1)) { bad = YES; break; }
                valRelaunch = (NSInteger)v;
                hasRelaunch = YES;
            } else {
                bad = YES;
                break;
            }
        }

        if (bad || !hasThr || !hasBg || !hasMax || !hasRelaunch) {
            JPRestartLine *l = [JPRestartLine new];
            l.isUnknownOrComment = YES;
            l.rawText = rawLine;
            [cfg.lines addObject:l];
            continue;
        }

        JPRestartRule *r = [JPRestartRule new];
        r.bid = bid;
        r.thr = valThr;
        r.bg = valBg;
        r.max = valMax;
        r.relaunch = valRelaunch;
        r.isEnabled = !isOff;
        r.lineIndex = (NSInteger)cfg.lines.count;

        JPRestartLine *l = [JPRestartLine new];
        l.rule = r;
        l.rawText = rawLine;
        l.isUnknownOrComment = NO;
        [cfg.lines addObject:l];
        [cfg.rules addObject:r];
    }

    [cfg updateDuplicateRules];
    return cfg;
}

- (void)updateDuplicateRules {
    NSMutableSet<NSString *> *seenBids = [NSMutableSet set];
    for (JPRestartLine *line in self.lines) {
        if (!line.isUnknownOrComment && line.rule) {
            NSString *b = line.rule.bid;
            if ([seenBids containsObject:b]) {
                line.rule.isDuplicate = YES;
            } else {
                line.rule.isDuplicate = NO;
                [seenBids addObject:b];
            }
        }
    }
}

- (JPRestartRule *)ruleForBundleId:(NSString *)bid {
    if (bid.length == 0) return nil;
    for (JPRestartLine *l in self.lines) {
        if (!l.isUnknownOrComment && l.rule && [l.rule.bid isEqualToString:bid]) {
            return l.rule;
        }
    }
    return nil;
}

- (void)setRule:(JPRestartRule *)rule forBundleId:(NSString *)bid {
    if (bid.length == 0 || !rule) return;
    rule.bid = bid;

    // 1. 按行定位：优先按 rule 对象本身匹配所在行
    for (NSUInteger i = 0; i < self.lines.count; i++) {
        JPRestartLine *l = self.lines[i];
        if (!l.isUnknownOrComment && l.rule == rule) {
            rule.lineIndex = (NSInteger)i;
            l.rawText = nil;
            [self updateDuplicateRules];
            return;
        }
    }

    // 2. 按行定位：若 rule 指针因重新载入而变更，按记录的 lineIndex 定位
    if (rule.lineIndex >= 0 && (NSUInteger)rule.lineIndex < self.lines.count) {
        JPRestartLine *l = self.lines[(NSUInteger)rule.lineIndex];
        if (!l.isUnknownOrComment && l.rule && [l.rule.bid isEqualToString:bid]) {
            NSUInteger idx = [self.rules indexOfObject:l.rule];
            if (idx != NSNotFound) {
                self.rules[idx] = rule;
            }
            l.rule = rule;
            l.rawText = nil;
            [self updateDuplicateRules];
            return;
        }
    }

    // 3. 若为单条已有规则更新（非重复行场景）
    JPRestartLine *singleMatch = nil;
    int matchCount = 0;
    for (JPRestartLine *l in self.lines) {
        if (!l.isUnknownOrComment && l.rule && [l.rule.bid isEqualToString:bid]) {
            singleMatch = l;
            matchCount++;
        }
    }
    if (matchCount == 1 && singleMatch) {
        NSUInteger idx = [self.rules indexOfObject:singleMatch.rule];
        if (idx != NSNotFound) {
            self.rules[idx] = rule;
        }
        singleMatch.rule = rule;
        singleMatch.rawText = nil;
        rule.lineIndex = (NSInteger)[self.lines indexOfObject:singleMatch];
        [self updateDuplicateRules];
        return;
    }

    // 4. 新建规则行
    JPRestartLine *newLine = [JPRestartLine new];
    newLine.rule = rule;
    newLine.rawText = nil;
    newLine.isUnknownOrComment = NO;
    rule.lineIndex = (NSInteger)self.lines.count;
    [self.lines addObject:newLine];
    [self.rules addObject:rule];
    [self updateDuplicateRules];
}

- (JPRestartLine *)lineForRule:(JPRestartRule *)rule {
    if (!rule) return nil;

    // 1. 在 self.lines 里找 l.rule == rule 的行（按对象指针）
    for (JPRestartLine *l in self.lines) {
        if (!l.isUnknownOrComment && l.rule == rule) {
            return l;
        }
    }

    // 2. 找不到时，按 rule.lineIndex 取那一行，并核对 bid 一致
    if (rule.lineIndex >= 0 && (NSUInteger)rule.lineIndex < self.lines.count) {
        JPRestartLine *l = self.lines[(NSUInteger)rule.lineIndex];
        if (!l.isUnknownOrComment && l.rule && [l.rule.bid isEqualToString:rule.bid]) {
            return l;
        }
    }

    return nil;
}

- (void)removeLine:(JPRestartLine *)line {
    if (!line) return;
    if (line.rule) {
        [self.rules removeObject:line.rule];
    }
    [self.lines removeObject:line];
    for (NSUInteger i = 0; i < self.lines.count; i++) {
        JPRestartLine *l = self.lines[i];
        if (!l.isUnknownOrComment && l.rule) {
            l.rule.lineIndex = (NSInteger)i;
        }
    }
    [self updateDuplicateRules];
}

- (BOOL)saveToPath:(NSString *)path error:(NSError **)error {
    if (!path || path.length == 0) return NO;

    NSMutableArray<NSString *> *outputLines = [NSMutableArray array];
    for (JPRestartLine *l in self.lines) {
        if (l.isUnknownOrComment || !l.rule) {
            [outputLines addObject:l.rawText ?: @""];
        } else {
            JPRestartRule *r = l.rule;
            NSMutableString *rLine = [NSMutableString string];
            if (!r.isEnabled) {
                [rLine appendString:@"#off "];
            }
            [rLine appendFormat:@"bid:%@ thr=%ld bg=%ld max=%ld relaunch=%ld",
             r.bid, (long)r.thr, (long)r.bg, (long)r.max, (long)r.relaunch];
            [outputLines addObject:rLine];
            l.rawText = rLine; // 沿用 0.7.3 保存逻辑：更新 rawText
        }
    }

    NSMutableString *outStr = [NSMutableString string];
    if (outputLines.count > 0) {
        [outStr appendString:[outputLines componentsJoinedByString:@"\n"]];
        if (self.hasTrailingNewline) {
            [outStr appendString:@"\n"];
        }
    }

    return [outStr writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:error];
}

+ (NSDictionary<NSString *, NSNumber *> *)loadAll7DayRestartCounts {
    NSMutableDictionary<NSString *, NSNumber *> *counts = [NSMutableDictionary dictionary];
    NSString *raw = [NSString stringWithContentsOfFile:kRestartState encoding:NSUTF8StringEncoding error:nil];
    if (!raw || raw.length == 0) return counts;

    NSArray<NSString *> *lines = [raw componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]];
    if (lines.count == 0) return counts;

    NSString *firstLine = [lines[0] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (![firstLine isEqualToString:@"v1"]) {
        return counts;
    }

    time_t now_wall = time(NULL);
    struct tm tm_now;
    localtime_r(&now_wall, &tm_now);
    int y_now = tm_now.tm_year + 1900;
    int m_now = tm_now.tm_mon + 1;
    int d_now = tm_now.tm_mday;

    struct tm cur_tm = {0};
    cur_tm.tm_year = y_now - 1900;
    cur_tm.tm_mon = m_now - 1;
    cur_tm.tm_mday = d_now;
    cur_tm.tm_hour = 12;
    cur_tm.tm_isdst = -1;
    time_t cur_midday = mktime(&cur_tm);

    for (NSUInteger i = 1; i < lines.count; i++) {
        NSString *line = [lines[i] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if (line.length == 0 || [line hasPrefix:@"#"] || [line hasPrefix:@"last "]) continue;
        if (![line hasPrefix:@"bid:"]) continue;

        NSArray<NSString *> *parts = [line componentsSeparatedByCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        NSMutableArray<NSString *> *tokens = [NSMutableArray array];
        for (NSString *p in parts) {
            if (p.length > 0) [tokens addObject:p];
        }
        if (tokens.count != 3) continue;

        NSString *lineBid = [tokens[0] substringFromIndex:4];
        if (lineBid.length == 0) continue;

        NSString *dateStr = tokens[1];
        int ey = 0, em = 0, ed = 0;
        if (sscanf(dateStr.UTF8String, "%4d-%2d-%2d", &ey, &em, &ed) != 3) continue;

        struct tm e_tm = {0};
        e_tm.tm_year = ey - 1900;
        e_tm.tm_mon = em - 1;
        e_tm.tm_mday = ed;
        e_tm.tm_hour = 12;
        e_tm.tm_isdst = -1;
        time_t e_midday = mktime(&e_tm);

        if (e_midday != (time_t)-1 && cur_midday != (time_t)-1) {
            double sec_diff = difftime(cur_midday, e_midday);
            long diffDays = (long)((sec_diff + 43200.0) / 86400.0);
            if (diffDays >= 0 && diffDays <= 6) {
                NSInteger val = [tokens[2] integerValue];
                NSInteger cur = [counts[lineBid] integerValue];
                counts[lineBid] = @(cur + val);
            }
        }
    }
    return counts;
}

+ (NSInteger)load7DayRestartCountForBundleId:(NSString *)bid {
    if (bid.length == 0) return 0;
    NSDictionary<NSString *, NSNumber *> *counts = [self loadAll7DayRestartCounts];
    return [counts[bid] integerValue];
}

+ (NSInteger)loadWarnThresholdForBundleId:(NSString *)bid pathFragment:(NSString *)frag {
    if (bid.length == 0) return 0;
    JPPulseConfig *pcfg = [JPPulseConfig loadFromPath:kPulseConfig];
    JPPulseRule *pr = [pcfg ruleForBundleId:bid];
    if (pr && pr.enabled) {
        return pr.target;
    }
    return 0;
}

@end

typedef struct __SecTask *SecTaskRef;
typedef SecTaskRef (*SecTaskCreateFromSelfFunc)(CFAllocatorRef allocator);
typedef CFTypeRef (*SecTaskCopyValueForEntitlementFunc)(SecTaskRef task, CFStringRef entitlement, CFErrorRef *error);

static BOOL JPAppHasPersonaEntitlement(void) {
    void *secHandle = dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_LAZY);
    if (!secHandle) secHandle = RTLD_DEFAULT;
    SecTaskCreateFromSelfFunc taskCreate = (SecTaskCreateFromSelfFunc)dlsym(secHandle, "SecTaskCreateFromSelf");
    SecTaskCopyValueForEntitlementFunc copyEnt = (SecTaskCopyValueForEntitlementFunc)dlsym(secHandle, "SecTaskCopyValueForEntitlement");
    if (!taskCreate || !copyEnt) return NO;

    SecTaskRef task = taskCreate(NULL);
    if (!task) return NO;

    CFErrorRef err = NULL;
    CFTypeRef val = copyEnt(task, CFSTR("com.apple.private.persona-mgmt"), &err);
    BOOL hasEnt = NO;
    if (val) {
        if (CFGetTypeID(val) == CFBooleanGetTypeID()) {
            hasEnt = CFBooleanGetValue((CFBooleanRef)val);
        }
        CFRelease(val);
    }
    if (err) CFRelease(err);
    CFRelease(task);
    return hasEnt;
}

static int JPSpawnPersonaProbe(int *exitCodeOut, int *errOut) {
    NSString *bin = [NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:@"jetpilotd"];
    const char *cpath = bin.fileSystemRepresentation;
    const char *argv[] = { cpath, "--probe", NULL };

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
    posix_spawnattr_set_persona_np(&attr, 99, POSIX_SPAWN_PERSONA_FLAGS_OVERRIDE);
    posix_spawnattr_set_persona_uid_np(&attr, 0);
    posix_spawnattr_set_persona_gid_np(&attr, 0);

    pid_t pid = 0;
    int rc = posix_spawn(&pid, cpath, NULL, &attr, (char **)argv, environ);
    posix_spawnattr_destroy(&attr);

    int exitCode = -1;
    int err = 0;
    if (rc != 0) {
        err = rc;
    } else if (pid > 0) {
        int status = 0;
        pid_t wpid = waitpid(pid, &status, 0);
        if (wpid == pid) {
            if (WIFEXITED(status)) {
                exitCode = WEXITSTATUS(status);
            }
        } else {
            err = errno;
        }
    }
    if (exitCodeOut) *exitCodeOut = exitCode;
    if (errOut) *errOut = err;
    return rc;
}

static int JPSpawnPlainProbe(int *exitCodeOut, int *errOut) {
    NSString *bin = [NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:@"jetpilotd"];
    const char *cpath = bin.fileSystemRepresentation;
    const char *argv[] = { cpath, "--probe", NULL };

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);

    pid_t pid = 0;
    int rc = posix_spawn(&pid, cpath, NULL, &attr, (char **)argv, environ);
    posix_spawnattr_destroy(&attr);

    int exitCode = -1;
    int err = 0;
    if (rc != 0) {
        err = rc;
    } else if (pid > 0) {
        int status = 0;
        pid_t wpid = waitpid(pid, &status, 0);
        if (wpid == pid) {
            if (WIFEXITED(status)) {
                exitCode = WEXITSTATUS(status);
            }
        } else {
            err = errno;
        }
    }
    if (exitCodeOut) *exitCodeOut = exitCode;
    if (errOut) *errOut = err;
    return rc;
}

@interface JPDaemonManager : NSObject
+ (JPDaemonState)daemonStateWithLastSpawnTime:(CFAbsoluteTime)lastSpawnTime;
+ (int)startDaemonWithLastSpawnTime:(CFAbsoluteTime *)lastSpawnTimeOut;
+ (void)stopDaemon;
+ (void)runProbeDiagnosticsFromViewController:(UIViewController *)vc;
+ (BOOL)precheckDaemonWithRc:(int *)rcOut exitOut:(int *)exitOut errOut:(int *)errOut reason:(NSString **)reasonOut;
+ (void)handleSpawnFailure:(int)rc fromViewController:(UIViewController *)vc;
@end

@implementation JPDaemonManager

+ (JPDaemonState)daemonStateWithLastSpawnTime:(CFAbsoluteTime)lastSpawnTime {
    NSFileManager *fm = NSFileManager.defaultManager;
    if (![fm fileExistsAtPath:kFlag]) return JPDaemonStateStopped;

    if (lastSpawnTime > 0 && CFAbsoluteTimeGetCurrent() - lastSpawnTime < 10.0)
        return JPDaemonStateRunning;

    NSDate *mtime = [[fm attributesOfItemAtPath:kStatus error:nil] fileModificationDate];
    time_t boot = JPBootTime();
    if (!mtime) return JPDaemonStateDead;
    if (boot > 0 && (time_t)mtime.timeIntervalSince1970 < boot) return JPDaemonStateDead;

    NSString *pidStr = [NSString stringWithContentsOfFile:kPidFile encoding:NSUTF8StringEncoding error:nil];
    pid_t pid = (pid_t)[pidStr intValue];
    if (pid <= 0) return JPDaemonStateDead;
    if (kill(pid, 0) != 0 && errno == ESRCH) return JPDaemonStateDead;
    return JPDaemonStateRunning;
}

+ (int)startDaemonWithLastSpawnTime:(CFAbsoluteTime *)lastSpawnTimeOut {
    [@"run" writeToFile:kFlag atomically:YES encoding:NSUTF8StringEncoding error:nil];

    NSString *bin = [NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:@"jetpilotd"];
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
    posix_spawnattr_set_persona_np(&attr, 99, POSIX_SPAWN_PERSONA_FLAGS_OVERRIDE);
    posix_spawnattr_set_persona_uid_np(&attr, 0);
    posix_spawnattr_set_persona_gid_np(&attr, 0);

    pid_t pid = 0;
    const char *argv[] = { bin.fileSystemRepresentation, NULL };
    int rc = posix_spawn(&pid, bin.fileSystemRepresentation, NULL, &attr, (char **)argv, environ);
    posix_spawnattr_destroy(&attr);
    if (rc == 0 && lastSpawnTimeOut) {
        *lastSpawnTimeOut = CFAbsoluteTimeGetCurrent();
    }
    return rc;
}

+ (void)stopDaemon {
    [NSFileManager.defaultManager removeItemAtPath:kFlag error:nil];
}

+ (void)runProbeDiagnosticsFromViewController:(UIViewController *)vc {
    /*
     * E2-4 --probe 探测模式（仅限手动触发的诊断）
     * 约束：探测只在用户手动点诊断时运行，不进入正常启动流程；
     * 不带 persona 的子进程因为 --probe 会立即退出，不会以 mobile 身份常驻。
     *
     * 判读（写进代码注释，不用实现自动判断）：
     * (a) 返回 EPERM、(b) 成功 → 问题在 persona 这一步；
     * (a)(b) 都返回 EPERM → 内核拒绝执行这个文件；
     * 两者都成功 → 问题在常规启动路径的环境里。
     */
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        int persona_exit = -1;
        int persona_rc = JPSpawnPersonaProbe(&persona_exit, NULL);

        int plain_exit = -1;
        int plain_rc = JPSpawnPlainProbe(&plain_exit, NULL);

        BOOL persona_ent = JPAppHasPersonaEntitlement();

        // 记录结论行（E2-4 格式规范）
        NSLog(@"SPAWN_PROBE persona_rc=%d plain_rc=%d plain_exit=%d persona_ent=%d",
              persona_rc, plain_rc, plain_exit, (int)(persona_ent ? 1 : 0));

        dispatch_async(dispatch_get_main_queue(), ^{
            if (!vc) return;
            NSString *diagMsg = [NSString stringWithFormat:
                @"SPAWN_PROBE persona_rc=%d plain_rc=%d plain_exit=%d persona_ent=%d\n\n"
                @"【探测详情】\n"
                @"a. 带 persona: rc=%d (%s), exit=%d\n"
                @"b. 不带 persona: rc=%d (%s), exit=%d\n"
                @"c. persona-mgmt 权限: %@\n\n"
                @"【判读参考】\n"
                @"• (a) EPERM、(b) 成功 → 问题在 persona 这一步\n"
                @"• (a)(b) 都 EPERM → 内核拒绝执行该文件\n"
                @"• 两者都成功 → 问题在常规启动环境",
                persona_rc, plain_rc, plain_exit, (int)(persona_ent ? 1 : 0),
                persona_rc, strerror(persona_rc), persona_exit,
                plain_rc, strerror(plain_rc), plain_exit,
                persona_ent ? @"YES" : @"NO"];

            UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"进程探测诊断"
                                                                           message:diagMsg
                                                                    preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:@"确定" style:UIAlertActionStyleDefault handler:nil]];
            [vc presentViewController:alert animated:YES completion:nil];
        });
    });
}

+ (BOOL)precheckDaemonWithRc:(int *)rcOut exitOut:(int *)exitOut errOut:(int *)errOut reason:(NSString **)reasonOut {
    /*
     * E2-1 重启前预检（先验后停）
     * 1. jetpilotd 存在且可执行（access(X_OK)）；
     * 2. App 自身持有 persona-mgmt（SecTaskCopyValueForEntitlement）；
     * 3. 带 persona spawn jetpilotd --probe（即 E2-4 (a)），rc 为 0 且退出码为 0 才允许停旧进程。
     */
    NSString *bin = [NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:@"jetpilotd"];
    const char *cpath = bin.fileSystemRepresentation;

    // 1. jetpilotd 存在且可执行（access(X_OK)）
    if (access(cpath, X_OK) != 0) {
        int e = errno;
        if (rcOut) *rcOut = -1;
        if (exitOut) *exitOut = -1;
        if (errOut) *errOut = e;
        if (reasonOut) *reasonOut = [NSString stringWithFormat:@"jetpilotd 不存在或不可执行 (access X_OK 失败: %s)", strerror(e)];
        return NO;
    }

    // 2. App 自身持有 persona-mgmt（SecTaskCopyValueForEntitlement）
    if (!JPAppHasPersonaEntitlement()) {
        if (rcOut) *rcOut = -1;
        if (exitOut) *exitOut = -1;
        if (errOut) *errOut = EPERM;
        if (reasonOut) *reasonOut = @"App 缺少 com.apple.private.persona-mgmt 权限";
        return NO;
    }

    // 3. 带 persona spawn jetpilotd --probe (E2-4 a)
    int probe_exit = -1;
    int probe_err = 0;
    int probe_rc = JPSpawnPersonaProbe(&probe_exit, &probe_err);
    if (probe_rc != 0) {
        if (rcOut) *rcOut = probe_rc;
        if (exitOut) *exitOut = -1;
        if (errOut) *errOut = (probe_err != 0) ? probe_err : probe_rc;
        if (reasonOut) *reasonOut = [NSString stringWithFormat:@"带 persona 探测 spawn 失败: %s", strerror(probe_rc)];
        return NO;
    }

    if (probe_err != 0) {
        if (rcOut) *rcOut = 0;
        if (exitOut) *exitOut = probe_exit;
        if (errOut) *errOut = probe_err;
        if (reasonOut) *reasonOut = [NSString stringWithFormat:@"带 persona 探测 waitpid 失败: %s", strerror(probe_err)];
        return NO;
    }

    if (probe_exit != 0) {
        if (rcOut) *rcOut = 0;
        if (exitOut) *exitOut = probe_exit;
        if (errOut) *errOut = 0;
        if (reasonOut) *reasonOut = [NSString stringWithFormat:@"带 persona 探测退出码非 0: %d", probe_exit];
        return NO;
    }

    if (rcOut) *rcOut = 0;
    if (exitOut) *exitOut = 0;
    if (errOut) *errOut = 0;
    if (reasonOut) *reasonOut = nil;
    return YES;
}

+ (void)handleSpawnFailure:(int)rc fromViewController:(UIViewController *)vc {
    /*
     * E2-1 第 3 条: spawn 失败时（正常启动/重启路径），弹窗与日志写出：
     * rc 与 strerror、二进制路径、存在与否、mode、大小、App 是否持有 persona-mgmt。
     * 并在日志里加一行结论。
     * 第 4 条约束: 不允许失败后改成不带 persona 再拉一次（那样会以 mobile 身份运行，权限全失，属于静默降级）。
     */
    NSString *bin = [NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:@"jetpilotd"];
    const char *cpath = bin.fileSystemRepresentation;

    struct stat st;
    memset(&st, 0, sizeof(st));
    int stat_rc = stat(cpath, &st);
    BOOL exists = (stat_rc == 0);
    mode_t mode = exists ? st.st_mode : 0;
    off_t size = exists ? st.st_size : 0;
    BOOL persona_ent = JPAppHasPersonaEntitlement();

    NSLog(@"SPAWN_FAIL rc=%d errno=%d bin=%s exists=%d mode=0%o size=%lld persona_ent=%d",
          rc, rc, cpath, (int)exists, (unsigned int)(mode & 07777), (long long)size, (int)(persona_ent ? 1 : 0));

    if (vc) {
        NSString *msg = [NSString stringWithFormat:
            @"posix_spawn 失败 (rc=%d: %s)\n\n"
            @"二进制路径: %@\n"
            @"文件存在: %@\n"
            @"权限 mode: 0%o\n"
            @"文件大小: %lld 字节\n"
            @"persona-mgmt 权限: %@",
            rc, strerror(rc),
            bin,
            exists ? @"YES" : @"NO",
            (unsigned int)(mode & 07777),
            (long long)size,
            persona_ent ? @"YES" : @"NO"];

        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"启动失败"
                                                                       message:msg
                                                                preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
        [vc presentViewController:alert animated:YES completion:nil];
    }
}

@end


#pragma mark - Matching Helpers & Jetsam Models

static BOOL FragmentMatchesProcessName(NSString *frag, NSString *procName) {
    if (frag.length == 0 || procName.length == 0) return NO;
    NSString *clean = frag;
    if ([clean hasSuffix:@"/"]) clean = [clean substringToIndex:clean.length - 1];
    if ([clean.pathExtension caseInsensitiveCompare:@"app"] == NSOrderedSame)
        clean = [clean stringByDeletingPathExtension];
    if (clean.length < 3) return NO;   // 太短的片段不参与匹配，避免误判
    return [procName rangeOfString:clean options:NSCaseInsensitiveSearch].location != NSNotFound;
}

// 真机待测：Reynard 的扩展子进程在 JetsamEvent 里的 name 可能不带 "Reynard"，
// 装好后看一份真实日志，必要时给规则加别名。

static NSString *ResolveDisplayNameHelper(NSString *frag, NSArray<JPInstalledApp *> *apps) {
    if (!frag || frag.length == 0) return @"";
    NSString *resolved = nil;
    if ([frag hasPrefix:@"bid:"]) {
        NSString *targetBid = [frag substringFromIndex:4];
        for (JPInstalledApp *app in apps) {
            if (app.bundleId.length > 0 && [app.bundleId isEqualToString:targetBid]) {
                resolved = app.displayName;
                break;
            }
        }
    } else {
        for (JPInstalledApp *app in apps) {
            if ([frag containsString:app.bundleName] || [app.bundleName containsString:frag]) {
                resolved = app.displayName;
                break;
            }
        }
    }
    if (resolved.length > 0) {
        if ([frag containsString:@"PushBundle"]) {
            return [NSString stringWithFormat:@"%@（网页 App 推送）", resolved];
        }
        return resolved;
    }
    return frag;
}

static BOOL RuleMatchesProcess(JPRule *rule, NSString *procName, NSArray<JPInstalledApp *> *apps) {
    if (!rule || !procName || procName.length == 0) return NO;
    if ([rule.pathFragment hasPrefix:@"bid:"]) {
        NSString *targetBid = [rule.pathFragment substringFromIndex:4];
        if (apps) {
            for (JPInstalledApp *app in apps) {
                if (app.bundleId.length > 0 && [app.bundleId isEqualToString:targetBid]) {
                    if ([procName localizedCaseInsensitiveCompare:app.bundleId] == NSOrderedSame) return YES;
                    if (app.displayName.length > 0 && [procName localizedCaseInsensitiveCompare:app.displayName] == NSOrderedSame) return YES;
                    if (FragmentMatchesProcessName(app.bundleName, procName)) return YES;
                    break;
                }
            }
        }
        return NO;
    }
    if (FragmentMatchesProcessName(rule.pathFragment, procName)) return YES;
    if (apps) {
        for (JPInstalledApp *app in apps) {
            if ([rule.pathFragment containsString:app.bundleName] || [app.bundleName containsString:rule.pathFragment]) {
                if (FragmentMatchesProcessName(app.bundleName, procName)) return YES;
                if (app.bundleId.length > 0 && [procName localizedCaseInsensitiveCompare:app.bundleId] == NSOrderedSame) return YES;
                if (app.displayName.length > 0 && [procName localizedCaseInsensitiveCompare:app.displayName] == NSOrderedSame) return YES;
                break;
            }
        }
    }
    return NO;
}

@interface JPJetsamDaySummary : NSObject
@property (nonatomic, copy) NSString *dateKey;
@property (nonatomic, copy) NSString *dateFormatted;
@property (nonatomic, copy) NSString *displayName;
@property (nonatomic, copy) NSString *pathFragment;
@property (nonatomic, assign) NSInteger killCount;
@property (nonatomic, copy) NSString *primaryReason;
@end

@implementation JPJetsamDaySummary
@end

@interface JPKilledStatsInfo : NSObject
@property (nonatomic, assign) NSInteger totalKills;
@property (nonatomic, copy) NSArray<JPJetsamDaySummary *> *daySummaries;
@property (nonatomic, assign) NSInteger skippedFilesCount;
@property (nonatomic, assign) BOOL available;
+ (JPKilledStatsInfo *)loadStatsForRules:(NSArray<JPRule *> *)rules installedApps:(NSArray<JPInstalledApp *> *)apps;
@end

@implementation JPKilledStatsInfo

+ (JPKilledStatsInfo *)loadStatsForRules:(NSArray<JPRule *> *)rules installedApps:(NSArray<JPInstalledApp *> *)apps {
    JPKilledStatsInfo *result = [JPKilledStatsInfo new];
    result.totalKills = 0;
    result.daySummaries = @[];
    result.skippedFilesCount = 0;
    result.available = NO;

    @try {
        NSFileManager *fm = NSFileManager.defaultManager;
        BOOL isDir = NO;
        if (![fm fileExistsAtPath:kCrashReporterDir isDirectory:&isDir] || !isDir) {
            return result;
        }

        NSError *dirErr = nil;
        NSArray<NSString *> *allFiles = [fm contentsOfDirectoryAtPath:kCrashReporterDir error:&dirErr];
        if (!allFiles) {
            return result;
        }

        result.available = YES;

        if (allFiles.count == 0) {
            return result;
        }

        NSInteger jetsamFilesCount = 0;
        for (NSString *filename in allFiles) {
            if ([filename hasPrefix:@"JetsamEvent-"] && [filename.pathExtension isEqualToString:@"ips"]) {
                jetsamFilesCount++;
            }
        }
        if (jetsamFilesCount == 0) {
            return result;
        }

        NSMutableArray<JPRule *> *protectRules = [NSMutableArray array];
        for (JPRule *r in rules) {
            if (r.type == JPRuleTypeProtect && r.pathFragment.length > 0) {
                [protectRules addObject:r];
            }
        }
        if (protectRules.count == 0) {
            return result;
        }

        NSCalendar *calendar = [NSCalendar currentCalendar];
        NSDate *now = [NSDate date];
        NSDate *startOfToday = nil;
        if (![calendar rangeOfUnit:NSCalendarUnitDay startDate:&startOfToday interval:NULL forDate:now]) {
            startOfToday = now;
        }
        NSDateComponents *sub7Comps = [NSDateComponents new];
        sub7Comps.day = -6;
        NSDate *sevenDaysAgo = [calendar dateByAddingComponents:sub7Comps toDate:startOfToday options:0] ?: [now dateByAddingTimeInterval:-7*86400];

        // Regex for JetsamEvent-YYYY-MM-DD-HHMMSS.ips
        // TODO: [iOS 16.4.1 真机待测] 日志文件名时间提取：文件名通常为 JetsamEvent-YYYY-MM-DD-HHMMSS.ips，部分系统版本可能附带 .synced 或序号后缀。若文件名无法提取时间，跳过并计入 skippedCount；后续可在真机验证后补充从 JSON 头部 timestamp 字段提取备选时间的逻辑。
        static NSRegularExpression *fileRegex = nil;
        static dispatch_once_t onceToken;
        dispatch_once(&onceToken, ^{
            fileRegex = [NSRegularExpression regularExpressionWithPattern:@"^JetsamEvent-(\\d{4})-(\\d{2})-(\\d{2})(?:-(\\d{2})(\\d{2})(\\d{2}))?"
                                                                  options:0
                                                                    error:nil];
        });

        NSArray<NSString *> *weekdayNames = @[@"周日", @"周一", @"周二", @"周三", @"周四", @"周五", @"周六"];

        NSMutableDictionary<NSString *, NSMutableDictionary *> *groups = [NSMutableDictionary dictionary];
        NSInteger totalKills = 0;
        NSInteger skippedCount = 0;

        for (NSString *filename in allFiles) {
            if (![filename hasPrefix:@"JetsamEvent-"] || ![filename.pathExtension isEqualToString:@"ips"]) {
                continue;
            }

            NSTextCheckingResult *match = [fileRegex firstMatchInString:filename options:0 range:NSMakeRange(0, filename.length)];
            NSDate *fileDate = nil;
            NSString *dateKey = nil;
            NSString *dateFormatted = nil;

            if (match && match.numberOfRanges >= 4) {
                NSInteger year = [[filename substringWithRange:[match rangeAtIndex:1]] integerValue];
                NSInteger month = [[filename substringWithRange:[match rangeAtIndex:2]] integerValue];
                NSInteger day = [[filename substringWithRange:[match rangeAtIndex:3]] integerValue];
                NSInteger hour = 0, minute = 0, second = 0;
                if (match.numberOfRanges >= 7 && [match rangeAtIndex:4].location != NSNotFound) {
                    hour = [[filename substringWithRange:[match rangeAtIndex:4]] integerValue];
                    minute = [[filename substringWithRange:[match rangeAtIndex:5]] integerValue];
                    second = [[filename substringWithRange:[match rangeAtIndex:6]] integerValue];
                }

                NSDateComponents *c = [NSDateComponents new];
                c.year = year;
                c.month = month;
                c.day = day;
                c.hour = hour;
                c.minute = minute;
                c.second = second;
                c.timeZone = [NSTimeZone systemTimeZone];
                fileDate = [calendar dateFromComponents:c];

                dateKey = [NSString stringWithFormat:@"%04ld-%02ld-%02ld", (long)year, (long)month, (long)day];

                NSDateComponents *wc = [calendar components:NSCalendarUnitWeekday fromDate:fileDate];
                NSInteger wd = wc.weekday;
                NSString *wdStr = (wd >= 1 && wd <= 7) ? weekdayNames[wd - 1] : @"周日";
                dateFormatted = [NSString stringWithFormat:@"%02ld-%02ld（%@）", (long)month, (long)day, wdStr];
            }

            if (!fileDate || !dateKey) {
                skippedCount++;
                continue;
            }

            if ([fileDate compare:sevenDaysAgo] == NSOrderedAscending) {
                continue;
            }
            if ([fileDate timeIntervalSinceDate:now] > 86400) {
                continue;
            }

            NSString *fullPath = [kCrashReporterDir stringByAppendingPathComponent:filename];
            @try {
                NSError *readErr = nil;
                NSString *fileContent = [NSString stringWithContentsOfFile:fullPath encoding:NSUTF8StringEncoding error:&readErr];
                if (!fileContent || fileContent.length == 0) {
                    skippedCount++;
                    continue;
                }

                NSRange nlRange = [fileContent rangeOfString:@"\n"];
                NSString *jsonBody = nil;
                if (nlRange.location != NSNotFound) {
                    jsonBody = [[fileContent substringFromIndex:nlRange.location + 1] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
                }
                if (!jsonBody || jsonBody.length == 0) {
                    jsonBody = fileContent;
                }

                NSData *jsonData = [jsonBody dataUsingEncoding:NSUTF8StringEncoding];
                if (!jsonData) {
                    skippedCount++;
                    continue;
                }

                NSError *jsonErr = nil;
                id jsonRoot = [NSJSONSerialization JSONObjectWithData:jsonData options:0 error:&jsonErr];
                if (!jsonRoot || ![jsonRoot isKindOfClass:[NSDictionary class]]) {
                    if (jsonBody != fileContent) {
                        jsonRoot = [NSJSONSerialization JSONObjectWithData:[fileContent dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
                    }
                    if (!jsonRoot || ![jsonRoot isKindOfClass:[NSDictionary class]]) {
                        skippedCount++;
                        continue;
                    }
                }

                NSDictionary *rootDict = (NSDictionary *)jsonRoot;

                // TODO: [iOS 16.4.1 真机待测] 进程列表 JSON 键名：标准字段为 "processes"，此处做多字段兼容：依次尝试 "processes"、"processList"、"sortedByResidentSize"、"processes_list"。
                id procs = rootDict[@"processes"];
                if (!procs) procs = rootDict[@"processList"];
                if (!procs) procs = rootDict[@"sortedByResidentSize"];
                if (!procs) procs = rootDict[@"processes_list"];

                if (!procs || ![procs isKindOfClass:[NSArray class]]) {
                    skippedCount++;
                    continue;
                }

                NSArray *procArray = (NSArray *)procs;

                for (id procItem in procArray) {
                    if (![procItem isKindOfClass:[NSDictionary class]]) continue;
                    NSDictionary *pDict = (NSDictionary *)procItem;

                    // TODO: [iOS 16.4.1 真机待测] 进程名称 JSON 键名：标准字段为 "name"，此处做多字段兼容：依次尝试 "name"、"process_name"、"procName"、"proc_name"、"bundleID"、"bundleIdentifier"。
                    NSString *procName = pDict[@"name"];
                    if (![procName isKindOfClass:[NSString class]] || procName.length == 0) procName = pDict[@"process_name"];
                    if (![procName isKindOfClass:[NSString class]] || procName.length == 0) procName = pDict[@"procName"];
                    if (![procName isKindOfClass:[NSString class]] || procName.length == 0) procName = pDict[@"proc_name"];
                    if (![procName isKindOfClass:[NSString class]] || procName.length == 0) procName = pDict[@"bundleID"];
                    if (![procName isKindOfClass:[NSString class]] || procName.length == 0) procName = pDict[@"bundleIdentifier"];

                    if (![procName isKindOfClass:[NSString class]] || procName.length == 0) {
                        continue;
                    }

                    // TODO: [iOS 16.4.1 真机待测] 被杀原因 JSON 键名：iOS Jetsam 日志中仅真正被杀的进程带有 reason 字段（未被杀的存活进程快照无此字段）。此处做多字段兼容：依次尝试 "reason"、"terminationReason"、"exitReason"、"killReason"、"jetsamReason"，并支持字符串或字典类型。
                    id rObj = pDict[@"reason"];
                    if (!rObj) rObj = pDict[@"terminationReason"];
                    if (!rObj) rObj = pDict[@"exitReason"];
                    if (!rObj) rObj = pDict[@"killReason"];
                    if (!rObj) rObj = pDict[@"jetsamReason"];

                    if (!rObj || rObj == [NSNull null]) {
                        continue;
                    }

                    NSString *reasonStr = nil;
                    if ([rObj isKindOfClass:[NSString class]]) {
                        reasonStr = (NSString *)rObj;
                    } else if ([rObj isKindOfClass:[NSDictionary class]]) {
                        reasonStr = rObj[@"type"] ?: rObj[@"name"] ?: [rObj description];
                    } else {
                        reasonStr = [rObj description];
                    }
                    reasonStr = [reasonStr stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
                    if (reasonStr.length == 0) {
                        reasonStr = @"未知原因";
                    }

                    for (JPRule *pRule in protectRules) {
                        if (RuleMatchesProcess(pRule, procName, apps)) {
                            totalKills++;
                            NSString *groupKey = [NSString stringWithFormat:@"%@|%@", dateKey, pRule.pathFragment];
                            NSMutableDictionary *group = groups[groupKey];
                            if (!group) {
                                NSString *dName = pRule.displayName;
                                if (!dName || dName.length == 0) {
                                    dName = ResolveDisplayNameHelper(pRule.pathFragment, apps);
                                }
                                group = [NSMutableDictionary dictionaryWithDictionary:@{
                                    @"dateKey": dateKey,
                                    @"dateFormatted": dateFormatted,
                                    @"pathFragment": pRule.pathFragment,
                                    @"displayName": dName ?: pRule.pathFragment,
                                    @"killCount": @(0),
                                    @"reasons": [NSMutableArray array]
                                }];
                                groups[groupKey] = group;
                            }

                            NSInteger curCount = [group[@"killCount"] integerValue];
                            group[@"killCount"] = @(curCount + 1);
                            [group[@"reasons"] addObject:reasonStr];
                            break;
                        }
                    }
                }
            } @catch (NSException *ex) {
                skippedCount++;
            }
        }

        NSMutableArray<JPJetsamDaySummary *> *summaries = [NSMutableArray array];
        for (NSString *key in groups) {
            NSMutableDictionary *dict = groups[key];
            JPJetsamDaySummary *s = [JPJetsamDaySummary new];
            s.dateKey = dict[@"dateKey"];
            s.dateFormatted = dict[@"dateFormatted"];
            s.pathFragment = dict[@"pathFragment"];
            s.displayName = dict[@"displayName"];
            s.killCount = [dict[@"killCount"] integerValue];

            NSArray<NSString *> *reasons = dict[@"reasons"];
            NSCountedSet *counts = [NSCountedSet setWithArray:reasons];
            NSString *bestReason = @"未知原因";
            NSUInteger maxCount = 0;
            for (NSString *r in counts) {
                NSUInteger c = [counts countForObject:r];
                if (c > maxCount) {
                    maxCount = c;
                    bestReason = r;
                }
            }
            s.primaryReason = bestReason;
            [summaries addObject:s];
        }

        [summaries sortUsingComparator:^NSComparisonResult(JPJetsamDaySummary *a, JPJetsamDaySummary *b) {
            NSComparisonResult r = [b.dateKey compare:a.dateKey];
            if (r != NSOrderedSame) return r;
            if (a.killCount < b.killCount) return NSOrderedDescending;
            if (a.killCount > b.killCount) return NSOrderedAscending;
            return [a.displayName localizedStandardCompare:b.displayName];
        }];

        result.totalKills = totalKills;
        result.daySummaries = [summaries copy];
        result.skippedFilesCount = skippedCount;
    } @catch (NSException *ex) {
        // Complete outer protection
    }
    return result;
}

@end

#pragma mark - Views: Status Card

@interface JPStatusCardView : UIView
@property (nonatomic, strong) UIView *statusDot;
@property (nonatomic, strong) UILabel *statusTitleLabel;
@property (nonatomic, strong) UILabel *updatedLabel;
@property (nonatomic, strong) UILabel *protectedCountLabel;
@property (nonatomic, strong) UILabel *demotedCountLabel;
@property (nonatomic, strong) UILabel *reassertCountLabel;
@property (nonatomic, strong) UIButton *actionButton;
@property (nonatomic, strong) UIView *protectedColumn;
@property (nonatomic, copy) void (^onActionToggle)(void);
@property (nonatomic, copy) void (^onTapProtected)(void);
@property (nonatomic, strong) UIStackView *metricsAndErrorStack;
@property (nonatomic, strong) UIView *errorContainerView;
@property (nonatomic, strong) UILabel *failureCountLabel;
@property (nonatomic, strong) UILabel *failureDetailLabel;
@property (nonatomic, strong) UIButton *killedStatsEntryButton;
@property (nonatomic, copy) void (^onTapKilledStats)(void);

- (void)updateWithStatus:(JPStatusInfo *)status isRunning:(BOOL)isRunning;
- (void)updateKilledCount:(NSInteger)totalKills available:(BOOL)available;
- (void)updateKilledCount:(NSInteger)totalKills;
@end

@implementation JPStatusCardView

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        [self setupUI];
    }
    return self;
}

- (void)setupUI {
    self.backgroundColor = UIColor.secondarySystemGroupedBackgroundColor;
    self.layer.cornerRadius = 14;
    self.layer.cornerCurve = kCACornerCurveContinuous;
    self.clipsToBounds = YES;
    self.translatesAutoresizingMaskIntoConstraints = NO;

    self.statusDot = [UIView new];
    self.statusDot.translatesAutoresizingMaskIntoConstraints = NO;
    self.statusDot.layer.cornerRadius = 5;
    self.statusDot.layer.masksToBounds = YES;
    self.statusDot.backgroundColor = UIColor.systemGray3Color;

    self.statusTitleLabel = [UILabel new];
    self.statusTitleLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.statusTitleLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightSemibold];
    self.statusTitleLabel.textColor = UIColor.labelColor;
    self.statusTitleLabel.text = @"守护进程已停止";

    self.updatedLabel = [UILabel new];
    self.updatedLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.updatedLabel.font = [UIFont systemFontOfSize:12 weight:UIFontWeightRegular];
    self.updatedLabel.textColor = UIColor.secondaryLabelColor;
    self.updatedLabel.textAlignment = NSTextAlignmentRight;
    self.updatedLabel.text = @"未运行";

    UIView *topRow = [UIView new];
    topRow.translatesAutoresizingMaskIntoConstraints = NO;
    [topRow addSubview:self.statusDot];
    [topRow addSubview:self.statusTitleLabel];
    [topRow addSubview:self.updatedLabel];

    [NSLayoutConstraint activateConstraints:@[
        [self.statusDot.leadingAnchor constraintEqualToAnchor:topRow.leadingAnchor],
        [self.statusDot.centerYAnchor constraintEqualToAnchor:topRow.centerYAnchor],
        [self.statusDot.widthAnchor constraintEqualToConstant:10],
        [self.statusDot.heightAnchor constraintEqualToConstant:10],

        [self.statusTitleLabel.leadingAnchor constraintEqualToAnchor:self.statusDot.trailingAnchor constant:8],
        [self.statusTitleLabel.centerYAnchor constraintEqualToAnchor:topRow.centerYAnchor],

        [self.updatedLabel.leadingAnchor constraintGreaterThanOrEqualToAnchor:self.statusTitleLabel.trailingAnchor constant:8],
        [self.updatedLabel.trailingAnchor constraintEqualToAnchor:topRow.trailingAnchor],
        [self.updatedLabel.centerYAnchor constraintEqualToAnchor:topRow.centerYAnchor],
        [topRow.heightAnchor constraintEqualToConstant:24]
    ]];

    UIView *div1 = [UIView new];
    div1.translatesAutoresizingMaskIntoConstraints = NO;
    div1.backgroundColor = UIColor.separatorColor;

    UILabel *pLabel = nil;
    UIView *col1 = [self createColumnWithTitle:@"保护中进程" numberLabel:&pLabel tintColor:UIColor.systemGreenColor hasChevron:YES];
    self.protectedCountLabel = pLabel;
    self.protectedColumn = col1;

    UITapGestureRecognizer *tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(protectedColumnTapped)];
    [col1 addGestureRecognizer:tap];
    col1.userInteractionEnabled = YES;
    col1.isAccessibilityElement = YES;
    col1.accessibilityTraits = UIAccessibilityTraitButton;
    col1.accessibilityLabel = @"保护中进程";
    col1.accessibilityHint = @"点击查看受保护的 App 名单";

    UILabel *dLabel = nil;
    UIView *col2 = [self createColumnWithTitle:@"降级中进程" numberLabel:&dLabel tintColor:UIColor.systemOrangeColor hasChevron:NO];
    self.demotedCountLabel = dLabel;

    UILabel *rLabel = nil;
    UIView *col3 = [self createColumnWithTitle:@"累计生效" numberLabel:&rLabel tintColor:UIColor.systemBlueColor hasChevron:NO];
    self.reassertCountLabel = rLabel;

    UIStackView *metricsStack = [[UIStackView alloc] initWithArrangedSubviews:@[col1, col2, col3]];
    metricsStack.translatesAutoresizingMaskIntoConstraints = NO;
    metricsStack.axis = UILayoutConstraintAxisHorizontal;
    metricsStack.distribution = UIStackViewDistributionFillEqually;
    metricsStack.alignment = UIStackViewAlignmentCenter;

    self.errorContainerView = [UIView new];
    self.errorContainerView.translatesAutoresizingMaskIntoConstraints = NO;
    self.errorContainerView.backgroundColor = [UIColor.systemRedColor colorWithAlphaComponent:0.08];
    self.errorContainerView.layer.cornerRadius = 8;
    self.errorContainerView.clipsToBounds = YES;
    self.errorContainerView.hidden = YES;

    self.failureCountLabel = [UILabel new];
    self.failureCountLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.failureCountLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightSemibold];
    self.failureCountLabel.textColor = UIColor.systemRedColor;

    self.failureDetailLabel = [UILabel new];
    self.failureDetailLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.failureDetailLabel.font = [UIFont systemFontOfSize:12 weight:UIFontWeightRegular];
    self.failureDetailLabel.textColor = UIColor.systemRedColor;
    self.failureDetailLabel.numberOfLines = 0;

    UIStackView *errorStack = [[UIStackView alloc] initWithArrangedSubviews:@[self.failureCountLabel, self.failureDetailLabel]];
    errorStack.translatesAutoresizingMaskIntoConstraints = NO;
    errorStack.axis = UILayoutConstraintAxisVertical;
    errorStack.spacing = 3;

    [self.errorContainerView addSubview:errorStack];
    [NSLayoutConstraint activateConstraints:@[
        [errorStack.topAnchor constraintEqualToAnchor:self.errorContainerView.topAnchor constant:8],
        [errorStack.bottomAnchor constraintEqualToAnchor:self.errorContainerView.bottomAnchor constant:-8],
        [errorStack.leadingAnchor constraintEqualToAnchor:self.errorContainerView.leadingAnchor constant:12],
        [errorStack.trailingAnchor constraintEqualToAnchor:self.errorContainerView.trailingAnchor constant:-12]
    ]];

    self.metricsAndErrorStack = [[UIStackView alloc] initWithArrangedSubviews:@[metricsStack, self.errorContainerView]];
    self.metricsAndErrorStack.translatesAutoresizingMaskIntoConstraints = NO;
    self.metricsAndErrorStack.axis = UILayoutConstraintAxisVertical;
    self.metricsAndErrorStack.spacing = 0;

    UIView *div2 = [UIView new];
    div2.translatesAutoresizingMaskIntoConstraints = NO;
    div2.backgroundColor = UIColor.separatorColor;

    self.killedStatsEntryButton = [UIButton buttonWithType:UIButtonTypeSystem];
    self.killedStatsEntryButton.translatesAutoresizingMaskIntoConstraints = NO;
    [self.killedStatsEntryButton setTitle:@"被杀统计读取中…" forState:UIControlStateNormal];
    [self.killedStatsEntryButton setTitleColor:UIColor.secondaryLabelColor forState:UIControlStateNormal];
    self.killedStatsEntryButton.titleLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightRegular];
    self.killedStatsEntryButton.contentHorizontalAlignment = UIControlContentHorizontalAlignmentCenter;
    [self.killedStatsEntryButton addTarget:self action:@selector(killedStatsEntryTapped) forControlEvents:UIControlEventTouchUpInside];

    UIView *div3 = [UIView new];
    div3.translatesAutoresizingMaskIntoConstraints = NO;
    div3.backgroundColor = UIColor.separatorColor;

    self.actionButton = [UIButton buttonWithType:UIButtonTypeSystem];
    self.actionButton.translatesAutoresizingMaskIntoConstraints = NO;
    self.actionButton.titleLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightSemibold];
    self.actionButton.layer.cornerRadius = 12;
    self.actionButton.layer.cornerCurve = kCACornerCurveContinuous;
    self.actionButton.clipsToBounds = YES;
    [self.actionButton addTarget:self action:@selector(actionButtonTapped) forControlEvents:UIControlEventTouchUpInside];

    [self addSubview:topRow];
    [self addSubview:div1];
    [self addSubview:self.metricsAndErrorStack];
    [self addSubview:div2];
    [self addSubview:self.killedStatsEntryButton];
    [self addSubview:div3];
    [self addSubview:self.actionButton];

    [NSLayoutConstraint activateConstraints:@[
        [topRow.topAnchor constraintEqualToAnchor:self.topAnchor constant:16],
        [topRow.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:16],
        [topRow.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-16],

        [div1.topAnchor constraintEqualToAnchor:topRow.bottomAnchor constant:12],
        [div1.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:16],
        [div1.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-16],
        [div1.heightAnchor constraintEqualToConstant:0.5],

        [self.metricsAndErrorStack.topAnchor constraintEqualToAnchor:div1.bottomAnchor constant:12],
        [self.metricsAndErrorStack.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:16],
        [self.metricsAndErrorStack.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-16],

        [div2.topAnchor constraintEqualToAnchor:self.metricsAndErrorStack.bottomAnchor constant:12],
        [div2.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:16],
        [div2.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-16],
        [div2.heightAnchor constraintEqualToConstant:0.5],

        [self.killedStatsEntryButton.topAnchor constraintEqualToAnchor:div2.bottomAnchor constant:6],
        [self.killedStatsEntryButton.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:16],
        [self.killedStatsEntryButton.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-16],
        [self.killedStatsEntryButton.heightAnchor constraintEqualToConstant:30],

        [div3.topAnchor constraintEqualToAnchor:self.killedStatsEntryButton.bottomAnchor constant:6],
        [div3.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:16],
        [div3.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-16],
        [div3.heightAnchor constraintEqualToConstant:0.5],

        [self.actionButton.topAnchor constraintEqualToAnchor:div3.bottomAnchor constant:12],
        [self.actionButton.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:16],
        [self.actionButton.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-16],
        [self.actionButton.bottomAnchor constraintEqualToAnchor:self.bottomAnchor constant:-16],
        [self.actionButton.heightAnchor constraintEqualToConstant:48]
    ]];
}

- (UIView *)createColumnWithTitle:(NSString *)title numberLabel:(UILabel **)labelOut tintColor:(UIColor *)tint hasChevron:(BOOL)hasChevron {
    UIView *box = [UIView new];
    box.translatesAutoresizingMaskIntoConstraints = NO;

    UILabel *num = [UILabel new];
    num.translatesAutoresizingMaskIntoConstraints = NO;
    num.font = [UIFont systemFontOfSize:24 weight:UIFontWeightBold];
    num.textColor = tint;
    num.textAlignment = NSTextAlignmentCenter;
    num.text = @"0";

    UILabel *txt = [UILabel new];
    txt.translatesAutoresizingMaskIntoConstraints = NO;
    txt.font = [UIFont systemFontOfSize:12 weight:UIFontWeightRegular];
    txt.textColor = UIColor.secondaryLabelColor;
    txt.textAlignment = NSTextAlignmentCenter;
    txt.text = title;

    [box addSubview:num];

    [NSLayoutConstraint activateConstraints:@[
        [num.topAnchor constraintEqualToAnchor:box.topAnchor],
        [num.leadingAnchor constraintEqualToAnchor:box.leadingAnchor],
        [num.trailingAnchor constraintEqualToAnchor:box.trailingAnchor],
    ]];

    if (hasChevron) {
        UIImageView *chev = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"chevron.right"]];
        chev.translatesAutoresizingMaskIntoConstraints = NO;
        chev.tintColor = UIColor.tertiaryLabelColor;
        chev.contentMode = UIViewContentModeScaleAspectFit;

        UIStackView *titleStack = [[UIStackView alloc] initWithArrangedSubviews:@[txt, chev]];
        titleStack.translatesAutoresizingMaskIntoConstraints = NO;
        titleStack.axis = UILayoutConstraintAxisHorizontal;
        titleStack.alignment = UIStackViewAlignmentCenter;
        titleStack.spacing = 2;
        titleStack.userInteractionEnabled = NO;

        [box addSubview:titleStack];

        [NSLayoutConstraint activateConstraints:@[
            [chev.widthAnchor constraintEqualToConstant:7],
            [chev.heightAnchor constraintEqualToConstant:10],

            [titleStack.topAnchor constraintEqualToAnchor:num.bottomAnchor constant:4],
            [titleStack.centerXAnchor constraintEqualToAnchor:box.centerXAnchor],
            [titleStack.bottomAnchor constraintEqualToAnchor:box.bottomAnchor],
            [titleStack.leadingAnchor constraintGreaterThanOrEqualToAnchor:box.leadingAnchor],
            [titleStack.trailingAnchor constraintLessThanOrEqualToAnchor:box.trailingAnchor]
        ]];
    } else {
        [box addSubview:txt];

        [NSLayoutConstraint activateConstraints:@[
            [txt.topAnchor constraintEqualToAnchor:num.bottomAnchor constant:4],
            [txt.leadingAnchor constraintEqualToAnchor:box.leadingAnchor],
            [txt.trailingAnchor constraintEqualToAnchor:box.trailingAnchor],
            [txt.bottomAnchor constraintEqualToAnchor:box.bottomAnchor]
        ]];
    }

    if (labelOut) *labelOut = num;
    return box;
}

- (void)protectedColumnTapped {
    [UIView animateWithDuration:0.1 animations:^{
        self.protectedColumn.alpha = 0.5;
    } completion:^(BOOL finished) {
        [UIView animateWithDuration:0.15 animations:^{
            self.protectedColumn.alpha = 1.0;
        }];
    }];
    if (self.onTapProtected) {
        self.onTapProtected();
    }
}

- (void)actionButtonTapped {
    if (self.onActionToggle) {
        self.onActionToggle();
    }
}

- (void)killedStatsEntryTapped {
    if (self.onTapKilledStats) {
        self.onTapKilledStats();
    }
}

- (void)updateKilledCount:(NSInteger)totalKills available:(BOOL)available {
    if (!available) {
        [self.killedStatsEntryButton setTitle:@"被杀统计不可用" forState:UIControlStateNormal];
        [self.killedStatsEntryButton setTitleColor:UIColor.secondaryLabelColor forState:UIControlStateNormal];
        self.killedStatsEntryButton.titleLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightRegular];
    } else if (totalKills == 0) {
        [self.killedStatsEntryButton setTitle:@"近 7 天无被杀 ›" forState:UIControlStateNormal];
        [self.killedStatsEntryButton setTitleColor:UIColor.secondaryLabelColor forState:UIControlStateNormal];
        self.killedStatsEntryButton.titleLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightRegular];
    } else {
        [self.killedStatsEntryButton setTitle:[NSString stringWithFormat:@"近 7 天被杀 %ld 次 ›", (long)totalKills] forState:UIControlStateNormal];
        [self.killedStatsEntryButton setTitleColor:UIColor.systemRedColor forState:UIControlStateNormal];
        self.killedStatsEntryButton.titleLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightMedium];
    }
}

- (void)updateKilledCount:(NSInteger)totalKills {
    [self updateKilledCount:totalKills available:YES];
}

- (void)updateWithStatus:(JPStatusInfo *)status isRunning:(BOOL)isRunning {
    self.statusDot.backgroundColor = isRunning ? UIColor.systemGreenColor : UIColor.systemGray3Color;
    self.statusTitleLabel.text = isRunning ? @"守护进程运行中" : @"守护进程已停止";

    if (isRunning) {
        if (status.updated.length > 0) {
            self.updatedLabel.text = [NSString stringWithFormat:@"更新于 %@", status.updated];
        } else {
            self.updatedLabel.text = @"等待数据…";
        }
        [self.actionButton setTitle:@" 停止守护进程" forState:UIControlStateNormal];
        [self.actionButton setImage:[UIImage systemImageNamed:@"stop.fill"] forState:UIControlStateNormal];
        self.actionButton.backgroundColor = [UIColor.systemRedColor colorWithAlphaComponent:0.12];
        [self.actionButton setTitleColor:UIColor.systemRedColor forState:UIControlStateNormal];
        self.actionButton.tintColor = UIColor.systemRedColor;
    } else {
        self.updatedLabel.text = @"未运行";
        [self.actionButton setTitle:@" 启动守护进程" forState:UIControlStateNormal];
        [self.actionButton setImage:[UIImage systemImageNamed:@"play.fill"] forState:UIControlStateNormal];
        self.actionButton.backgroundColor = UIColor.systemBlueColor;
        [self.actionButton setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
        self.actionButton.tintColor = UIColor.whiteColor;
    }

    self.protectedCountLabel.text = [NSString stringWithFormat:@"%ld", (long)status.protectedPids];
    self.demotedCountLabel.text = [NSString stringWithFormat:@"%ld", (long)status.demotedPids];
    self.reassertCountLabel.text = [NSString stringWithFormat:@"%lu", status.reassertsTotal];

    BOOL showFailure = (isRunning && status.hasSetFailTotal && status.setFailTotal > 0);
    if (showFailure) {
        self.errorContainerView.hidden = NO;
        self.metricsAndErrorStack.spacing = 10;
        self.failureCountLabel.text = [NSString stringWithFormat:@"提权失败 ×%lu", status.setFailTotal];

        if (status.hasLastError) {
            // daemon 直接给的错误文本优先（v0.5.1+）；旧版只有错误号时用本地映射兜底
            NSString *errText = status.lastErrorText.length > 0 ? status.lastErrorText : ErrnoName(status.lastErrorErrno);
            NSString *errnoStr = errText ? [NSString stringWithFormat:@"errno %d (%@)", status.lastErrorErrno, errText]
                                         : [NSString stringWithFormat:@"errno %d", status.lastErrorErrno];
            NSString *timeStr = FormatShortTimestamp(status.lastErrorTime);
            self.failureDetailLabel.text = [NSString stringWithFormat:@"最近：pid %d → 目标 %d · %@ · %@",
                                            (int)status.lastErrorPid, status.lastErrorTarget, errnoStr, timeStr];
            self.failureDetailLabel.hidden = NO;
        } else {
            self.failureDetailLabel.text = @"";
            self.failureDetailLabel.hidden = YES;
        }
    } else {
        self.errorContainerView.hidden = YES;
        self.metricsAndErrorStack.spacing = 0;
    }
}

@end

#pragma mark - Views: Killed Stats Card

@interface JPKilledStatsCardView : UIView
@property (nonatomic, strong) UILabel *titleLabel;
@property (nonatomic, strong) UILabel *subtitleLabel;
@property (nonatomic, strong) UIView *div1;
@property (nonatomic, strong) UILabel *summaryLabel;
@property (nonatomic, strong) UIView *div2;
@property (nonatomic, strong) UIStackView *rowsStackView;
@property (nonatomic, strong) UILabel *footnoteLabel;
@property (nonatomic, strong) UIStackView *bodyStackView;

- (void)updateWithStats:(JPKilledStatsInfo *)stats;
@end

@implementation JPKilledStatsCardView

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        [self setupUI];
    }
    return self;
}

- (void)setupUI {
    self.backgroundColor = UIColor.secondarySystemGroupedBackgroundColor;
    self.layer.cornerRadius = 14;
    self.layer.cornerCurve = kCACornerCurveContinuous;
    self.clipsToBounds = YES;
    self.translatesAutoresizingMaskIntoConstraints = NO;

    self.titleLabel = [UILabel new];
    self.titleLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.titleLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightSemibold];
    self.titleLabel.textColor = UIColor.labelColor;
    self.titleLabel.text = @"被杀统计";

    self.subtitleLabel = [UILabel new];
    self.subtitleLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.subtitleLabel.font = [UIFont systemFontOfSize:12 weight:UIFontWeightRegular];
    self.subtitleLabel.textColor = UIColor.secondaryLabelColor;
    self.subtitleLabel.textAlignment = NSTextAlignmentRight;
    self.subtitleLabel.text = @"最近 7 天 · 系统 Jetsam 日志";

    UIView *topRow = [UIView new];
    topRow.translatesAutoresizingMaskIntoConstraints = NO;
    [topRow addSubview:self.titleLabel];
    [topRow addSubview:self.subtitleLabel];

    [NSLayoutConstraint activateConstraints:@[
        [self.titleLabel.leadingAnchor constraintEqualToAnchor:topRow.leadingAnchor],
        [self.titleLabel.centerYAnchor constraintEqualToAnchor:topRow.centerYAnchor],

        [self.subtitleLabel.leadingAnchor constraintGreaterThanOrEqualToAnchor:self.titleLabel.trailingAnchor constant:8],
        [self.subtitleLabel.trailingAnchor constraintEqualToAnchor:topRow.trailingAnchor],
        [self.subtitleLabel.centerYAnchor constraintEqualToAnchor:topRow.centerYAnchor],
        [topRow.heightAnchor constraintEqualToConstant:24]
    ]];

    self.div1 = [UIView new];
    self.div1.translatesAutoresizingMaskIntoConstraints = NO;
    self.div1.backgroundColor = UIColor.separatorColor;

    self.summaryLabel = [UILabel new];
    self.summaryLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.summaryLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
    self.summaryLabel.textColor = UIColor.secondaryLabelColor;
    self.summaryLabel.text = @"被杀统计读取中…";

    self.div2 = [UIView new];
    self.div2.translatesAutoresizingMaskIntoConstraints = NO;
    self.div2.backgroundColor = UIColor.separatorColor;
    self.div2.hidden = YES;

    [NSLayoutConstraint activateConstraints:@[
        [self.div2.heightAnchor constraintEqualToConstant:0.5]
    ]];

    self.rowsStackView = [UIStackView new];
    self.rowsStackView.translatesAutoresizingMaskIntoConstraints = NO;
    self.rowsStackView.axis = UILayoutConstraintAxisVertical;
    self.rowsStackView.spacing = 10;
    self.rowsStackView.hidden = YES;

    self.footnoteLabel = [UILabel new];
    self.footnoteLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.footnoteLabel.font = [UIFont systemFontOfSize:11 weight:UIFontWeightRegular];
    self.footnoteLabel.textColor = UIColor.tertiaryLabelColor;
    self.footnoteLabel.textAlignment = NSTextAlignmentCenter;
    self.footnoteLabel.hidden = YES;

    self.bodyStackView = [[UIStackView alloc] initWithArrangedSubviews:@[self.summaryLabel, self.div2, self.rowsStackView, self.footnoteLabel]];
    self.bodyStackView.translatesAutoresizingMaskIntoConstraints = NO;
    self.bodyStackView.axis = UILayoutConstraintAxisVertical;
    self.bodyStackView.spacing = 10;

    [self addSubview:topRow];
    [self addSubview:self.div1];
    [self addSubview:self.bodyStackView];

    [NSLayoutConstraint activateConstraints:@[
        [topRow.topAnchor constraintEqualToAnchor:self.topAnchor constant:16],
        [topRow.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:16],
        [topRow.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-16],

        [self.div1.topAnchor constraintEqualToAnchor:topRow.bottomAnchor constant:12],
        [self.div1.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:16],
        [self.div1.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-16],
        [self.div1.heightAnchor constraintEqualToConstant:0.5],

        [self.bodyStackView.topAnchor constraintEqualToAnchor:self.div1.bottomAnchor constant:12],
        [self.bodyStackView.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:16],
        [self.bodyStackView.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-16],
        [self.bodyStackView.bottomAnchor constraintEqualToAnchor:self.bottomAnchor constant:-16]
    ]];
}

- (UIView *)createRowViewForSummary:(JPJetsamDaySummary *)summary {
    UIView *row = [UIView new];
    row.translatesAutoresizingMaskIntoConstraints = NO;

    UILabel *line1 = [UILabel new];
    line1.translatesAutoresizingMaskIntoConstraints = NO;
    line1.font = [UIFont systemFontOfSize:14 weight:UIFontWeightMedium];
    line1.textColor = UIColor.labelColor;
    line1.text = [NSString stringWithFormat:@"%@　%@　×%ld", summary.dateFormatted, summary.displayName, (long)summary.killCount];

    UILabel *line2 = [UILabel new];
    line2.translatesAutoresizingMaskIntoConstraints = NO;
    line2.font = [UIFont systemFontOfSize:12 weight:UIFontWeightRegular];
    line2.textColor = UIColor.secondaryLabelColor;
    line2.numberOfLines = 1;
    line2.lineBreakMode = NSLineBreakByTruncatingTail;
    line2.text = [NSString stringWithFormat:@"主要原因：%@", summary.primaryReason];

    [row addSubview:line1];
    [row addSubview:line2];

    [NSLayoutConstraint activateConstraints:@[
        [line1.topAnchor constraintEqualToAnchor:row.topAnchor],
        [line1.leadingAnchor constraintEqualToAnchor:row.leadingAnchor],
        [line1.trailingAnchor constraintEqualToAnchor:row.trailingAnchor],

        [line2.topAnchor constraintEqualToAnchor:line1.bottomAnchor constant:3],
        [line2.leadingAnchor constraintEqualToAnchor:row.leadingAnchor],
        [line2.trailingAnchor constraintEqualToAnchor:row.trailingAnchor],
        [line2.bottomAnchor constraintEqualToAnchor:row.bottomAnchor]
    ]];

    return row;
}

- (void)updateWithStats:(JPKilledStatsInfo *)stats {
    for (UIView *v in self.rowsStackView.arrangedSubviews) {
        [self.rowsStackView removeArrangedSubview:v];
        [v removeFromSuperview];
    }

    if (!stats.available) {
        self.summaryLabel.textColor = UIColor.secondaryLabelColor;
        self.summaryLabel.text = @"被杀统计不可用";
        self.div2.hidden = YES;
        self.rowsStackView.hidden = YES;
    } else if (stats.totalKills == 0) {
        self.summaryLabel.textColor = UIColor.systemGreenColor;
        self.summaryLabel.text = @"无被杀记录";
        self.div2.hidden = YES;
        self.rowsStackView.hidden = YES;
    } else {
        self.summaryLabel.textColor = UIColor.labelColor;
        self.summaryLabel.text = [NSString stringWithFormat:@"共被杀 %ld 次", (long)stats.totalKills];
        self.div2.hidden = NO;
        self.rowsStackView.hidden = NO;

        for (JPJetsamDaySummary *s in stats.daySummaries) {
            UIView *row = [self createRowViewForSummary:s];
            [self.rowsStackView addArrangedSubview:row];
        }
    }

    if (stats.skippedFilesCount > 0) {
        self.footnoteLabel.text = [NSString stringWithFormat:@"跳过 %ld 个无法解析的日志文件", (long)stats.skippedFilesCount];
        self.footnoteLabel.hidden = NO;
    } else {
        self.footnoteLabel.text = @"";
        self.footnoteLabel.hidden = YES;
    }
}

@end

#pragma mark - Views: Rule Card

@class JPRuleCardView;

@protocol JPRuleCardViewDelegate <NSObject>
@optional
- (void)ruleCardViewDidRequestDelete:(JPRuleCardView *)card;
- (void)ruleCardViewDidTap:(JPRuleCardView *)card;
@end

@interface JPRuleCardView : UIView
@property (nonatomic, strong) JPRule *rule;
@property (nonatomic, weak) id<JPRuleCardViewDelegate> delegate;
@property (nonatomic, copy) NSArray<JPInstalledApp *> *installedApps;
@property (nonatomic, strong) UIImageView *iconImageView;
@property (nonatomic, strong) UILabel *titleLabel;
@property (nonatomic, strong) UILabel *pathLabel;
@property (nonatomic, strong) UILabel *statusLabel;
@property (nonatomic, strong) UILabel *bandLabel;
@property (nonatomic, strong) UIButton *deleteButton;

- (instancetype)initWithRule:(JPRule *)rule delegate:(id<JPRuleCardViewDelegate>)delegate;
- (instancetype)initWithRule:(JPRule *)rule delegate:(id<JPRuleCardViewDelegate>)delegate installedApps:(NSArray<JPInstalledApp *> *)installedApps;
- (void)updateStatusWithInfo:(JPStatusInfo *)status daemonRunning:(BOOL)daemonRunning;
@end

@implementation JPRuleCardView

- (instancetype)initWithRule:(JPRule *)rule delegate:(id<JPRuleCardViewDelegate>)delegate {
    return [self initWithRule:rule delegate:delegate installedApps:nil];
}

- (instancetype)initWithRule:(JPRule *)rule delegate:(id<JPRuleCardViewDelegate>)delegate installedApps:(NSArray<JPInstalledApp *> *)installedApps {
    self = [super initWithFrame:CGRectZero];
    if (self) {
        _rule = rule;
        _delegate = delegate;
        _installedApps = [installedApps copy];
        [self setupUI];
    }
    return self;
}

- (void)setupUI {
    self.backgroundColor = UIColor.secondarySystemGroupedBackgroundColor;
    self.layer.cornerRadius = 14;
    self.layer.cornerCurve = kCACornerCurveContinuous;
    self.clipsToBounds = YES;
    self.translatesAutoresizingMaskIntoConstraints = NO;

    self.iconImageView = [UIImageView new];
    self.iconImageView.translatesAutoresizingMaskIntoConstraints = NO;
    self.iconImageView.layer.cornerRadius = 8;
    self.iconImageView.layer.cornerCurve = kCACornerCurveContinuous;
    self.iconImageView.clipsToBounds = YES;
    self.iconImageView.contentMode = UIViewContentModeScaleAspectFit;
    self.iconImageView.tintColor = UIColor.systemBlueColor;
    [self addSubview:self.iconImageView];

    self.titleLabel = [UILabel new];
    self.titleLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.titleLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightSemibold];
    self.titleLabel.textColor = UIColor.labelColor;
    self.titleLabel.text = self.rule.displayName.length > 0 ? self.rule.displayName : self.rule.pathFragment;
    [self addSubview:self.titleLabel];

    self.pathLabel = [UILabel new];
    self.pathLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.pathLabel.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
    self.pathLabel.textColor = UIColor.secondaryLabelColor;
    self.pathLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
    self.pathLabel.text = self.rule.pathFragment;
    [self addSubview:self.pathLabel];

    self.statusLabel = [UILabel new];
    self.statusLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.statusLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightRegular];
    self.statusLabel.textColor = UIColor.secondaryLabelColor;
    self.statusLabel.numberOfLines = 1;
    [self addSubview:self.statusLabel];

    self.bandLabel = [UILabel new];
    self.bandLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.bandLabel.font = [UIFont systemFontOfSize:17 weight:UIFontWeightBold];
    self.bandLabel.textAlignment = NSTextAlignmentRight;
    [self addSubview:self.bandLabel];

    self.deleteButton = [UIButton buttonWithType:UIButtonTypeSystem];
    self.deleteButton.translatesAutoresizingMaskIntoConstraints = NO;
    [self.deleteButton setImage:[UIImage systemImageNamed:@"trash"] forState:UIControlStateNormal];
    self.deleteButton.tintColor = UIColor.systemRedColor;
    [self.deleteButton addTarget:self action:@selector(deleteTapped) forControlEvents:UIControlEventTouchUpInside];
    [self addSubview:self.deleteButton];

    UIImageView *chevron = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"chevron.right"]];
    chevron.translatesAutoresizingMaskIntoConstraints = NO;
    chevron.tintColor = UIColor.tertiaryLabelColor;
    chevron.contentMode = UIViewContentModeScaleAspectFit;
    [self addSubview:chevron];

    [NSLayoutConstraint activateConstraints:@[
        [self.iconImageView.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:14],
        [self.iconImageView.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [self.iconImageView.widthAnchor constraintEqualToConstant:40],
        [self.iconImageView.heightAnchor constraintEqualToConstant:40],

        [chevron.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-14],
        [chevron.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [chevron.widthAnchor constraintEqualToConstant:8],
        [chevron.heightAnchor constraintEqualToConstant:13],

        [self.deleteButton.trailingAnchor constraintEqualToAnchor:chevron.leadingAnchor constant:-4],
        [self.deleteButton.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [self.deleteButton.widthAnchor constraintEqualToConstant:32],
        [self.deleteButton.heightAnchor constraintEqualToConstant:32],

        [self.bandLabel.trailingAnchor constraintEqualToAnchor:self.deleteButton.leadingAnchor constant:-6],
        [self.bandLabel.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [self.bandLabel.widthAnchor constraintGreaterThanOrEqualToConstant:36],

        [self.titleLabel.topAnchor constraintEqualToAnchor:self.topAnchor constant:12],
        [self.titleLabel.leadingAnchor constraintEqualToAnchor:self.iconImageView.trailingAnchor constant:12],
        [self.titleLabel.trailingAnchor constraintLessThanOrEqualToAnchor:self.bandLabel.leadingAnchor constant:-8],

        [self.pathLabel.topAnchor constraintEqualToAnchor:self.titleLabel.bottomAnchor constant:2],
        [self.pathLabel.leadingAnchor constraintEqualToAnchor:self.iconImageView.trailingAnchor constant:12],
        [self.pathLabel.trailingAnchor constraintLessThanOrEqualToAnchor:self.bandLabel.leadingAnchor constant:-8],

        [self.statusLabel.topAnchor constraintEqualToAnchor:self.pathLabel.bottomAnchor constant:3],
        [self.statusLabel.leadingAnchor constraintEqualToAnchor:self.iconImageView.trailingAnchor constant:12],
        [self.statusLabel.trailingAnchor constraintLessThanOrEqualToAnchor:self.bandLabel.leadingAnchor constant:-8],
        [self.statusLabel.bottomAnchor constraintEqualToAnchor:self.bottomAnchor constant:-12]
    ]];

    UITapGestureRecognizer *tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(cardTapped:)];
    [self addGestureRecognizer:tap];
}

- (void)cardTapped:(UITapGestureRecognizer *)gesture {
    CGPoint pt = [gesture locationInView:self];
    if (CGRectContainsPoint(self.deleteButton.frame, pt)) return;
    if ([self.delegate respondsToSelector:@selector(ruleCardViewDidTap:)]) {
        [self.delegate ruleCardViewDidTap:self];
    }
}

- (void)updateStatusWithInfo:(JPStatusInfo *)status daemonRunning:(BOOL)daemonRunning {
    self.iconImageView.image = JPIconForFragment(self.rule.pathFragment, self.installedApps);
    BOOL installed = JPIsAppInstalled(self.rule.pathFragment, self.installedApps);

    if (self.rule.type == JPRuleTypeProtect) {
        self.bandLabel.text = [NSString stringWithFormat:@"%ld", (long)self.rule.targetBand];
        self.bandLabel.textColor = UIColor.systemBlueColor;
        self.pathLabel.text = self.rule.pathFragment;
    } else {
        NSInteger band = self.rule.targetBand;
        if (band < 0) band = 0;
        if (band > 30) band = 30;
        self.bandLabel.text = [NSString stringWithFormat:@"%ld", (long)band];
        self.bandLabel.textColor = UIColor.systemOrangeColor;
        self.pathLabel.text = [NSString stringWithFormat:@"%@ · 不在前台时降到 %ld", self.rule.pathFragment, (long)band];
    }

    if (!installed) {
        self.statusLabel.text = @"未安装";
        self.statusLabel.textColor = UIColor.secondaryLabelColor;
        self.alpha = 0.65;
        return;
    }
    self.alpha = 1.0;

    if (self.rule.isDuplicate) {
        self.statusLabel.text = @"重复，只有第一条生效";
        self.statusLabel.textColor = UIColor.systemRedColor;
        return;
    }
    if (self.rule.isConflictWithDemote) {
        self.statusLabel.text = @"与降级规则冲突";
        self.statusLabel.textColor = UIColor.systemRedColor;
        return;
    }
    if (self.rule.isConflict) {
        NSString *names = [self.rule.conflictAppNames componentsJoinedByString:@"、"] ?: @"";
        self.statusLabel.text = [NSString stringWithFormat:@"命中多个 App：%@，点此重选", names];
        self.statusLabel.textColor = UIColor.systemOrangeColor;
        return;
    }

    if (!daemonRunning) {
        self.statusLabel.text = @"未运行（守护进程未运行）";
        self.statusLabel.textColor = UIColor.secondaryLabelColor;
        return;
    }

    if (self.rule.type == JPRuleTypeDemote) {
        NSInteger band = self.rule.targetBand;
        if (band < 0) band = 0;
        if (band > 30) band = 30;

        if (!status || !status.hasDemotedAppLines) {
            self.statusLabel.text = [NSString stringWithFormat:@"不在前台时降到 %ld", (long)band];
            self.statusLabel.textColor = UIColor.secondaryLabelColor;
            return;
        }

        NSMutableArray<JPProtectedApp *> *matchedDemoted = [NSMutableArray array];
        for (JPProtectedApp *pApp in status.demotedApps) {
            if ([self.rule.pathFragment hasPrefix:@"bid:"]) {
                NSString *bid = [self.rule.pathFragment substringFromIndex:4];
                if ([pApp.bundleId isEqualToString:bid] || [pApp.pathFragment isEqualToString:self.rule.pathFragment]) {
                    [matchedDemoted addObject:pApp];
                }
            } else {
                if ([pApp.pathFragment isEqualToString:self.rule.pathFragment]) {
                    [matchedDemoted addObject:pApp];
                }
            }
        }

        if (matchedDemoted.count == 1) {
            JPProtectedApp *pApp = matchedDemoted.firstObject;
            if (pApp.currentBand <= pApp.targetBand) {
                self.statusLabel.text = [NSString stringWithFormat:@"运行中 · pid %d · 当前 %ld 档", pApp.pid, (long)pApp.currentBand];
                self.statusLabel.textColor = UIColor.systemGreenColor;
            } else {
                self.statusLabel.text = [NSString stringWithFormat:@"运行中 · pid %d · 前台中，不降", pApp.pid];
                self.statusLabel.textColor = UIColor.systemOrangeColor;
            }
        } else if (matchedDemoted.count > 1) {
            self.statusLabel.text = [NSString stringWithFormat:@"运行中 · %lu 个进程", (unsigned long)matchedDemoted.count];
            self.statusLabel.textColor = UIColor.systemGreenColor;
        } else {
            self.statusLabel.text = @"App 没在运行";
            self.statusLabel.textColor = UIColor.secondaryLabelColor;
        }
        return;
    }

    NSMutableArray<NSNumber *> *matchedPids = [NSMutableArray array];
    if (status) {
        for (JPProtectedApp *pApp in status.protectedApps) {
            if ([self.rule.pathFragment hasPrefix:@"bid:"]) {
                NSString *bid = [self.rule.pathFragment substringFromIndex:4];
                if ([pApp.bundleId isEqualToString:bid] || [pApp.pathFragment isEqualToString:self.rule.pathFragment]) {
                    [matchedPids addObject:@(pApp.pid)];
                }
            } else {
                if ([pApp.pathFragment isEqualToString:self.rule.pathFragment]) {
                    [matchedPids addObject:@(pApp.pid)];
                }
            }
        }
    }

    NSString *sameBandHint = (self.rule.hasSameBand && self.rule.type == JPRuleTypeProtect) ? @" · 同档：内存大的先被杀" : @"";

    if (matchedPids.count == 1) {
        self.statusLabel.text = [NSString stringWithFormat:@"运行中 · pid %d%@", [matchedPids.firstObject intValue], sameBandHint];
        self.statusLabel.textColor = UIColor.systemGreenColor;
    } else if (matchedPids.count > 1) {
        self.statusLabel.text = [NSString stringWithFormat:@"运行中 · %lu 个进程%@", (unsigned long)matchedPids.count, sameBandHint];
        self.statusLabel.textColor = UIColor.systemGreenColor;
    } else {
        self.statusLabel.text = [NSString stringWithFormat:@"App 没在运行%@", sameBandHint];
        self.statusLabel.textColor = UIColor.secondaryLabelColor;
    }
}

- (void)deleteTapped {
    if ([self.delegate respondsToSelector:@selector(ruleCardViewDidRequestDelete:)]) {
        [self.delegate ruleCardViewDidRequestDelete:self];
    }
}

@end


#pragma mark - Controllers: App Picker Sheet

@interface JPAppPickerViewController : UIViewController <UITableViewDataSource, UITableViewDelegate, UISearchResultsUpdating>
@property (nonatomic, copy) NSArray<JPInstalledApp *> *allApps;
@property (nonatomic, strong) NSMutableArray<JPInstalledApp *> *filteredApps;
@property (nonatomic, copy) NSArray<JPRule *> *existingRules;
@property (nonatomic, assign) BOOL userAppsOnly;
@property (nonatomic, assign) BOOL isRestartPicker;
@property (nonatomic, assign) BOOL isPulsePicker;
@property (nonatomic, copy) BOOL (^isAppExistingBlock)(JPInstalledApp *app);
@property (nonatomic, strong) UITableView *tableView;
@property (nonatomic, strong) UISearchController *searchController;
@property (nonatomic, strong) UIView *emptyView;
@property (nonatomic, strong) JPBandsSnapshot *bandsSnapshot;
@property (nonatomic, copy) void (^onSelectApp)(NSString *pathFragment, NSString *displayName);
@property (nonatomic, copy) void (^onSelectRestartBid)(NSString *bid, NSString *displayName);
@property (nonatomic, copy) void (^onSelectPulseBid)(NSString *bid, NSString *displayName);
@property (nonatomic, copy) void (^onManualAdd)(NSString *pathFragment, JPRuleType type, NSInteger band);
@end

@implementation JPAppPickerViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"添加规则";
    self.view.backgroundColor = UIColor.systemGroupedBackgroundColor;

    if (self.isPulsePicker) {
        NSMutableArray<JPInstalledApp *> *validApps = [NSMutableArray array];
        for (JPInstalledApp *app in self.allApps) {
            if (app.bundleId.length > 0 &&
                !JPIsRestartBlockedBundleId(app.bundleId) &&
                ![app.bundleId isEqualToString:@"com.apple.mobilesafari"]) {
                [validApps addObject:app];
            }
        }
        self.filteredApps = validApps;
        self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"手动输入"
                                                                                 style:UIBarButtonItemStylePlain
                                                                                target:self
                                                                                action:@selector(showManualPulseBidInputDialog)];
    } else if (self.isRestartPicker) {
        NSMutableArray<JPInstalledApp *> *validApps = [NSMutableArray array];
        for (JPInstalledApp *app in self.allApps) {
            if (app.bundleId.length > 0 && !JPIsRestartBlockedBundleId(app.bundleId)) {
                [validApps addObject:app];
            }
        }
        self.filteredApps = validApps;
        self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"手动输入"
                                                                                 style:UIBarButtonItemStylePlain
                                                                                target:self
                                                                                action:@selector(showManualRestartBidInputDialog)];
    } else {
        self.filteredApps = [NSMutableArray arrayWithArray:self.allApps];
        if (!self.userAppsOnly) {
            self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"手动输入"
                                                                                     style:UIBarButtonItemStylePlain
                                                                                    target:self
                                                                                    action:@selector(showManualInputDialog)];
        }
    }
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"取消"
                                                                              style:UIBarButtonItemStylePlain
                                                                             target:self
                                                                             action:@selector(cancelTapped)];

    self.searchController = [[UISearchController alloc] initWithSearchResultsController:nil];
    self.searchController.searchResultsUpdater = self;
    self.searchController.obscuresBackgroundDuringPresentation = NO;
    self.searchController.searchBar.placeholder = @"搜索 App 名称或路径";
    self.navigationItem.searchController = self.searchController;
    self.navigationItem.hidesSearchBarWhenScrolling = NO;
    self.definesPresentationContext = YES;

    self.tableView = [[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStyleInsetGrouped];
    self.tableView.translatesAutoresizingMaskIntoConstraints = NO;
    self.tableView.dataSource = self;
    self.tableView.delegate = self;
    [self.view addSubview:self.tableView];

    [NSLayoutConstraint activateConstraints:@[
        [self.tableView.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [self.tableView.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [self.tableView.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [self.tableView.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor]
    ]];

    [self setupEmptyView];
    [self updateEmptyState];

    __weak typeof(self) weakSelf = self;
    [JPBandsSnapshot requestSnapshotWithCompletion:^(JPBandsSnapshot *snapshot) {
        __strong typeof(weakSelf) strongSelf = weakSelf;
        if (!strongSelf) return;
        strongSelf.bandsSnapshot = snapshot;
        [strongSelf updateFooterView];
        [strongSelf.tableView reloadData];
    }];
    [self updateFooterView];
}

- (void)updateFooterView {
    UILabel *footer = [[UILabel alloc] initWithFrame:CGRectMake(0, 0, self.view.bounds.size.width, 56)];
    footer.font = [UIFont systemFontOfSize:12];
    footer.textColor = UIColor.secondaryLabelColor;
    footer.textAlignment = NSTextAlignmentCenter;
    footer.numberOfLines = 0;
    if (self.bandsSnapshot.timedOut) {
        footer.text = @"档位快照：守护进程未响应";
        footer.textColor = UIColor.systemOrangeColor;
    } else if (self.bandsSnapshot) {
        footer.text = @"档位会随状态变化（挂起 0、刚切后台 30–40、收推送时会被拉高），这里只是此刻的值。原档位未知 = 守护进程重启前就接管了。";
    } else {
        footer.text = @"正在获取进程档位快照…";
    }
    self.tableView.tableFooterView = footer;
}

- (void)setupEmptyView {
    self.emptyView = [UIView new];
    self.emptyView.translatesAutoresizingMaskIntoConstraints = NO;
    self.emptyView.hidden = YES;

    UIImageView *icon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"app.dashed"]];
    icon.translatesAutoresizingMaskIntoConstraints = NO;
    icon.tintColor = UIColor.tertiaryLabelColor;

    UILabel *tip = [UILabel new];
    tip.translatesAutoresizingMaskIntoConstraints = NO;
    tip.font = [UIFont systemFontOfSize:14 weight:UIFontWeightRegular];
    tip.textColor = UIColor.secondaryLabelColor;
    tip.textAlignment = NSTextAlignmentCenter;
    tip.numberOfLines = 0;
    tip.text = @"未在 /var/containers/Bundle/Application 扫描到已安装 App\n可通过左上角「手动输入」直接指定路径片段";

    UIButton *manualBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    manualBtn.translatesAutoresizingMaskIntoConstraints = NO;
    if (self.isPulsePicker) {
        [manualBtn setTitle:@"手动输入 Bundle ID" forState:UIControlStateNormal];
        [manualBtn addTarget:self action:@selector(showManualPulseBidInputDialog) forControlEvents:UIControlEventTouchUpInside];
    } else if (self.isRestartPicker) {
        [manualBtn setTitle:@"手动输入 Bundle ID" forState:UIControlStateNormal];
        [manualBtn addTarget:self action:@selector(showManualRestartBidInputDialog) forControlEvents:UIControlEventTouchUpInside];
    } else {
        [manualBtn setTitle:@"手动输入路径片段" forState:UIControlStateNormal];
        [manualBtn addTarget:self action:@selector(showManualInputDialog) forControlEvents:UIControlEventTouchUpInside];
    }
    manualBtn.titleLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightMedium];

    [self.emptyView addSubview:icon];
    [self.emptyView addSubview:tip];
    if (!self.userAppsOnly || self.isRestartPicker || self.isPulsePicker) {
        [self.emptyView addSubview:manualBtn];
    }

    [NSLayoutConstraint activateConstraints:@[
        [icon.centerXAnchor constraintEqualToAnchor:self.emptyView.centerXAnchor],
        [icon.topAnchor constraintEqualToAnchor:self.emptyView.topAnchor],
        [icon.widthAnchor constraintEqualToConstant:44],
        [icon.heightAnchor constraintEqualToConstant:44],

        [tip.topAnchor constraintEqualToAnchor:icon.bottomAnchor constant:12],
        [tip.leadingAnchor constraintEqualToAnchor:self.emptyView.leadingAnchor constant:24],
        [tip.trailingAnchor constraintEqualToAnchor:self.emptyView.trailingAnchor constant:-24],
    ]];

    if (!self.userAppsOnly || self.isRestartPicker) {
        [NSLayoutConstraint activateConstraints:@[
            [manualBtn.topAnchor constraintEqualToAnchor:tip.bottomAnchor constant:16],
            [manualBtn.centerXAnchor constraintEqualToAnchor:self.emptyView.centerXAnchor],
            [manualBtn.bottomAnchor constraintEqualToAnchor:self.emptyView.bottomAnchor]
        ]];
    } else {
        [tip.bottomAnchor constraintEqualToAnchor:self.emptyView.bottomAnchor].active = YES;
    }

    [self.view addSubview:self.emptyView];
    [NSLayoutConstraint activateConstraints:@[
        [self.emptyView.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [self.emptyView.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor]
    ]];
}

- (void)updateEmptyState {
    BOOL noAppsAtAll = (self.allApps.count == 0);
    self.emptyView.hidden = !noAppsAtAll;
    self.tableView.hidden = noAppsAtAll;
}

- (void)jp_dismissThen:(void (^)(void))then {
    self.searchController.active = NO;
    UIViewController *presenter = self.navigationController.presentingViewController;
    if (presenter) {
        [presenter dismissViewControllerAnimated:YES completion:then];
    } else {
        [self dismissViewControllerAnimated:YES completion:then];
    }
}

- (void)cancelTapped {
    [self jp_dismissThen:nil];
}

- (void)updateSearchResultsForSearchController:(UISearchController *)searchController {
    NSString *term = [searchController.searchBar.text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (term.length == 0) {
        if (self.isPulsePicker) {
            NSMutableArray<JPInstalledApp *> *validApps = [NSMutableArray array];
            for (JPInstalledApp *app in self.allApps) {
                if (app.bundleId.length > 0 &&
                    !JPIsRestartBlockedBundleId(app.bundleId) &&
                    ![app.bundleId isEqualToString:@"com.apple.mobilesafari"]) {
                    [validApps addObject:app];
                }
            }
            self.filteredApps = validApps;
        } else if (self.isRestartPicker) {
            NSMutableArray<JPInstalledApp *> *validApps = [NSMutableArray array];
            for (JPInstalledApp *app in self.allApps) {
                if (app.bundleId.length > 0 && !JPIsRestartBlockedBundleId(app.bundleId)) {
                    [validApps addObject:app];
                }
            }
            self.filteredApps = validApps;
        } else {
            self.filteredApps = [NSMutableArray arrayWithArray:self.allApps];
        }
    } else {
        [self.filteredApps removeAllObjects];
        for (JPInstalledApp *app in self.allApps) {
            if (self.isPulsePicker) {
                if (!app.bundleId || app.bundleId.length == 0 ||
                    JPIsRestartBlockedBundleId(app.bundleId) ||
                    [app.bundleId isEqualToString:@"com.apple.mobilesafari"]) {
                    continue;
                }
            } else if (self.isRestartPicker) {
                if (!app.bundleId || app.bundleId.length == 0 || JPIsRestartBlockedBundleId(app.bundleId)) {
                    continue;
                }
            }
            if ([app.displayName localizedCaseInsensitiveContainsString:term] ||
                [app.bundleName localizedCaseInsensitiveContainsString:term] ||
                [app.bundleId localizedCaseInsensitiveContainsString:term] ||
                [app.pathFragment localizedCaseInsensitiveContainsString:term]) {
                [self.filteredApps addObject:app];
            }
        }
    }
    [self.tableView reloadData];
}

- (BOOL)isAppAlreadyAdded:(JPInstalledApp *)app {
    if (self.isAppExistingBlock) {
        return self.isAppExistingBlock(app);
    }
    for (JPRule *r in self.existingRules) {
        if ([r.pathFragment hasPrefix:@"bid:"]) {
            NSString *ruleBid = [r.pathFragment substringFromIndex:4];
            if (app.bundleId.length > 0 && [app.bundleId isEqualToString:ruleBid]) {
                return YES;
            }
        } else {
            if ([r.pathFragment isEqualToString:app.pathFragment] ||
                [r.pathFragment containsString:app.bundleName] ||
                [app.bundleName containsString:r.pathFragment]) {
                return YES;
            }
        }
    }
    return NO;
}

#pragma mark - Table View

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    return self.filteredApps.count;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    static NSString *const kCellId = @"AppCell";
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:kCellId];
    if (!cell) {
        cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:kCellId];
    }

    JPInstalledApp *app = self.filteredApps[indexPath.row];
    cell.textLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightMedium];
    cell.textLabel.text = app.displayName;
    cell.textLabel.numberOfLines = 0;
    cell.detailTextLabel.numberOfLines = 0;

    JPBandsProcess *matchedProc = nil;
    if (self.bandsSnapshot && app.bundleId.length > 0) {
        matchedProc = [self.bandsSnapshot bestProcessForBundleId:app.bundleId];
    }

    NSString *procDesc = matchedProc ? [matchedProc bandDescription] : nil;

    BOOL added = [self isAppAlreadyAdded:app];
    if (self.isRestartPicker || self.isPulsePicker) {
        if (added) {
            cell.detailTextLabel.text = [NSString stringWithFormat:@"%@ · 已在规则中（点此编辑）", app.bundleId];
            cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
            cell.selectionStyle = UITableViewCellSelectionStyleDefault;
        } else {
            cell.detailTextLabel.text = app.bundleId;
            cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
            cell.accessoryType = UITableViewCellAccessoryNone;
            cell.selectionStyle = UITableViewCellSelectionStyleDefault;
        }
        return cell;
    }
    if (added) {
        if (procDesc) {
            cell.detailTextLabel.text = [NSString stringWithFormat:@"%@ · 已在规则中（点此编辑）· %@", app.pathFragment, procDesc];
        } else {
            cell.detailTextLabel.text = [NSString stringWithFormat:@"%@ · 已在规则中（点此编辑）", app.pathFragment];
        }
        cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        cell.selectionStyle = UITableViewCellSelectionStyleDefault;
    } else {
        if (procDesc) {
            cell.detailTextLabel.text = [NSString stringWithFormat:@"%@ · %@", app.pathFragment, procDesc];
        } else {
            cell.detailTextLabel.text = app.pathFragment;
        }
        cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
        cell.accessoryType = UITableViewCellAccessoryNone;
        cell.selectionStyle = UITableViewCellSelectionStyleDefault;
    }
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    JPInstalledApp *app = self.filteredApps[indexPath.row];
    NSString *bid = app.bundleId;

    if (self.isPulsePicker) {
        if (!bid || bid.length == 0 || JPIsRestartBlockedBundleId(bid) || [bid isEqualToString:@"com.apple.mobilesafari"]) {
            UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"不支持添加"
                                                                           message:@"系统 App 不支持后台释放"
                                                                    preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
            [self presentViewController:alert animated:YES completion:nil];
            return;
        }
        void (^callback)(NSString *, NSString *) = self.onSelectPulseBid;
        NSString *dispName = app.displayName ?: bid;
        [self jp_dismissThen:^{
            if (callback) {
                callback(bid, dispName);
            }
        }];
        return;
    }

    if (self.isRestartPicker) {
        if (!bid || bid.length == 0 || JPIsRestartBlockedBundleId(bid)) {
            UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"无法添加"
                                                                           message:@"该 App 无法配置超内存重启规则"
                                                                    preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
            [self presentViewController:alert animated:YES completion:nil];
            return;
        }
        void (^callback)(NSString *, NSString *) = self.onSelectRestartBid;
        NSString *dispName = app.displayName ?: bid;
        [self jp_dismissThen:^{
            if (callback) {
                callback(bid, dispName);
            }
        }];
        return;
    }

    if (self.userAppsOnly) {
        if (!bid || bid.length == 0) {
            UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"无法添加"
                                                                           message:@"该 App 未获取到 Bundle ID，无法配置内存释放规则"
                                                                    preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
            [self presentViewController:alert animated:YES completion:nil];
            return;
        }
        if (self.onSelectApp) {
            self.onSelectApp([NSString stringWithFormat:@"bid:%@", bid], app.displayName);
        }
        [self jp_dismissThen:nil];
        return;
    }

    if (bid && bid.length > 0) {
        NSString *frag = [NSString stringWithFormat:@"bid:%@", bid];
        NSString *name = app.displayName;
        void (^callback)(NSString *, NSString *) = self.onSelectApp;
        [self jp_dismissThen:^{
            if (callback) {
                callback(frag, name);
            }
        }];
    } else {
        __weak typeof(self) weakSelf = self;
        UIAlertController *warn = [UIAlertController alertControllerWithTitle:@"提示"
                                                                      message:@"这个 App 读不到标识，按目录名匹配，可能会误伤同名 App"
                                                               preferredStyle:UIAlertControllerStyleAlert];
        [warn addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:^(UIAlertAction * _Nonnull action) {
            NSString *frag = app.pathFragment;
            NSString *name = app.displayName;
            void (^callback)(NSString *, NSString *) = weakSelf.onSelectApp;
            [weakSelf jp_dismissThen:^{
                if (callback) {
                    callback(frag, name);
                }
            }];
        }]];
        [warn addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
        [self presentViewController:warn animated:YES completion:nil];
    }
}

- (void)promptManualAddWithText:(NSString *)text type:(JPRuleType)type band:(NSInteger)band {
    NSMutableArray<NSString *> *matchedNames = [NSMutableArray array];
    for (JPInstalledApp *app in self.allApps) {
        if ([app.pathFragment containsString:text] || [text containsString:app.pathFragment] ||
            [app.bundleName containsString:text] || [text containsString:app.bundleName]) {
            [matchedNames addObject:app.displayName];
        }
    }
    __weak typeof(self) weakSelf = self;
    if (matchedNames.count >= 2) {
        UIAlertController *warn = [UIAlertController alertControllerWithTitle:@"警告"
                                                                      message:[NSString stringWithFormat:@"该路径片段会同时命中多个 App：%@。确定要继续添加吗？", [matchedNames componentsJoinedByString:@"、"]]
                                                               preferredStyle:UIAlertControllerStyleAlert];
        [warn addAction:[UIAlertAction actionWithTitle:@"继续添加" style:UIAlertActionStyleDefault handler:^(UIAlertAction * _Nonnull action) {
            void (^callback)(NSString *, JPRuleType, NSInteger) = weakSelf.onManualAdd;
            [weakSelf jp_dismissThen:^{
                if (callback) {
                    callback(text, type, band);
                }
            }];
        }]];
        [warn addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
        [self presentViewController:warn animated:YES completion:nil];
    } else {
        void (^callback)(NSString *, JPRuleType, NSInteger) = self.onManualAdd;
        [self jp_dismissThen:^{
            if (callback) {
                callback(text, type, band);
            }
        }];
    }
}

- (void)showManualInputDialog {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"手动添加规则"
                                                                   message:@"输入 App 的路径片段（例如 Reynard.app/）"
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField * _Nonnull textField) {
        textField.placeholder = @"路径片段，如 Reynard.app/";
        textField.autocorrectionType = UITextAutocorrectionTypeNo;
        textField.autocapitalizationType = UITextAutocapitalizationTypeNone;
    }];

    __weak typeof(alert) weakAlert = alert;
    __weak typeof(self) weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"添加为保护 (50 档)" style:UIAlertActionStyleDefault handler:^(UIAlertAction * _Nonnull action) {
        NSString *text = [weakAlert.textFields.firstObject.text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        if (text.length == 0) return;
        if (![text containsString:@"/"] && ![text hasSuffix:@".app"]) {
            text = [text stringByAppendingString:@".app/"];
        }
        [weakSelf promptManualAddWithText:text type:JPRuleTypeProtect band:50];
    }]];

    [alert addAction:[UIAlertAction actionWithTitle:@"添加为降级" style:UIAlertActionStyleDefault handler:^(UIAlertAction * _Nonnull action) {
        NSString *text = [weakAlert.textFields.firstObject.text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        if (text.length == 0) return;
        if (![text containsString:@"/"] && ![text hasSuffix:@".app"]) {
            text = [text stringByAppendingString:@".app/"];
        }
        [weakSelf promptManualAddWithText:text type:JPRuleTypeDemote band:30];
    }]];

    [alert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)showManualRestartBidInputDialog {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"手动添加重启规则"
                                                                   message:@"请输入以 bid: 开头的 Bundle ID（例如 bid:com.example.app）"
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField * _Nonnull textField) {
        textField.placeholder = @"bid:com.example.app";
        textField.autocorrectionType = UITextAutocorrectionTypeNo;
        textField.autocapitalizationType = UITextAutocapitalizationTypeNone;
    }];

    __weak typeof(alert) weakAlert = alert;
    __weak typeof(self) weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"确定" style:UIAlertActionStyleDefault handler:^(UIAlertAction * _Nonnull action) {
        NSString *text = [weakAlert.textFields.firstObject.text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        if (![text hasPrefix:@"bid:"]) {
            UIAlertController *err = [UIAlertController alertControllerWithTitle:@"格式错误"
                                                                         message:@"超内存重启规则仅支持 Bundle ID，必须以 \"bid:\" 开头（不接受路径片段或 web:）"
                                                                  preferredStyle:UIAlertControllerStyleAlert];
            [err addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
            [weakSelf presentViewController:err animated:YES completion:nil];
            return;
        }

        NSString *bid = [text substringFromIndex:4];
        if (bid.length == 0 || JPIsRestartBlockedBundleId(bid)) {
            UIAlertController *err = [UIAlertController alertControllerWithTitle:@"无效的 Bundle ID"
                                                                         message:@"该 Bundle ID 不合法或在系统禁止名单中"
                                                                  preferredStyle:UIAlertControllerStyleAlert];
            [err addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
            [weakSelf presentViewController:err animated:YES completion:nil];
            return;
        }

        NSString *dispName = bid;
        for (JPInstalledApp *app in weakSelf.allApps) {
            if ([app.bundleId isEqualToString:bid]) {
                dispName = app.displayName ?: bid;
                break;
            }
        }

        void (^callback)(NSString *, NSString *) = weakSelf.onSelectRestartBid;
        [weakSelf jp_dismissThen:^{
            if (callback) {
                callback(bid, dispName);
            }
        }];
    }]];

    [alert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)showManualPulseBidInputDialog {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"手动添加释放规则"
                                                                   message:@"请输入以 bid: 开头的 Bundle ID（例如 bid:com.example.app）"
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField * _Nonnull textField) {
        textField.placeholder = @"bid:com.example.app";
        textField.autocorrectionType = UITextAutocorrectionTypeNo;
        textField.autocapitalizationType = UITextAutocapitalizationTypeNone;
    }];

    __weak typeof(alert) weakAlert = alert;
    __weak typeof(self) weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"确定" style:UIAlertActionStyleDefault handler:^(UIAlertAction * _Nonnull action) {
        NSString *text = [weakAlert.textFields.firstObject.text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        if (![text hasPrefix:@"bid:"]) {
            UIAlertController *err = [UIAlertController alertControllerWithTitle:@"格式错误"
                                                                         message:@"后台释放规则仅支持 Bundle ID，必须以 \"bid:\" 开头（不接受路径片段或 web:）"
                                                                  preferredStyle:UIAlertControllerStyleAlert];
            [err addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
            [weakSelf presentViewController:err animated:YES completion:nil];
            return;
        }

        NSString *bid = [text substringFromIndex:4];
        if (bid.length == 0 || JPIsRestartBlockedBundleId(bid)) {
            UIAlertController *err = [UIAlertController alertControllerWithTitle:@"无效的 Bundle ID"
                                                                         message:@"该 Bundle ID 不合法或在系统禁止名单中"
                                                                  preferredStyle:UIAlertControllerStyleAlert];
            [err addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
            [weakSelf presentViewController:err animated:YES completion:nil];
            return;
        }

        JPInstalledApp *matchedApp = nil;
        for (JPInstalledApp *app in weakSelf.allApps) {
            if ([app.bundleId isEqualToString:bid]) {
                matchedApp = app;
                break;
            }
        }

        /* 系统 App 与 Safari 彻底排除 (S1 / Hark-A Q3) */
        if (!matchedApp || [bid isEqualToString:@"com.apple.mobilesafari"]) {
            UIAlertController *err = [UIAlertController alertControllerWithTitle:@"不支持添加"
                                                                         message:@"系统 App 不支持后台释放"
                                                                  preferredStyle:UIAlertControllerStyleAlert];
            [err addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
            [weakSelf presentViewController:err animated:YES completion:nil];
            return;
        }

        NSString *dispName = matchedApp.displayName ?: bid;

        void (^callback)(NSString *, NSString *) = weakSelf.onSelectPulseBid;
        [weakSelf jp_dismissThen:^{
            if (callback) {
                callback(bid, dispName);
            }
        }];
    }]];

    [alert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

@end


#pragma mark - Controllers: Protected List

@interface JPProtectedListViewController : UIViewController <UITableViewDataSource, UITableViewDelegate>
@property (nonatomic, strong) UITableView *tableView;
@property (nonatomic, strong) UIView *emptyView;
@property (nonatomic, strong) UIImageView *emptyIconView;
@property (nonatomic, strong) UILabel *emptyLabel;
@property (nonatomic, strong) NSTimer *pollingTimer;
@property (nonatomic, strong) JPStatusInfo *status;
@property (nonatomic, copy) NSArray<JPInstalledApp *> *installedApps;
@property (nonatomic, copy) NSString * (^nameResolver)(NSString *frag);
@end

@implementation JPProtectedListViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"保护中";
    self.view.backgroundColor = UIColor.systemGroupedBackgroundColor;

    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"arrow.clockwise"]
                                                                              style:UIBarButtonItemStylePlain
                                                                             target:self
                                                                             action:@selector(refreshData)];

    self.tableView = [[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStyleInsetGrouped];
    self.tableView.translatesAutoresizingMaskIntoConstraints = NO;
    self.tableView.dataSource = self;
    self.tableView.delegate = self;

    UIRefreshControl *rc = [UIRefreshControl new];
    [rc addTarget:self action:@selector(refreshData) forControlEvents:UIControlEventValueChanged];
    self.tableView.refreshControl = rc;

    [self.view addSubview:self.tableView];

    [NSLayoutConstraint activateConstraints:@[
        [self.tableView.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [self.tableView.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [self.tableView.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [self.tableView.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor]
    ]];

    [self setupEmptyView];
    [self refreshData];

    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(refreshData)
                                                 name:UIApplicationDidBecomeActiveNotification
                                               object:nil];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self refreshData];
    [self startPollingTimer];
}

- (void)viewWillDisappear:(BOOL)animated {
    [super viewWillDisappear:animated];
    [self stopPollingTimer];
}

- (void)dealloc {
    [self stopPollingTimer];
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (void)startPollingTimer {
    [self stopPollingTimer];
    __weak typeof(self) weakSelf = self;
    self.pollingTimer = [NSTimer scheduledTimerWithTimeInterval:3.0 repeats:YES block:^(NSTimer * _Nonnull timer) {
        [weakSelf refreshData];
    }];
}

- (void)stopPollingTimer {
    [self.pollingTimer invalidate];
    self.pollingTimer = nil;
}

- (void)setupEmptyView {
    self.emptyView = [UIView new];
    self.emptyView.translatesAutoresizingMaskIntoConstraints = NO;
    self.emptyView.hidden = YES;

    self.emptyIconView = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"shield.slash"]];
    self.emptyIconView.translatesAutoresizingMaskIntoConstraints = NO;
    self.emptyIconView.tintColor = UIColor.tertiaryLabelColor;
    self.emptyIconView.contentMode = UIViewContentModeScaleAspectFit;

    self.emptyLabel = [UILabel new];
    self.emptyLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.emptyLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightMedium];
    self.emptyLabel.textColor = UIColor.secondaryLabelColor;
    self.emptyLabel.textAlignment = NSTextAlignmentCenter;
    self.emptyLabel.numberOfLines = 0;
    self.emptyLabel.text = @"暂无受保护的 App";

    [self.emptyView addSubview:self.emptyIconView];
    [self.emptyView addSubview:self.emptyLabel];

    [NSLayoutConstraint activateConstraints:@[
        [self.emptyIconView.centerXAnchor constraintEqualToAnchor:self.emptyView.centerXAnchor],
        [self.emptyIconView.topAnchor constraintEqualToAnchor:self.emptyView.topAnchor],
        [self.emptyIconView.widthAnchor constraintEqualToConstant:44],
        [self.emptyIconView.heightAnchor constraintEqualToConstant:44],

        [self.emptyLabel.topAnchor constraintEqualToAnchor:self.emptyIconView.bottomAnchor constant:12],
        [self.emptyLabel.leadingAnchor constraintEqualToAnchor:self.emptyView.leadingAnchor constant:24],
        [self.emptyLabel.trailingAnchor constraintEqualToAnchor:self.emptyView.trailingAnchor constant:-24],
        [self.emptyLabel.bottomAnchor constraintEqualToAnchor:self.emptyView.bottomAnchor]
    ]];

    [self.view addSubview:self.emptyView];
    [NSLayoutConstraint activateConstraints:@[
        [self.emptyView.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [self.emptyView.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor]
    ]];
}

- (void)refreshData {
    self.status = [JPStatusInfo loadCurrentStatus];
    [self updateUI];
    if (self.tableView.refreshControl.isRefreshing) {
        [self.tableView.refreshControl endRefreshing];
    }
}

- (void)updateUI {
    NSInteger count = self.status.protectedApps.count;
    if (count == 0) {
        self.tableView.hidden = YES;
        self.emptyView.hidden = NO;
        if (!self.status.hasProtectedAppLines && self.status.protectedPids > 0) {
            self.emptyIconView.image = [UIImage systemImageNamed:@"exclamationmark.triangle"];
            self.emptyLabel.text = @"暂无数据（守护进程版本过旧）";
        } else {
            self.emptyIconView.image = [UIImage systemImageNamed:@"shield.slash"];
            self.emptyLabel.text = @"暂无受保护的 App";
        }
    } else {
        self.emptyView.hidden = YES;
        self.tableView.hidden = NO;
        [self.tableView reloadData];
    }
}

#pragma mark - Table View

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    return self.status.protectedApps.count;
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    if (self.status.protectedApps.count == 0) return nil;
    return [NSString stringWithFormat:@"受保护进程 (%lu)", (unsigned long)self.status.protectedApps.count];
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    static NSString *const kCellId = @"JPProtectedAppCell";
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:kCellId];
    if (!cell) {
        cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:kCellId];
    }

    JPProtectedApp *app = self.status.protectedApps[indexPath.row];
    NSString *ruleName = self.nameResolver ? self.nameResolver(app.pathFragment) : app.pathFragment;
    if (!ruleName || ruleName.length == 0) {
        ruleName = app.pathFragment ?: @"";
    }

    JPInstalledApp *realApp = nil;
    if (app.bundleId.length > 0 && self.installedApps) {
        for (JPInstalledApp *inst in self.installedApps) {
            if ([inst.bundleId isEqualToString:app.bundleId]) {
                realApp = inst;
                break;
            }
        }
    }

    cell.textLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightMedium];

    BOOL isBidRule = [app.pathFragment hasPrefix:@"bid:"];
    if (isBidRule) {
        NSString *displayName = realApp.displayName ?: ruleName;
        cell.textLabel.attributedText = nil;
        cell.textLabel.text = displayName;
        cell.textLabel.textColor = UIColor.labelColor;
    } else {
        if (realApp && realApp.displayName.length > 0 && ![realApp.displayName isEqualToString:ruleName]) {
            NSMutableAttributedString *attr = [[NSMutableAttributedString alloc] initWithString:ruleName attributes:@{
                NSForegroundColorAttributeName: UIColor.labelColor,
                NSFontAttributeName: [UIFont systemFontOfSize:16 weight:UIFontWeightMedium]
            }];
            NSString *extra = [NSString stringWithFormat:@" 实际是 %@", realApp.displayName];
            NSAttributedString *orangePart = [[NSAttributedString alloc] initWithString:extra attributes:@{
                NSForegroundColorAttributeName: UIColor.systemOrangeColor,
                NSFontAttributeName: [UIFont systemFontOfSize:13 weight:UIFontWeightRegular]
            }];
            [attr appendAttributedString:orangePart];
            cell.textLabel.attributedText = attr;
        } else {
            cell.textLabel.attributedText = nil;
            cell.textLabel.text = ruleName;
            cell.textLabel.textColor = UIColor.labelColor;
        }
    }

    NSString *humanStatus;
    if (app.currentBand >= 100) {
        humanStatus = @"前台中";
    } else if (app.currentBand == app.targetBand) {
        humanStatus = @"已达标";
    } else if (app.currentBand < app.targetBand) {
        humanStatus = @"等待提升";
    } else {
        humanStatus = @"高于目标";
    }
    NSString *kind = @"";
    if (app.procLabel.length > 0)
        kind = [app.procLabel isEqualToString:@"main"]
             ? @"主程序 · "
             : [NSString stringWithFormat:@"扩展 %@ · ", app.procLabel];
    cell.detailTextLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightRegular];
    cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
    cell.detailTextLabel.text = [NSString stringWithFormat:@"%@pid %d · 当前 %ld → 目标 %ld · %@",
                                 kind, (int)app.pid, (long)app.currentBand, (long)app.targetBand, humanStatus];
    cell.detailTextLabel.numberOfLines = 0;   /* 扩展名长时换行，不截断 */
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    return cell;
}

@end

#pragma mark - Controllers: Log View Controller

static uint32_t JPComputeCRC32(const uint8_t *data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(int)(crc & 1)));
        }
    }
    return ~crc;
}

static NSData *JPCreateZipArchive(NSArray<NSDictionary *> *entries) {
    NSMutableData *zipData = [NSMutableData data];
    NSMutableData *cdData = [NSMutableData data];

    NSCalendar *cal = [NSCalendar currentCalendar];
    NSDateComponents *comp = [cal components:(NSCalendarUnitYear | NSCalendarUnitMonth | NSCalendarUnitDay |
                                              NSCalendarUnitHour | NSCalendarUnitMinute | NSCalendarUnitSecond)
                                    fromDate:[NSDate date]];
    NSInteger y = comp.year;
    NSInteger m = comp.month;
    NSInteger d = comp.day;
    NSInteger h = comp.hour;
    NSInteger min = comp.minute;
    NSInteger s = comp.second;
    if (y < 1980) y = 1980;
    if (m < 1) m = 1;
    if (m > 12) m = 12;
    if (d < 1) d = 1;
    if (d > 31) d = 31;
    uint16_t dosDate = (uint16_t)((((y - 1980) & 0x7F) << 9) | ((m & 0x0F) << 5) | (d & 0x1F));
    uint16_t dosTime = (uint16_t)(((h & 0x1F) << 11) | ((min & 0x3F) << 5) | ((s / 2) & 0x1F));

    for (NSDictionary *entry in entries) {
        NSString *name = entry[@"name"];
        NSData *data = entry[@"data"] ?: [NSData data];
        NSData *nameData = [name dataUsingEncoding:NSUTF8StringEncoding];

        uint32_t offset = (uint32_t)zipData.length;
        uint32_t crc = JPComputeCRC32(data.bytes, data.length);
        uint32_t size = (uint32_t)data.length;
        uint16_t nameLen = (uint16_t)nameData.length;

        // Local file header: 30 bytes
        uint8_t localHeader[30] = {0};
        localHeader[0] = 0x50; localHeader[1] = 0x4b; localHeader[2] = 0x03; localHeader[3] = 0x04;
        localHeader[4] = 20;   localHeader[5] = 0;
        localHeader[6] = 0;    localHeader[7] = 0;
        localHeader[8] = 0;    localHeader[9] = 0;
        localHeader[10] = (uint8_t)(dosTime & 0xFF);
        localHeader[11] = (uint8_t)((dosTime >> 8) & 0xFF);
        localHeader[12] = (uint8_t)(dosDate & 0xFF);
        localHeader[13] = (uint8_t)((dosDate >> 8) & 0xFF);
        localHeader[14] = (uint8_t)(crc & 0xFF);
        localHeader[15] = (uint8_t)((crc >> 8) & 0xFF);
        localHeader[16] = (uint8_t)((crc >> 16) & 0xFF);
        localHeader[17] = (uint8_t)((crc >> 24) & 0xFF);
        localHeader[18] = (uint8_t)(size & 0xFF);
        localHeader[19] = (uint8_t)((size >> 8) & 0xFF);
        localHeader[20] = (uint8_t)((size >> 16) & 0xFF);
        localHeader[21] = (uint8_t)((size >> 24) & 0xFF);
        localHeader[22] = (uint8_t)(size & 0xFF);
        localHeader[23] = (uint8_t)((size >> 8) & 0xFF);
        localHeader[24] = (uint8_t)((size >> 16) & 0xFF);
        localHeader[25] = (uint8_t)((size >> 24) & 0xFF);
        localHeader[26] = (uint8_t)(nameLen & 0xFF);
        localHeader[27] = (uint8_t)((nameLen >> 8) & 0xFF);
        localHeader[28] = 0;   localHeader[29] = 0;

        [zipData appendBytes:localHeader length:30];
        [zipData appendData:nameData];
        [zipData appendData:data];

        // Central directory header: 46 bytes
        uint8_t cdHeader[46] = {0};
        cdHeader[0] = 0x50; cdHeader[1] = 0x4b; cdHeader[2] = 0x01; cdHeader[3] = 0x02;
        cdHeader[4] = 20;   cdHeader[5] = 0;
        cdHeader[6] = 20;   cdHeader[7] = 0;
        cdHeader[8] = 0;    cdHeader[9] = 0;
        cdHeader[10] = 0;   cdHeader[11] = 0;
        cdHeader[12] = (uint8_t)(dosTime & 0xFF);
        cdHeader[13] = (uint8_t)((dosTime >> 8) & 0xFF);
        cdHeader[14] = (uint8_t)(dosDate & 0xFF);
        cdHeader[15] = (uint8_t)((dosDate >> 8) & 0xFF);
        cdHeader[16] = (uint8_t)(crc & 0xFF);
        cdHeader[17] = (uint8_t)((crc >> 8) & 0xFF);
        cdHeader[18] = (uint8_t)((crc >> 16) & 0xFF);
        cdHeader[19] = (uint8_t)((crc >> 24) & 0xFF);
        cdHeader[20] = (uint8_t)(size & 0xFF);
        cdHeader[21] = (uint8_t)((size >> 8) & 0xFF);
        cdHeader[22] = (uint8_t)((size >> 16) & 0xFF);
        cdHeader[23] = (uint8_t)((size >> 24) & 0xFF);
        cdHeader[24] = (uint8_t)(size & 0xFF);
        cdHeader[25] = (uint8_t)((size >> 8) & 0xFF);
        cdHeader[26] = (uint8_t)((size >> 16) & 0xFF);
        cdHeader[27] = (uint8_t)((size >> 24) & 0xFF);
        cdHeader[28] = (uint8_t)(nameLen & 0xFF);
        cdHeader[29] = (uint8_t)((nameLen >> 8) & 0xFF);
        cdHeader[30] = 0;   cdHeader[31] = 0;
        cdHeader[32] = 0;   cdHeader[33] = 0;
        cdHeader[34] = 0;   cdHeader[35] = 0;
        cdHeader[36] = 0;   cdHeader[37] = 0;
        cdHeader[38] = 0;   cdHeader[39] = 0; cdHeader[40] = 0; cdHeader[41] = 0;
        cdHeader[42] = (uint8_t)(offset & 0xFF);
        cdHeader[43] = (uint8_t)((offset >> 8) & 0xFF);
        cdHeader[44] = (uint8_t)((offset >> 16) & 0xFF);
        cdHeader[45] = (uint8_t)((offset >> 24) & 0xFF);

        [cdData appendBytes:cdHeader length:46];
        [cdData appendData:nameData];
    }

    uint32_t cdOffset = (uint32_t)zipData.length;
    uint32_t cdSize = (uint32_t)cdData.length;
    uint16_t totalEntries = (uint16_t)entries.count;

    [zipData appendData:cdData];

    // End of Central Directory record: 22 bytes
    uint8_t eocd[22] = {0};
    eocd[0] = 0x50; eocd[1] = 0x4b; eocd[2] = 0x05; eocd[3] = 0x06;
    eocd[4] = 0; eocd[5] = 0;
    eocd[6] = 0; eocd[7] = 0;
    eocd[8] = (uint8_t)(totalEntries & 0xFF);
    eocd[9] = (uint8_t)((totalEntries >> 8) & 0xFF);
    eocd[10] = (uint8_t)(totalEntries & 0xFF);
    eocd[11] = (uint8_t)((totalEntries >> 8) & 0xFF);
    eocd[12] = (uint8_t)(cdSize & 0xFF);
    eocd[13] = (uint8_t)((cdSize >> 8) & 0xFF);
    eocd[14] = (uint8_t)((cdSize >> 16) & 0xFF);
    eocd[15] = (uint8_t)((cdSize >> 24) & 0xFF);
    eocd[16] = (uint8_t)(cdOffset & 0xFF);
    eocd[17] = (uint8_t)((cdOffset >> 8) & 0xFF);
    eocd[18] = (uint8_t)((cdOffset >> 16) & 0xFF);
    eocd[19] = (uint8_t)((cdOffset >> 24) & 0xFF);
    eocd[20] = 0; eocd[21] = 0;

    [zipData appendBytes:eocd length:22];
    return zipData;
}

@interface JPLogViewController : UIViewController
@property (nonatomic, strong) UITextView *textView;
@property (nonatomic, strong) UIBarButtonItem *exportItem;
@property (nonatomic, strong) UIBarButtonItem *clearItem;
@property (nonatomic, copy) NSString *logLevel;
@property (nonatomic, copy) void (^onLogLevelChanged)(NSString *newLevel);
@property (nonatomic, strong) UIButton *levelButton;
@end

@implementation JPLogViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"调试日志";
    self.view.backgroundColor = UIColor.systemGroupedBackgroundColor;

    self.exportItem = [[UIBarButtonItem alloc] initWithTitle:@"导出"
                                                       style:UIBarButtonItemStylePlain
                                                      target:self
                                                      action:@selector(exportLogs)];
    UIBarButtonItem *copyBtn = [[UIBarButtonItem alloc] initWithTitle:@"复制"
                                                                style:UIBarButtonItemStylePlain
                                                               target:self
                                                               action:@selector(copyLog)];
    self.clearItem = [[UIBarButtonItem alloc] initWithTitle:@"清空"
                                                      style:UIBarButtonItemStylePlain
                                                     target:self
                                                     action:@selector(confirmClearLog)];
    self.navigationItem.rightBarButtonItems = @[self.exportItem, copyBtn, self.clearItem];

    UIView *headerCard = [UIView new];
    headerCard.translatesAutoresizingMaskIntoConstraints = NO;
    headerCard.backgroundColor = UIColor.secondarySystemGroupedBackgroundColor;
    headerCard.layer.cornerRadius = 12;
    headerCard.layer.cornerCurve = kCACornerCurveContinuous;
    headerCard.clipsToBounds = YES;

    UIStackView *headerStack = [UIStackView new];
    headerStack.translatesAutoresizingMaskIntoConstraints = NO;
    headerStack.axis = UILayoutConstraintAxisVertical;
    headerStack.spacing = 6;

    UIStackView *levelRow = [UIStackView new];
    levelRow.translatesAutoresizingMaskIntoConstraints = NO;
    levelRow.axis = UILayoutConstraintAxisHorizontal;
    levelRow.alignment = UIStackViewAlignmentCenter;
    levelRow.distribution = UIStackViewDistributionEqualSpacing;

    UILabel *titleLabel = [UILabel new];
    titleLabel.translatesAutoresizingMaskIntoConstraints = NO;
    titleLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightMedium];
    titleLabel.textColor = UIColor.labelColor;
    titleLabel.text = @"日志等级";

    self.levelButton = [UIButton buttonWithType:UIButtonTypeSystem];
    self.levelButton.translatesAutoresizingMaskIntoConstraints = NO;
    self.levelButton.titleLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightMedium];
    self.levelButton.showsMenuAsPrimaryAction = YES;
    [self updateLevelMenu];

    UILabel *tipLabel = [UILabel new];
    tipLabel.translatesAutoresizingMaskIntoConstraints = NO;
    tipLabel.font = [UIFont systemFontOfSize:12 weight:UIFontWeightRegular];
    tipLabel.textColor = UIColor.secondaryLabelColor;
    tipLabel.text = @"排查问题时请选 Info 或 Debug";

    [levelRow addArrangedSubview:titleLabel];
    [levelRow addArrangedSubview:self.levelButton];

    [headerStack addArrangedSubview:levelRow];
    [headerStack addArrangedSubview:tipLabel];
    [headerCard addSubview:headerStack];

    [NSLayoutConstraint activateConstraints:@[
        [headerStack.topAnchor constraintEqualToAnchor:headerCard.topAnchor constant:10],
        [headerStack.leadingAnchor constraintEqualToAnchor:headerCard.leadingAnchor constant:16],
        [headerStack.trailingAnchor constraintEqualToAnchor:headerCard.trailingAnchor constant:-16],
        [headerStack.bottomAnchor constraintEqualToAnchor:headerCard.bottomAnchor constant:-10]
    ]];

    [self.view addSubview:headerCard];

    self.textView = [[UITextView alloc] initWithFrame:CGRectZero];
    self.textView.translatesAutoresizingMaskIntoConstraints = NO;
    self.textView.editable = NO;
    self.textView.font = [UIFont monospacedSystemFontOfSize:11 weight:UIFontWeightRegular];
    self.textView.textColor = UIColor.labelColor;
    self.textView.backgroundColor = UIColor.systemGroupedBackgroundColor;
    self.textView.alwaysBounceVertical = YES;
    self.textView.textContainerInset = UIEdgeInsetsMake(12, 12, 12, 12);
    self.textView.layoutManager.allowsNonContiguousLayout = NO;

    UIRefreshControl *rc = [UIRefreshControl new];
    [rc addTarget:self action:@selector(loadLog) forControlEvents:UIControlEventValueChanged];
    self.textView.refreshControl = rc;

    [self.view addSubview:self.textView];

    [NSLayoutConstraint activateConstraints:@[
        [headerCard.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor constant:10],
        [headerCard.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor constant:16],
        [headerCard.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor constant:-16],

        [self.textView.topAnchor constraintEqualToAnchor:headerCard.bottomAnchor constant:10],
        [self.textView.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [self.textView.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [self.textView.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor]
    ]];
}

- (void)updateLevelMenu {
    NSString *current = self.logLevel.lowercaseString ?: @"info";
    NSDictionary *titles = @{
        @"off": @"OFF",
        @"error": @"Error",
        @"warning": @"Warning",
        @"info": @"Info",
        @"debug": @"Debug"
    };
    NSString *display = titles[current] ?: @"Info";
    [self.levelButton setTitle:[NSString stringWithFormat:@"%@  ▾", display] forState:UIControlStateNormal];

    NSArray *keys = @[@"off", @"error", @"warning", @"info", @"debug"];
    NSMutableArray<UIAction *> *actions = [NSMutableArray array];
    __weak typeof(self) weakSelf = self;
    for (NSString *key in keys) {
        NSString *title = titles[key];
        BOOL isSel = [key isEqualToString:current];
        UIAction *act = [UIAction actionWithTitle:title image:nil identifier:nil handler:^(__kindof UIAction * _Nonnull action) {
            [weakSelf selectLogLevel:key];
        }];
        if (isSel) {
            act.state = UIMenuElementStateOn;
        }
        [actions addObject:act];
    }
    self.levelButton.menu = [UIMenu menuWithTitle:@"" children:actions];
}

- (void)selectLogLevel:(NSString *)newLevel {
    NSString *oldLevel = self.logLevel;
    self.logLevel = newLevel;
    [self updateLevelMenu];
    if (self.onLogLevelChanged) {
        self.onLogLevelChanged(newLevel);
    } else {
        NSError *err = nil;
        NSString *content = [NSString stringWithContentsOfFile:kConfig encoding:NSUTF8StringEncoding error:&err];
        BOOL ok = NO;
        if (content) {
            NSArray *lines = [content componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]];
            NSMutableArray *newLines = [NSMutableArray array];
            BOOL found = NO;
            for (NSString *line in lines) {
                NSString *trimmed = [line stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
                if ([trimmed hasPrefix:@"loglevel"]) {
                    [newLines addObject:[NSString stringWithFormat:@"loglevel %@", newLevel]];
                    found = YES;
                } else {
                    [newLines addObject:line];
                }
            }
            if (!found) {
                [newLines insertObject:[NSString stringWithFormat:@"loglevel %@", newLevel] atIndex:0];
            }
            NSString *outContent = [newLines componentsJoinedByString:@"\n"];
            ok = [outContent writeToFile:kConfig atomically:YES encoding:NSUTF8StringEncoding error:&err];
        }
        if (!ok) {
            self.logLevel = oldLevel;
            [self updateLevelMenu];
            UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"保存失败"
                                                                           message:[NSString stringWithFormat:@"日志等级保存失败：%@", err.localizedDescription ?: @"写入错误"]
                                                                    preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
            [self presentViewController:alert animated:YES completion:nil];
        }
    }
}

- (void)saveLogLevelToConfigFile:(NSString *)level {
    NSError *err = nil;
    NSString *content = [NSString stringWithContentsOfFile:kConfig encoding:NSUTF8StringEncoding error:&err];
    if (!content) return;
    NSArray *lines = [content componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]];
    NSMutableArray *newLines = [NSMutableArray array];
    BOOL found = NO;
    for (NSString *line in lines) {
        NSString *trimmed = [line stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if ([trimmed hasPrefix:@"loglevel"]) {
            [newLines addObject:[NSString stringWithFormat:@"loglevel %@", level]];
            found = YES;
        } else {
            [newLines addObject:line];
        }
    }
    if (!found) {
        [newLines insertObject:[NSString stringWithFormat:@"loglevel %@", level] atIndex:0];
    }
    NSString *outContent = [newLines componentsJoinedByString:@"\n"];
    [outContent writeToFile:kConfig atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self loadLog];
}

- (void)loadLog {
    NSFileManager *fm = [NSFileManager defaultManager];
    if (![fm fileExistsAtPath:kLogFile]) {
        self.textView.text = @"暂无日志";
        self.textView.textColor = UIColor.secondaryLabelColor;
        self.textView.font = [UIFont monospacedSystemFontOfSize:11 weight:UIFontWeightRegular];
        [self.textView.refreshControl endRefreshing];
        return;
    }

    NSError *err = nil;
    NSDictionary<NSFileAttributeKey, id> *attrs = [fm attributesOfItemAtPath:kLogFile error:&err];
    unsigned long long fileSize = [attrs[NSFileSize] unsignedLongLongValue];
    if (fileSize == 0) {
        self.textView.text = @"暂无日志";
        self.textView.textColor = UIColor.secondaryLabelColor;
        self.textView.font = [UIFont monospacedSystemFontOfSize:11 weight:UIFontWeightRegular];
        [self.textView.refreshControl endRefreshing];
        return;
    }

    NSFileHandle *handle = [NSFileHandle fileHandleForReadingAtPath:kLogFile];
    if (!handle) {
        self.textView.text = @"暂无日志";
        self.textView.textColor = UIColor.secondaryLabelColor;
        self.textView.font = [UIFont monospacedSystemFontOfSize:11 weight:UIFontWeightRegular];
        [self.textView.refreshControl endRefreshing];
        return;
    }

    const unsigned long long kTailLimit = 64 * 1024;
    BOOL seeked = NO;
    if (fileSize > kTailLimit) {
        [handle seekToFileOffset:(fileSize - kTailLimit)];
        seeked = YES;
    }
    NSData *data = [handle readDataToEndOfFile];
    [handle closeFile];

    NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    if (!text) {
        text = [[NSString alloc] initWithData:data encoding:NSISOLatin1StringEncoding] ?: @"";
    }

    if (seeked) {
        /* 丢掉第一行残行再显示 */
        NSRange firstNewline = [text rangeOfString:@"\n"];
        if (firstNewline.location != NSNotFound && firstNewline.location + 1 < text.length) {
            text = [text substringFromIndex:firstNewline.location + 1];
        }
    }

    if (text.length == 0) {
        self.textView.text = @"暂无日志";
        self.textView.textColor = UIColor.secondaryLabelColor;
    } else {
        self.textView.text = text;
        self.textView.textColor = UIColor.labelColor;
        [self scrollToBottom];
    }
    self.textView.font = [UIFont monospacedSystemFontOfSize:11 weight:UIFontWeightRegular];
    [self.textView.refreshControl endRefreshing];
}

- (void)scrollToBottom {
    if (self.textView.text.length == 0) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        NSRange bottom = NSMakeRange(self.textView.text.length - 1, 1);
        [self.textView scrollRangeToVisible:bottom];
    });
}

- (void)copyLog {
    if (self.textView.text.length == 0 || [self.textView.text isEqualToString:@"暂无日志"]) {
        return;
    }
    if (self.presentedViewController) return;
    [UIPasteboard generalPasteboard].string = self.textView.text;
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"已复制"
                                                                   message:nil
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [self presentViewController:alert animated:YES completion:^{
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.0 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            [alert dismissViewControllerAnimated:YES completion:nil];
        });
    }];
}

- (void)confirmClearLog {
    if (self.presentedViewController) return;
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"清空日志"
                                                                   message:@"确定清空全部调试日志吗？"
                                                            preferredStyle:UIAlertControllerStyleActionSheet];
    UIAlertAction *clearAction = [UIAlertAction actionWithTitle:@"清空"
                                                          style:UIAlertActionStyleDestructive
                                                        handler:^(UIAlertAction * _Nonnull action) {
        [self clearLog];
    }];
    UIAlertAction *cancelAction = [UIAlertAction actionWithTitle:@"取消"
                                                           style:UIAlertActionStyleCancel
                                                         handler:nil];
    [alert addAction:clearAction];
    [alert addAction:cancelAction];

    if (alert.popoverPresentationController) {
        alert.popoverPresentationController.barButtonItem = self.clearItem;
    }

    [self presentViewController:alert animated:YES completion:nil];
}

- (void)clearLog {
    NSFileManager *fm = [NSFileManager defaultManager];
    [fm removeItemAtPath:kLogFile error:nil];
    [fm removeItemAtPath:kLogOldFile error:nil];
    [self loadLog];
}

- (void)exportLogs {
    if (self.presentedViewController) return;

    UIActivityIndicatorView *spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
    [spinner startAnimating];
    self.exportItem.customView = spinner;

    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        NSFileManager *fm = [NSFileManager defaultManager];
        NSMutableArray<NSDictionary *> *entries = [NSMutableArray array];
        NSMutableArray<NSURL *> *fileURLs = [NSMutableArray array];

        // 1. daemon.log (拼接 daemon.log.1 和 daemon.log)
        NSMutableData *logData = [NSMutableData data];
        if ([fm fileExistsAtPath:kLogFile]) {
            [fileURLs addObject:[NSURL fileURLWithPath:kLogFile]];
        }
        if ([fm fileExistsAtPath:kLogOldFile]) {
            [fileURLs addObject:[NSURL fileURLWithPath:kLogOldFile]];
        }
        if ([fm fileExistsAtPath:kLogOldFile]) {
            NSData *oldD = [NSData dataWithContentsOfFile:kLogOldFile];
            if (oldD) [logData appendData:oldD];
        }
        if ([fm fileExistsAtPath:kLogFile]) {
            NSData *curD = [NSData dataWithContentsOfFile:kLogFile];
            if (curD) [logData appendData:curD];
        }
        if (logData.length > 0) {
            [entries addObject:@{@"name": @"daemon.log", @"data": logData}];
        }

        // 2. 附带文件：mempressure*.txt、config.txt、status.txt、restart.txt、restart_state.txt、pulse.txt、pulse.state
        NSArray<NSArray<NSString *> *> *attachFiles = @[
            @[kMemPressureConfig,    @"mempressure.txt"],
            @[kMemPressureConfigOff, @"mempressure.txt.off"],
            @[kMemPressureState,     @"mempressure_state.txt"],
            @[kMemPressureFloor,     @"mempressure_floor.txt"],
            @[kConfig,               @"config.txt"],
            @[kStatus,               @"status.txt"],
            @[kRestartConfig,        @"restart.txt"],
            @[kRestartState,         @"restart_state.txt"],
            @[kPulseConfig,          @"pulse.txt"],
            @[kPulseState,           @"pulse.state"]
        ];

        for (NSArray<NSString *> *pair in attachFiles) {
            NSString *filePath = pair[0];
            NSString *entryName = pair[1];
            if ([fm fileExistsAtPath:filePath]) {
                NSData *d = [NSData dataWithContentsOfFile:filePath];
                if (d) {
                    [entries addObject:@{@"name": entryName, @"data": d}];
                    [fileURLs addObject:[NSURL fileURLWithPath:filePath]];
                }
            }
        }

        // 检查并创建 export 目录
        NSString *exportDir = kExportDir;
        if (![fm fileExistsAtPath:exportDir]) {
            [fm createDirectoryAtPath:exportDir withIntermediateDirectories:YES attributes:nil error:nil];
        }

        // 清理 export/ 目录以及 tmp/ 里旧的 JetPilot-logs-*.zip，确保只保留最新 1 个
        NSArray<NSString *> *existingExport = [fm contentsOfDirectoryAtPath:exportDir error:nil];
        for (NSString *fn in existingExport) {
            if ([fn hasPrefix:@"JetPilot-logs-"] && [fn hasSuffix:@".zip"]) {
                [fm removeItemAtPath:[exportDir stringByAppendingPathComponent:fn] error:nil];
            }
        }
        NSString *tmpDir = NSTemporaryDirectory();
        if (tmpDir) {
            NSArray<NSString *> *existingTmp = [fm contentsOfDirectoryAtPath:tmpDir error:nil];
            for (NSString *fn in existingTmp) {
                if ([fn hasPrefix:@"JetPilot-logs-"] && [fn hasSuffix:@".zip"]) {
                    [fm removeItemAtPath:[tmpDir stringByAppendingPathComponent:fn] error:nil];
                }
            }
        }

        NSDateFormatter *df = [NSDateFormatter new];
        df.dateFormat = @"yyyyMMdd-HHmmss";
        NSString *dateStr = [df stringFromDate:[NSDate date]];
        NSString *ver = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"0.7.8";
        NSString *zipFileName = [NSString stringWithFormat:@"JetPilot-logs-%@-v%@.zip", dateStr, ver];
        NSString *zipPath = [exportDir stringByAppendingPathComponent:zipFileName];

        NSData *zipData = JPCreateZipArchive(entries);
        NSError *zipErr = nil;
        BOOL zipOk = NO;
        if (zipData.length > 0) {
            zipOk = [zipData writeToFile:zipPath options:NSDataWritingAtomic error:&zipErr];
        } else {
            zipErr = [NSError errorWithDomain:@"JetPilotExportErrorDomain"
                                         code:-1
                                     userInfo:@{NSLocalizedDescriptionKey: @"未找到可导出的日志或配置文件数据"}];
        }

        dispatch_async(dispatch_get_main_queue(), ^{
            __strong typeof(weakSelf) strongSelf = weakSelf;
            if (!strongSelf) return;

            strongSelf.exportItem.customView = nil;
            strongSelf.exportItem.title = @"导出";

            if (zipOk) {
                // 分享成功时，分享项只有一个 zip
                UIActivityViewController *avc = [[UIActivityViewController alloc] initWithActivityItems:@[[NSURL fileURLWithPath:zipPath]] applicationActivities:nil];
                if (avc.popoverPresentationController) {
                    avc.popoverPresentationController.barButtonItem = strongSelf.exportItem;
                }
                [strongSelf presentViewController:avc animated:YES completion:nil];
            } else {
                // 打包失败：不要悄悄退回零散文件，弹窗显示「打包失败：<错误描述>」，提供「改为分享单个文件」和「取消」两个选项，并把错误写进 App 侧日志或复制到剪贴板
                NSString *errDesc = zipErr.localizedDescription ?: @"未知错误";
                NSLog(@"[JetPilot] 打包失败: %@", zipErr);
                [UIPasteboard generalPasteboard].string = [NSString stringWithFormat:@"打包失败：%@", errDesc];

                NSString *alertMsg = [NSString stringWithFormat:@"打包失败：%@\n\n（错误信息已复制到剪贴板）", errDesc];
                UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"打包失败"
                                                                               message:alertMsg
                                                                        preferredStyle:UIAlertControllerStyleAlert];

                if (fileURLs.count > 0) {
                    [alert addAction:[UIAlertAction actionWithTitle:@"改为分享单个文件"
                                                              style:UIAlertActionStyleDefault
                                                            handler:^(UIAlertAction * _Nonnull action) {
                        UIActivityViewController *fallbackAvc = [[UIActivityViewController alloc] initWithActivityItems:fileURLs applicationActivities:nil];
                        if (fallbackAvc.popoverPresentationController) {
                            fallbackAvc.popoverPresentationController.barButtonItem = strongSelf.exportItem;
                        }
                        [strongSelf presentViewController:fallbackAvc animated:YES completion:nil];
                    }]];
                }
                [alert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];

                [strongSelf presentViewController:alert animated:YES completion:nil];
            }
        });
    });
}

@end

#pragma mark - Controllers: Process Bands

@interface JPProcessBandsViewController : UIViewController <UITableViewDataSource, UITableViewDelegate>
@property (nonatomic, copy) NSString *targetBundleId;
@property (nonatomic, copy) NSString *appName;
@property (nonatomic, strong) JPBandsSnapshot *snapshot;
@property (nonatomic, copy) NSArray<JPBandsProcess *> *processes;
@property (nonatomic, strong) UITableView *tableView;
@end

@implementation JPProcessBandsViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"进程档位";
    self.view.backgroundColor = UIColor.systemGroupedBackgroundColor;

    UIBarButtonItem *refreshItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh target:self action:@selector(refreshBands)];
    self.navigationItem.rightBarButtonItem = refreshItem;

    self.tableView = [[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStyleInsetGrouped];
    self.tableView.translatesAutoresizingMaskIntoConstraints = NO;
    self.tableView.dataSource = self;
    self.tableView.delegate = self;
    [self.view addSubview:self.tableView];

    [NSLayoutConstraint activateConstraints:@[
        [self.tableView.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [self.tableView.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [self.tableView.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [self.tableView.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor]
    ]];

    [self reloadProcessesFromSnapshot];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    if (self.targetBundleId.length == 0) {
        [self reloadProcessesFromSnapshot];
        return;
    }
    if (!self.snapshot) {
        [self refreshBands];
    } else {
        [self reloadProcessesFromSnapshot];
    }
}

- (void)reloadProcessesFromSnapshot {
    NSMutableArray<JPBandsProcess *> *matched = [NSMutableArray array];
    if (self.targetBundleId.length > 0 && self.snapshot.processes.count > 0) {
        for (JPBandsProcess *p in self.snapshot.processes) {
            if ([p.bundleId isEqualToString:self.targetBundleId]) {
                [matched addObject:p];
            }
        }
    }
    [matched sortUsingComparator:^NSComparisonResult(JPBandsProcess *p1, JPBandsProcess *p2) {
        BOOL isMain1 = [p1.procLabel isEqualToString:@"main"];
        BOOL isMain2 = [p2.procLabel isEqualToString:@"main"];
        if (isMain1 && !isMain2) return NSOrderedAscending;
        if (!isMain1 && isMain2) return NSOrderedDescending;
        if (p1.curBand > p2.curBand) return NSOrderedAscending;
        if (p1.curBand < p2.curBand) return NSOrderedDescending;
        if (p1.pid < p2.pid) return NSOrderedAscending;
        if (p1.pid > p2.pid) return NSOrderedDescending;
        return NSOrderedSame;
    }];
    self.processes = [matched copy];
    [self.tableView reloadData];
}

- (void)refreshBands {
    UIActivityIndicatorView *spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
    [spinner startAnimating];
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithCustomView:spinner];

    __weak typeof(self) weakSelf = self;
    [JPBandsSnapshot requestSnapshotWithCompletion:^(JPBandsSnapshot *snap) {
        weakSelf.snapshot = snap;
        [weakSelf reloadProcessesFromSnapshot];
        UIBarButtonItem *item = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh target:weakSelf action:@selector(refreshBands)];
        weakSelf.navigationItem.rightBarButtonItem = item;
    }];
}

#pragma mark - Table View

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    return 1;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    return (self.processes.count > 0) ? self.processes.count : 1;
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    if (self.targetBundleId.length == 0) {
        return @"活跃进程";
    }
    if (self.snapshot.updatedTime.length > 0) {
        return [NSString stringWithFormat:@"快照时间：%@（%lu 个进程）", self.snapshot.updatedTime, (unsigned long)self.processes.count];
    }
    return self.processes.count > 0 ? [NSString stringWithFormat:@"活跃进程（%lu 个）", (unsigned long)self.processes.count] : @"活跃进程";
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    return @"档位会随状态变化（挂起 0、刚切后台 30–40、收推送时会被拉高），这里只是此刻的值。原档位未知 = 守护进程重启前就接管了。";
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    static NSString *const kCellId = @"JPProcessBandCell";
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:kCellId];
    if (!cell) {
        cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:kCellId];
    }
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.textLabel.numberOfLines = 0;
    cell.detailTextLabel.numberOfLines = 0;

    if (self.processes.count == 0) {
        cell.textLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightMedium];
        cell.textLabel.textColor = UIColor.secondaryLabelColor;
        if (self.targetBundleId.length == 0) {
            cell.textLabel.text = @"无法确定这条规则对应的 App";
            cell.detailTextLabel.text = @"";
        } else if (self.snapshot.timedOut) {
            cell.textLabel.text = @"守护进程未响应";
            cell.detailTextLabel.text = @"获取档位快照超时";
        } else if (!self.snapshot) {
            cell.textLabel.text = @"正在获取快照…";
            cell.detailTextLabel.text = @"请稍候";
        } else {
            cell.textLabel.text = @"未检测到运行中的进程";
            cell.detailTextLabel.text = @"当前 App 没有处于运行中的主程序或扩展进程";
        }
        cell.detailTextLabel.textColor = UIColor.tertiaryLabelColor;
        return cell;
    }

    JPBandsProcess *bp = self.processes[indexPath.row];
    BOOL isMain = [bp.procLabel isEqualToString:@"main"];
    NSString *name = isMain ? [NSString stringWithFormat:@"%@ (主程序)", self.appName ?: @"主程序"] : [NSString stringWithFormat:@"%@ (扩展)", bp.procLabel];
    cell.textLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightMedium];
    cell.textLabel.textColor = UIColor.labelColor;
    cell.textLabel.text = name;

    NSString *origStr = bp.hasOrig ? [NSString stringWithFormat:@"原 %ld", (long)bp.origBand] : @"原档位未知";
    NSString *memStr = bp.hasFp ? [NSString stringWithFormat:@"%ld MB", (long)bp.fpMb] : @"内存未知";
    cell.detailTextLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightRegular];
    cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
    cell.detailTextLabel.text = [NSString stringWithFormat:@"pid %d · 现 %ld · %@ · %@",
                                 (int)bp.pid, (long)bp.curBand, origStr, memStr];

    return cell;
}

@end

#pragma mark - Controllers: Rule Edit

@protocol JPRuleEditViewControllerDelegate <NSObject>
- (void)ruleEditViewControllerDidSave:(JPRule *)rule;
- (void)ruleEditViewControllerDidDelete:(JPRule *)rule;
@end

@interface JPRuleEditViewController : UIViewController <UITableViewDataSource, UITableViewDelegate>
@property (nonatomic, strong) JPRule *rule;
@property (nonatomic, copy) NSArray<JPInstalledApp *> *installedApps;
@property (nonatomic, copy) NSArray<JPRule *> *existingRules;
@property (nonatomic, weak) id<JPRuleEditViewControllerDelegate> delegate;

@property (nonatomic, strong) UITableView *tableView;
@property (nonatomic, assign) NSInteger currentBand;
@property (nonatomic, copy) NSString *currentFrag;
@property (nonatomic, copy) NSString *currentDisplayName;
@property (nonatomic, strong) UILabel *sliderValLabel;
@property (nonatomic, strong) UISlider *slider;
@property (nonatomic, strong) UILabel *auxTipLabel;
@property (nonatomic, strong) JPBandsSnapshot *bandsSnapshot;
@end

@implementation JPRuleEditViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"编辑规则";
    self.view.backgroundColor = UIColor.systemGroupedBackgroundColor;

    self.currentBand = self.rule.targetBand;
    if (self.rule.type == JPRuleTypeProtect) {
        if (self.currentBand < 30) self.currentBand = 30;
        if (self.currentBand > 95) self.currentBand = 95;
    } else {
        if (self.currentBand < 0) self.currentBand = 0;
        if (self.currentBand > 30) self.currentBand = 30;
    }
    self.currentFrag = self.rule.pathFragment ?: @"";
    self.currentDisplayName = self.rule.displayName ?: @"";

    self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"取消"
                                                                             style:UIBarButtonItemStylePlain
                                                                            target:self
                                                                            action:@selector(cancelTapped)];
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"保存"
                                                                              style:UIBarButtonItemStyleDone
                                                                             target:self
                                                                             action:@selector(saveTapped)];

    self.tableView = [[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStyleInsetGrouped];
    self.tableView.translatesAutoresizingMaskIntoConstraints = NO;
    self.tableView.dataSource = self;
    self.tableView.delegate = self;
    [self.view addSubview:self.tableView];

    [NSLayoutConstraint activateConstraints:@[
        [self.tableView.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [self.tableView.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [self.tableView.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [self.tableView.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor]
    ]];

    __weak typeof(self) weakSelf = self;
    [JPBandsSnapshot requestSnapshotWithCompletion:^(JPBandsSnapshot *snapshot) {
        __strong typeof(weakSelf) strongSelf = weakSelf;
        if (!strongSelf) return;
        strongSelf.bandsSnapshot = snapshot;
        [strongSelf.tableView reloadData];
    }];
}

- (void)cancelTapped {
    [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)saveTapped {
    self.rule.pathFragment = self.currentFrag;
    self.rule.displayName = self.currentDisplayName;
    self.rule.targetBand = self.currentBand;
    self.rule.isConflict = NO;
    self.rule.conflictAppNames = nil;
    if ([self.delegate respondsToSelector:@selector(ruleEditViewControllerDidSave:)]) {
        [self.delegate ruleEditViewControllerDidSave:self.rule];
    }
    [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)reselectApp {
    JPAppPickerViewController *picker = [JPAppPickerViewController new];
    picker.allApps = self.installedApps;
    picker.existingRules = self.existingRules;
    __weak typeof(self) weakSelf = self;
    picker.onSelectApp = ^(NSString *pathFragment, NSString *displayName) {
        weakSelf.currentFrag = pathFragment;
        weakSelf.currentDisplayName = displayName;
        [weakSelf.tableView reloadData];
    };
    UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:picker];
    if (@available(iOS 15.0, *)) {
        UISheetPresentationController *sheet = nav.sheetPresentationController;
        sheet.detents = @[UISheetPresentationControllerDetent.mediumDetent, UISheetPresentationControllerDetent.largeDetent];
        sheet.prefersGrabberVisible = YES;
    }
    [self presentViewController:nav animated:YES completion:nil];
}

- (void)confirmDelete {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"移除规则"
                                                                   message:[NSString stringWithFormat:@"确定要移除对「%@」的规则吗？", self.currentDisplayName]
                                                            preferredStyle:UIAlertControllerStyleActionSheet];
    __weak typeof(self) weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"删除" style:UIAlertActionStyleDestructive handler:^(UIAlertAction * _Nonnull action) {
        if ([weakSelf.delegate respondsToSelector:@selector(ruleEditViewControllerDidDelete:)]) {
            [weakSelf.delegate ruleEditViewControllerDidDelete:weakSelf.rule];
        }
        [weakSelf dismissViewControllerAnimated:YES completion:nil];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

#pragma mark - Table View

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    return 3;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    if (section == 0) return 4;
    if (section == 1) return 1;
    return 1;
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    if (section == 0) return @"应用信息";
    if (section == 1) {
        return (self.rule.type == JPRuleTypeProtect) ? @"目标保护档位（步进 1）" : @"目标降级档位（步进 1）";
    }
    return nil;
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    if (section == 0) {
        return @"档位会随状态变化（挂起 0、刚切后台 30–40、收推送时会被拉高），这里只是此刻的值。原档位未知 = 守护进程重启前就接管了。";
    }
    return nil;
}

- (void)updateBandDisplay {
    self.sliderValLabel.text = BandDescription(self.currentBand);
    if (self.rule.type == JPRuleTypeProtect) {
        self.auxTipLabel.text = @"90 是系统前台辅助档。内存释放触发时，这些 App 会被临时让到 80 约 5 秒。";
        self.auxTipLabel.hidden = (self.currentBand < 90);
    } else {
        self.auxTipLabel.text = @"0 档最先被杀。靠推送唤醒的 App（如微信）会被系统反复拉高，再被压回 0，不建议用。";
        self.auxTipLabel.hidden = (self.currentBand != 0);
    }
}

- (void)bandMinusTapped {
    self.currentBand -= 1;
    NSInteger minBand = (self.rule.type == JPRuleTypeProtect) ? 30 : 0;
    if (self.currentBand < minBand) self.currentBand = minBand;
    self.slider.value = (float)self.currentBand;
    [self updateBandDisplay];
}

- (void)bandPlusTapped {
    self.currentBand += 1;
    NSInteger maxBand = (self.rule.type == JPRuleTypeProtect) ? 95 : 30;
    if (self.currentBand > maxBand) self.currentBand = maxBand;
    self.slider.value = (float)self.currentBand;
    [self updateBandDisplay];
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    if (indexPath.section == 0) {
        if (indexPath.row == 0) {
            UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
            cell.textLabel.text = @"应用名称";
            cell.detailTextLabel.text = self.currentDisplayName.length > 0 ? self.currentDisplayName : @"未命名";
            cell.selectionStyle = UITableViewCellSelectionStyleNone;
            return cell;
        } else if (indexPath.row == 1) {
            UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
            cell.textLabel.text = @"规则标识";
            cell.detailTextLabel.text = self.currentFrag;
            cell.detailTextLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
            cell.selectionStyle = UITableViewCellSelectionStyleNone;
            return cell;
        } else if (indexPath.row == 2) {
            UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
            cell.textLabel.text = @"当前档位";
            cell.detailTextLabel.numberOfLines = 0;
            NSString *desc = nil;
            if (self.bandsSnapshot) {
                if (self.bandsSnapshot.timedOut) {
                    desc = @"守护进程未响应";
                } else {
                    NSString *targetBid = nil;
                    if ([self.currentFrag hasPrefix:@"bid:"]) {
                        targetBid = [self.currentFrag substringFromIndex:4];
                    } else {
                        for (JPInstalledApp *ia in self.installedApps) {
                            if ([ia.pathFragment isEqualToString:self.currentFrag]) {
                                targetBid = ia.bundleId;
                                break;
                            }
                        }
                    }
                    JPBandsProcess *matchedProc = nil;
                    if (targetBid.length > 0) {
                        matchedProc = [self.bandsSnapshot bestProcessForBundleId:targetBid];
                    }
                    if (matchedProc) {
                        desc = [matchedProc bandDescription];
                    } else {
                        desc = @"未运行";
                    }
                }
            } else {
                desc = @"获取中…";
            }
            cell.detailTextLabel.text = desc;
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
            cell.selectionStyle = UITableViewCellSelectionStyleDefault;
            return cell;
        } else {
            UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
            cell.textLabel.text = @"重新选择 App";
            cell.textLabel.textColor = UIColor.systemBlueColor;
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
            return cell;
        }
    } else if (indexPath.section == 1) {
        UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
        cell.selectionStyle = UITableViewCellSelectionStyleNone;

        self.sliderValLabel = [UILabel new];
        self.sliderValLabel.translatesAutoresizingMaskIntoConstraints = NO;
        self.sliderValLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightMedium];
        self.sliderValLabel.textColor = UIColor.systemBlueColor;
        self.sliderValLabel.textAlignment = NSTextAlignmentCenter;

        self.slider = [UISlider new];
        self.slider.translatesAutoresizingMaskIntoConstraints = NO;
        self.slider.minimumValue = (self.rule.type == JPRuleTypeProtect) ? 30.0f : 0.0f;
        self.slider.maximumValue = (self.rule.type == JPRuleTypeProtect) ? 95.0f : 30.0f;
        self.slider.value = (float)self.currentBand;
        [self.slider addTarget:self action:@selector(sliderMoved:) forControlEvents:UIControlEventValueChanged];

        self.auxTipLabel = [UILabel new];
        self.auxTipLabel.translatesAutoresizingMaskIntoConstraints = NO;
        self.auxTipLabel.font = [UIFont systemFontOfSize:12 weight:UIFontWeightRegular];
        self.auxTipLabel.textColor = UIColor.systemOrangeColor;
        self.auxTipLabel.numberOfLines = 0;

        [self updateBandDisplay];

        UIButton *minusBtn = [UIButton buttonWithType:UIButtonTypeSystem];
        minusBtn.translatesAutoresizingMaskIntoConstraints = NO;
        [minusBtn setTitle:@"-1" forState:UIControlStateNormal];
        minusBtn.titleLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightMedium];
        [minusBtn addTarget:self action:@selector(bandMinusTapped) forControlEvents:UIControlEventTouchUpInside];

        UIButton *plusBtn = [UIButton buttonWithType:UIButtonTypeSystem];
        plusBtn.translatesAutoresizingMaskIntoConstraints = NO;
        [plusBtn setTitle:@"+1" forState:UIControlStateNormal];
        plusBtn.titleLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightMedium];
        [plusBtn addTarget:self action:@selector(bandPlusTapped) forControlEvents:UIControlEventTouchUpInside];

        [cell.contentView addSubview:minusBtn];
        [cell.contentView addSubview:self.sliderValLabel];
        [cell.contentView addSubview:plusBtn];
        [cell.contentView addSubview:self.slider];
        [cell.contentView addSubview:self.auxTipLabel];

        [NSLayoutConstraint activateConstraints:@[
            [minusBtn.leadingAnchor constraintEqualToAnchor:cell.contentView.leadingAnchor constant:16],
            [minusBtn.topAnchor constraintEqualToAnchor:cell.contentView.topAnchor constant:10],
            [minusBtn.widthAnchor constraintEqualToConstant:44],
            [minusBtn.heightAnchor constraintEqualToConstant:32],

            [plusBtn.trailingAnchor constraintEqualToAnchor:cell.contentView.trailingAnchor constant:-16],
            [plusBtn.centerYAnchor constraintEqualToAnchor:minusBtn.centerYAnchor],
            [plusBtn.widthAnchor constraintEqualToConstant:44],
            [plusBtn.heightAnchor constraintEqualToConstant:32],

            [self.sliderValLabel.centerYAnchor constraintEqualToAnchor:minusBtn.centerYAnchor],
            [self.sliderValLabel.leadingAnchor constraintEqualToAnchor:minusBtn.trailingAnchor constant:8],
            [self.sliderValLabel.trailingAnchor constraintEqualToAnchor:plusBtn.leadingAnchor constant:-8],

            [self.slider.topAnchor constraintEqualToAnchor:minusBtn.bottomAnchor constant:8],
            [self.slider.leadingAnchor constraintEqualToAnchor:cell.contentView.leadingAnchor constant:16],
            [self.slider.trailingAnchor constraintEqualToAnchor:cell.contentView.trailingAnchor constant:-16],

            [self.auxTipLabel.topAnchor constraintEqualToAnchor:self.slider.bottomAnchor constant:8],
            [self.auxTipLabel.leadingAnchor constraintEqualToAnchor:cell.contentView.leadingAnchor constant:16],
            [self.auxTipLabel.trailingAnchor constraintEqualToAnchor:cell.contentView.trailingAnchor constant:-16],
            [self.auxTipLabel.bottomAnchor constraintEqualToAnchor:cell.contentView.bottomAnchor constant:-12]
        ]];
        return cell;
    } else {
        UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
        cell.textLabel.text = @"删除规则";
        cell.textLabel.textColor = UIColor.systemRedColor;
        cell.textLabel.textAlignment = NSTextAlignmentCenter;
        return cell;
    }
}

- (void)sliderMoved:(UISlider *)slider {
    NSInteger stepped = (NSInteger)lroundf(slider.value);
    NSInteger minBand = (self.rule.type == JPRuleTypeProtect) ? 30 : 0;
    NSInteger maxBand = (self.rule.type == JPRuleTypeProtect) ? 95 : 30;
    if (stepped < minBand) stepped = minBand;
    if (stepped > maxBand) stepped = maxBand;
    self.currentBand = stepped;
    [self updateBandDisplay];
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    if (indexPath.section == 0 && indexPath.row == 2) {
        NSString *targetBid = nil;
        if ([self.currentFrag hasPrefix:@"bid:"]) {
            targetBid = [self.currentFrag substringFromIndex:4];
        } else {
            for (JPInstalledApp *ia in self.installedApps) {
                if ([ia.pathFragment isEqualToString:self.currentFrag]) {
                    targetBid = ia.bundleId;
                    break;
                }
            }
        }
        JPProcessBandsViewController *vc = [JPProcessBandsViewController new];
        vc.targetBundleId = targetBid;
        vc.appName = self.currentDisplayName.length > 0 ? self.currentDisplayName : self.currentFrag;
        vc.snapshot = self.bandsSnapshot;
        [self.navigationController pushViewController:vc animated:YES];
    } else if (indexPath.section == 0 && indexPath.row == 3) {
        [self reselectApp];
    } else if (indexPath.section == 2) {
        [self confirmDelete];
    }
}

@end

#pragma mark - Controllers: Main ViewController

@interface MainViewController : UIViewController <JPRuleCardViewDelegate, JPRuleEditViewControllerDelegate>
@property (nonatomic, strong) NSMutableArray<JPRule *> *rules;
@property (nonatomic, copy) NSArray<JPInstalledApp *> *installedApps;
@property (nonatomic, copy) NSString *logLevel;
@property (nonatomic, strong) NSMutableArray<NSString *> *configExtraLines;

@property (nonatomic, strong) UIScrollView *scrollView;
@property (nonatomic, strong) UIStackView *contentStackView;

@property (nonatomic, strong) JPStatusCardView *statusCard;
@property (nonatomic, strong) JPKilledStatsCardView *killedStatsCard;
@property (nonatomic, strong) dispatch_queue_t killStatsQueue;
@property (nonatomic, assign) NSInteger killStatsGeneration;
@property (nonatomic, copy) NSString *killStatsLastSig;
@property (nonatomic, strong) UILabel *rulesSectionHeaderLabel;
@property (nonatomic, strong) UIStackView *rulesStackView;
@property (nonatomic, strong) UIView *addRuleCard;

@property (nonatomic, strong) UIView *saveNoticeToast;
@property (nonatomic, strong) NSTimer *saveNoticeTimer;
@property (nonatomic, strong) NSTimer *pollingTimer;
@property (nonatomic, strong) UIButton *deadBanner;
@property (nonatomic, strong) UIButton *versionWarningBanner;
@property (nonatomic, assign) CFAbsoluteTime lastSpawnTime;
@property (nonatomic, assign) BOOL migrationAttemptedThisSession;
@property (nonatomic, strong) JPStatusInfo *lastLoadedStatus;
@property (nonatomic, assign) BOOL restartInProgress;
@end

static BOOL s_restartInProgress = NO;

static BOOL JPIsPidJetpilotd(pid_t pid) {
    if (pid <= 0) return NO;
    char path[PROC_PIDPATHINFO_MAXSIZE];
    memset(path, 0, sizeof(path));
    if (proc_pidpath(pid, path, sizeof(path)) <= 0) {
        return NO;
    }
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    return (strcmp(name, "jetpilotd") == 0);
}

static BOOL JPWaitForDaemonExit(pid_t pid) {
    if (pid <= 0) return YES;
    for (int attempts = 0; attempts < 300; attempts++) {
        if (!JPIsPidJetpilotd(pid)) {
            return YES;
        }
        // 非 ESRCH/proc_pidpath 失败时（如 mobile 查 root 进程权限未知）不得当作进程已走，
        // 须用 pid 文件判断旧进程退出：旧守护进程退出时仅当 pid 文件里还是自己的 pid 才 unlink，
        // 所以 pid 文件消失或内容变化即说明旧进程已走。
        NSString *curPidStr = [NSString stringWithContentsOfFile:kPidFile encoding:NSUTF8StringEncoding error:nil];
        if (curPidStr == nil) {
            return YES;
        }
        NSString *trimmed = [curPidStr stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        if (trimmed.length == 0 || trimmed.intValue != pid) {
            return YES;
        }
        usleep(100000);
    }
    return NO;
}

@implementation MainViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"进程优先级";
    self.view.backgroundColor = UIColor.systemGroupedBackgroundColor;
    self.navigationController.navigationBar.prefersLargeTitles = YES;

    self.rules = [NSMutableArray array];
    self.killStatsQueue = dispatch_queue_create("com.lili.jetpilot.killstats", DISPATCH_QUEUE_SERIAL);

    NSFileManager *fm = NSFileManager.defaultManager;
    [fm createDirectoryAtPath:kDir withIntermediateDirectories:YES
                   attributes:@{NSFilePosixPermissions: @0777} error:nil];
    if (![fm fileExistsAtPath:kConfig]) {
        [DefaultConfig() writeToFile:kConfig atomically:YES encoding:NSUTF8StringEncoding error:nil];
    }

    self.installedApps = [JPInstalledApp scanInstalledApps];
    [self loadConfig];

    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"arrow.clockwise"]
                                                                              style:UIBarButtonItemStylePlain
                                                                             target:self
                                                                             action:@selector(refreshStatus)];

    [self setupScrollView];
    [self setupSaveNoticeToast];
    [self rebuildRulesCards];
    [self refreshStatus];

    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(refreshStatus)
                                                 name:UIApplicationDidBecomeActiveNotification
                                               object:nil];
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(stopPollingTimer)
                                                 name:UIApplicationDidEnterBackgroundNotification
                                               object:nil];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self loadConfig];
    [self rebuildRulesCards];
    [self refreshStatus];
    [self startPollingTimer];
}

- (void)viewWillDisappear:(BOOL)animated {
    [super viewWillDisappear:animated];
    [self stopPollingTimer];
}

- (void)dealloc {
    [self stopPollingTimer];
    [self.saveNoticeTimer invalidate];
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

#pragma mark - Timer

- (void)startPollingTimer {
    [self stopPollingTimer];
    __weak typeof(self) weakSelf = self;
    self.pollingTimer = [NSTimer scheduledTimerWithTimeInterval:3.0 repeats:YES block:^(NSTimer * _Nonnull timer) {
        [weakSelf refreshStatus];
    }];
}

- (void)stopPollingTimer {
    [self.pollingTimer invalidate];
    self.pollingTimer = nil;
}

#pragma mark - UI Setup

- (void)setupScrollView {
    self.scrollView = [UIScrollView new];
    self.scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    self.scrollView.alwaysBounceVertical = YES;
    [self.view addSubview:self.scrollView];

    self.contentStackView = [UIStackView new];
    self.contentStackView.translatesAutoresizingMaskIntoConstraints = NO;
    self.contentStackView.axis = UILayoutConstraintAxisVertical;
    self.contentStackView.spacing = 16;
    [self.scrollView addSubview:self.contentStackView];

    [NSLayoutConstraint activateConstraints:@[
        [self.scrollView.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [self.scrollView.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [self.scrollView.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [self.scrollView.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],

        [self.contentStackView.topAnchor constraintEqualToAnchor:self.scrollView.contentLayoutGuide.topAnchor constant:12],
        [self.contentStackView.leadingAnchor constraintEqualToAnchor:self.scrollView.contentLayoutGuide.leadingAnchor constant:16],
        [self.contentStackView.trailingAnchor constraintEqualToAnchor:self.scrollView.contentLayoutGuide.trailingAnchor constant:-16],
        [self.contentStackView.bottomAnchor constraintEqualToAnchor:self.scrollView.contentLayoutGuide.bottomAnchor constant:-32],
        [self.contentStackView.widthAnchor constraintEqualToAnchor:self.scrollView.frameLayoutGuide.widthAnchor constant:-32]
    ]];

    // 0. 守护进程异常未运行时的红条（默认隐藏，UIStackView 自动收起）
    self.deadBanner = [UIButton buttonWithType:UIButtonTypeSystem];
    self.deadBanner.translatesAutoresizingMaskIntoConstraints = NO;
    self.deadBanner.backgroundColor = UIColor.systemRedColor;
    self.deadBanner.layer.cornerRadius = 12;
    self.deadBanner.layer.cornerCurve = kCACornerCurveContinuous;
    [self.deadBanner setTitle:@"守护进程未运行，点击启动" forState:UIControlStateNormal];
    [self.deadBanner setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    self.deadBanner.titleLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
    [self.deadBanner.heightAnchor constraintEqualToConstant:44].active = YES;
    [self.deadBanner addTarget:self action:@selector(startDaemon) forControlEvents:UIControlEventTouchUpInside];
    self.deadBanner.hidden = YES;
    [self.contentStackView addArrangedSubview:self.deadBanner];

    self.versionWarningBanner = [UIButton buttonWithType:UIButtonTypeSystem];
    self.versionWarningBanner.translatesAutoresizingMaskIntoConstraints = NO;
    self.versionWarningBanner.backgroundColor = UIColor.systemRedColor;
    self.versionWarningBanner.layer.cornerRadius = 12;
    self.versionWarningBanner.layer.cornerCurve = kCACornerCurveContinuous;
    [self.versionWarningBanner setTitle:@"守护进程不是当前版本，请重启。点击重启守护进程。" forState:UIControlStateNormal];
    [self.versionWarningBanner setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    self.versionWarningBanner.titleLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightMedium];
    self.versionWarningBanner.titleLabel.numberOfLines = 0;
    self.versionWarningBanner.titleLabel.textAlignment = NSTextAlignmentCenter;
    self.versionWarningBanner.contentEdgeInsets = UIEdgeInsetsMake(10, 14, 10, 14);
    [self.versionWarningBanner addTarget:self action:@selector(restartDaemon) forControlEvents:UIControlEventTouchUpInside];
    self.versionWarningBanner.hidden = YES;
    [self.contentStackView addArrangedSubview:self.versionWarningBanner];

    // 1. Status Section
    UILabel *statusHeader = [self createSectionHeader:@"运行状态"];
    self.statusCard = [JPStatusCardView new];
    self.statusCard.userInteractionEnabled = YES;
    UILongPressGestureRecognizer *statusLongPress = [[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(handleStatusCardLongPress:)];
    statusLongPress.minimumPressDuration = 0.8;
    [self.statusCard addGestureRecognizer:statusLongPress];
    __weak typeof(self) weakSelf = self;
    self.statusCard.onActionToggle = ^{
        [weakSelf toggleDaemon];
    };
    self.statusCard.onTapProtected = ^{
        [weakSelf showProtectedList];
    };
    self.statusCard.onTapKilledStats = ^{
        [weakSelf scrollToKilledStatsCard];
    };

    [self.contentStackView addArrangedSubview:statusHeader];
    [self.contentStackView addArrangedSubview:self.statusCard];

    // 2. Rules Section
    self.rulesSectionHeaderLabel = [self createSectionHeader:@"保护规则"];
    UILabel *rulesNote = [UILabel new];
    rulesNote.translatesAutoresizingMaskIntoConstraints = NO;
    rulesNote.font = [UIFont systemFontOfSize:13 weight:UIFontWeightRegular];
    rulesNote.textColor = UIColor.secondaryLabelColor;
    rulesNote.numberOfLines = 0;
    rulesNote.text = @"数值越大越晚被杀。内存真不够时，所有 App 都可能被杀，包括排第一的。";

    self.rulesStackView = [UIStackView new];
    self.rulesStackView.translatesAutoresizingMaskIntoConstraints = NO;
    self.rulesStackView.axis = UILayoutConstraintAxisVertical;
    self.rulesStackView.spacing = 12;

    [self.contentStackView addArrangedSubview:self.rulesSectionHeaderLabel];
    [self.contentStackView addArrangedSubview:rulesNote];
    [self.contentStackView addArrangedSubview:self.rulesStackView];

    // 3. Add Rule Card
    self.addRuleCard = [self createAddRuleCard];
    [self.contentStackView addArrangedSubview:self.addRuleCard];

    // 4. Killed Stats Section（卡片挪到主页最底部，放在「关于」上面）
    self.killedStatsCard = [JPKilledStatsCardView new];
    [self.contentStackView addArrangedSubview:self.killedStatsCard];

    UILabel *aboutHeader = [self createSectionHeader:@"关于"];
    [self.contentStackView addArrangedSubview:aboutHeader];
    UIView *aboutCard = [self createAboutCard];
    [self.contentStackView addArrangedSubview:aboutCard];

    UILabel *footer = [UILabel new];
    footer.translatesAutoresizingMaskIntoConstraints = NO;
    footer.font = [UIFont systemFontOfSize:12 weight:UIFontWeightRegular];
    footer.textColor = UIColor.tertiaryLabelColor;
    footer.textAlignment = NSTextAlignmentCenter;
    footer.numberOfLines = 0;
    footer.text = @"改动实时自动保存 · 亮屏约 10 秒内，锁屏最长约 60 秒生效";
    [self.contentStackView addArrangedSubview:footer];
}

- (UILabel *)createSectionHeader:(NSString *)title {
    UILabel *lbl = [UILabel new];
    lbl.translatesAutoresizingMaskIntoConstraints = NO;
    lbl.font = [UIFont systemFontOfSize:13 weight:UIFontWeightMedium];
    lbl.textColor = UIColor.secondaryLabelColor;
    lbl.text = title;
    return lbl;
}

- (UIView *)createAddRuleCard {
    UIView *card = [UIView new];
    card.translatesAutoresizingMaskIntoConstraints = NO;
    card.backgroundColor = UIColor.secondarySystemGroupedBackgroundColor;
    card.layer.cornerRadius = 14;
    card.layer.cornerCurve = kCACornerCurveContinuous;
    card.clipsToBounds = YES;

    UIImageView *icon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"plus.circle.fill"]];
    icon.translatesAutoresizingMaskIntoConstraints = NO;
    icon.tintColor = UIColor.systemBlueColor;

    UILabel *title = [UILabel new];
    title.translatesAutoresizingMaskIntoConstraints = NO;
    title.font = [UIFont systemFontOfSize:16 weight:UIFontWeightSemibold];
    title.textColor = UIColor.systemBlueColor;
    title.text = @"添加规则";

    UIImageView *chev = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"chevron.right"]];
    chev.translatesAutoresizingMaskIntoConstraints = NO;
    chev.tintColor = UIColor.tertiaryLabelColor;

    [card addSubview:icon];
    [card addSubview:title];
    [card addSubview:chev];

    [NSLayoutConstraint activateConstraints:@[
        [icon.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:16],
        [icon.centerYAnchor constraintEqualToAnchor:card.centerYAnchor],
        [icon.widthAnchor constraintEqualToConstant:22],
        [icon.heightAnchor constraintEqualToConstant:22],

        [title.leadingAnchor constraintEqualToAnchor:icon.trailingAnchor constant:10],
        [title.centerYAnchor constraintEqualToAnchor:card.centerYAnchor],

        [chev.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-16],
        [chev.centerYAnchor constraintEqualToAnchor:card.centerYAnchor],
        [chev.widthAnchor constraintEqualToConstant:12],
        [chev.heightAnchor constraintEqualToConstant:14],

        [card.heightAnchor constraintEqualToConstant:52]
    ]];

    UITapGestureRecognizer *tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(showAddRuleSheet)];
    [card addGestureRecognizer:tap];
    card.userInteractionEnabled = YES;
    return card;
}


- (UIView *)createAboutCard {
    UIView *card = [UIView new];
    card.translatesAutoresizingMaskIntoConstraints = NO;
    card.backgroundColor = UIColor.secondarySystemGroupedBackgroundColor;
    card.layer.cornerRadius = 14;
    card.layer.cornerCurve = kCACornerCurveContinuous;
    card.clipsToBounds = YES;

    UIStackView *stack = [UIStackView new];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 8;
    stack.alignment = UIStackViewAlignmentLeading;

    UILabel *versionLabel = [UILabel new];
    versionLabel.translatesAutoresizingMaskIntoConstraints = NO;
    versionLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightMedium];
    versionLabel.textColor = UIColor.labelColor;
    NSString *version = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"";
    NSString *build = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleVersion"] ?: @"";
    NSString *commit = [NSString stringWithUTF8String:JP_COMMIT] ?: @"dev";
    if (build.length > 0) {
        versionLabel.text = [NSString stringWithFormat:@"JetPilot v%@ (%@) · %@", version, build, commit];
    } else {
        versionLabel.text = [NSString stringWithFormat:@"JetPilot v%@ · %@", version, commit];
    }

    UIButton *repoButton = [UIButton buttonWithType:UIButtonTypeSystem];
    repoButton.translatesAutoresizingMaskIntoConstraints = NO;
    [repoButton setTitle:@"开源仓库：github.com/csjoyxy/jetpilot" forState:UIControlStateNormal];
    [repoButton setTitleColor:UIColor.systemBlueColor forState:UIControlStateNormal];
    repoButton.titleLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightRegular];
    repoButton.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeft;
    [repoButton addTarget:self action:@selector(openGitHubRepo) forControlEvents:UIControlEventTouchUpInside];

    UIButton *logButton = [UIButton buttonWithType:UIButtonTypeSystem];
    logButton.translatesAutoresizingMaskIntoConstraints = NO;
    [logButton setTitle:@"调试日志 ›" forState:UIControlStateNormal];
    logButton.titleLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightRegular];
    logButton.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeft;
    [logButton addTarget:self action:@selector(openLogViewer) forControlEvents:UIControlEventTouchUpInside];

    [stack addArrangedSubview:versionLabel];
    [stack addArrangedSubview:repoButton];
    [stack addArrangedSubview:logButton];
    [card addSubview:stack];

    [NSLayoutConstraint activateConstraints:@[
        [stack.topAnchor constraintEqualToAnchor:card.topAnchor constant:12],
        [stack.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:12],
        [stack.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-12],
        [stack.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-12]
    ]];

    return card;
}

- (void)openGitHubRepo {
    [[UIApplication sharedApplication] openURL:[NSURL URLWithString:@"https://github.com/csjoyxy/jetpilot"] options:@{} completionHandler:nil];
}

- (void)openLogViewer {
    JPLogViewController *vc = [JPLogViewController new];
    vc.logLevel = self.logLevel.length > 0 ? self.logLevel : @"info";
    __weak typeof(self) weakSelf = self;
    __weak typeof(vc) weakVC = vc;
    vc.onLogLevelChanged = ^(NSString *newLevel) {
        NSString *oldLevel = weakSelf.logLevel;
        weakSelf.logLevel = newLevel;
        NSError *err = nil;
        if (![weakSelf saveConfigWithError:&err]) {
            weakSelf.logLevel = oldLevel;
            weakVC.logLevel = oldLevel;
            [weakVC updateLevelMenu];
            UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"保存失败"
                                                                           message:[NSString stringWithFormat:@"日志等级保存失败：%@", err.localizedDescription ?: @"写入错误"]
                                                                    preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
            [weakVC presentViewController:alert animated:YES completion:nil];
        }
    };
    if (self.navigationController) {
        [self.navigationController pushViewController:vc animated:YES];
    } else {
        UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:vc];
        vc.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"关闭"
                                                                               style:UIBarButtonItemStylePlain
                                                                              target:self
                                                                              action:@selector(dismissPresentedController)];
        [self presentViewController:nav animated:YES completion:nil];
    }
}

- (void)setupSaveNoticeToast {
    self.saveNoticeToast = [UIView new];
    self.saveNoticeToast.translatesAutoresizingMaskIntoConstraints = NO;
    self.saveNoticeToast.backgroundColor = [UIColor colorWithDynamicProvider:^UIColor * _Nonnull(UITraitCollection * _Nonnull tc) {
        if (tc.userInterfaceStyle == UIUserInterfaceStyleDark) {
            return [UIColor colorWithWhite:0.25 alpha:0.96];
        } else {
            return [UIColor colorWithWhite:0.95 alpha:0.96];
        }
    }];
    self.saveNoticeToast.layer.cornerRadius = 18;
    self.saveNoticeToast.layer.cornerCurve = kCACornerCurveContinuous;
    self.saveNoticeToast.layer.shadowColor = UIColor.blackColor.CGColor;
    self.saveNoticeToast.layer.shadowOpacity = 0.12;
    self.saveNoticeToast.layer.shadowOffset = CGSizeMake(0, 3);
    self.saveNoticeToast.layer.shadowRadius = 8;
    self.saveNoticeToast.userInteractionEnabled = NO;
    self.saveNoticeToast.alpha = 0.0;

    UIImageView *icon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"checkmark.circle.fill"]];
    icon.translatesAutoresizingMaskIntoConstraints = NO;
    icon.tintColor = UIColor.systemGreenColor;

    UILabel *lbl = [UILabel new];
    lbl.translatesAutoresizingMaskIntoConstraints = NO;
    lbl.font = [UIFont systemFontOfSize:13 weight:UIFontWeightMedium];
    lbl.textColor = UIColor.labelColor;
    lbl.text = @"改动已保存 · 亮屏约 10 秒内，锁屏最长约 60 秒生效";

    [self.saveNoticeToast addSubview:icon];
    [self.saveNoticeToast addSubview:lbl];

    [NSLayoutConstraint activateConstraints:@[
        [icon.leadingAnchor constraintEqualToAnchor:self.saveNoticeToast.leadingAnchor constant:14],
        [icon.centerYAnchor constraintEqualToAnchor:self.saveNoticeToast.centerYAnchor],
        [icon.widthAnchor constraintEqualToConstant:18],
        [icon.heightAnchor constraintEqualToConstant:18],

        [lbl.leadingAnchor constraintEqualToAnchor:icon.trailingAnchor constant:8],
        [lbl.trailingAnchor constraintEqualToAnchor:self.saveNoticeToast.trailingAnchor constant:-14],
        [lbl.centerYAnchor constraintEqualToAnchor:self.saveNoticeToast.centerYAnchor],

        [self.saveNoticeToast.heightAnchor constraintEqualToConstant:36]
    ]];

    [self.view addSubview:self.saveNoticeToast];
    [NSLayoutConstraint activateConstraints:@[
        [self.saveNoticeToast.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [self.saveNoticeToast.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor constant:8]
    ]];
}

- (void)showSaveNotice {
    [self.saveNoticeTimer invalidate];
    [self.view bringSubviewToFront:self.saveNoticeToast];
    [UIView animateWithDuration:0.2 animations:^{
        self.saveNoticeToast.alpha = 1.0;
        self.saveNoticeToast.transform = CGAffineTransformIdentity;
    }];

    __weak typeof(self) weakSelf = self;
    self.saveNoticeTimer = [NSTimer scheduledTimerWithTimeInterval:2.5 repeats:NO block:^(NSTimer * _Nonnull timer) {
        __strong typeof(weakSelf) strongSelf = weakSelf;
        if (!strongSelf) return;
        [UIView animateWithDuration:0.25 animations:^{
            strongSelf.saveNoticeToast.alpha = 0.0;
            strongSelf.saveNoticeToast.transform = CGAffineTransformMakeTranslation(0, -8);
        }];
    }];
}

#pragma mark - Config Management

- (NSString *)resolveDisplayNameForFragment:(NSString *)frag {
    return ResolveDisplayNameHelper(frag, self.installedApps);
}

- (void)evaluateRuleConflict:(JPRule *)r {
    if (![r.pathFragment hasPrefix:@"bid:"]) {
        NSMutableArray<NSString *> *matching = [NSMutableArray array];
        for (JPInstalledApp *app in self.installedApps) {
            if ([app.pathFragment isEqualToString:r.pathFragment]) {
                [matching addObject:app.displayName];
            }
        }
        if (matching.count >= 2) {
            r.isConflict = YES;
            r.conflictAppNames = [matching copy];
        } else {
            r.isConflict = NO;
            r.conflictAppNames = nil;
        }
    } else {
        r.isConflict = NO;
        r.conflictAppNames = nil;
    }
}

- (void)loadConfig {
    [self.rules removeAllObjects];
    self.configExtraLines = [NSMutableArray array];
    self.logLevel = @"info";
    NSError *err = nil;
    NSString *content = [NSString stringWithContentsOfFile:kConfig encoding:NSUTF8StringEncoding error:&err];
    if (!content || content.length == 0) {
        content = DefaultConfig();
        [content writeToFile:kConfig atomically:YES encoding:NSUTF8StringEncoding error:nil];
    }

    NSArray<NSString *> *lines = [content componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]];
    for (NSString *rawLine in lines) {
        NSString *line = [rawLine stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if (line.length == 0 || [line hasPrefix:@"#"]) continue;

        NSArray<NSString *> *tokens = [line componentsSeparatedByCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        NSMutableArray<NSString *> *cleanTokens = [NSMutableArray array];
        for (NSString *t in tokens) {
            if (t.length > 0) [cleanTokens addObject:t];
        }
        if (cleanTokens.count == 0) continue;

        NSString *verb = cleanTokens[0];
        if ([verb isEqualToString:@"protect"]) {
            if (cleanTokens.count < 2) continue;
            NSString *frag = cleanTokens[1];
            NSInteger band = 50;
            if (cleanTokens.count >= 3) {
                band = [cleanTokens[2] integerValue];
            }
            if (band <= 0) band = 50;
            if (band < 30) band = 30;
            if (band > 95) band = 95;

            JPRule *r = [JPRule new];
            r.type = JPRuleTypeProtect;
            r.pathFragment = frag;
            r.targetBand = band;
            r.displayName = [self resolveDisplayNameForFragment:frag];
            [self evaluateRuleConflict:r];
            [self.rules addObject:r];
        } else if ([verb isEqualToString:@"demote"]) {
            if (cleanTokens.count < 2) continue;
            NSString *frag = cleanTokens[1];
            NSInteger band = 30;
            if (cleanTokens.count >= 3) {
                band = [cleanTokens[2] integerValue];
            }
            if (band < 0) band = 0;
            if (band > 30) band = 30;

            JPRule *r = [JPRule new];
            r.type = JPRuleTypeDemote;
            r.pathFragment = frag;
            r.targetBand = band;
            r.displayName = [self resolveDisplayNameForFragment:frag];
            [self evaluateRuleConflict:r];
            [self.rules addObject:r];
        } else if ([verb isEqualToString:@"loglevel"]) {
            if (cleanTokens.count >= 2) {
                NSString *val = cleanTokens[1].lowercaseString;
                if ([val isEqualToString:@"off"]) {
                    self.logLevel = @"off";
                } else if ([val isEqualToString:@"error"] || [val isEqualToString:@"err"]) {
                    self.logLevel = @"error";
                } else if ([val isEqualToString:@"warning"] || [val isEqualToString:@"warn"] || [val isEqualToString:@"1"]) {
                    self.logLevel = @"warning";
                } else if ([val isEqualToString:@"info"] || [val isEqualToString:@"2"]) {
                    self.logLevel = @"info";
                } else if ([val isEqualToString:@"debug"] || [val isEqualToString:@"3"]) {
                    self.logLevel = @"debug";
                } else {
                    self.logLevel = @"info";
                }
            } else {
                self.logLevel = @"info";
            }
        } else {
            [self.configExtraLines addObject:rawLine];
        }
    }
}

- (BOOL)saveConfigWithError:(NSError **)errorOut {
    NSMutableString *outStr = [NSMutableString string];
    [outStr appendString:@"# JetPilot 规则（iOS 16 档位：后台 30，前台 100）\n"];
    [outStr appendString:@"# protect <路径片段> <档位，最高 95>\n"];
    [outStr appendString:@"# demote  <路径片段> [档位] （降到指定档位，默认 30）\n"];

    NSString *lvl = self.logLevel.length > 0 ? self.logLevel : @"info";
    [outStr appendFormat:@"loglevel %@\n", lvl];

    for (JPRule *r in self.rules) {
        if (r.type == JPRuleTypeProtect) {
            NSInteger band = r.targetBand;
            if (band < 30) band = 30;
            if (band > 95) band = 95;
            [outStr appendFormat:@"protect %@ %ld\n", r.pathFragment, (long)band];
        } else if (r.type == JPRuleTypeDemote) {
            NSInteger band = r.targetBand;
            if (band < 0) band = 0;
            if (band > 30) band = 30;
            [outStr appendFormat:@"demote %@ %ld\n", r.pathFragment, (long)band];
        }
    }

    for (NSString *extra in self.configExtraLines) {
        [outStr appendFormat:@"%@\n", extra];
    }

    NSError *err = nil;
    BOOL ok = [outStr writeToFile:kConfig atomically:YES encoding:NSUTF8StringEncoding error:&err];
    if (errorOut) *errorOut = err;
    if (ok) {
        [self showSaveNotice];
    }
    return ok;
}

- (void)saveConfig {
    [self saveConfigWithError:nil];
}

- (void)checkAndMigrateLegacyRulesIfNeeded:(JPStatusInfo *)status {
    // 必改 3-1: installedApps 没加载完时先尝试补扫；若仍为空则直接返回，且不置位本次尝试
    if (self.installedApps.count == 0) {
        self.installedApps = [JPInstalledApp scanInstalledApps];
        if (self.installedApps.count > 0) {
            for (JPRule *r in self.rules) {
                [self evaluateRuleConflict:r];
            }
            [self rebuildRulesCards];
        }
    }
    if (self.installedApps.count == 0) return;

    NSFileManager *fm = NSFileManager.defaultManager;
    // 必改 3-4: 撤销持久化标记检查，若用户此前撤销过则不再自动迁移
    if ([fm fileExistsAtPath:kMigrationDeclined]) return;

    if (!status.bidSupport || [self daemonState] != JPDaemonStateRunning) return;
    if (self.migrationAttemptedThisSession) return;
    self.migrationAttemptedThisSession = YES;

    // 先检查是否有可迁移规则，不提前污染 self.rules 内存
    NSMutableArray<NSString *> *migratedNames = [NSMutableArray array];
    NSInteger conflictCount = 0;
    BOOL hasChanges = NO;

    for (JPRule *r in self.rules) {
        if ([r.pathFragment hasPrefix:@"bid:"]) {
            continue; // 已是 bid 的跳过
        }
        NSMutableArray<JPInstalledApp *> *matches = [NSMutableArray array];
        for (JPInstalledApp *app in self.installedApps) {
            if ([app.pathFragment isEqualToString:r.pathFragment]) {
                [matches addObject:app];
            }
        }
        if (matches.count == 1) {
            JPInstalledApp *app = matches.firstObject;
            if (app.bundleId.length > 0) {
                [migratedNames addObject:app.displayName];
                hasChanges = YES;
            }
        } else if (matches.count >= 2) {
            conflictCount++;
        }
    }

    if (!hasChanges) {
        return;
    }

    // 终审: 每次迁移前都覆盖备份，备份含义为"本次迁移之前"，撤销永远只撤这一次
    NSError *backupErr = nil;
    NSString *configContent = [NSString stringWithContentsOfFile:kConfig encoding:NSUTF8StringEncoding error:&backupErr];
    if (!configContent || ![configContent writeToFile:kConfigBackup atomically:YES encoding:NSUTF8StringEncoding error:&backupErr]) {
        NSLog(@"[JetPilot] 规则备份失败，取消迁移: %@", backupErr);
        return;
    }

    // 备份成功后再修改规则对象
    for (JPRule *r in self.rules) {
        if ([r.pathFragment hasPrefix:@"bid:"]) continue;

        NSMutableArray<JPInstalledApp *> *matches = [NSMutableArray array];
        for (JPInstalledApp *app in self.installedApps) {
            if ([app.pathFragment isEqualToString:r.pathFragment]) {
                [matches addObject:app];
            }
        }
        if (matches.count == 1) {
            JPInstalledApp *app = matches.firstObject;
            if (app.bundleId.length > 0) {
                r.pathFragment = [NSString stringWithFormat:@"bid:%@", app.bundleId];
                r.displayName = app.displayName;
                r.isConflict = NO;
                r.conflictAppNames = nil;
            }
        } else if (matches.count >= 2) {
            r.isConflict = YES;
            r.conflictAppNames = [matches valueForKey:@"displayName"];
        }
    }

    NSError *saveErr = nil;
    if (![self saveConfigWithError:&saveErr]) {
        [self loadConfig];
        [self rebuildRulesCards];
        UIAlertController *errAlert = [UIAlertController alertControllerWithTitle:@"规则升级失败"
                                                                          message:[NSString stringWithFormat:@"规则升级失败，已保持原样：%@", saveErr ? saveErr.localizedDescription : @"写入错误"]
                                                                   preferredStyle:UIAlertControllerStyleAlert];
        [errAlert addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:errAlert animated:YES completion:nil];
        return;
    }

    [self rebuildRulesCards];

    NSString *msg;
    NSString *namesStr = [migratedNames componentsJoinedByString:@"、"];
    if (conflictCount > 0) {
        msg = [NSString stringWithFormat:@"已把 %lu 条规则升级为按 App 识别，更准确：%@。另有 %ld 条会同时命中多个 App，需要你重新选择。",
               (unsigned long)migratedNames.count, namesStr, (long)conflictCount];
    } else {
        msg = [NSString stringWithFormat:@"已把 %lu 条规则升级为按 App 识别，更准确：%@。",
               (unsigned long)migratedNames.count, namesStr];
    }

    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"规则升级提示"
                                                                   message:msg
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];

    __weak typeof(self) weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"撤销" style:UIAlertActionStyleDestructive handler:^(UIAlertAction * _Nonnull action) {
        __strong typeof(weakSelf) strongSelf = weakSelf;
        if (!strongSelf) return;
        NSFileManager *fileMgr = NSFileManager.defaultManager;
        if (![fileMgr fileExistsAtPath:kConfigBackup]) {
            UIAlertController *errAlert = [UIAlertController alertControllerWithTitle:@"撤销失败"
                                                                              message:@"未找到备份文件，无法撤销。"
                                                                       preferredStyle:UIAlertControllerStyleAlert];
            [errAlert addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
            [strongSelf presentViewController:errAlert animated:YES completion:nil];
            return;
        }

        // 必改 3-3: 撤销原子化，读取备份写回，失败弹窗且保持现状
        NSError *readErr = nil;
        NSString *backupContent = [NSString stringWithContentsOfFile:kConfigBackup encoding:NSUTF8StringEncoding error:&readErr];
        NSError *writeErr = nil;
        BOOL ok = NO;
        if (backupContent) {
            ok = [backupContent writeToFile:kConfig atomically:YES encoding:NSUTF8StringEncoding error:&writeErr];
        }
        if (!ok) {
            UIAlertController *errAlert = [UIAlertController alertControllerWithTitle:@"撤销失败"
                                                                              message:[NSString stringWithFormat:@"恢复规则备份失败，规则文件保持不变：%@", writeErr ? writeErr.localizedDescription : (readErr ? readErr.localizedDescription : @"未知错误")]
                                                                       preferredStyle:UIAlertControllerStyleAlert];
            [errAlert addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
            [strongSelf presentViewController:errAlert animated:YES completion:nil];
            return;
        }

        // 必改 3-4: 撤销成功后写入持久标记，下次打开不再自动迁移
        [@"declined" writeToFile:kMigrationDeclined atomically:YES encoding:NSUTF8StringEncoding error:nil];
        [strongSelf loadConfig];
        [strongSelf rebuildRulesCards];
        [strongSelf showSaveNotice];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)rebuildRulesCards {
    for (UIView *v in self.rulesStackView.arrangedSubviews) {
        [self.rulesStackView removeArrangedSubview:v];
        [v removeFromSuperview];
    }

    // Mark duplicates and conflictWithDemote
    NSMutableSet<NSString *> *seenProtect = [NSMutableSet set];
    NSMutableSet<NSString *> *seenDemote = [NSMutableSet set];
    for (JPRule *r in self.rules) {
        r.isDuplicate = NO;
        r.isConflictWithDemote = NO;
        NSString *key = r.pathFragment ?: @"";
        if (r.type == JPRuleTypeProtect) {
            if ([seenProtect containsObject:key]) {
                r.isDuplicate = YES;
            } else {
                [seenProtect addObject:key];
            }
            if ([seenDemote containsObject:key]) {
                r.isConflictWithDemote = YES;
            }
        } else {
            if ([seenDemote containsObject:key]) {
                r.isDuplicate = YES;
            } else {
                [seenDemote addObject:key];
            }
            if ([seenProtect containsObject:key]) {
                r.isConflictWithDemote = YES;
            }
        }
    }

    NSMutableArray<JPRule *> *protectRules = [NSMutableArray array];
    NSMutableArray<JPRule *> *demoteRules = [NSMutableArray array];
    for (JPRule *r in self.rules) {
        if (r.type == JPRuleTypeProtect) [protectRules addObject:r];
        else [demoteRules addObject:r];
    }

    [protectRules sortUsingComparator:^NSComparisonResult(JPRule *a, JPRule *b) {
        if (a.targetBand > b.targetBand) return NSOrderedAscending;
        if (a.targetBand < b.targetBand) return NSOrderedDescending;
        return [a.displayName localizedStandardCompare:b.displayName];
    }];

    NSCountedSet *bandCounts = [NSCountedSet new];
    for (JPRule *r in protectRules) {
        [bandCounts addObject:@(r.targetBand)];
    }
    for (JPRule *r in protectRules) {
        r.hasSameBand = ([bandCounts countForObject:@(r.targetBand)] > 1);
    }

    if (self.rules.count == 0) {
        UIView *emptyCard = [UIView new];
        emptyCard.translatesAutoresizingMaskIntoConstraints = NO;
        emptyCard.backgroundColor = UIColor.secondarySystemGroupedBackgroundColor;
        emptyCard.layer.cornerRadius = 14;
        emptyCard.layer.cornerCurve = kCACornerCurveContinuous;

        UILabel *emptyLabel = [UILabel new];
        emptyLabel.translatesAutoresizingMaskIntoConstraints = NO;
        emptyLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightRegular];
        emptyLabel.textColor = UIColor.secondaryLabelColor;
        emptyLabel.text = @"暂无规则，点击下方「添加规则」进行配置";
        emptyLabel.textAlignment = NSTextAlignmentCenter;
        [emptyCard addSubview:emptyLabel];

        [NSLayoutConstraint activateConstraints:@[
            [emptyLabel.centerXAnchor constraintEqualToAnchor:emptyCard.centerXAnchor],
            [emptyLabel.centerYAnchor constraintEqualToAnchor:emptyCard.centerYAnchor],
            [emptyCard.heightAnchor constraintEqualToConstant:64]
        ]];
        [self.rulesStackView addArrangedSubview:emptyCard];
    } else {
        JPStatusInfo *st = self.lastLoadedStatus ?: [JPStatusInfo loadCurrentStatus];
        BOOL isRunning = ([self daemonState] == JPDaemonStateRunning);

        for (JPRule *rule in protectRules) {
            JPRuleCardView *card = [[JPRuleCardView alloc] initWithRule:rule delegate:self installedApps:self.installedApps];
            [card updateStatusWithInfo:st daemonRunning:isRunning];
            [self.rulesStackView addArrangedSubview:card];
        }

        if (demoteRules.count > 0) {
            UILabel *demoteHeader = [self createSectionHeader:@"降级规则"];
            [self.rulesStackView addArrangedSubview:demoteHeader];
            for (JPRule *rule in demoteRules) {
                JPRuleCardView *card = [[JPRuleCardView alloc] initWithRule:rule delegate:self installedApps:self.installedApps];
                [card updateStatusWithInfo:st daemonRunning:isRunning];
                [self.rulesStackView addArrangedSubview:card];
            }
        }
    }

    self.rulesSectionHeaderLabel.text = [NSString stringWithFormat:@"保护规则 (%lu)", (unsigned long)self.rules.count];
    if (self.killedStatsCard) {
        [self refreshKilledStatsIfNeeded:YES];
    }
}

#pragma mark - Daemon Control

- (JPDaemonState)daemonState {
    return [JPDaemonManager daemonStateWithLastSpawnTime:self.lastSpawnTime];
}

- (BOOL)isDaemonRunning {
    return [self daemonState] == JPDaemonStateRunning;
}

- (void)toggleDaemon {
    if ([self daemonState] == JPDaemonStateRunning) {
        [self stopDaemon];
    } else {
        [self startDaemon];
    }
}

- (void)restartDaemon {
    if (self.restartInProgress || s_restartInProgress) return;
    self.restartInProgress = YES;
    s_restartInProgress = YES;
    self.versionWarningBanner.enabled = NO;

    int pre_rc = 0;
    int pre_exit = 0;
    int pre_errno = 0;
    NSString *pre_reason = nil;
    if (![JPDaemonManager precheckDaemonWithRc:&pre_rc exitOut:&pre_exit errOut:&pre_errno reason:&pre_reason]) {
        NSLog(@"SPAWN_PRECHECK_FAIL rc=%d exit=%d errno=%d", pre_rc, pre_exit, pre_errno);
        self.restartInProgress = NO;
        s_restartInProgress = NO;
        self.versionWarningBanner.enabled = YES;
        NSString *errDetail = (pre_errno != 0)
            ? [NSString stringWithFormat:@"(rc=%d, exit=%d, errno=%d: %s)", pre_rc, pre_exit, pre_errno, strerror(pre_errno)]
            : [NSString stringWithFormat:@"(rc=%d, exit=%d)", pre_rc, pre_exit];
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"预检失败，已保持旧守护进程运行"
                                                                       message:[NSString stringWithFormat:@"重启预检失败: %@\n%@\n旧守护进程未停止，继续保持运行。",
                                                                                pre_reason ?: @"未知错误", errDetail]
                                                                preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:alert animated:YES completion:nil];
        return;
    }

    [self stopDaemon];
    NSString *pidStr = [NSString stringWithContentsOfFile:kPidFile encoding:NSUTF8StringEncoding error:nil];
    pid_t pid = (pid_t)[pidStr intValue];
    if (pid > 0 && JPIsPidJetpilotd(pid)) {
        kill(pid, SIGTERM);
    } else {
        [NSFileManager.defaultManager removeItemAtPath:kPidFile error:nil];
        pid = 0;
    }
    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        if (!JPWaitForDaemonExit(pid)) {
            [@"run" writeToFile:kFlag atomically:YES encoding:NSUTF8StringEncoding error:nil];
            dispatch_async(dispatch_get_main_queue(), ^{
                weakSelf.restartInProgress = NO;
                s_restartInProgress = NO;
                weakSelf.versionWarningBanner.enabled = YES;
                [weakSelf refreshStatus];
                UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"重启失败"
                                                                               message:@"旧守护进程未退出，请稍后再试"
                                                                        preferredStyle:UIAlertControllerStyleAlert];
                [alert addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
                [weakSelf presentViewController:alert animated:YES completion:nil];
            });
            return;
        }
        [NSFileManager.defaultManager removeItemAtPath:kPidFile error:nil];
        dispatch_async(dispatch_get_main_queue(), ^{
            weakSelf.restartInProgress = NO;
            s_restartInProgress = NO;
            weakSelf.versionWarningBanner.enabled = YES;
            [weakSelf startDaemon];
        });
    });
}

- (void)handleStatusCardLongPress:(UILongPressGestureRecognizer *)gesture {
    if (gesture.state != UIGestureRecognizerStateBegan) return;
    [JPDaemonManager runProbeDiagnosticsFromViewController:self];
}

- (void)startDaemon {
    if ([self daemonState] == JPDaemonStateRunning) {
        [self restartDaemon];
        return;
    }
    if ([self daemonState] == JPDaemonStateDead)
        [NSFileManager.defaultManager removeItemAtPath:kPidFile error:nil];
    [self saveConfig];
    CFAbsoluteTime t = 0;
    int rc = [JPDaemonManager startDaemonWithLastSpawnTime:&t];
    if (rc == 0) self.lastSpawnTime = t;

    if (rc != 0) {
        [JPDaemonManager handleSpawnFailure:rc fromViewController:self];
    }
    [self refreshStatus];
}

- (void)stopDaemon {
    [JPDaemonManager stopDaemon];
    [self refreshStatus];
}

- (void)scrollToKilledStatsCard {
    if (!self.killedStatsCard) return;
    CGRect rect = [self.contentStackView convertRect:self.killedStatsCard.bounds fromView:self.killedStatsCard];
    rect.origin.y += 12; // 算上 contentStackView 顶部 12pt 偏移，卡片底边完整露出 (P8)
    [self.scrollView scrollRectToVisible:rect animated:YES];
    [UIView animateWithDuration:0.2 animations:^{
        self.killedStatsCard.alpha = 0.5;
    } completion:^(BOOL finished) {
        [UIView animateWithDuration:0.3 animations:^{
            self.killedStatsCard.alpha = 1.0;
        }];
    }];
}

- (void)refreshKilledStatsIfNeeded:(BOOL)force {
    if (!self.killedStatsCard) return;

    NSCalendar *cal = [NSCalendar currentCalendar];
    NSDateComponents *dc = [cal components:(NSCalendarUnitYear | NSCalendarUnitMonth | NSCalendarUnitDay)
                                  fromDate:[NSDate date]];
    NSString *day = [NSString stringWithFormat:@"%ld-%02ld-%02ld",
                     (long)dc.year, (long)dc.month, (long)dc.day];
    NSArray *files = [[NSFileManager.defaultManager contentsOfDirectoryAtPath:kCrashReporterDir error:nil]
        filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"SELF BEGINSWITH 'JetsamEvent-'"]];
    NSString *newest = [[files sortedArrayUsingSelector:@selector(compare:)] lastObject] ?: @"";
    NSString *sig = [NSString stringWithFormat:@"%@|%lu|%@|%lu", day,
                     (unsigned long)files.count, newest, (unsigned long)self.rules.count];
    if (!force && [sig isEqualToString:self.killStatsLastSig]) return;

    self.killStatsLastSig = sig;
    self.killStatsGeneration++;
    NSInteger gen = self.killStatsGeneration;
    NSArray *rulesCopy = [self.rules copy];
    NSArray *appsCopy = [self.installedApps copy];
    __weak typeof(self) weakSelf = self;
    dispatch_async(self.killStatsQueue, ^{
        JPKilledStatsInfo *stats = [JPKilledStatsInfo loadStatsForRules:rulesCopy installedApps:appsCopy];
        dispatch_async(dispatch_get_main_queue(), ^{
            __strong typeof(weakSelf) strongSelf = weakSelf;
            if (!strongSelf || strongSelf.killStatsGeneration != gen) return;
            [strongSelf.killedStatsCard updateWithStats:stats];
            [strongSelf.statusCard updateKilledCount:stats.totalKills available:stats.available];
        });
    });
}

- (BOOL)hasBidRules {
    for (JPRule *r in self.rules) {
        if ([r.pathFragment hasPrefix:@"bid:"]) return YES;
    }
    return NO;
}

- (void)refreshStatus {
    JPDaemonState state = [self daemonState];
    BOOL running = (state == JPDaemonStateRunning);
    self.deadBanner.hidden = (state != JPDaemonStateDead);
    JPStatusInfo *st = [JPStatusInfo loadCurrentStatus];
    self.lastLoadedStatus = st;
    NSString *appVersion = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"0.7.8";
    NSString *appCommit = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"JPGitCommit"] ?: @"";
    BOOL versionMismatch = running && (JPCheckVersionOrCommitMismatch(st, appVersion, appCommit) || ([self hasBidRules] && !st.bidSupport));
    self.versionWarningBanner.hidden = !versionMismatch;
    [self.statusCard updateWithStatus:st isRunning:running];
    [self refreshKilledStatsIfNeeded:NO];
    for (UIView *v in self.rulesStackView.arrangedSubviews) {
        if ([v isKindOfClass:[JPRuleCardView class]]) {
            [(JPRuleCardView *)v updateStatusWithInfo:st daemonRunning:running];
        }
    }
    if (running && st.bidSupport) {
        [self checkAndMigrateLegacyRulesIfNeeded:st];
    }
}

#pragma mark - JPRuleCardViewDelegate

- (void)ruleCardViewDidRequestDelete:(JPRuleCardView *)card {
    JPRule *rule = card.rule;
    NSString *name = rule.displayName.length > 0 ? rule.displayName : rule.pathFragment;
    NSString *msg = [NSString stringWithFormat:@"确定要移除对「%@」的规则吗？", name];
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"移除规则"
                                                                   message:msg
                                                            preferredStyle:UIAlertControllerStyleActionSheet];

    __weak typeof(self) weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"移除" style:UIAlertActionStyleDestructive handler:^(UIAlertAction * _Nonnull action) {
        [weakSelf.rules removeObject:rule];
        [weakSelf rebuildRulesCards];
        [weakSelf saveConfig];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];

    if (alert.popoverPresentationController) {
        alert.popoverPresentationController.sourceView = card;
        alert.popoverPresentationController.sourceRect = card.bounds;
    }

    [self presentViewController:alert animated:YES completion:nil];
}

- (void)ruleCardViewDidTap:(JPRuleCardView *)card {
    if (card.rule.isConflict) {
        [self reselectAppForRule:card.rule];
    } else {
        [self openEditRuleController:card.rule];
    }
}

- (void)reselectAppForRule:(JPRule *)rule {
    JPAppPickerViewController *picker = [JPAppPickerViewController new];
    picker.allApps = self.installedApps;
    picker.existingRules = self.rules;
    __weak typeof(self) weakSelf = self;
    picker.onSelectApp = ^(NSString *pathFragment, NSString *displayName) {
        rule.pathFragment = pathFragment;
        rule.displayName = displayName;
        rule.isConflict = NO;
        rule.conflictAppNames = nil;
        [weakSelf rebuildRulesCards];
        [weakSelf saveConfig];
    };
    UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:picker];
    if (@available(iOS 15.0, *)) {
        UISheetPresentationController *sheet = nav.sheetPresentationController;
        sheet.detents = @[UISheetPresentationControllerDetent.mediumDetent, UISheetPresentationControllerDetent.largeDetent];
        sheet.prefersGrabberVisible = YES;
    }
    [self presentViewController:nav animated:YES completion:nil];
}

- (void)openEditRuleController:(JPRule *)rule {
    JPRuleEditViewController *vc = [JPRuleEditViewController new];
    vc.rule = rule;
    vc.installedApps = self.installedApps;
    vc.existingRules = self.rules;
    vc.delegate = self;
    UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:vc];
    [self presentViewController:nav animated:YES completion:nil];
}

#pragma mark - JPRuleEditViewControllerDelegate

- (void)ruleEditViewControllerDidSave:(JPRule *)rule {
    [self rebuildRulesCards];
    [self saveConfig];
}

- (void)ruleEditViewControllerDidDelete:(JPRule *)rule {
    [self.rules removeObject:rule];
    [self rebuildRulesCards];
    [self saveConfig];
}

#pragma mark - Add Rule Action

- (BOOL)status:(JPStatusInfo *)status hasMatchingAppForRule:(JPRule *)rule {
    if (!status || status.protectedApps.count == 0) return NO;
    for (JPProtectedApp *pApp in status.protectedApps) {
        if ([rule.pathFragment hasPrefix:@"bid:"]) {
            NSString *bid = [rule.pathFragment substringFromIndex:4];
            if ([pApp.bundleId isEqualToString:bid] || [pApp.pathFragment isEqualToString:rule.pathFragment]) {
                return YES;
            }
        } else {
            if ([pApp.pathFragment isEqualToString:rule.pathFragment]) {
                return YES;
            }
        }
    }
    return NO;
}

- (void)checkRuleRunStateAfterAdd:(JPRule *)rule
                          appName:(NSString *)appName
                       targetBand:(NSInteger)band
                             type:(JPRuleType)type
                         saveTime:(NSDate *)saveTime {
    NSString *name = appName.length > 0 ? appName : rule.displayName;
    if (name.length == 0) name = rule.pathFragment;

    // 必改 4-2: 守护进程未运行 / 版本过低时按规则卡片同等优先级直接提示，不轮询
    BOOL running = ([self daemonState] == JPDaemonStateRunning);
    if (!running) {
        NSString *actionDesc = (type == JPRuleTypeDemote) ? @"降级" : @"保护";
        NSString *msg = [NSString stringWithFormat:@"已添加。守护进程没在运行，启动守护进程后才会%@。点卡片可以改档位。", actionDesc];
        [self showAddResultAlert:msg];
        return;
    }

    JPStatusInfo *initSt = self.lastLoadedStatus ?: [JPStatusInfo loadCurrentStatus];
    if ([rule.pathFragment hasPrefix:@"bid:"] && (!initSt || !initSt.bidSupport)) {
        [self showAddResultAlert:@"已添加，但守护进程是旧版本，这条规则不会生效。请停止守护进程再启动。"];
        return;
    }

    // 必改 4-3: 降级规则固定提示，不轮询（status.txt 仅记录保护规则）
    if (type == JPRuleTypeDemote) {
        NSInteger targetBand = (rule ? rule.targetBand : band);
        if (targetBand < 0) targetBand = 0;
        if (targetBand > 30) targetBand = 30;
        NSString *msg = [NSString stringWithFormat:@"已添加。%@ 不在前台时会降到 %ld。点卡片可以改档位。", name, (long)targetBand];
        [self showAddResultAlert:msg];
        return;
    }

    // 必改 4-1: 保护规则按 status.txt 的 mtime 是否已反映新规则轮询判定，最长等 15 秒
    NSDate *t0 = saveTime ?: [NSDate date];
    __block NSInteger checksRemaining = 30; // 30 * 0.5s = 15.0s
    __weak typeof(self) weakSelf = self;
    NSTimer *timer = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer * _Nonnull t) {
        __strong typeof(weakSelf) strongSelf = weakSelf;
        if (!strongSelf) { [t invalidate]; return; }

        NSFileManager *fm = NSFileManager.defaultManager;
        NSDictionary *attrs = [fm attributesOfItemAtPath:kStatus error:nil];
        NSDate *mtime = attrs.fileModificationDate;

        // mtime > t0，说明守护进程已在规则保存后写出新的 status.txt
        if (mtime && [mtime compare:t0] == NSOrderedDescending) {
            [t invalidate];
            [strongSelf refreshStatus];
            JPStatusInfo *currentSt = strongSelf.lastLoadedStatus ?: [JPStatusInfo loadCurrentStatus];
            if ([strongSelf status:currentSt hasMatchingAppForRule:rule]) {
                NSString *msg = [NSString stringWithFormat:@"已添加，%@ 正在运行，已立即保护到 %ld。点卡片可以改档位。", name, (long)band];
                [strongSelf showAddResultAlert:msg];
            } else {
                NSString *msg = [NSString stringWithFormat:@"已添加。%@ 现在没运行，你打开它时会自动保护（不会替你打开）。点卡片可以改档位。", name];
                [strongSelf showAddResultAlert:msg];
            }
            return;
        }

        checksRemaining--;
        if (checksRemaining <= 0) {
            [t invalidate];
            [strongSelf refreshStatus];
            NSString *msg = @"已添加，守护进程还没处理到这条规则，稍后在规则卡片上看状态。点卡片可以改档位。";
            [strongSelf showAddResultAlert:msg];
        }
    }];
    [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
}

- (void)showAddResultAlert:(NSString *)msg {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"提示"
                                                                   message:msg
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)showAddRuleSheet {
    if (self.rules.count >= 32) {
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"规则已达上限"
                                                                       message:@"最多支持添加 32 条保护规则"
                                                                preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:alert animated:YES completion:nil];
        return;
    }

    JPAppPickerViewController *picker = [JPAppPickerViewController new];
    picker.allApps = self.installedApps;
    picker.existingRules = self.rules;

    __weak typeof(self) weakSelf = self;
    picker.onSelectApp = ^(NSString *pathFragment, NSString *displayName) {
        JPRule *existing = nil;
        for (JPRule *r in weakSelf.rules) {
            if ([r.pathFragment isEqualToString:pathFragment]) {
                existing = r;
                break;
            }
        }
        if (existing) {
            [weakSelf openEditRuleController:existing];
            return;
        }

        NSString *title = displayName.length > 0 ? displayName : pathFragment;
        UIAlertController *actionSheet = [UIAlertController alertControllerWithTitle:title
                                                                             message:nil
                                                                      preferredStyle:UIAlertControllerStyleActionSheet];
        [actionSheet addAction:[UIAlertAction actionWithTitle:@"保护（50 档）"
                                                        style:UIAlertActionStyleDefault
                                                      handler:^(UIAlertAction * _Nonnull action) {
            JPRule *r = [JPRule new];
            r.type = JPRuleTypeProtect;
            r.pathFragment = pathFragment;
            r.targetBand = 50;
            r.displayName = displayName;
            [weakSelf evaluateRuleConflict:r];
            [weakSelf.rules addObject:r];
            [weakSelf rebuildRulesCards];
            NSDate *t0 = [NSDate date];
            [weakSelf saveConfig];
            [weakSelf checkRuleRunStateAfterAdd:r appName:displayName targetBand:50 type:JPRuleTypeProtect saveTime:t0];
        }]];
        [actionSheet addAction:[UIAlertAction actionWithTitle:@"降级（30 档）"
                                                        style:UIAlertActionStyleDefault
                                                      handler:^(UIAlertAction * _Nonnull action) {
            JPRule *r = [JPRule new];
            r.type = JPRuleTypeDemote;
            r.pathFragment = pathFragment;
            r.targetBand = 30;
            r.displayName = displayName;
            [weakSelf evaluateRuleConflict:r];
            [weakSelf.rules addObject:r];
            [weakSelf rebuildRulesCards];
            NSDate *t0 = [NSDate date];
            [weakSelf saveConfig];
            [weakSelf checkRuleRunStateAfterAdd:r appName:displayName targetBand:30 type:JPRuleTypeDemote saveTime:t0];
        }]];
        [actionSheet addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];

        if (actionSheet.popoverPresentationController) {
            actionSheet.popoverPresentationController.sourceView = weakSelf.view;
            actionSheet.popoverPresentationController.sourceRect = CGRectMake(CGRectGetMidX(weakSelf.view.bounds), CGRectGetMidY(weakSelf.view.bounds), 1, 1);
            actionSheet.popoverPresentationController.permittedArrowDirections = 0;
        }
        [weakSelf presentViewController:actionSheet animated:YES completion:nil];
    };

    picker.onManualAdd = ^(NSString *pathFragment, JPRuleType type, NSInteger band) {
        JPRule *existing = nil;
        for (JPRule *r in weakSelf.rules) {
            if ([r.pathFragment isEqualToString:pathFragment]) {
                existing = r;
                break;
            }
        }
        if (existing) {
            [weakSelf openEditRuleController:existing];
            return;
        }

        JPRule *r = [JPRule new];
        r.type = type;
        r.pathFragment = pathFragment;
        r.targetBand = band;
        r.displayName = [weakSelf resolveDisplayNameForFragment:pathFragment];
        [weakSelf evaluateRuleConflict:r];
        [weakSelf.rules addObject:r];
        [weakSelf rebuildRulesCards];
        NSDate *t0 = [NSDate date];
        [weakSelf saveConfig];
        [weakSelf checkRuleRunStateAfterAdd:r appName:r.displayName targetBand:band type:type saveTime:t0];
    };

    UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:picker];
    if (@available(iOS 15.0, *)) {
        UISheetPresentationController *sheet = nav.sheetPresentationController;
        sheet.detents = @[UISheetPresentationControllerDetent.mediumDetent, UISheetPresentationControllerDetent.largeDetent];
        sheet.prefersGrabberVisible = YES;
    }
    [self presentViewController:nav animated:YES completion:nil];
}

- (void)showProtectedList {
    JPProtectedListViewController *vc = [JPProtectedListViewController new];
    vc.installedApps = self.installedApps;
    __weak typeof(self) weakSelf = self;
    vc.nameResolver = ^NSString *(NSString *frag) {
        return [weakSelf resolveDisplayNameForFragment:frag];
    };
    if (self.navigationController) {
        [self.navigationController pushViewController:vc animated:YES];
    } else {
        UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:vc];
        vc.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"关闭"
                                                                               style:UIBarButtonItemStylePlain
                                                                              target:self
                                                                              action:@selector(dismissPresentedController)];
        [self presentViewController:nav animated:YES completion:nil];
    }
}

- (void)dismissPresentedController {
    [self dismissViewControllerAnimated:YES completion:nil];
}

@end


#pragma mark - Controllers: MemPressure ViewController (Retired in 0.7.8)

@interface JPMemPressureViewController : UIViewController
@end

@implementation JPMemPressureViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"内存释放";
    self.view.backgroundColor = UIColor.systemGroupedBackgroundColor;
    self.navigationController.navigationBar.prefersLargeTitles = YES;

    UILabel *noticeLabel = [UILabel new];
    noticeLabel.translatesAutoresizingMaskIntoConstraints = NO;
    noticeLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightMedium];
    noticeLabel.textColor = UIColor.secondaryLabelColor;
    noticeLabel.textAlignment = NSTextAlignmentCenter;
    noticeLabel.numberOfLines = 0;
    noticeLabel.text = @"已由『后台释放』替代";

    [self.view addSubview:noticeLabel];

    [NSLayoutConstraint activateConstraints:@[
        [noticeLabel.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [noticeLabel.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
        [noticeLabel.leadingAnchor constraintGreaterThanOrEqualToAnchor:self.view.leadingAnchor constant:24],
        [noticeLabel.trailingAnchor constraintLessThanOrEqualToAnchor:self.view.trailingAnchor constant:-24],
    ]];
}

@end



#pragma mark - Views: Restart Rule Card

@class JPRestartRuleCardView;

@protocol JPRestartRuleCardViewDelegate <NSObject>
@optional
- (void)restartRuleCardViewDidTap:(JPRestartRuleCardView *)card;
- (void)restartRuleCardViewDidRequestDelete:(JPRestartRuleCardView *)card;
@end

@interface JPRestartRuleCardView : UIView
@property (nonatomic, strong) JPRestartRule *rule;
@property (nonatomic, weak) id<JPRestartRuleCardViewDelegate> delegate;
@property (nonatomic, strong) UIImageView *iconImageView;
@property (nonatomic, strong) UILabel *titleLabel;
@property (nonatomic, strong) UILabel *fragLabel;
@property (nonatomic, strong) UILabel *paramLabel;
@property (nonatomic, strong) UILabel *statusLabel;
@property (nonatomic, strong) UILabel *thrLabel;
@property (nonatomic, strong) UIButton *deleteButton;

- (instancetype)initWithRule:(JPRestartRule *)rule
                    delegate:(id<JPRestartRuleCardViewDelegate>)delegate
               installedApps:(NSArray<JPInstalledApp *> *)apps;
- (instancetype)initWithRule:(JPRestartRule *)rule
                    delegate:(id<JPRestartRuleCardViewDelegate>)delegate
               installedApps:(NSArray<JPInstalledApp *> *)apps
            restart7DayCount:(NSInteger)count7;
@end

@implementation JPRestartRuleCardView

- (instancetype)initWithRule:(JPRestartRule *)rule
                    delegate:(id<JPRestartRuleCardViewDelegate>)delegate
               installedApps:(NSArray<JPInstalledApp *> *)apps {
    return [self initWithRule:rule delegate:delegate installedApps:apps restart7DayCount:-1];
}

- (instancetype)initWithRule:(JPRestartRule *)rule
                    delegate:(id<JPRestartRuleCardViewDelegate>)delegate
               installedApps:(NSArray<JPInstalledApp *> *)apps
            restart7DayCount:(NSInteger)count7 {
    self = [super initWithFrame:CGRectZero];
    if (self) {
        _rule = rule;
        _delegate = delegate;
        [self setupUIWithApps:apps restart7DayCount:count7];
    }
    return self;
}

- (void)setupUIWithApps:(NSArray<JPInstalledApp *> *)apps restart7DayCount:(NSInteger)providedCount {
    self.backgroundColor = UIColor.secondarySystemGroupedBackgroundColor;
    self.layer.cornerRadius = 14;
    self.layer.cornerCurve = kCACornerCurveContinuous;
    self.clipsToBounds = YES;
    self.translatesAutoresizingMaskIntoConstraints = NO;

    self.iconImageView = [UIImageView new];
    self.iconImageView.translatesAutoresizingMaskIntoConstraints = NO;
    self.iconImageView.layer.cornerRadius = 8;
    self.iconImageView.layer.cornerCurve = kCACornerCurveContinuous;
    self.iconImageView.clipsToBounds = YES;
    self.iconImageView.contentMode = UIViewContentModeScaleAspectFit;
    self.iconImageView.image = JPIconForFragment([NSString stringWithFormat:@"bid:%@", self.rule.bid], apps);
    [self addSubview:self.iconImageView];

    NSString *name = ResolveDisplayNameHelper([NSString stringWithFormat:@"bid:%@", self.rule.bid], apps);
    if (name.length == 0) name = self.rule.displayName.length > 0 ? self.rule.displayName : self.rule.bid;

    self.titleLabel = [UILabel new];
    self.titleLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.titleLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightSemibold];
    self.titleLabel.textColor = UIColor.labelColor;
    self.titleLabel.text = name;
    [self addSubview:self.titleLabel];

    self.fragLabel = [UILabel new];
    self.fragLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.fragLabel.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
    self.fragLabel.textColor = UIColor.secondaryLabelColor;
    self.fragLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
    self.fragLabel.text = self.rule.bid;
    [self addSubview:self.fragLabel];

    self.paramLabel = [UILabel new];
    self.paramLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.paramLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightRegular];
    self.paramLabel.textColor = UIColor.secondaryLabelColor;
    self.paramLabel.numberOfLines = 1;

    NSString *bgStr = (self.rule.bg % 60 == 0) ?
        [NSString stringWithFormat:@"%ld分", (long)(self.rule.bg / 60)] :
        [NSString stringWithFormat:@"%ld秒", (long)self.rule.bg];
    NSString *relaunchStr = (self.rule.relaunch == 1) ? @"自动拉起" : @"不拉起";
    self.paramLabel.text = [NSString stringWithFormat:@"%ld MB · 后台 %@ · 每天 %ld 次 · %@",
                            (long)self.rule.thr, bgStr, (long)self.rule.max, relaunchStr];
    [self addSubview:self.paramLabel];

    self.statusLabel = [UILabel new];
    self.statusLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.statusLabel.font = [UIFont systemFontOfSize:12 weight:UIFontWeightRegular];
    self.statusLabel.numberOfLines = 1;
    [self addSubview:self.statusLabel];

    self.thrLabel = [UILabel new];
    self.thrLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.thrLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightBold];
    self.thrLabel.textAlignment = NSTextAlignmentRight;
    self.thrLabel.text = [NSString stringWithFormat:@"%ld MB", (long)self.rule.thr];
    [self addSubview:self.thrLabel];

    self.deleteButton = [UIButton buttonWithType:UIButtonTypeSystem];
    self.deleteButton.translatesAutoresizingMaskIntoConstraints = NO;
    [self.deleteButton setImage:[UIImage systemImageNamed:@"trash"] forState:UIControlStateNormal];
    self.deleteButton.tintColor = UIColor.systemRedColor;
    [self.deleteButton addTarget:self action:@selector(deleteTapped) forControlEvents:UIControlEventTouchUpInside];
    [self addSubview:self.deleteButton];

    UIImageView *chevron = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"chevron.right"]];
    chevron.translatesAutoresizingMaskIntoConstraints = NO;
    chevron.tintColor = UIColor.tertiaryLabelColor;
    chevron.contentMode = UIViewContentModeScaleAspectFit;
    [self addSubview:chevron];

    [NSLayoutConstraint activateConstraints:@[
        [self.iconImageView.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:14],
        [self.iconImageView.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [self.iconImageView.widthAnchor constraintEqualToConstant:40],
        [self.iconImageView.heightAnchor constraintEqualToConstant:40],

        [chevron.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-14],
        [chevron.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [chevron.widthAnchor constraintEqualToConstant:8],
        [chevron.heightAnchor constraintEqualToConstant:13],

        [self.deleteButton.trailingAnchor constraintEqualToAnchor:chevron.leadingAnchor constant:-4],
        [self.deleteButton.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [self.deleteButton.widthAnchor constraintEqualToConstant:32],
        [self.deleteButton.heightAnchor constraintEqualToConstant:32],

        [self.thrLabel.trailingAnchor constraintEqualToAnchor:self.deleteButton.leadingAnchor constant:-6],
        [self.thrLabel.topAnchor constraintEqualToAnchor:self.topAnchor constant:12],
        [self.thrLabel.widthAnchor constraintGreaterThanOrEqualToConstant:60],

        [self.titleLabel.topAnchor constraintEqualToAnchor:self.topAnchor constant:12],
        [self.titleLabel.leadingAnchor constraintEqualToAnchor:self.iconImageView.trailingAnchor constant:12],
        [self.titleLabel.trailingAnchor constraintLessThanOrEqualToAnchor:self.thrLabel.leadingAnchor constant:-8],

        [self.fragLabel.topAnchor constraintEqualToAnchor:self.titleLabel.bottomAnchor constant:2],
        [self.fragLabel.leadingAnchor constraintEqualToAnchor:self.iconImageView.trailingAnchor constant:12],
        [self.fragLabel.trailingAnchor constraintLessThanOrEqualToAnchor:self.thrLabel.leadingAnchor constant:-8],

        [self.paramLabel.topAnchor constraintEqualToAnchor:self.fragLabel.bottomAnchor constant:3],
        [self.paramLabel.leadingAnchor constraintEqualToAnchor:self.iconImageView.trailingAnchor constant:12],
        [self.paramLabel.trailingAnchor constraintLessThanOrEqualToAnchor:self.deleteButton.leadingAnchor constant:-8],

        [self.statusLabel.topAnchor constraintEqualToAnchor:self.paramLabel.bottomAnchor constant:3],
        [self.statusLabel.leadingAnchor constraintEqualToAnchor:self.iconImageView.trailingAnchor constant:12],
        [self.statusLabel.trailingAnchor constraintLessThanOrEqualToAnchor:self.deleteButton.leadingAnchor constant:-8],
        [self.statusLabel.bottomAnchor constraintEqualToAnchor:self.bottomAnchor constant:-12]
    ]];

    BOOL installed = NO;
    for (JPInstalledApp *app in apps) {
        if ([app.bundleId isEqualToString:self.rule.bid]) {
            installed = YES;
            break;
        }
    }

    NSInteger count7 = (providedCount >= 0) ? providedCount : [JPRestartConfig load7DayRestartCountForBundleId:self.rule.bid];
    if (!installed) {
        self.statusLabel.text = [NSString stringWithFormat:@"未安装 · 近 7 天 %ld 次", (long)count7];
        self.statusLabel.textColor = UIColor.secondaryLabelColor;
        self.alpha = 0.65;
        self.thrLabel.textColor = UIColor.secondaryLabelColor;
    } else if (!self.rule.isEnabled) {
        self.statusLabel.text = [NSString stringWithFormat:@"已停用 · 近 7 天 %ld 次", (long)count7];
        self.statusLabel.textColor = UIColor.secondaryLabelColor;
        self.alpha = 0.75;
        self.thrLabel.textColor = UIColor.secondaryLabelColor;
    } else if (self.rule.isDuplicate) {
        self.statusLabel.text = [NSString stringWithFormat:@"⚠️ 重复，只有第一条生效 · 近 7 天 %ld 次", (long)count7];
        self.statusLabel.textColor = UIColor.systemRedColor;
        self.alpha = 1.0;
        self.thrLabel.textColor = UIColor.systemBlueColor;
    } else {
        self.statusLabel.text = [NSString stringWithFormat:@"已开启 · 近 7 天 %ld 次", (long)count7];
        self.statusLabel.textColor = UIColor.secondaryLabelColor;
        self.alpha = 1.0;
        self.thrLabel.textColor = UIColor.systemBlueColor;
    }

    UITapGestureRecognizer *tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(cardTapped:)];
    [self addGestureRecognizer:tap];
}

- (void)cardTapped:(UITapGestureRecognizer *)gesture {
    CGPoint pt = [gesture locationInView:self];
    if (CGRectContainsPoint(self.deleteButton.frame, pt)) return;
    if ([self.delegate respondsToSelector:@selector(restartRuleCardViewDidTap:)]) {
        [self.delegate restartRuleCardViewDidTap:self];
    }
}

- (void)deleteTapped {
    if ([self.delegate respondsToSelector:@selector(restartRuleCardViewDidRequestDelete:)]) {
        [self.delegate restartRuleCardViewDidRequestDelete:self];
    }
}

@end


#pragma mark - Views: Restart Unknown Line Card

@protocol JPRestartUnknownLineCardViewDelegate <NSObject>
@optional
- (void)restartUnknownLineCardViewDidRequestDelete:(JPRestartLine *)line;
@end

@interface JPRestartUnknownLineCardView : UIView
@property (nonatomic, strong) JPRestartLine *line;
@property (nonatomic, weak) id<JPRestartUnknownLineCardViewDelegate> delegate;
@property (nonatomic, strong) UIImageView *iconImageView;
@property (nonatomic, strong) UILabel *badgeLabel;
@property (nonatomic, strong) UILabel *rawLabel;
@property (nonatomic, strong) UILabel *tipLabel;
@property (nonatomic, strong) UIButton *deleteButton;

- (instancetype)initWithLine:(JPRestartLine *)line delegate:(id<JPRestartUnknownLineCardViewDelegate>)delegate;
@end

@implementation JPRestartUnknownLineCardView

- (instancetype)initWithLine:(JPRestartLine *)line delegate:(id<JPRestartUnknownLineCardViewDelegate>)delegate {
    self = [super initWithFrame:CGRectZero];
    if (self) {
        _line = line;
        _delegate = delegate;
        [self setupUI];
    }
    return self;
}

- (void)setupUI {
    self.backgroundColor = [UIColor.systemRedColor colorWithAlphaComponent:0.08];
    self.layer.borderColor = [UIColor.systemRedColor colorWithAlphaComponent:0.35].CGColor;
    self.layer.borderWidth = 1.0;
    self.layer.cornerRadius = 14;
    self.layer.cornerCurve = kCACornerCurveContinuous;
    self.clipsToBounds = YES;
    self.translatesAutoresizingMaskIntoConstraints = NO;

    self.iconImageView = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"exclamationmark.triangle.fill"]];
    self.iconImageView.translatesAutoresizingMaskIntoConstraints = NO;
    self.iconImageView.tintColor = UIColor.systemRedColor;
    self.iconImageView.contentMode = UIViewContentModeScaleAspectFit;
    [self addSubview:self.iconImageView];

    self.badgeLabel = [UILabel new];
    self.badgeLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.badgeLabel.font = [UIFont systemFontOfSize:11 weight:UIFontWeightBold];
    self.badgeLabel.textColor = UIColor.systemRedColor;
    self.badgeLabel.text = @"无效/未知配置行（已忽略）";
    [self addSubview:self.badgeLabel];

    self.rawLabel = [UILabel new];
    self.rawLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.rawLabel.font = [UIFont monospacedSystemFontOfSize:13 weight:UIFontWeightRegular];
    self.rawLabel.textColor = UIColor.labelColor;
    self.rawLabel.numberOfLines = 2;
    self.rawLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    self.rawLabel.text = self.line.rawText ?: @"";
    [self addSubview:self.rawLabel];

    self.tipLabel = [UILabel new];
    self.tipLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.tipLabel.font = [UIFont systemFontOfSize:11 weight:UIFontWeightRegular];
    self.tipLabel.textColor = UIColor.secondaryLabelColor;
    self.tipLabel.text = @"语法不符合规范，不可编辑，原样保留；可点击右侧删除";
    [self addSubview:self.tipLabel];

    self.deleteButton = [UIButton buttonWithType:UIButtonTypeSystem];
    self.deleteButton.translatesAutoresizingMaskIntoConstraints = NO;
    [self.deleteButton setImage:[UIImage systemImageNamed:@"trash"] forState:UIControlStateNormal];
    self.deleteButton.tintColor = UIColor.systemRedColor;
    [self.deleteButton addTarget:self action:@selector(deleteTapped) forControlEvents:UIControlEventTouchUpInside];
    [self addSubview:self.deleteButton];

    [NSLayoutConstraint activateConstraints:@[
        [self.iconImageView.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:14],
        [self.iconImageView.topAnchor constraintEqualToAnchor:self.topAnchor constant:14],
        [self.iconImageView.widthAnchor constraintEqualToConstant:24],
        [self.iconImageView.heightAnchor constraintEqualToConstant:24],

        [self.deleteButton.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-14],
        [self.deleteButton.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [self.deleteButton.widthAnchor constraintEqualToConstant:32],
        [self.deleteButton.heightAnchor constraintEqualToConstant:32],

        [self.badgeLabel.topAnchor constraintEqualToAnchor:self.topAnchor constant:12],
        [self.badgeLabel.leadingAnchor constraintEqualToAnchor:self.iconImageView.trailingAnchor constant:10],
        [self.badgeLabel.trailingAnchor constraintLessThanOrEqualToAnchor:self.deleteButton.leadingAnchor constant:-8],

        [self.rawLabel.topAnchor constraintEqualToAnchor:self.badgeLabel.bottomAnchor constant:3],
        [self.rawLabel.leadingAnchor constraintEqualToAnchor:self.iconImageView.trailingAnchor constant:10],
        [self.rawLabel.trailingAnchor constraintLessThanOrEqualToAnchor:self.deleteButton.leadingAnchor constant:-8],

        [self.tipLabel.topAnchor constraintEqualToAnchor:self.rawLabel.bottomAnchor constant:3],
        [self.tipLabel.leadingAnchor constraintEqualToAnchor:self.iconImageView.trailingAnchor constant:10],
        [self.tipLabel.trailingAnchor constraintLessThanOrEqualToAnchor:self.deleteButton.leadingAnchor constant:-8],
        [self.tipLabel.bottomAnchor constraintEqualToAnchor:self.bottomAnchor constant:-12],
    ]];

    UITapGestureRecognizer *tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(cardTapped:)];
    [self addGestureRecognizer:tap];
}

- (void)cardTapped:(UITapGestureRecognizer *)g {
    CGPoint pt = [g locationInView:self];
    if (CGRectContainsPoint(self.deleteButton.frame, pt)) return;
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"无法识别的配置行"
                                                                   message:[NSString stringWithFormat:@"该行内容：\n\n%@\n\n该行不符合语法规范，守护进程已忽略并不生效。App 默认原样保留；若不需要，可点击删除。", self.line.rawText ?: @""]
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"确定" style:UIAlertActionStyleDefault handler:nil]];
    UIResponder *resp = self.nextResponder;
    while (resp && ![resp isKindOfClass:[UIViewController class]]) {
        resp = resp.nextResponder;
    }
    if (resp) {
        [(UIViewController *)resp presentViewController:alert animated:YES completion:nil];
    }
}

- (void)deleteTapped {
    if ([self.delegate respondsToSelector:@selector(restartUnknownLineCardViewDidRequestDelete:)]) {
        [self.delegate restartUnknownLineCardViewDidRequestDelete:self.line];
    }
}

@end


#pragma mark - Controllers: Restart Rule Edit

@class JPRestartRuleEditViewController;

@protocol JPRestartRuleEditViewControllerDelegate <NSObject>
- (BOOL)restartRuleEditViewControllerDidSave:(JPRestartRule *)rule error:(NSError **)error;
- (BOOL)restartRuleEditViewControllerDidDelete:(JPRestartRule *)rule error:(NSError **)error;
@end

@interface JPRestartRuleEditViewController : UIViewController <UITableViewDataSource, UITableViewDelegate>
@property (nonatomic, strong) JPRestartRule *rule;
@property (nonatomic, copy) NSArray<JPInstalledApp *> *installedApps;
@property (nonatomic, assign) BOOL isNewRule;
@property (nonatomic, weak) id<JPRestartRuleEditViewControllerDelegate> delegate;

@property (nonatomic, strong) UITableView *tableView;
@property (nonatomic, assign) NSInteger currentThr;
@property (nonatomic, assign) NSInteger currentBgMinutes;
@property (nonatomic, assign) NSInteger currentBgSeconds;
@property (nonatomic, assign) BOOL bgEdited;
@property (nonatomic, assign) NSInteger currentMax;
@property (nonatomic, assign) BOOL relaunch;
@property (nonatomic, assign) BOOL isEnabled;
@property (nonatomic, copy) NSString *currentDisplayName;
@property (nonatomic, assign) uint64_t memcapMB;
@property (nonatomic, assign) NSInteger appWarnThr;
@property (nonatomic, assign) NSInteger restart7DayCount;
@end

@implementation JPRestartRuleEditViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = self.isNewRule ? @"添加重启规则" : @"编辑重启规则";
    self.view.backgroundColor = UIColor.systemGroupedBackgroundColor;

    self.memcapMB = JPGetDeviceMemcapMB();
    self.currentThr = (self.rule.thr >= 200) ? self.rule.thr : 800;
    if ((uint64_t)self.currentThr > self.memcapMB) self.currentThr = (NSInteger)self.memcapMB;

    self.currentBgSeconds = (self.rule.bg >= 60 && self.rule.bg <= 7200) ? self.rule.bg : 600;
    self.currentBgMinutes = (self.currentBgSeconds >= 60) ? (self.currentBgSeconds / 60) : 10;
    if (self.currentBgMinutes < 1) self.currentBgMinutes = 1;
    if (self.currentBgMinutes > 120) self.currentBgMinutes = 120;
    self.bgEdited = NO;

    self.currentMax = (self.rule.max >= 1 && self.rule.max <= 10) ? self.rule.max : 3;
    self.relaunch = (self.rule.relaunch == 1);
    self.isEnabled = self.rule.isEnabled;
    self.currentDisplayName = self.rule.displayName ?: @"";

    self.appWarnThr = [JPRestartConfig loadWarnThresholdForBundleId:self.rule.bid pathFragment:nil];
    self.restart7DayCount = [JPRestartConfig load7DayRestartCountForBundleId:self.rule.bid];

    self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"取消"
                                                                             style:UIBarButtonItemStylePlain
                                                                            target:self
                                                                            action:@selector(cancelTapped)];
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"保存"
                                                                              style:UIBarButtonItemStyleDone
                                                                             target:self
                                                                             action:@selector(saveTapped)];

    self.tableView = [[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStyleInsetGrouped];
    self.tableView.translatesAutoresizingMaskIntoConstraints = NO;
    self.tableView.dataSource = self;
    self.tableView.delegate = self;
    self.tableView.estimatedSectionFooterHeight = 44.0;
    self.tableView.sectionFooterHeight = UITableViewAutomaticDimension;
    [self.view addSubview:self.tableView];

    [NSLayoutConstraint activateConstraints:@[
        [self.tableView.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [self.tableView.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [self.tableView.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [self.tableView.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor]
    ]];
}

- (void)cancelTapped {
    [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)saveTapped {
    if (self.isEnabled) {
        if (self.currentThr < 200 || (uint64_t)self.currentThr > self.memcapMB) {
            UIAlertController *err = [UIAlertController alertControllerWithTitle:@"阈值超出范围"
                                                                         message:[NSString stringWithFormat:@"重启阈值必须在 200 到 %llu MB 之间", self.memcapMB]
                                                                  preferredStyle:UIAlertControllerStyleAlert];
            [err addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
            [self presentViewController:err animated:YES completion:nil];
            return;
        }
        if (self.currentBgSeconds < 60 || self.currentBgSeconds > 7200 || (self.bgEdited && (self.currentBgMinutes < 1 || self.currentBgMinutes > 120))) {
            UIAlertController *err = [UIAlertController alertControllerWithTitle:@"后台时长超出范围"
                                                                         message:@"后台判定时长必须在 1 到 120 分钟之间"
                                                                  preferredStyle:UIAlertControllerStyleAlert];
            [err addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
            [self presentViewController:err animated:YES completion:nil];
            return;
        }
        if (self.currentMax < 1 || self.currentMax > 10) {
            UIAlertController *err = [UIAlertController alertControllerWithTitle:@"次数超出范围"
                                                                         message:@"每天重启上限必须在 1 到 10 次之间"
                                                                  preferredStyle:UIAlertControllerStyleAlert];
            [err addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
            [self presentViewController:err animated:YES completion:nil];
            return;
        }

    }

    self.rule.thr = self.currentThr;
    if (self.bgEdited) {
        self.rule.bg = self.currentBgMinutes * 60;
    }
    self.rule.max = self.currentMax;
    self.rule.relaunch = self.relaunch ? 1 : 0;
    self.rule.isEnabled = self.isEnabled;
    self.rule.displayName = self.currentDisplayName;

    NSError *error = nil;
    BOOL ok = YES;
    if ([self.delegate respondsToSelector:@selector(restartRuleEditViewControllerDidSave:error:)]) {
        ok = [self.delegate restartRuleEditViewControllerDidSave:self.rule error:&error];
    }
    if (!ok) {
        NSString *errMsg = error ? error.localizedDescription : @"未知错误";
        UIAlertController *errAlert = [UIAlertController alertControllerWithTitle:@"保存配置失败"
                                                                         message:[NSString stringWithFormat:@"写入 restart.txt 失败：%@", errMsg]
                                                                  preferredStyle:UIAlertControllerStyleAlert];
        [errAlert addAction:[UIAlertAction actionWithTitle:@"确定" style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:errAlert animated:YES completion:nil];
        return;
    }
    [self dismissViewControllerAnimated:YES completion:nil];
}

#pragma mark - Table View

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    return self.isNewRule ? 3 : 4;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    if (section == 0) return 3;
    if (section == 1) return 1;
    if (section == 2) return 4;
    if (section == 3) return 1;
    return 0;
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    if (section == 0) return @"应用信息";
    if (section == 2) return @"参数设置";
    return nil;
}

- (UIView *)tableView:(UITableView *)tableView viewForFooterInSection:(NSInteger)section {
    if (section == 2) {
        BOOL showWarn = self.isEnabled && (self.appWarnThr > 0) && (self.currentThr <= self.appWarnThr);
        UIView *footer = [UIView new];
        UILabel *label = [UILabel new];
        label.translatesAutoresizingMaskIntoConstraints = NO;
        label.font = [UIFont systemFontOfSize:12 weight:UIFontWeightRegular];
        label.numberOfLines = 0;
        if (showWarn) {
            label.textColor = UIColor.systemOrangeColor;
            label.text = [NSString stringWithFormat:@"⚠️ 建议重启阈值高于释放阈值（当前释放阈值 %ld MB）", (long)self.appWarnThr];
        } else {
            label.textColor = UIColor.secondaryLabelColor;
            label.text = @"守护进程在后台定时检测内存；满足后台驻留时长且超限时将触发终止与拉起。";
        }
        [footer addSubview:label];
        [NSLayoutConstraint activateConstraints:@[
            [label.topAnchor constraintEqualToAnchor:footer.topAnchor constant:6],
            [label.leadingAnchor constraintEqualToAnchor:footer.leadingAnchor constant:16],
            [label.trailingAnchor constraintEqualToAnchor:footer.trailingAnchor constant:-16],
            [label.bottomAnchor constraintEqualToAnchor:footer.bottomAnchor constant:-6]
        ]];
        return footer;
    }
    return nil;
}

- (CGFloat)tableView:(UITableView *)tableView heightForFooterInSection:(NSInteger)section {
    return UITableViewAutomaticDimension;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    if (indexPath.section == 0) {
        UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
        if (indexPath.row == 0) {
            cell.textLabel.text = @"App 名称";
            NSString *dName = ResolveDisplayNameHelper([NSString stringWithFormat:@"bid:%@", self.rule.bid], self.installedApps);
            if (dName.length == 0) dName = self.currentDisplayName.length > 0 ? self.currentDisplayName : self.rule.bid;
            cell.detailTextLabel.text = dName;
            cell.imageView.image = JPIconForFragment([NSString stringWithFormat:@"bid:%@", self.rule.bid], self.installedApps);
        } else if (indexPath.row == 1) {
            cell.textLabel.text = @"Bundle ID";
            cell.detailTextLabel.text = self.rule.bid;
            cell.detailTextLabel.font = [UIFont monospacedSystemFontOfSize:13 weight:UIFontWeightRegular];
        } else if (indexPath.row == 2) {
            cell.textLabel.text = @"近 7 天自动重启";
            cell.detailTextLabel.text = [NSString stringWithFormat:@"%ld 次", (long)self.restart7DayCount];
        }
        return cell;
    } else if (indexPath.section == 1) {
        UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
        cell.textLabel.text = @"启用超内存重启";
        UISwitch *sw = [UISwitch new];
        sw.on = self.isEnabled;
        [sw addTarget:self action:@selector(masterSwitchToggled:) forControlEvents:UIControlEventValueChanged];
        cell.accessoryView = sw;
        return cell;
    } else if (indexPath.section == 2) {
        if (indexPath.row == 0) {
            UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
            cell.textLabel.text = @"重启阈值";
            cell.detailTextLabel.text = [NSString stringWithFormat:@"%ld MB", (long)self.currentThr];
            BOOL showWarn = self.isEnabled && (self.appWarnThr > 0) && (self.currentThr <= self.appWarnThr);
            if (!self.isEnabled) {
                cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
                cell.textLabel.textColor = UIColor.secondaryLabelColor;
                cell.accessoryType = UITableViewCellAccessoryNone;
                cell.selectionStyle = UITableViewCellSelectionStyleNone;
            } else {
                cell.detailTextLabel.textColor = showWarn ? UIColor.systemOrangeColor : UIColor.systemBlueColor;
                cell.textLabel.textColor = UIColor.labelColor;
                cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
                cell.selectionStyle = UITableViewCellSelectionStyleDefault;
            }
            return cell;
        } else if (indexPath.row == 1) {
            UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
            cell.textLabel.text = @"后台驻留时间";
            NSString *bgText = (self.currentBgSeconds % 60 == 0) ?
                [NSString stringWithFormat:@"%ld 分钟", (long)(self.currentBgSeconds / 60)] :
                [NSString stringWithFormat:@"%ld 秒", (long)self.currentBgSeconds];
            cell.detailTextLabel.text = bgText;
            if (!self.isEnabled) {
                cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
                cell.textLabel.textColor = UIColor.secondaryLabelColor;
                cell.accessoryType = UITableViewCellAccessoryNone;
                cell.selectionStyle = UITableViewCellSelectionStyleNone;
            } else {
                cell.detailTextLabel.textColor = UIColor.labelColor;
                cell.textLabel.textColor = UIColor.labelColor;
                cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
                cell.selectionStyle = UITableViewCellSelectionStyleDefault;
            }
            return cell;
        } else if (indexPath.row == 2) {
            UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
            cell.textLabel.text = @"每日最大重启次数";
            cell.detailTextLabel.text = [NSString stringWithFormat:@"%ld 次", (long)self.currentMax];
            if (!self.isEnabled) {
                cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
                cell.textLabel.textColor = UIColor.secondaryLabelColor;
                cell.accessoryType = UITableViewCellAccessoryNone;
                cell.selectionStyle = UITableViewCellSelectionStyleNone;
            } else {
                cell.detailTextLabel.textColor = UIColor.labelColor;
                cell.textLabel.textColor = UIColor.labelColor;
                cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
                cell.selectionStyle = UITableViewCellSelectionStyleDefault;
            }
            return cell;
        } else {
            UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
            cell.selectionStyle = UITableViewCellSelectionStyleNone;
            cell.textLabel.text = @"终止后自动拉起";
            if (!self.isEnabled) {
                cell.textLabel.textColor = UIColor.secondaryLabelColor;
            } else {
                cell.textLabel.textColor = UIColor.labelColor;
            }
            UISwitch *sw = [UISwitch new];
            sw.on = self.relaunch;
            sw.enabled = self.isEnabled;
            [sw addTarget:self action:@selector(relaunchSwitchToggled:) forControlEvents:UIControlEventValueChanged];
            cell.accessoryView = sw;
            return cell;
        }
    } else {
        UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
        cell.textLabel.text = @"删除规则";
        cell.textLabel.textColor = UIColor.systemRedColor;
        cell.textLabel.textAlignment = NSTextAlignmentCenter;
        return cell;
    }
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    if (indexPath.section == 2) {
        if (!self.isEnabled) return;
        if (indexPath.row == 0) {
            [self promptEditThreshold];
        } else if (indexPath.row == 1) {
            [self promptEditBgMinutes];
        } else if (indexPath.row == 2) {
            [self promptEditMax];
        }
    } else if (indexPath.section == 3) {
        [self confirmDelete];
    }
}

- (void)masterSwitchToggled:(UISwitch *)sender {
    self.isEnabled = sender.isOn;
    [self.tableView reloadData];
}

- (void)relaunchSwitchToggled:(UISwitch *)sender {
    self.relaunch = sender.isOn;
}

- (void)promptEditThreshold {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"设置重启阈值"
                                                                   message:[NSString stringWithFormat:@"请输入 200 到 %llu MB 之间的数值", self.memcapMB]
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField * _Nonnull textField) {
        textField.keyboardType = UIKeyboardTypeNumberPad;
        textField.placeholder = @"MB (200 - memcap)";
        textField.text = [NSString stringWithFormat:@"%ld", (long)self.currentThr];
    }];

    __weak typeof(alert) weakAlert = alert;
    __weak typeof(self) weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"确定" style:UIAlertActionStyleDefault handler:^(UIAlertAction * _Nonnull action) {
        NSString *txt = [weakAlert.textFields.firstObject.text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        NSInteger val = [txt integerValue];
        if (txt.length == 0 || val < 200 || (uint64_t)val > weakSelf.memcapMB) {
            UIAlertController *err = [UIAlertController alertControllerWithTitle:@"数值超出范围"
                                                                         message:[NSString stringWithFormat:@"重启阈值必须在 200 到 %llu MB 之间", weakSelf.memcapMB]
                                                                  preferredStyle:UIAlertControllerStyleAlert];
            [err addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
            [weakSelf presentViewController:err animated:YES completion:nil];
            return;
        }
        weakSelf.currentThr = val;
        [weakSelf.tableView reloadData];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)promptEditBgMinutes {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"设置后台驻留时间"
                                                                   message:@"请输入 1 到 120 之间的分钟数"
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField * _Nonnull textField) {
        textField.keyboardType = UIKeyboardTypeNumberPad;
        textField.placeholder = @"分钟 (1 - 120)";
        NSInteger defaultMin = (self.currentBgSeconds >= 60) ? (self.currentBgSeconds / 60) : 10;
        textField.text = [NSString stringWithFormat:@"%ld", (long)defaultMin];
    }];

    __weak typeof(alert) weakAlert = alert;
    __weak typeof(self) weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"确定" style:UIAlertActionStyleDefault handler:^(UIAlertAction * _Nonnull action) {
        NSString *txt = [weakAlert.textFields.firstObject.text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        NSInteger val = [txt integerValue];
        if (txt.length == 0 || val < 1 || val > 120) {
            UIAlertController *err = [UIAlertController alertControllerWithTitle:@"数值超出范围"
                                                                         message:@"后台判定时长必须在 1 到 120 分钟之间"
                                                                  preferredStyle:UIAlertControllerStyleAlert];
            [err addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
            [weakSelf presentViewController:err animated:YES completion:nil];
            return;
        }
        weakSelf.currentBgMinutes = val;
        weakSelf.currentBgSeconds = val * 60;
        weakSelf.bgEdited = YES;
        [weakSelf.tableView reloadData];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)promptEditMax {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"设置每日最大重启次数"
                                                                   message:@"请输入 1 到 10 之间的次数"
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField * _Nonnull textField) {
        textField.keyboardType = UIKeyboardTypeNumberPad;
        textField.placeholder = @"次数 (1 - 10)";
        textField.text = [NSString stringWithFormat:@"%ld", (long)self.currentMax];
    }];

    __weak typeof(alert) weakAlert = alert;
    __weak typeof(self) weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"确定" style:UIAlertActionStyleDefault handler:^(UIAlertAction * _Nonnull action) {
        NSString *txt = [weakAlert.textFields.firstObject.text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        NSInteger val = [txt integerValue];
        if (txt.length == 0 || val < 1 || val > 10) {
            UIAlertController *err = [UIAlertController alertControllerWithTitle:@"数值超出范围"
                                                                         message:@"每天重启上限必须在 1 到 10 次之间"
                                                                  preferredStyle:UIAlertControllerStyleAlert];
            [err addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
            [weakSelf presentViewController:err animated:YES completion:nil];
            return;
        }
        weakSelf.currentMax = val;
        [weakSelf.tableView reloadData];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)confirmDelete {
    NSString *dName = ResolveDisplayNameHelper([NSString stringWithFormat:@"bid:%@", self.rule.bid], self.installedApps);
    if (dName.length == 0) dName = self.currentDisplayName.length > 0 ? self.currentDisplayName : self.rule.bid;

    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"移除规则"
                                                                   message:[NSString stringWithFormat:@"确定要移除对「%@」的超内存重启规则吗？", dName]
                                                            preferredStyle:UIAlertControllerStyleActionSheet];
    __weak typeof(self) weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"删除" style:UIAlertActionStyleDestructive handler:^(UIAlertAction * _Nonnull action) {
        __strong typeof(weakSelf) strongSelf = weakSelf;
        if (!strongSelf) return;
        NSError *error = nil;
        BOOL ok = YES;
        if ([strongSelf.delegate respondsToSelector:@selector(restartRuleEditViewControllerDidDelete:error:)]) {
            ok = [strongSelf.delegate restartRuleEditViewControllerDidDelete:strongSelf.rule error:&error];
        }
        if (!ok) {
            NSString *title = @"删除配置失败";
            NSString *msg = [NSString stringWithFormat:@"写入 restart.txt 失败：%@", error ? error.localizedDescription : @"未知错误"];
            if ([error.domain isEqualToString:@"JetPilot"] && error.code == 1001) {
                title = @"提示";
                msg = error.localizedDescription;
            }
            UIAlertController *errAlert = [UIAlertController alertControllerWithTitle:title
                                                                             message:msg
                                                                      preferredStyle:UIAlertControllerStyleAlert];
            [errAlert addAction:[UIAlertAction actionWithTitle:@"确定" style:UIAlertActionStyleDefault handler:nil]];
            dispatch_async(dispatch_get_main_queue(), ^{
                [strongSelf presentViewController:errAlert animated:YES completion:nil];
            });
            return;
        }
        [strongSelf dismissViewControllerAnimated:YES completion:nil];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

@end


#pragma mark - Views: Pulse Rule Card (v0.7.8 C2-7)

@class JPPulseRuleCardView;

@protocol JPPulseRuleCardViewDelegate <NSObject>
@optional
- (void)pulseRuleCardViewDidTap:(JPPulseRuleCardView *)card;
- (void)pulseRuleCardViewDidToggle:(JPPulseRuleCardView *)card enabled:(BOOL)enabled;
- (void)pulseRuleCardViewDidRequestDelete:(JPPulseRuleCardView *)card;
@end

@interface JPPulseRuleCardView : UIView
@property (nonatomic, strong) JPPulseRule *rule;
@property (nonatomic, weak) id<JPPulseRuleCardViewDelegate> delegate;
@property (nonatomic, strong) UIImageView *iconImageView;
@property (nonatomic, strong) UILabel *titleLabel;
@property (nonatomic, strong) UILabel *fragLabel;
@property (nonatomic, strong) UILabel *paramLabel;
@property (nonatomic, strong) UILabel *statusLabel;
@property (nonatomic, strong) UILabel *thrLabel;
@property (nonatomic, strong) UISwitch *toggleSwitch;
@property (nonatomic, strong) UIButton *deleteButton;

- (instancetype)initWithRule:(JPPulseRule *)rule
                    delegate:(id<JPPulseRuleCardViewDelegate>)delegate
               installedApps:(NSArray<JPInstalledApp *> *)apps
                    appState:(JPPulseAppState *)appState;
@end

@implementation JPPulseRuleCardView

- (instancetype)initWithRule:(JPPulseRule *)rule
                    delegate:(id<JPPulseRuleCardViewDelegate>)delegate
               installedApps:(NSArray<JPInstalledApp *> *)apps
                    appState:(JPPulseAppState *)appState {
    self = [super initWithFrame:CGRectZero];
    if (self) {
        _rule = rule;
        _delegate = delegate;
        [self setupUIWithApps:apps appState:appState];
    }
    return self;
}

- (void)setupUIWithApps:(NSArray<JPInstalledApp *> *)apps appState:(JPPulseAppState *)appState {
    self.backgroundColor = UIColor.secondarySystemGroupedBackgroundColor;
    self.layer.cornerRadius = 14;
    self.layer.cornerCurve = kCACornerCurveContinuous;
    self.clipsToBounds = YES;
    self.translatesAutoresizingMaskIntoConstraints = NO;

    self.iconImageView = [UIImageView new];
    self.iconImageView.translatesAutoresizingMaskIntoConstraints = NO;
    self.iconImageView.layer.cornerRadius = 8;
    self.iconImageView.layer.cornerCurve = kCACornerCurveContinuous;
    self.iconImageView.clipsToBounds = YES;
    self.iconImageView.contentMode = UIViewContentModeScaleAspectFit;
    self.iconImageView.image = JPIconForFragment([NSString stringWithFormat:@"bid:%@", self.rule.bid], apps);
    [self addSubview:self.iconImageView];

    NSString *name = ResolveDisplayNameHelper([NSString stringWithFormat:@"bid:%@", self.rule.bid], apps);
    if (name.length == 0) name = self.rule.displayName.length > 0 ? self.rule.displayName : self.rule.bid;

    self.titleLabel = [UILabel new];
    self.titleLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.titleLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightSemibold];
    self.titleLabel.textColor = UIColor.labelColor;
    self.titleLabel.text = name;
    [self addSubview:self.titleLabel];

    self.fragLabel = [UILabel new];
    self.fragLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.fragLabel.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
    self.fragLabel.textColor = UIColor.secondaryLabelColor;
    self.fragLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
    self.fragLabel.text = self.rule.bid;
    [self addSubview:self.fragLabel];

    self.paramLabel = [UILabel new];
    self.paramLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.paramLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightRegular];
    self.paramLabel.textColor = UIColor.secondaryLabelColor;
    self.paramLabel.numberOfLines = 1;
    self.paramLabel.text = [NSString stringWithFormat:@"目标 %ld MB · 等待 %ld 秒 · 间隔 %ld 分 · 每天 %ld 次",
                            (long)self.rule.target, (long)self.rule.delay, (long)self.rule.n, (long)self.rule.daymax];
    [self addSubview:self.paramLabel];

    self.statusLabel = [UILabel new];
    self.statusLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.statusLabel.font = [UIFont systemFontOfSize:12 weight:UIFontWeightRegular];
    self.statusLabel.numberOfLines = 1;

    JPRestartConfig *restartCfg = [JPRestartConfig loadFromPath:kRestartConfig];
    JPRestartRule *rst = [restartCfg ruleForBundleId:self.rule.bid];
    BOOL targetTooHigh = (self.rule.enabled && rst && rst.isEnabled && rst.thr > 0 && self.rule.target > rst.thr - 200);

    if (!self.rule.enabled) {
        self.statusLabel.text = @"规则已关闭";
        self.statusLabel.textColor = UIColor.secondaryLabelColor;
        self.alpha = 0.65;
    } else if (self.rule.isDuplicate) {
        self.statusLabel.text = @"⚠️ 重复，只有第一条生效";
        self.statusLabel.textColor = UIColor.systemRedColor;
        self.alpha = 1.0;
    } else if (appState.trippedToday) {
        self.statusLabel.text = @"⚠️ 今日已熔断（切后台后异常退出）";
        self.statusLabel.textColor = UIColor.systemRedColor;
        self.alpha = 1.0;
    } else if (targetTooHigh) {
        self.statusLabel.text = @"目标高于重启阈值 − 200，释放可能来不及";
        self.statusLabel.textColor = UIColor.systemOrangeColor;
        self.alpha = 1.0;
    } else if (appState.unresponsive) {
        self.statusLabel.text = @"⚠️ 此 App 对释放无响应";
        self.statusLabel.textColor = UIColor.systemOrangeColor;
        self.alpha = 1.0;
    } else if (appState.dayCount > 0) {
        self.statusLabel.text = [NSString stringWithFormat:@"今日已释放 %ld 次", (long)appState.dayCount];
        self.statusLabel.textColor = UIColor.systemGreenColor;
        self.alpha = 1.0;
    } else {
        self.statusLabel.text = @"后台满足条件时自动释放";
        self.statusLabel.textColor = UIColor.secondaryLabelColor;
        self.alpha = 1.0;
    }
    [self addSubview:self.statusLabel];

    self.thrLabel = [UILabel new];
    self.thrLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.thrLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightBold];
    self.thrLabel.textAlignment = NSTextAlignmentRight;
    self.thrLabel.text = [NSString stringWithFormat:@"%ld MB", (long)self.rule.target];
    [self addSubview:self.thrLabel];

    self.deleteButton = [UIButton buttonWithType:UIButtonTypeSystem];
    self.deleteButton.translatesAutoresizingMaskIntoConstraints = NO;
    [self.deleteButton setImage:[UIImage systemImageNamed:@"trash"] forState:UIControlStateNormal];
    self.deleteButton.tintColor = UIColor.systemRedColor;
    [self.deleteButton addTarget:self action:@selector(deleteTapped) forControlEvents:UIControlEventTouchUpInside];
    [self addSubview:self.deleteButton];

    self.toggleSwitch = [UISwitch new];
    self.toggleSwitch.translatesAutoresizingMaskIntoConstraints = NO;
    self.toggleSwitch.on = self.rule.enabled;
    [self.toggleSwitch addTarget:self action:@selector(switchToggled:) forControlEvents:UIControlEventValueChanged];
    [self addSubview:self.toggleSwitch];

    UIImageView *chevron = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"chevron.right"]];
    chevron.translatesAutoresizingMaskIntoConstraints = NO;
    chevron.tintColor = UIColor.tertiaryLabelColor;
    chevron.contentMode = UIViewContentModeScaleAspectFit;
    [self addSubview:chevron];

    [NSLayoutConstraint activateConstraints:@[
        [self.iconImageView.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:14],
        [self.iconImageView.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [self.iconImageView.widthAnchor constraintEqualToConstant:40],
        [self.iconImageView.heightAnchor constraintEqualToConstant:40],

        [chevron.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-14],
        [chevron.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [chevron.widthAnchor constraintEqualToConstant:8],
        [chevron.heightAnchor constraintEqualToConstant:13],

        [self.deleteButton.trailingAnchor constraintEqualToAnchor:chevron.leadingAnchor constant:-4],
        [self.deleteButton.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [self.deleteButton.widthAnchor constraintEqualToConstant:32],
        [self.deleteButton.heightAnchor constraintEqualToConstant:32],

        [self.toggleSwitch.trailingAnchor constraintEqualToAnchor:self.deleteButton.leadingAnchor constant:-8],
        [self.toggleSwitch.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],

        [self.thrLabel.trailingAnchor constraintEqualToAnchor:self.toggleSwitch.leadingAnchor constant:-8],
        [self.thrLabel.topAnchor constraintEqualToAnchor:self.topAnchor constant:12],
        [self.thrLabel.widthAnchor constraintGreaterThanOrEqualToConstant:60],

        [self.titleLabel.topAnchor constraintEqualToAnchor:self.topAnchor constant:12],
        [self.titleLabel.leadingAnchor constraintEqualToAnchor:self.iconImageView.trailingAnchor constant:12],
        [self.titleLabel.trailingAnchor constraintLessThanOrEqualToAnchor:self.thrLabel.leadingAnchor constant:-8],

        [self.fragLabel.topAnchor constraintEqualToAnchor:self.titleLabel.bottomAnchor constant:2],
        [self.fragLabel.leadingAnchor constraintEqualToAnchor:self.iconImageView.trailingAnchor constant:12],
        [self.fragLabel.trailingAnchor constraintLessThanOrEqualToAnchor:self.thrLabel.leadingAnchor constant:-8],

        [self.paramLabel.topAnchor constraintEqualToAnchor:self.fragLabel.bottomAnchor constant:3],
        [self.paramLabel.leadingAnchor constraintEqualToAnchor:self.iconImageView.trailingAnchor constant:12],
        [self.paramLabel.trailingAnchor constraintLessThanOrEqualToAnchor:self.toggleSwitch.leadingAnchor constant:-8],

        [self.statusLabel.topAnchor constraintEqualToAnchor:self.paramLabel.bottomAnchor constant:3],
        [self.statusLabel.leadingAnchor constraintEqualToAnchor:self.iconImageView.trailingAnchor constant:12],
        [self.statusLabel.trailingAnchor constraintLessThanOrEqualToAnchor:self.toggleSwitch.leadingAnchor constant:-8],
        [self.statusLabel.bottomAnchor constraintEqualToAnchor:self.bottomAnchor constant:-12]
    ]];

    UITapGestureRecognizer *tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(cardTapped:)];
    [self addGestureRecognizer:tap];
    self.userInteractionEnabled = YES;
}

- (void)cardTapped:(UITapGestureRecognizer *)sender {
    CGPoint pt = [sender locationInView:self];
    if (CGRectContainsPoint(self.deleteButton.frame, pt) || CGRectContainsPoint(self.toggleSwitch.frame, pt)) {
        return;
    }
    if ([self.delegate respondsToSelector:@selector(pulseRuleCardViewDidTap:)]) {
        [self.delegate pulseRuleCardViewDidTap:self];
    }
}

- (void)switchToggled:(UISwitch *)sender {
    self.rule.enabled = sender.isOn;
    self.alpha = sender.isOn ? 1.0 : 0.65;
    if (!sender.isOn) {
        self.statusLabel.text = @"规则已关闭";
        self.statusLabel.textColor = UIColor.secondaryLabelColor;
    } else {
        JPRestartConfig *restartCfg = [JPRestartConfig loadFromPath:kRestartConfig];
        JPRestartRule *rst = [restartCfg ruleForBundleId:self.rule.bid];
        if (rst && rst.isEnabled && rst.thr > 0 && self.rule.target > rst.thr - 200) {
            self.statusLabel.text = @"目标高于重启阈值 − 200，释放可能来不及";
            self.statusLabel.textColor = UIColor.systemOrangeColor;
        } else {
            self.statusLabel.text = @"后台满足条件时自动释放";
            self.statusLabel.textColor = UIColor.secondaryLabelColor;
        }
    }
    if ([self.delegate respondsToSelector:@selector(pulseRuleCardViewDidToggle:enabled:)]) {
        [self.delegate pulseRuleCardViewDidToggle:self enabled:sender.isOn];
    }
}

- (void)deleteTapped {
    if ([self.delegate respondsToSelector:@selector(pulseRuleCardViewDidRequestDelete:)]) {
        [self.delegate pulseRuleCardViewDidRequestDelete:self];
    }
}

@end

#pragma mark - Views: Pulse Unknown Line Card (v0.7.8 C2-7)

@class JPPulseUnknownLineCardView;

@protocol JPPulseUnknownLineCardViewDelegate <NSObject>
@optional
- (void)pulseUnknownLineCardViewDidRequestDelete:(JPPulseLine *)line;
@end

@interface JPPulseUnknownLineCardView : UIView
@property (nonatomic, strong) JPPulseLine *line;
@property (nonatomic, weak) id<JPPulseUnknownLineCardViewDelegate> delegate;
@property (nonatomic, strong) UILabel *titleLabel;
@property (nonatomic, strong) UILabel *contentLabel;
@property (nonatomic, strong) UIButton *deleteButton;
- (instancetype)initWithLine:(JPPulseLine *)line delegate:(id<JPPulseUnknownLineCardViewDelegate>)delegate;
@end

@implementation JPPulseUnknownLineCardView

- (instancetype)initWithLine:(JPPulseLine *)line delegate:(id<JPPulseUnknownLineCardViewDelegate>)delegate {
    self = [super initWithFrame:CGRectZero];
    if (self) {
        _line = line;
        _delegate = delegate;
        [self setupUI];
    }
    return self;
}

- (void)setupUI {
    self.backgroundColor = UIColor.secondarySystemGroupedBackgroundColor;
    self.layer.cornerRadius = 14;
    self.layer.cornerCurve = kCACornerCurveContinuous;
    self.clipsToBounds = YES;
    self.translatesAutoresizingMaskIntoConstraints = NO;

    self.titleLabel = [UILabel new];
    self.titleLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.titleLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightMedium];
    self.titleLabel.textColor = UIColor.systemOrangeColor;
    self.titleLabel.text = @"无效配置行（点击右侧删除）";
    [self addSubview:self.titleLabel];

    self.contentLabel = [UILabel new];
    self.contentLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.contentLabel.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
    self.contentLabel.textColor = UIColor.secondaryLabelColor;
    self.contentLabel.numberOfLines = 0;
    self.contentLabel.text = self.line.rawText ?: @"";
    [self addSubview:self.contentLabel];

    self.deleteButton = [UIButton buttonWithType:UIButtonTypeSystem];
    self.deleteButton.translatesAutoresizingMaskIntoConstraints = NO;
    [self.deleteButton setImage:[UIImage systemImageNamed:@"trash"] forState:UIControlStateNormal];
    self.deleteButton.tintColor = UIColor.systemRedColor;
    [self.deleteButton addTarget:self action:@selector(deleteTapped) forControlEvents:UIControlEventTouchUpInside];
    [self addSubview:self.deleteButton];

    [NSLayoutConstraint activateConstraints:@[
        [self.deleteButton.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-14],
        [self.deleteButton.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [self.deleteButton.widthAnchor constraintEqualToConstant:32],
        [self.deleteButton.heightAnchor constraintEqualToConstant:32],

        [self.titleLabel.topAnchor constraintEqualToAnchor:self.topAnchor constant:12],
        [self.titleLabel.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:14],
        [self.titleLabel.trailingAnchor constraintLessThanOrEqualToAnchor:self.deleteButton.leadingAnchor constant:-8],

        [self.contentLabel.topAnchor constraintEqualToAnchor:self.titleLabel.bottomAnchor constant:4],
        [self.contentLabel.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:14],
        [self.contentLabel.trailingAnchor constraintLessThanOrEqualToAnchor:self.deleteButton.leadingAnchor constant:-8],
        [self.contentLabel.bottomAnchor constraintEqualToAnchor:self.bottomAnchor constant:-12]
    ]];
}

- (void)deleteTapped {
    if ([self.delegate respondsToSelector:@selector(pulseUnknownLineCardViewDidRequestDelete:)]) {
        [self.delegate pulseUnknownLineCardViewDidRequestDelete:self.line];
    }
}

@end

#pragma mark - Controllers: Pulse Rule Edit (v0.7.8 C2-7)

@class JPPulseRuleEditViewController;

@protocol JPPulseRuleEditViewControllerDelegate <NSObject>
- (BOOL)pulseRuleEditViewControllerDidSave:(JPPulseRule *)rule error:(NSError **)error;
- (BOOL)pulseRuleEditViewControllerDidDelete:(JPPulseRule *)rule error:(NSError **)error;
@end

@interface JPPulseRuleEditViewController : UIViewController <UITableViewDataSource, UITableViewDelegate>
@property (nonatomic, strong) JPPulseRule *rule;
@property (nonatomic, copy) NSArray<JPInstalledApp *> *installedApps;
@property (nonatomic, assign) BOOL isNewRule;
@property (nonatomic, weak) id<JPPulseRuleEditViewControllerDelegate> delegate;

@property (nonatomic, strong) UITableView *tableView;
@property (nonatomic, assign) NSInteger currentTarget;
@property (nonatomic, assign) NSInteger currentDelay;
@property (nonatomic, assign) NSInteger currentN;
@property (nonatomic, assign) NSInteger currentDaymax;
@property (nonatomic, assign) BOOL isEnabled;
@property (nonatomic, copy) NSString *currentDisplayName;
@end

@implementation JPPulseRuleEditViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = self.isNewRule ? @"添加释放规则" : @"编辑释放规则";
    self.view.backgroundColor = UIColor.systemGroupedBackgroundColor;

    self.currentTarget = (self.rule.target >= 300 && self.rule.target <= 1500) ? self.rule.target : 550;
    self.currentDelay = (self.rule.delay >= 3 && self.rule.delay <= 20) ? self.rule.delay : 10;
    self.currentN = (self.rule.n >= 5 && self.rule.n <= 60) ? self.rule.n : 10;
    self.currentDaymax = (self.rule.daymax >= 1 && self.rule.daymax <= 100) ? self.rule.daymax : 30;
    self.isEnabled = self.rule.enabled;
    self.currentDisplayName = self.rule.displayName ?: @"";

    self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"取消"
                                                                             style:UIBarButtonItemStylePlain
                                                                            target:self
                                                                            action:@selector(cancelTapped)];
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"保存"
                                                                              style:UIBarButtonItemStyleDone
                                                                             target:self
                                                                             action:@selector(saveTapped)];

    self.tableView = [[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStyleInsetGrouped];
    self.tableView.translatesAutoresizingMaskIntoConstraints = NO;
    self.tableView.dataSource = self;
    self.tableView.delegate = self;
    self.tableView.estimatedSectionFooterHeight = 44.0;
    self.tableView.sectionFooterHeight = UITableViewAutomaticDimension;
    [self.view addSubview:self.tableView];

    [NSLayoutConstraint activateConstraints:@[
        [self.tableView.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [self.tableView.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [self.tableView.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [self.tableView.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor]
    ]];
}

- (void)cancelTapped {
    [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)saveTapped {
    if (self.isEnabled) {
        if (self.currentTarget < 300 || self.currentTarget > 1500) {
            UIAlertController *err = [UIAlertController alertControllerWithTitle:@"目标值超出范围"
                                                                         message:@"目标内存必须在 300 到 1500 MB 之间"
                                                                  preferredStyle:UIAlertControllerStyleAlert];
            [err addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
            [self presentViewController:err animated:YES completion:nil];
            return;
        }
        if (self.currentDelay < 3 || self.currentDelay > 20) {
            UIAlertController *err = [UIAlertController alertControllerWithTitle:@"等待时间超出范围"
                                                                         message:@"后台等待时间必须在 3 到 20 秒之间"
                                                                  preferredStyle:UIAlertControllerStyleAlert];
            [err addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
            [self presentViewController:err animated:YES completion:nil];
            return;
        }
        if (self.currentN < 5 || self.currentN > 60) {
            UIAlertController *err = [UIAlertController alertControllerWithTitle:@"间隔超出范围"
                                                                         message:@"释放最小间隔必须在 5 到 60 分钟之间"
                                                                  preferredStyle:UIAlertControllerStyleAlert];
            [err addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
            [self presentViewController:err animated:YES completion:nil];
            return;
        }
        if (self.currentDaymax < 1 || self.currentDaymax > 100) {
            UIAlertController *err = [UIAlertController alertControllerWithTitle:@"上限超出范围"
                                                                         message:@"每天释放上限必须在 1 到 100 次之间"
                                                                  preferredStyle:UIAlertControllerStyleAlert];
            [err addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
            [self presentViewController:err animated:YES completion:nil];
            return;
        }

        // 有重启规则的才做 target <= 阈值 - 200 校验 (Q14)
        JPRestartConfig *restartCfg = [JPRestartConfig loadFromPath:kRestartConfig];
        JPRestartRule *rst = [restartCfg ruleForBundleId:self.rule.bid];
        if (rst && rst.thr > 0) {
            if (self.currentTarget > rst.thr - 200) {
                UIAlertController *err = [UIAlertController alertControllerWithTitle:@"目标值过高"
                                                                             message:[NSString stringWithFormat:@"目标值必须比该 App 的超内存重启阈值（%ld MB）低至少 200 MB。当前重启阈值为 %ld MB，目标值最高只能设为 %ld MB。", (long)rst.thr, (long)rst.thr, (long)(rst.thr - 200)]
                                                                      preferredStyle:UIAlertControllerStyleAlert];
                [err addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
                [self presentViewController:err animated:YES completion:nil];
                return;
            }
        }
    }

    self.rule.target = self.currentTarget;
    self.rule.delay = self.currentDelay;
    self.rule.n = self.currentN;
    self.rule.daymax = self.currentDaymax;
    self.rule.enabled = self.isEnabled;
    self.rule.displayName = self.currentDisplayName;

    NSError *error = nil;
    BOOL ok = YES;
    if ([self.delegate respondsToSelector:@selector(pulseRuleEditViewControllerDidSave:error:)]) {
        ok = [self.delegate pulseRuleEditViewControllerDidSave:self.rule error:&error];
    }
    if (!ok) {
        NSString *errMsg = error ? error.localizedDescription : @"未知错误";
        UIAlertController *errAlert = [UIAlertController alertControllerWithTitle:@"保存配置失败"
                                                                         message:[NSString stringWithFormat:@"写入 pulse.txt 失败：%@", errMsg]
                                                                  preferredStyle:UIAlertControllerStyleAlert];
        [errAlert addAction:[UIAlertAction actionWithTitle:@"确定" style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:errAlert animated:YES completion:nil];
        return;
    }
    [self dismissViewControllerAnimated:YES completion:nil];
}

#pragma mark - Table View

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    return self.isNewRule ? 3 : 4;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    if (section == 0) return 2;
    if (section == 1) return 1;
    if (section == 2) return 4;
    if (section == 3) return 1;
    return 0;
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    if (section == 0) return @"应用信息";
    if (section == 1) return @"功能开关";
    if (section == 2) return @"释放参数设置";
    return nil;
}

- (UIView *)tableView:(UITableView *)tableView viewForFooterInSection:(NSInteger)section {
    if (section == 1) {
        UIView *footer = [UIView new];
        UILabel *label = [UILabel new];
        label.translatesAutoresizingMaskIntoConstraints = NO;
        label.font = [UIFont systemFontOfSize:12 weight:UIFontWeightRegular];
        label.numberOfLines = 0;
        label.textColor = UIColor.secondaryLabelColor;
        label.text = @"切到后台时提示 App 释放内存，不会结束 App。";
        [footer addSubview:label];
        [NSLayoutConstraint activateConstraints:@[
            [label.topAnchor constraintEqualToAnchor:footer.topAnchor constant:6],
            [label.leadingAnchor constraintEqualToAnchor:footer.leadingAnchor constant:16],
            [label.trailingAnchor constraintEqualToAnchor:footer.trailingAnchor constant:-16],
            [label.bottomAnchor constraintEqualToAnchor:footer.bottomAnchor constant:-6]
        ]];
        return footer;
    }
    if (section == 2) {
        UIView *footer = [UIView new];
        UILabel *label = [UILabel new];
        label.translatesAutoresizingMaskIntoConstraints = NO;
        label.font = [UIFont systemFontOfSize:12 weight:UIFontWeightRegular];
        label.numberOfLines = 0;
        label.textColor = UIColor.secondaryLabelColor;
        label.text = [NSString stringWithFormat:@"后台等待时间：切到后台大约 %ld 秒后释放一次（可能提前几秒）；回前台以前不会再动。\n目标内存：当进入后台且物理内存达到此值时尝试释放。", (long)self.currentDelay];
        [footer addSubview:label];
        [NSLayoutConstraint activateConstraints:@[
            [label.topAnchor constraintEqualToAnchor:footer.topAnchor constant:6],
            [label.leadingAnchor constraintEqualToAnchor:footer.leadingAnchor constant:16],
            [label.trailingAnchor constraintEqualToAnchor:footer.trailingAnchor constant:-16],
            [label.bottomAnchor constraintEqualToAnchor:footer.bottomAnchor constant:-6]
        ]];
        return footer;
    }
    return nil;
}

- (CGFloat)tableView:(UITableView *)tableView heightForFooterInSection:(NSInteger)section {
    return UITableViewAutomaticDimension;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    if (indexPath.section == 0) {
        UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
        if (indexPath.row == 0) {
            cell.textLabel.text = @"App 名称";
            NSString *dName = ResolveDisplayNameHelper([NSString stringWithFormat:@"bid:%@", self.rule.bid], self.installedApps);
            if (dName.length == 0) dName = self.currentDisplayName.length > 0 ? self.currentDisplayName : self.rule.bid;
            cell.detailTextLabel.text = dName;
            cell.imageView.image = JPIconForFragment([NSString stringWithFormat:@"bid:%@", self.rule.bid], self.installedApps);
        } else if (indexPath.row == 1) {
            cell.textLabel.text = @"Bundle ID";
            cell.detailTextLabel.text = self.rule.bid;
            cell.detailTextLabel.font = [UIFont monospacedSystemFontOfSize:13 weight:UIFontWeightRegular];
        }
        return cell;
    } else if (indexPath.section == 1) {
        UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
        cell.textLabel.text = @"启用后台释放";
        UISwitch *sw = [UISwitch new];
        sw.on = self.isEnabled;
        [sw addTarget:self action:@selector(masterSwitchToggled:) forControlEvents:UIControlEventValueChanged];
        cell.accessoryView = sw;
        return cell;
    } else if (indexPath.section == 2) {
        UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
        if (!self.isEnabled) {
            cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
            cell.textLabel.textColor = UIColor.secondaryLabelColor;
            cell.accessoryType = UITableViewCellAccessoryNone;
            cell.selectionStyle = UITableViewCellSelectionStyleNone;
        } else {
            cell.detailTextLabel.textColor = UIColor.labelColor;
            cell.textLabel.textColor = UIColor.labelColor;
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
            cell.selectionStyle = UITableViewCellSelectionStyleDefault;
        }

        if (indexPath.row == 0) {
            cell.textLabel.text = @"目标内存";
            cell.detailTextLabel.text = [NSString stringWithFormat:@"%ld MB", (long)self.currentTarget];
            if (self.isEnabled) cell.detailTextLabel.textColor = UIColor.systemBlueColor;
        } else if (indexPath.row == 1) {
            cell.textLabel.text = @"后台等待时间";
            cell.detailTextLabel.text = [NSString stringWithFormat:@"%ld 秒", (long)self.currentDelay];
        } else if (indexPath.row == 2) {
            cell.textLabel.text = @"释放最小间隔";
            cell.detailTextLabel.text = [NSString stringWithFormat:@"%ld 分钟", (long)self.currentN];
        } else {
            cell.textLabel.text = @"每天释放上限";
            cell.detailTextLabel.text = [NSString stringWithFormat:@"%ld 次", (long)self.currentDaymax];
        }
        return cell;
    } else {
        UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
        cell.textLabel.text = @"删除规则";
        cell.textLabel.textColor = UIColor.systemRedColor;
        cell.textLabel.textAlignment = NSTextAlignmentCenter;
        return cell;
    }
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    if (indexPath.section == 2) {
        if (!self.isEnabled) return;
        if (indexPath.row == 0) {
            [self promptEditTarget];
        } else if (indexPath.row == 1) {
            [self promptEditDelay];
        } else if (indexPath.row == 2) {
            [self promptEditN];
        } else if (indexPath.row == 3) {
            [self promptEditDaymax];
        }
    } else if (indexPath.section == 3) {
        [self confirmDelete];
    }
}

- (void)masterSwitchToggled:(UISwitch *)sender {
    self.isEnabled = sender.isOn;
    [self.tableView reloadData];
}

- (void)promptEditTarget {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"设置目标内存"
                                                                   message:@"请输入 300 到 1500 MB 之间的数值"
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField * _Nonnull textField) {
        textField.keyboardType = UIKeyboardTypeNumberPad;
        textField.placeholder = @"MB (300 - 1500)";
        textField.text = [NSString stringWithFormat:@"%ld", (long)self.currentTarget];
    }];

    __weak typeof(alert) weakAlert = alert;
    __weak typeof(self) weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"确定" style:UIAlertActionStyleDefault handler:^(UIAlertAction * _Nonnull action) {
        NSString *txt = [weakAlert.textFields.firstObject.text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        NSInteger val = [txt integerValue];
        if (txt.length == 0 || val < 300 || val > 1500) {
            UIAlertController *err = [UIAlertController alertControllerWithTitle:@"数值超出范围"
                                                                         message:@"目标内存必须在 300 到 1500 MB 之间"
                                                                  preferredStyle:UIAlertControllerStyleAlert];
            [err addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
            [weakSelf presentViewController:err animated:YES completion:nil];
            return;
        }
        weakSelf.currentTarget = val;
        [weakSelf.tableView reloadData];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)promptEditDelay {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"设置后台等待时间"
                                                                   message:@"切后台满这么多秒后释放一次；回前台前不会再动"
                                                            preferredStyle:UIAlertControllerStyleActionSheet];
    __weak typeof(self) weakSelf = self;
    NSArray<NSNumber *> *options = @[@5, @10, @15, @20];
    for (NSNumber *opt in options) {
        NSInteger s = [opt integerValue];
        NSString *title = [NSString stringWithFormat:@"%ld 秒%@", (long)s, (s == self.currentDelay ? @"（当前）" : (s == 10 ? @"（默认）" : @""))];
        [alert addAction:[UIAlertAction actionWithTitle:title style:UIAlertActionStyleDefault handler:^(UIAlertAction * _Nonnull action) {
            weakSelf.currentDelay = s;
            [weakSelf.tableView reloadData];
        }]];
    }
    [alert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];

    if (alert.popoverPresentationController) {
        UITableViewCell *cell = [self.tableView cellForRowAtIndexPath:[NSIndexPath indexPathForRow:1 inSection:2]];
        if (cell) {
            alert.popoverPresentationController.sourceView = cell;
            alert.popoverPresentationController.sourceRect = cell.bounds;
        } else {
            alert.popoverPresentationController.sourceView = self.view;
            alert.popoverPresentationController.sourceRect = CGRectMake(self.view.bounds.size.width / 2, self.view.bounds.size.height / 2, 1, 1);
        }
    }
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)promptEditN {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"设置释放最小间隔"
                                                                   message:@"两次释放之间的最小间隔时间。请输入 5 到 60 之间的分钟数"
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField * _Nonnull textField) {
        textField.keyboardType = UIKeyboardTypeNumberPad;
        textField.placeholder = @"分钟 (5 - 60)";
        textField.text = [NSString stringWithFormat:@"%ld", (long)self.currentN];
    }];

    __weak typeof(alert) weakAlert = alert;
    __weak typeof(self) weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"确定" style:UIAlertActionStyleDefault handler:^(UIAlertAction * _Nonnull action) {
        NSString *txt = [weakAlert.textFields.firstObject.text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        NSInteger val = [txt integerValue];
        if (txt.length == 0 || val < 5 || val > 60) {
            UIAlertController *err = [UIAlertController alertControllerWithTitle:@"数值超出范围"
                                                                         message:@"释放最小间隔必须在 5 到 60 分钟之间"
                                                                  preferredStyle:UIAlertControllerStyleAlert];
            [err addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
            [weakSelf presentViewController:err animated:YES completion:nil];
            return;
        }
        weakSelf.currentN = val;
        [weakSelf.tableView reloadData];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)promptEditDaymax {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"设置每天释放上限"
                                                                   message:@"每个 App 每天最多释放次数。请输入 1 到 100 之间的次数"
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField * _Nonnull textField) {
        textField.keyboardType = UIKeyboardTypeNumberPad;
        textField.placeholder = @"次数 (1 - 100)";
        textField.text = [NSString stringWithFormat:@"%ld", (long)self.currentDaymax];
    }];

    __weak typeof(alert) weakAlert = alert;
    __weak typeof(self) weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"确定" style:UIAlertActionStyleDefault handler:^(UIAlertAction * _Nonnull action) {
        NSString *txt = [weakAlert.textFields.firstObject.text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        NSInteger val = [txt integerValue];
        if (txt.length == 0 || val < 1 || val > 100) {
            UIAlertController *err = [UIAlertController alertControllerWithTitle:@"数值超出范围"
                                                                         message:@"每天释放上限必须在 1 到 100 次之间"
                                                                  preferredStyle:UIAlertControllerStyleAlert];
            [err addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
            [weakSelf presentViewController:err animated:YES completion:nil];
            return;
        }
        weakSelf.currentDaymax = val;
        [weakSelf.tableView reloadData];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)confirmDelete {
    NSString *dName = ResolveDisplayNameHelper([NSString stringWithFormat:@"bid:%@", self.rule.bid], self.installedApps);
    if (dName.length == 0) dName = self.currentDisplayName.length > 0 ? self.currentDisplayName : self.rule.bid;

    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"移除规则"
                                                                   message:[NSString stringWithFormat:@"确定要移除对「%@」的后台释放规则吗？", dName]
                                                            preferredStyle:UIAlertControllerStyleActionSheet];

    __weak typeof(self) weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"移除" style:UIAlertActionStyleDestructive handler:^(UIAlertAction * _Nonnull action) {
        __strong typeof(weakSelf) strongSelf = weakSelf;
        if (!strongSelf) return;
        NSError *error = nil;
        BOOL ok = YES;
        if ([strongSelf.delegate respondsToSelector:@selector(pulseRuleEditViewControllerDidDelete:error:)]) {
            ok = [strongSelf.delegate pulseRuleEditViewControllerDidDelete:strongSelf.rule error:&error];
        }
        if (!ok) {
            NSString *title = @"删除规则失败";
            NSString *msg = @"未知错误";
            if (error) {
                msg = error.localizedDescription;
            }
            UIAlertController *errAlert = [UIAlertController alertControllerWithTitle:title
                                                                             message:msg
                                                                      preferredStyle:UIAlertControllerStyleAlert];
            [errAlert addAction:[UIAlertAction actionWithTitle:@"确定" style:UIAlertActionStyleDefault handler:nil]];
            dispatch_async(dispatch_get_main_queue(), ^{
                [strongSelf presentViewController:errAlert animated:YES completion:nil];
            });
            return;
        }
        [strongSelf dismissViewControllerAnimated:YES completion:nil];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

@end


#pragma mark - Controllers: Restart ViewController

@interface JPRestartViewController : UIViewController <JPRestartRuleCardViewDelegate, JPRestartRuleEditViewControllerDelegate, JPRestartUnknownLineCardViewDelegate, JPPulseRuleCardViewDelegate, JPPulseRuleEditViewControllerDelegate, JPPulseUnknownLineCardViewDelegate>
@property (nonatomic, strong) JPRestartConfig *config;
@property (nonatomic, strong) JPPulseConfig *pulseConfig;
@property (nonatomic, copy) NSArray<JPInstalledApp *> *installedApps;

@property (nonatomic, strong) UIScrollView *scrollView;
@property (nonatomic, strong) UIStackView *contentStackView;

@property (nonatomic, strong) UIButton *deadBanner;
@property (nonatomic, strong) UIButton *versionWarningBanner;
@property (nonatomic, strong) UIView *pulseCorruptBanner;
@property (nonatomic, strong) UILabel *pulseCorruptLabel;
@property (nonatomic, strong) UILabel *moduleStatusLabel;
@property (nonatomic, strong) UILabel *rulesSectionHeaderLabel;
@property (nonatomic, strong) UIStackView *rulesStackView;
@property (nonatomic, strong) UIView *addRuleCard;

@property (nonatomic, strong) UILabel *pulseSectionHeaderLabel;
@property (nonatomic, strong) UIStackView *pulseRulesStackView;
@property (nonatomic, strong) UIView *addPulseRuleCard;

@property (nonatomic, strong) UIView *saveNoticeToast;
@property (nonatomic, strong) NSTimer *saveNoticeTimer;
@property (nonatomic, strong) NSTimer *pollingTimer;
@property (nonatomic, assign) CFAbsoluteTime lastSpawnTime;
@property (nonatomic, assign) BOOL restartSupport;
@property (nonatomic, assign) BOOL pulseSupport;
@property (nonatomic, assign) BOOL restartInProgress;
@end

@implementation JPRestartViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"超内存重启";
    self.view.backgroundColor = UIColor.systemGroupedBackgroundColor;
    self.navigationController.navigationBar.prefersLargeTitles = YES;

    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"arrow.clockwise"]
                                                                              style:UIBarButtonItemStylePlain
                                                                             target:self
                                                                             action:@selector(refreshStatus)];

    [self setupScrollView];
    [self setupSaveNoticeToast];
    [self loadConfig];
    [self rebuildRulesCards];
    [self rebuildPulseCards];
    [self refreshStatus];

    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(refreshStatus)
                                                 name:UIApplicationDidBecomeActiveNotification
                                               object:nil];
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(stopPollingTimer)
                                                 name:UIApplicationDidEnterBackgroundNotification
                                               object:nil];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self loadConfig];
    [self rebuildRulesCards];
    [self rebuildPulseCards];
    [self refreshStatus];
    [self startPollingTimer];
}

- (void)viewWillDisappear:(BOOL)animated {
    [super viewWillDisappear:animated];
    [self stopPollingTimer];
}

- (void)dealloc {
    [self stopPollingTimer];
    [self.saveNoticeTimer invalidate];
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

#pragma mark - Timer

- (void)startPollingTimer {
    [self stopPollingTimer];
    __weak typeof(self) weakSelf = self;
    self.pollingTimer = [NSTimer scheduledTimerWithTimeInterval:3.0 repeats:YES block:^(NSTimer * _Nonnull timer) {
        [weakSelf refreshStatus];
    }];
}

- (void)stopPollingTimer {
    [self.pollingTimer invalidate];
    self.pollingTimer = nil;
}

#pragma mark - UI Setup

- (void)setupScrollView {
    self.scrollView = [UIScrollView new];
    self.scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    self.scrollView.alwaysBounceVertical = YES;
    [self.view addSubview:self.scrollView];

    self.contentStackView = [UIStackView new];
    self.contentStackView.translatesAutoresizingMaskIntoConstraints = NO;
    self.contentStackView.axis = UILayoutConstraintAxisVertical;
    self.contentStackView.spacing = 16;
    [self.scrollView addSubview:self.contentStackView];

    [NSLayoutConstraint activateConstraints:@[
        [self.scrollView.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [self.scrollView.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [self.scrollView.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [self.scrollView.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],

        [self.contentStackView.topAnchor constraintEqualToAnchor:self.scrollView.contentLayoutGuide.topAnchor constant:12],
        [self.contentStackView.leadingAnchor constraintEqualToAnchor:self.scrollView.contentLayoutGuide.leadingAnchor constant:16],
        [self.contentStackView.trailingAnchor constraintEqualToAnchor:self.scrollView.contentLayoutGuide.trailingAnchor constant:-16],
        [self.contentStackView.bottomAnchor constraintEqualToAnchor:self.scrollView.contentLayoutGuide.bottomAnchor constant:-32],
        [self.contentStackView.widthAnchor constraintEqualToAnchor:self.scrollView.frameLayoutGuide.widthAnchor constant:-32]
    ]];

    self.deadBanner = [UIButton buttonWithType:UIButtonTypeSystem];
    self.deadBanner.translatesAutoresizingMaskIntoConstraints = NO;
    self.deadBanner.backgroundColor = UIColor.systemRedColor;
    self.deadBanner.layer.cornerRadius = 12;
    self.deadBanner.layer.cornerCurve = kCACornerCurveContinuous;
    [self.deadBanner setTitle:@"守护进程未运行，点击启动" forState:UIControlStateNormal];
    [self.deadBanner setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    self.deadBanner.titleLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
    [self.deadBanner.heightAnchor constraintEqualToConstant:44].active = YES;
    [self.deadBanner addTarget:self action:@selector(startDaemon) forControlEvents:UIControlEventTouchUpInside];
    self.deadBanner.hidden = YES;
    [self.contentStackView addArrangedSubview:self.deadBanner];

    self.versionWarningBanner = [UIButton buttonWithType:UIButtonTypeSystem];
    self.versionWarningBanner.translatesAutoresizingMaskIntoConstraints = NO;
    self.versionWarningBanner.backgroundColor = UIColor.systemRedColor;
    self.versionWarningBanner.layer.cornerRadius = 12;
    self.versionWarningBanner.layer.cornerCurve = kCACornerCurveContinuous;
    [self.versionWarningBanner setTitle:@"守护进程不是当前版本，请重启。点击重启守护进程。" forState:UIControlStateNormal];
    [self.versionWarningBanner setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    self.versionWarningBanner.titleLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightMedium];
    self.versionWarningBanner.titleLabel.numberOfLines = 0;
    self.versionWarningBanner.titleLabel.textAlignment = NSTextAlignmentCenter;
    self.versionWarningBanner.contentEdgeInsets = UIEdgeInsetsMake(10, 14, 10, 14);
    [self.versionWarningBanner addTarget:self action:@selector(restartDaemon) forControlEvents:UIControlEventTouchUpInside];
    self.versionWarningBanner.hidden = YES;
    [self.contentStackView addArrangedSubview:self.versionWarningBanner];

    // Pulse Corrupt Banner (v0.7.8 C2-5 / Q5)
    self.pulseCorruptBanner = [UIView new];
    self.pulseCorruptBanner.translatesAutoresizingMaskIntoConstraints = NO;
    self.pulseCorruptBanner.backgroundColor = UIColor.systemOrangeColor;
    self.pulseCorruptBanner.layer.cornerRadius = 12;
    self.pulseCorruptBanner.layer.cornerCurve = kCACornerCurveContinuous;
    self.pulseCorruptBanner.clipsToBounds = YES;

    self.pulseCorruptLabel = [UILabel new];
    self.pulseCorruptLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.pulseCorruptLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightMedium];
    self.pulseCorruptLabel.textColor = UIColor.whiteColor;
    self.pulseCorruptLabel.textAlignment = NSTextAlignmentCenter;
    self.pulseCorruptLabel.numberOfLines = 0;
    [self.pulseCorruptBanner addSubview:self.pulseCorruptLabel];

    [NSLayoutConstraint activateConstraints:@[
        [self.pulseCorruptLabel.topAnchor constraintEqualToAnchor:self.pulseCorruptBanner.topAnchor constant:10],
        [self.pulseCorruptLabel.bottomAnchor constraintEqualToAnchor:self.pulseCorruptBanner.bottomAnchor constant:-10],
        [self.pulseCorruptLabel.leadingAnchor constraintEqualToAnchor:self.pulseCorruptBanner.leadingAnchor constant:14],
        [self.pulseCorruptLabel.trailingAnchor constraintEqualToAnchor:self.pulseCorruptBanner.trailingAnchor constant:-14],
    ]];
    self.pulseCorruptBanner.hidden = YES;
    [self.contentStackView addArrangedSubview:self.pulseCorruptBanner];

    // Status Card
    UIView *statusCard = [self createStatusCard];
    statusCard.userInteractionEnabled = YES;
    UILongPressGestureRecognizer *restartStatusLongPress = [[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(handleStatusCardLongPress:)];
    restartStatusLongPress.minimumPressDuration = 0.8;
    [statusCard addGestureRecognizer:restartStatusLongPress];
    [self.contentStackView addArrangedSubview:statusCard];

    // Fixed Note
    UILabel *note = [UILabel new];
    note.translatesAutoresizingMaskIntoConstraints = NO;
    note.font = [UIFont systemFontOfSize:13 weight:UIFontWeightRegular];
    note.textColor = UIColor.secondaryLabelColor;
    note.numberOfLines = 0;
    note.text = @"当应用在后台驻留超时且内存占用超过阈值时，守护进程将自动终止该应用并按设定拉起。未达到后台判定时长或在前台使用时绝不触发。";
    [self.contentStackView addArrangedSubview:note];

    // Rules Section
    self.rulesSectionHeaderLabel = [self createSectionHeader:@"超内存重启规则"];
    self.rulesStackView = [UIStackView new];
    self.rulesStackView.translatesAutoresizingMaskIntoConstraints = NO;
    self.rulesStackView.axis = UILayoutConstraintAxisVertical;
    self.rulesStackView.spacing = 12;

    [self.contentStackView addArrangedSubview:self.rulesSectionHeaderLabel];
    [self.contentStackView addArrangedSubview:self.rulesStackView];

    // Add Rule Card
    self.addRuleCard = [self createAddRuleCard];
    [self.contentStackView addArrangedSubview:self.addRuleCard];

    // Pulse Section (后台释放 - v0.7.8 C2-7 / Q14)
    self.pulseSectionHeaderLabel = [self createSectionHeader:@"后台释放"];
    [self.contentStackView addArrangedSubview:self.pulseSectionHeaderLabel];

    UILabel *pulseNote = [UILabel new];
    pulseNote.translatesAutoresizingMaskIntoConstraints = NO;
    pulseNote.font = [UIFont systemFontOfSize:13 weight:UIFontWeightRegular];
    pulseNote.textColor = UIColor.secondaryLabelColor;
    pulseNote.numberOfLines = 0;
    pulseNote.text = @"切到后台时提示 App 释放内存，不会结束 App。不支持应用扩展（PlugIns）、系统应用及 Safari 网页进程。";
    [self.contentStackView addArrangedSubview:pulseNote];

    self.pulseRulesStackView = [UIStackView new];
    self.pulseRulesStackView.translatesAutoresizingMaskIntoConstraints = NO;
    self.pulseRulesStackView.axis = UILayoutConstraintAxisVertical;
    self.pulseRulesStackView.spacing = 12;
    [self.contentStackView addArrangedSubview:self.pulseRulesStackView];

    self.addPulseRuleCard = [self createAddPulseRuleCard];
    [self.contentStackView addArrangedSubview:self.addPulseRuleCard];

    // Footer
    UILabel *footer = [UILabel new];
    footer.translatesAutoresizingMaskIntoConstraints = NO;
    footer.font = [UIFont systemFontOfSize:12 weight:UIFontWeightRegular];
    footer.textColor = UIColor.tertiaryLabelColor;
    footer.textAlignment = NSTextAlignmentCenter;
    footer.numberOfLines = 0;
    footer.text = @"改动实时自动保存 · 亮屏约 10 秒内，锁屏最长约 60 秒生效";
    [self.contentStackView addArrangedSubview:footer];
}

- (UILabel *)createSectionHeader:(NSString *)title {
    UILabel *lbl = [UILabel new];
    lbl.translatesAutoresizingMaskIntoConstraints = NO;
    lbl.font = [UIFont systemFontOfSize:13 weight:UIFontWeightMedium];
    lbl.textColor = UIColor.secondaryLabelColor;
    lbl.text = title;
    return lbl;
}

- (UIView *)createStatusCard {
    UIView *card = [UIView new];
    card.translatesAutoresizingMaskIntoConstraints = NO;
    card.backgroundColor = UIColor.secondarySystemGroupedBackgroundColor;
    card.layer.cornerRadius = 14;
    card.layer.cornerCurve = kCACornerCurveContinuous;
    card.clipsToBounds = YES;

    UIImageView *icon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"arrow.clockwise.circle.fill"]];
    icon.translatesAutoresizingMaskIntoConstraints = NO;
    icon.tintColor = UIColor.systemBlueColor;

    UILabel *title = [UILabel new];
    title.translatesAutoresizingMaskIntoConstraints = NO;
    title.font = [UIFont systemFontOfSize:16 weight:UIFontWeightSemibold];
    title.textColor = UIColor.labelColor;
    title.text = @"超内存重启";

    self.moduleStatusLabel = [UILabel new];
    self.moduleStatusLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.moduleStatusLabel.font = [UIFont systemFontOfSize:12 weight:UIFontWeightRegular];
    self.moduleStatusLabel.textColor = UIColor.secondaryLabelColor;
    self.moduleStatusLabel.text = @"读取模块状态中...";

    [card addSubview:icon];
    [card addSubview:title];
    [card addSubview:self.moduleStatusLabel];

    [NSLayoutConstraint activateConstraints:@[
        [icon.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:16],
        [icon.centerYAnchor constraintEqualToAnchor:card.centerYAnchor],
        [icon.widthAnchor constraintEqualToConstant:24],
        [icon.heightAnchor constraintEqualToConstant:24],

        [title.topAnchor constraintEqualToAnchor:card.topAnchor constant:12],
        [title.leadingAnchor constraintEqualToAnchor:icon.trailingAnchor constant:12],
        [title.trailingAnchor constraintLessThanOrEqualToAnchor:card.trailingAnchor constant:-16],

        [self.moduleStatusLabel.topAnchor constraintEqualToAnchor:title.bottomAnchor constant:3],
        [self.moduleStatusLabel.leadingAnchor constraintEqualToAnchor:title.leadingAnchor],
        [self.moduleStatusLabel.trailingAnchor constraintLessThanOrEqualToAnchor:card.trailingAnchor constant:-16],
        [self.moduleStatusLabel.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-12],
    ]];

    return card;
}

- (UIView *)createAddRuleCard {
    UIView *card = [UIView new];
    card.translatesAutoresizingMaskIntoConstraints = NO;
    card.backgroundColor = UIColor.secondarySystemGroupedBackgroundColor;
    card.layer.cornerRadius = 14;
    card.layer.cornerCurve = kCACornerCurveContinuous;
    card.clipsToBounds = YES;

    UIImageView *icon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"plus.circle.fill"]];
    icon.translatesAutoresizingMaskIntoConstraints = NO;
    icon.tintColor = UIColor.systemBlueColor;

    UILabel *title = [UILabel new];
    title.translatesAutoresizingMaskIntoConstraints = NO;
    title.font = [UIFont systemFontOfSize:16 weight:UIFontWeightSemibold];
    title.textColor = UIColor.systemBlueColor;
    title.text = @"添加规则";

    UIImageView *chev = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"chevron.right"]];
    chev.translatesAutoresizingMaskIntoConstraints = NO;
    chev.tintColor = UIColor.tertiaryLabelColor;

    [card addSubview:icon];
    [card addSubview:title];
    [card addSubview:chev];

    [NSLayoutConstraint activateConstraints:@[
        [icon.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:16],
        [icon.centerYAnchor constraintEqualToAnchor:card.centerYAnchor],
        [icon.widthAnchor constraintEqualToConstant:22],
        [icon.heightAnchor constraintEqualToConstant:22],

        [title.leadingAnchor constraintEqualToAnchor:icon.trailingAnchor constant:10],
        [title.centerYAnchor constraintEqualToAnchor:card.centerYAnchor],

        [chev.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-16],
        [chev.centerYAnchor constraintEqualToAnchor:card.centerYAnchor],
        [chev.widthAnchor constraintEqualToConstant:12],
        [chev.heightAnchor constraintEqualToConstant:14],

        [card.heightAnchor constraintEqualToConstant:52]
    ]];

    UITapGestureRecognizer *tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(showAddRuleSheet)];
    [card addGestureRecognizer:tap];
    card.userInteractionEnabled = YES;
    return card;
}

- (void)setupSaveNoticeToast {
    self.saveNoticeToast = [UIView new];
    self.saveNoticeToast.translatesAutoresizingMaskIntoConstraints = NO;
    self.saveNoticeToast.backgroundColor = [UIColor colorWithDynamicProvider:^UIColor * _Nonnull(UITraitCollection * _Nonnull tc) {
        if (tc.userInterfaceStyle == UIUserInterfaceStyleDark) {
            return [UIColor colorWithWhite:0.25 alpha:0.96];
        } else {
            return [UIColor colorWithWhite:0.95 alpha:0.96];
        }
    }];
    self.saveNoticeToast.layer.cornerRadius = 18;
    self.saveNoticeToast.layer.cornerCurve = kCACornerCurveContinuous;
    self.saveNoticeToast.layer.shadowColor = UIColor.blackColor.CGColor;
    self.saveNoticeToast.layer.shadowOpacity = 0.12;
    self.saveNoticeToast.layer.shadowOffset = CGSizeMake(0, 3);
    self.saveNoticeToast.layer.shadowRadius = 8;
    self.saveNoticeToast.userInteractionEnabled = NO;
    self.saveNoticeToast.alpha = 0.0;

    UIImageView *icon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"checkmark.circle.fill"]];
    icon.translatesAutoresizingMaskIntoConstraints = NO;
    icon.tintColor = UIColor.systemGreenColor;

    UILabel *lbl = [UILabel new];
    lbl.translatesAutoresizingMaskIntoConstraints = NO;
    lbl.font = [UIFont systemFontOfSize:13 weight:UIFontWeightMedium];
    lbl.textColor = UIColor.labelColor;
    lbl.text = @"改动已保存 · 亮屏约 10 秒内，锁屏最长约 60 秒生效";

    [self.saveNoticeToast addSubview:icon];
    [self.saveNoticeToast addSubview:lbl];

    [NSLayoutConstraint activateConstraints:@[
        [icon.leadingAnchor constraintEqualToAnchor:self.saveNoticeToast.leadingAnchor constant:14],
        [icon.centerYAnchor constraintEqualToAnchor:self.saveNoticeToast.centerYAnchor],
        [icon.widthAnchor constraintEqualToConstant:18],
        [icon.heightAnchor constraintEqualToConstant:18],

        [lbl.leadingAnchor constraintEqualToAnchor:icon.trailingAnchor constant:8],
        [lbl.trailingAnchor constraintEqualToAnchor:self.saveNoticeToast.trailingAnchor constant:-14],
        [lbl.centerYAnchor constraintEqualToAnchor:self.saveNoticeToast.centerYAnchor],

        [self.saveNoticeToast.heightAnchor constraintEqualToConstant:36]
    ]];

    [self.view addSubview:self.saveNoticeToast];
    [NSLayoutConstraint activateConstraints:@[
        [self.saveNoticeToast.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [self.saveNoticeToast.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor constant:8]
    ]];
}

- (void)showSaveNotice {
    [self.saveNoticeTimer invalidate];
    [self.view bringSubviewToFront:self.saveNoticeToast];
    [UIView animateWithDuration:0.2 animations:^{
        self.saveNoticeToast.alpha = 1.0;
        self.saveNoticeToast.transform = CGAffineTransformIdentity;
    }];

    __weak typeof(self) weakSelf = self;
    self.saveNoticeTimer = [NSTimer scheduledTimerWithTimeInterval:2.5 repeats:NO block:^(NSTimer * _Nonnull timer) {
        __strong typeof(weakSelf) strongSelf = weakSelf;
        if (!strongSelf) return;
        [UIView animateWithDuration:0.25 animations:^{
            strongSelf.saveNoticeToast.alpha = 0.0;
            strongSelf.saveNoticeToast.transform = CGAffineTransformMakeTranslation(0, -8);
        }];
    }];
}

#pragma mark - Config Management

- (void)loadConfig {
    self.config = [JPRestartConfig loadFromPath:kRestartConfig];
    self.pulseConfig = [JPPulseConfig loadFromPath:kPulseConfig];
    self.installedApps = [JPInstalledApp scanInstalledApps];
}

- (BOOL)saveConfig {
    NSError *err = nil;
    BOOL ok = [self saveConfigWithError:&err];
    if (!ok) {
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"保存配置失败"
                                                                       message:[NSString stringWithFormat:@"写入 restart.txt 失败：%@", err ? err.localizedDescription : @"未知错误"]
                                                                preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"确定" style:UIAlertActionStyleDefault handler:nil]];
        __weak typeof(self) weakSelf = self;
        dispatch_async(dispatch_get_main_queue(), ^{
            [weakSelf presentViewController:alert animated:YES completion:nil];
        });
    }
    return ok;
}

- (BOOL)saveConfigWithError:(NSError **)error {
    NSError *err = nil;
    BOOL ok = [self.config saveToPath:kRestartConfig error:&err];
    if (error && err) {
        *error = err;
    }
    if (!ok) {
        [self loadConfig];
        [self rebuildRulesCards];
        return NO;
    }
    [self loadConfig];
    [self rebuildRulesCards];
    [self showSaveNotice];
    return YES;
}

- (void)rebuildRulesCards {
    [self.config updateDuplicateRules];

    for (UIView *v in self.rulesStackView.arrangedSubviews) {
        [self.rulesStackView removeArrangedSubview:v];
        [v removeFromSuperview];
    }

    NSUInteger visibleCardsCount = 0;
    for (JPRestartLine *line in self.config.lines) {
        if (!line.isUnknownOrComment && line.rule) {
            visibleCardsCount++;
        } else if (line.isUnknownOrComment && ![line isCommentOrEmpty]) {
            visibleCardsCount++;
        }
    }

    if (visibleCardsCount == 0) {
        UIView *emptyCard = [UIView new];
        emptyCard.translatesAutoresizingMaskIntoConstraints = NO;
        emptyCard.backgroundColor = UIColor.secondarySystemGroupedBackgroundColor;
        emptyCard.layer.cornerRadius = 14;
        emptyCard.layer.cornerCurve = kCACornerCurveContinuous;

        UILabel *emptyLabel = [UILabel new];
        emptyLabel.translatesAutoresizingMaskIntoConstraints = NO;
        emptyLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightRegular];
        emptyLabel.textColor = UIColor.secondaryLabelColor;
        emptyLabel.text = @"暂无超内存重启规则，点击下方「添加规则」进行配置";
        emptyLabel.textAlignment = NSTextAlignmentCenter;
        [emptyCard addSubview:emptyLabel];

        [NSLayoutConstraint activateConstraints:@[
            [emptyLabel.centerXAnchor constraintEqualToAnchor:emptyCard.centerXAnchor],
            [emptyLabel.centerYAnchor constraintEqualToAnchor:emptyCard.centerYAnchor],
            [emptyCard.heightAnchor constraintEqualToConstant:64]
        ]];
        [self.rulesStackView addArrangedSubview:emptyCard];
    } else {
        NSDictionary<NSString *, NSNumber *> *counts = [JPRestartConfig loadAll7DayRestartCounts];
        for (JPRestartLine *line in self.config.lines) {
            if (!line.isUnknownOrComment && line.rule) {
                NSInteger count7 = [counts[line.rule.bid] integerValue];
                JPRestartRuleCardView *card = [[JPRestartRuleCardView alloc] initWithRule:line.rule
                                                                                 delegate:self
                                                                            installedApps:self.installedApps
                                                                         restart7DayCount:count7];
                [self.rulesStackView addArrangedSubview:card];
            } else if (line.isUnknownOrComment && ![line isCommentOrEmpty]) {
                JPRestartUnknownLineCardView *card = [[JPRestartUnknownLineCardView alloc] initWithLine:line delegate:self];
                [self.rulesStackView addArrangedSubview:card];
            }
        }
    }

    self.rulesSectionHeaderLabel.text = [NSString stringWithFormat:@"超内存重启 (%lu)", (unsigned long)self.config.rules.count];
}

- (void)rebuildPulseCards {
    [self.pulseConfig updateDuplicateRules];

    for (UIView *v in self.pulseRulesStackView.arrangedSubviews) {
        [self.pulseRulesStackView removeArrangedSubview:v];
        [v removeFromSuperview];
    }

    NSUInteger visibleCardsCount = 0;
    for (JPPulseLine *line in self.pulseConfig.lines) {
        if (!line.isUnknownOrComment && line.rule) {
            visibleCardsCount++;
        } else if (line.isUnknownOrComment && ![line isCommentOrEmpty]) {
            visibleCardsCount++;
        }
    }

    if (visibleCardsCount == 0) {
        UIView *emptyCard = [UIView new];
        emptyCard.translatesAutoresizingMaskIntoConstraints = NO;
        emptyCard.backgroundColor = UIColor.secondarySystemGroupedBackgroundColor;
        emptyCard.layer.cornerRadius = 14;
        emptyCard.layer.cornerCurve = kCACornerCurveContinuous;

        UILabel *emptyLabel = [UILabel new];
        emptyLabel.translatesAutoresizingMaskIntoConstraints = NO;
        emptyLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightRegular];
        emptyLabel.textColor = UIColor.secondaryLabelColor;
        emptyLabel.text = @"暂无后台释放规则，点击下方「添加释放规则」进行配置";
        emptyLabel.textAlignment = NSTextAlignmentCenter;
        [emptyCard addSubview:emptyLabel];

        [NSLayoutConstraint activateConstraints:@[
            [emptyLabel.centerXAnchor constraintEqualToAnchor:emptyCard.centerXAnchor],
            [emptyLabel.centerYAnchor constraintEqualToAnchor:emptyCard.centerYAnchor],
            [emptyCard.heightAnchor constraintEqualToConstant:64]
        ]];
        [self.pulseRulesStackView addArrangedSubview:emptyCard];
    } else {
        JPStatusInfo *st = [JPStatusInfo loadCurrentStatus];
        for (JPPulseLine *line in self.pulseConfig.lines) {
            if (!line.isUnknownOrComment && line.rule) {
                JPPulseAppState *ast = st.pulseAppStates[line.rule.bid];
                JPPulseRuleCardView *card = [[JPPulseRuleCardView alloc] initWithRule:line.rule
                                                                             delegate:self
                                                                        installedApps:self.installedApps
                                                                             appState:ast];
                [self.pulseRulesStackView addArrangedSubview:card];
            } else if (line.isUnknownOrComment && ![line isCommentOrEmpty]) {
                JPPulseUnknownLineCardView *card = [[JPPulseUnknownLineCardView alloc] initWithLine:line delegate:self];
                [self.pulseRulesStackView addArrangedSubview:card];
            }
        }
    }

    self.pulseSectionHeaderLabel.text = [NSString stringWithFormat:@"后台释放 (%lu)", (unsigned long)self.pulseConfig.rules.count];
}

- (void)refreshStatus {
    JPDaemonState dState = [JPDaemonManager daemonStateWithLastSpawnTime:self.lastSpawnTime];
    BOOL running = (dState == JPDaemonStateRunning);
    self.deadBanner.hidden = (dState != JPDaemonStateDead);

    JPStatusInfo *st = [JPStatusInfo loadCurrentStatus];
    self.restartSupport = running && st.restartSupport;
    self.pulseSupport = running && st.pulseSupport;

    NSString *appVersion = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"0.7.8";
    NSString *appCommit = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"JPGitCommit"] ?: @"";
    BOOL isOutdated = running && (!st.restartSupport || !st.pulseSupport || JPCheckVersionOrCommitMismatch(st, appVersion, appCommit));
    self.versionWarningBanner.hidden = !isOutdated;

    if (running && st.pulseCorrupt) {
        if (st.pulseCorruptBid.length > 0) {
            NSString *appName = ResolveDisplayNameHelper([NSString stringWithFormat:@"bid:%@", st.pulseCorruptBid], self.installedApps);
            if (appName.length == 0) appName = st.pulseCorruptBid;
            self.pulseCorruptLabel.text = [NSString stringWithFormat:@"建议强制关闭「%@」一次（后台释放异常恢复）", appName];
        } else {
            self.pulseCorruptLabel.text = @"请强制关闭最近被后台释放的 App（后台释放异常恢复）";
        }
        self.pulseCorruptBanner.hidden = NO;
    } else {
        self.pulseCorruptBanner.hidden = YES;
    }

    if (!running) {
        self.moduleStatusLabel.text = @"守护进程未运行";
        self.moduleStatusLabel.textColor = UIColor.systemRedColor;
        self.addRuleCard.alpha = 0.5;
        self.addRuleCard.userInteractionEnabled = NO;
        self.addPulseRuleCard.alpha = 0.5;
        self.addPulseRuleCard.userInteractionEnabled = NO;
        return;
    }

    if (isOutdated) {
        self.moduleStatusLabel.text = @"守护进程不是当前版本，请重启";
        self.moduleStatusLabel.textColor = UIColor.systemOrangeColor;
        self.addRuleCard.alpha = 0.5;
        self.addRuleCard.userInteractionEnabled = NO;
        self.addPulseRuleCard.alpha = 0.5;
        self.addPulseRuleCard.userInteractionEnabled = NO;
    } else if (st.cfgBad > 0 || st.pulseCfgBad > 0) {
        NSMutableArray<NSString *> *badParts = [NSMutableArray array];
        if (st.cfgBad > 0) [badParts addObject:[NSString stringWithFormat:@"重启配置 %ld 行无效", (long)st.cfgBad]];
        if (st.pulseCfgBad > 0) [badParts addObject:[NSString stringWithFormat:@"释放配置 %ld 行无效", (long)st.pulseCfgBad]];
        self.moduleStatusLabel.text = [badParts componentsJoinedByString:@" · "];
        self.moduleStatusLabel.textColor = UIColor.systemOrangeColor;
        self.addRuleCard.alpha = 1.0;
        self.addRuleCard.userInteractionEnabled = YES;
        self.addPulseRuleCard.alpha = 1.0;
        self.addPulseRuleCard.userInteractionEnabled = YES;
    } else {
        if (st.pulseToday > 0) {
            self.moduleStatusLabel.text = [NSString stringWithFormat:@"模块状态：正常运行中 · 今日释放 %ld 次", (long)st.pulseToday];
        } else {
            self.moduleStatusLabel.text = @"模块状态：正常运行中";
        }
        self.moduleStatusLabel.textColor = UIColor.systemGreenColor;
        self.addRuleCard.alpha = 1.0;
        self.addRuleCard.userInteractionEnabled = YES;
        self.addPulseRuleCard.alpha = 1.0;
        self.addPulseRuleCard.userInteractionEnabled = YES;
    }
}

- (void)handleStatusCardLongPress:(UILongPressGestureRecognizer *)gesture {
    if (gesture.state != UIGestureRecognizerStateBegan) return;
    [JPDaemonManager runProbeDiagnosticsFromViewController:self];
}

- (void)startDaemon {
    if ([JPDaemonManager daemonStateWithLastSpawnTime:self.lastSpawnTime] == JPDaemonStateRunning) {
        [self restartDaemon];
        return;
    }
    if ([JPDaemonManager daemonStateWithLastSpawnTime:self.lastSpawnTime] == JPDaemonStateDead) {
        [NSFileManager.defaultManager removeItemAtPath:kPidFile error:nil];
    }
    CFAbsoluteTime t = 0;
    int rc = [JPDaemonManager startDaemonWithLastSpawnTime:&t];
    if (rc == 0) self.lastSpawnTime = t;

    if (rc != 0) {
        [JPDaemonManager handleSpawnFailure:rc fromViewController:self];
    }
    [self refreshStatus];
}

- (void)restartDaemon {
    if (self.restartInProgress || s_restartInProgress) return;
    self.restartInProgress = YES;
    s_restartInProgress = YES;
    self.versionWarningBanner.enabled = NO;

    int pre_rc = 0;
    int pre_exit = 0;
    int pre_errno = 0;
    NSString *pre_reason = nil;
    if (![JPDaemonManager precheckDaemonWithRc:&pre_rc exitOut:&pre_exit errOut:&pre_errno reason:&pre_reason]) {
        NSLog(@"SPAWN_PRECHECK_FAIL rc=%d exit=%d errno=%d", pre_rc, pre_exit, pre_errno);
        self.restartInProgress = NO;
        s_restartInProgress = NO;
        self.versionWarningBanner.enabled = YES;
        NSString *errDetail = (pre_errno != 0)
            ? [NSString stringWithFormat:@"(rc=%d, exit=%d, errno=%d: %s)", pre_rc, pre_exit, pre_errno, strerror(pre_errno)]
            : [NSString stringWithFormat:@"(rc=%d, exit=%d)", pre_rc, pre_exit];
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"预检失败，已保持旧守护进程运行"
                                                                       message:[NSString stringWithFormat:@"重启预检失败: %@\n%@\n旧守护进程未停止，继续保持运行。",
                                                                                pre_reason ?: @"未知错误", errDetail]
                                                                preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:alert animated:YES completion:nil];
        return;
    }

    [JPDaemonManager stopDaemon];
    NSString *pidStr = [NSString stringWithContentsOfFile:kPidFile encoding:NSUTF8StringEncoding error:nil];
    pid_t pid = (pid_t)[pidStr intValue];
    if (pid > 0 && JPIsPidJetpilotd(pid)) {
        kill(pid, SIGTERM);
    } else {
        [NSFileManager.defaultManager removeItemAtPath:kPidFile error:nil];
        pid = 0;
    }
    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        if (!JPWaitForDaemonExit(pid)) {
            [@"run" writeToFile:kFlag atomically:YES encoding:NSUTF8StringEncoding error:nil];
            dispatch_async(dispatch_get_main_queue(), ^{
                weakSelf.restartInProgress = NO;
                s_restartInProgress = NO;
                weakSelf.versionWarningBanner.enabled = YES;
                [weakSelf refreshStatus];
                UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"重启失败"
                                                                               message:@"旧守护进程未退出，请稍后再试"
                                                                        preferredStyle:UIAlertControllerStyleAlert];
                [alert addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
                [weakSelf presentViewController:alert animated:YES completion:nil];
            });
            return;
        }
        [NSFileManager.defaultManager removeItemAtPath:kPidFile error:nil];
        dispatch_async(dispatch_get_main_queue(), ^{
            weakSelf.restartInProgress = NO;
            s_restartInProgress = NO;
            weakSelf.versionWarningBanner.enabled = YES;
            [weakSelf startDaemon];
        });
    });
}

#pragma mark - Actions

- (void)showAddRuleSheet {
    if (!self.restartSupport) {
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"守护进程版本过旧"
                                                                       message:@"当前守护进程不支持超内存重启，请先重启守护进程"
                                                                preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:alert animated:YES completion:nil];
        return;
    }

    if (self.config.rules.count >= 32) {
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"规则已达上限"
                                                                       message:@"最多支持添加 32 条超内存重启规则"
                                                                preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:alert animated:YES completion:nil];
        return;
    }

    JPAppPickerViewController *picker = [JPAppPickerViewController new];
    picker.userAppsOnly = YES;
    picker.isRestartPicker = YES;
    picker.allApps = self.installedApps;
    __weak typeof(self) weakSelf = self;
    picker.isAppExistingBlock = ^BOOL(JPInstalledApp *app) {
        for (JPRestartRule *r in weakSelf.config.rules) {
            if ([r.bid isEqualToString:app.bundleId]) return YES;
        }
        return NO;
    };

    picker.onSelectRestartBid = ^(NSString *bid, NSString *displayName) {
        JPRestartRule *existing = [weakSelf.config ruleForBundleId:bid];
        if (existing) {
            [weakSelf openEditRuleController:existing isNew:NO];
            return;
        }

        JPRestartRule *newRule = [JPRestartRule new];
        newRule.bid = bid;
        newRule.displayName = displayName;
        newRule.thr = 800;
        newRule.bg = 600;
        newRule.max = 3;
        newRule.relaunch = 1;
        newRule.isEnabled = YES;

        [weakSelf openEditRuleController:newRule isNew:YES];
    };

    UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:picker];
    if (@available(iOS 15.0, *)) {
        UISheetPresentationController *sheet = nav.sheetPresentationController;
        sheet.detents = @[UISheetPresentationControllerDetent.mediumDetent, UISheetPresentationControllerDetent.largeDetent];
        sheet.prefersGrabberVisible = YES;
    }
    [self presentViewController:nav animated:YES completion:nil];
}

- (void)openEditRuleController:(JPRestartRule *)rule isNew:(BOOL)isNew {
    if (!self.restartSupport) {
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"守护进程版本过旧"
                                                                       message:@"当前守护进程不支持超内存重启，编辑项不可用，请先重启守护进程"
                                                                preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:alert animated:YES completion:nil];
        return;
    }

    JPRestartRuleEditViewController *vc = [JPRestartRuleEditViewController new];
    vc.rule = rule;
    vc.installedApps = self.installedApps;
    vc.isNewRule = isNew;
    vc.delegate = self;

    UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:vc];
    [self presentViewController:nav animated:YES completion:nil];
}

- (void)deleteRule:(JPRestartRule *)rule {
    JPRestartLine *line = [self.config lineForRule:rule];
    if (!line) {
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"提示"
                                                                       message:@"规则已变化，请刷新后重试"
                                                                preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"确定" style:UIAlertActionStyleDefault handler:nil]];
        dispatch_async(dispatch_get_main_queue(), ^{
            [self presentViewController:alert animated:YES completion:nil];
        });
        return;
    }
    [self.config removeLine:line];
    [self saveConfig];
    [self rebuildRulesCards];
}

#pragma mark - JPRestartRuleCardViewDelegate

- (void)restartRuleCardViewDidTap:(JPRestartRuleCardView *)card {
    [self openEditRuleController:card.rule isNew:NO];
}

- (void)restartRuleCardViewDidRequestDelete:(JPRestartRuleCardView *)card {
    JPRestartRule *rule = card.rule;
    NSString *name = ResolveDisplayNameHelper([NSString stringWithFormat:@"bid:%@", rule.bid], self.installedApps);
    if (name.length == 0) name = rule.displayName.length > 0 ? rule.displayName : rule.bid;

    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"移除规则"
                                                                   message:[NSString stringWithFormat:@"确定要移除对「%@」的超内存重启规则吗？", name]
                                                            preferredStyle:UIAlertControllerStyleActionSheet];

    __weak typeof(self) weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"移除" style:UIAlertActionStyleDestructive handler:^(UIAlertAction * _Nonnull action) {
        [weakSelf deleteRule:rule];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];

    if (alert.popoverPresentationController) {
        alert.popoverPresentationController.sourceView = card;
        alert.popoverPresentationController.sourceRect = card.bounds;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        [self presentViewController:alert animated:YES completion:nil];
    });
}

#pragma mark - JPRestartUnknownLineCardViewDelegate

- (void)restartUnknownLineCardViewDidRequestDelete:(JPRestartLine *)line {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"移除无效配置行"
                                                                   message:[NSString stringWithFormat:@"确定要移除此无效配置行吗？\n\n%@", line.rawText ?: @""]
                                                            preferredStyle:UIAlertControllerStyleActionSheet];
    __weak typeof(self) weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"移除" style:UIAlertActionStyleDestructive handler:^(UIAlertAction * _Nonnull action) {
        [weakSelf.config removeLine:line];
        [weakSelf saveConfig];
        [weakSelf rebuildRulesCards];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
    dispatch_async(dispatch_get_main_queue(), ^{
        [self presentViewController:alert animated:YES completion:nil];
    });
}

#pragma mark - JPRestartRuleEditViewControllerDelegate

- (BOOL)restartRuleEditViewControllerDidSave:(JPRestartRule *)rule error:(NSError **)error {
    [self.config setRule:rule forBundleId:rule.bid];
    return [self saveConfigWithError:error];
}

- (BOOL)restartRuleEditViewControllerDidDelete:(JPRestartRule *)rule error:(NSError **)error {
    JPRestartLine *line = [self.config lineForRule:rule];
    if (!line) {
        if (error) {
            *error = [NSError errorWithDomain:@"JetPilot"
                                         code:1001
                                     userInfo:@{NSLocalizedDescriptionKey: @"规则已变化，请刷新后重试"}];
        }
        return NO;
    }
    [self.config removeLine:line];
    return [self saveConfigWithError:error];
}

- (UIView *)createAddPulseRuleCard {
    UIView *card = [UIView new];
    card.translatesAutoresizingMaskIntoConstraints = NO;
    card.backgroundColor = UIColor.secondarySystemGroupedBackgroundColor;
    card.layer.cornerRadius = 14;
    card.layer.cornerCurve = kCACornerCurveContinuous;
    card.clipsToBounds = YES;

    UIImageView *icon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"plus.circle.fill"]];
    icon.translatesAutoresizingMaskIntoConstraints = NO;
    icon.tintColor = UIColor.systemBlueColor;

    UILabel *title = [UILabel new];
    title.translatesAutoresizingMaskIntoConstraints = NO;
    title.font = [UIFont systemFontOfSize:16 weight:UIFontWeightSemibold];
    title.textColor = UIColor.systemBlueColor;
    title.text = @"添加释放规则";

    UIImageView *chev = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"chevron.right"]];
    chev.translatesAutoresizingMaskIntoConstraints = NO;
    chev.tintColor = UIColor.tertiaryLabelColor;

    [card addSubview:icon];
    [card addSubview:title];
    [card addSubview:chev];

    [NSLayoutConstraint activateConstraints:@[
        [icon.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:16],
        [icon.centerYAnchor constraintEqualToAnchor:card.centerYAnchor],
        [icon.widthAnchor constraintEqualToConstant:22],
        [icon.heightAnchor constraintEqualToConstant:22],

        [title.leadingAnchor constraintEqualToAnchor:icon.trailingAnchor constant:10],
        [title.centerYAnchor constraintEqualToAnchor:card.centerYAnchor],

        [chev.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-16],
        [chev.centerYAnchor constraintEqualToAnchor:card.centerYAnchor],
        [chev.widthAnchor constraintEqualToConstant:12],
        [chev.heightAnchor constraintEqualToConstant:14],

        [card.heightAnchor constraintEqualToConstant:52]
    ]];

    UITapGestureRecognizer *tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(showAddPulseRuleSheet)];
    [card addGestureRecognizer:tap];
    card.userInteractionEnabled = YES;
    return card;
}

- (void)showAddPulseRuleSheet {
    if (!self.pulseSupport) {
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"守护进程版本过旧"
                                                                       message:@"当前守护进程不支持后台释放，请先重启守护进程"
                                                                preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:alert animated:YES completion:nil];
        return;
    }

    if (self.pulseConfig.rules.count >= 32) {
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"规则已达上限"
                                                                       message:@"最多支持添加 32 条后台释放规则"
                                                                preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:alert animated:YES completion:nil];
        return;
    }

    JPAppPickerViewController *picker = [JPAppPickerViewController new];
    picker.userAppsOnly = YES;
    picker.isPulsePicker = YES;
    picker.allApps = self.installedApps;
    __weak typeof(self) weakSelf = self;
    picker.isAppExistingBlock = ^BOOL(JPInstalledApp *app) {
        for (JPPulseRule *r in weakSelf.pulseConfig.rules) {
            if ([r.bid isEqualToString:app.bundleId]) return YES;
        }
        return NO;
    };

    picker.onSelectPulseBid = ^(NSString *bid, NSString *displayName) {
        JPPulseRule *existing = [weakSelf.pulseConfig ruleForBundleId:bid];
        if (existing) {
            [weakSelf openEditPulseRuleController:existing isNew:NO];
            return;
        }

        NSInteger defTarget = 400;
        if ([bid isEqualToString:@"com.tencent.xin"]) {
            defTarget = 550;
        } else {
            NSString *bContent = [NSString stringWithContentsOfFile:kBandsPath encoding:NSUTF8StringEncoding error:nil];
            if (bContent.length > 0) {
                JPBandsSnapshot *snap = [JPBandsSnapshot parseSnapshotContent:bContent expectedToken:nil];
                JPBandsProcess *p = [snap bestProcessForBundleId:bid];
                if (p && p.hasFp && p.fpMb > 0) {
                    defTarget = MAX(300, p.fpMb + 100);
                }
            }
        }
        if (defTarget > 1500) defTarget = 1500;
        if (defTarget < 300) defTarget = 300;

        JPRestartRule *rst = [weakSelf.config ruleForBundleId:bid];
        if (rst && rst.thr > 0 && defTarget > rst.thr - 200) {
            defTarget = MAX(300, rst.thr - 200);
        }

        JPPulseRule *newRule = [JPPulseRule new];
        newRule.bid = bid;
        newRule.displayName = displayName;
        newRule.target = defTarget;
        newRule.delay = 10;
        newRule.n = 10;
        newRule.daymax = 30;
        newRule.enabled = NO; // 手动新加规则默认关闭 (Q6 / C2-7)

        [weakSelf openEditPulseRuleController:newRule isNew:YES];
    };

    UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:picker];
    if (@available(iOS 15.0, *)) {
        UISheetPresentationController *sheet = nav.sheetPresentationController;
        sheet.detents = @[UISheetPresentationControllerDetent.mediumDetent, UISheetPresentationControllerDetent.largeDetent];
        sheet.prefersGrabberVisible = YES;
    }
    [self presentViewController:nav animated:YES completion:nil];
}

- (void)openEditPulseRuleController:(JPPulseRule *)rule isNew:(BOOL)isNew {
    if (!self.pulseSupport) {
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"守护进程版本过旧"
                                                                       message:@"当前守护进程不支持后台释放，编辑项不可用，请先重启守护进程"
                                                                preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:alert animated:YES completion:nil];
        return;
    }

    JPPulseRuleEditViewController *vc = [JPPulseRuleEditViewController new];
    vc.rule = rule;
    vc.installedApps = self.installedApps;
    vc.isNewRule = isNew;
    vc.delegate = self;

    UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:vc];
    [self presentViewController:nav animated:YES completion:nil];
}

- (void)deletePulseRule:(JPPulseRule *)rule {
    JPPulseLine *line = [self.pulseConfig lineForRule:rule];
    if (!line) {
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"提示"
                                                                       message:@"规则已变化，请刷新后重试"
                                                                preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"确定" style:UIAlertActionStyleDefault handler:nil]];
        dispatch_async(dispatch_get_main_queue(), ^{
            [self presentViewController:alert animated:YES completion:nil];
        });
        return;
    }
    [self.pulseConfig removeLine:line];
    [self savePulseConfig];
    [self rebuildPulseCards];
}

- (BOOL)savePulseConfig {
    NSError *err = nil;
    BOOL ok = [self savePulseConfigWithError:&err];
    if (!ok) {
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"保存配置失败"
                                                                       message:[NSString stringWithFormat:@"写入 pulse.txt 失败：%@", err ? err.localizedDescription : @"未知错误"]
                                                                preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"确定" style:UIAlertActionStyleDefault handler:nil]];
        __weak typeof(self) weakSelf = self;
        dispatch_async(dispatch_get_main_queue(), ^{
            [weakSelf presentViewController:alert animated:YES completion:nil];
        });
    }
    return ok;
}

- (BOOL)savePulseConfigWithError:(NSError **)error {
    NSError *err = nil;
    BOOL ok = [self.pulseConfig saveToPath:kPulseConfig error:&err];
    if (error && err) {
        *error = err;
    }
    if (!ok) {
        [self loadConfig];
        [self rebuildPulseCards];
        return NO;
    }
    [self loadConfig];
    [self rebuildPulseCards];
    [self showSaveNotice];
    return YES;
}

#pragma mark - JPPulseRuleCardViewDelegate

- (void)pulseRuleCardViewDidTap:(JPPulseRuleCardView *)card {
    [self openEditPulseRuleController:card.rule isNew:NO];
}

- (void)pulseRuleCardViewDidToggle:(JPPulseRuleCardView *)card enabled:(BOOL)enabled {
    if (enabled) {
        JPRestartConfig *restartCfg = [JPRestartConfig loadFromPath:kRestartConfig];
        JPRestartRule *rst = [restartCfg ruleForBundleId:card.rule.bid];
        if (rst && rst.thr > 0 && card.rule.target > rst.thr - 200) {
            UIAlertController *err = [UIAlertController alertControllerWithTitle:@"目标值过高"
                                                                         message:[NSString stringWithFormat:@"目标值必须比该 App 的超内存重启阈值（%ld MB）低至少 200 MB。当前重启阈值为 %ld MB，目标值最高只能设为 %ld MB。", (long)rst.thr, (long)rst.thr, (long)(rst.thr - 200)]
                                                                  preferredStyle:UIAlertControllerStyleAlert];
            [err addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
            [self presentViewController:err animated:YES completion:nil];
            [self rebuildPulseCards];
            return;
        }
    }
    card.rule.enabled = enabled;
    [self savePulseConfig];
    [self rebuildPulseCards];
}

- (void)pulseRuleCardViewDidRequestDelete:(JPPulseRuleCardView *)card {
    JPPulseRule *rule = card.rule;
    NSString *name = ResolveDisplayNameHelper([NSString stringWithFormat:@"bid:%@", rule.bid], self.installedApps);
    if (name.length == 0) name = rule.displayName.length > 0 ? rule.displayName : rule.bid;

    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"移除规则"
                                                                   message:[NSString stringWithFormat:@"确定要移除对「%@」的后台释放规则吗？", name]
                                                            preferredStyle:UIAlertControllerStyleActionSheet];

    __weak typeof(self) weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"移除" style:UIAlertActionStyleDestructive handler:^(UIAlertAction * _Nonnull action) {
        [weakSelf deletePulseRule:rule];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];

    if (alert.popoverPresentationController) {
        alert.popoverPresentationController.sourceView = card;
        alert.popoverPresentationController.sourceRect = card.bounds;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        [self presentViewController:alert animated:YES completion:nil];
    });
}

#pragma mark - JPPulseUnknownLineCardViewDelegate

- (void)pulseUnknownLineCardViewDidRequestDelete:(JPPulseLine *)line {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"移除无效配置行"
                                                                   message:[NSString stringWithFormat:@"确定要移除此无效配置行吗？\n\n%@", line.rawText ?: @""]
                                                            preferredStyle:UIAlertControllerStyleActionSheet];
    __weak typeof(self) weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"移除" style:UIAlertActionStyleDestructive handler:^(UIAlertAction * _Nonnull action) {
        [weakSelf.pulseConfig removeLine:line];
        [weakSelf savePulseConfig];
        [weakSelf rebuildPulseCards];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
    dispatch_async(dispatch_get_main_queue(), ^{
        [self presentViewController:alert animated:YES completion:nil];
    });
}

#pragma mark - JPPulseRuleEditViewControllerDelegate

- (BOOL)pulseRuleEditViewControllerDidSave:(JPPulseRule *)rule error:(NSError **)error {
    [self.pulseConfig setRule:rule forBundleId:rule.bid];
    return [self savePulseConfigWithError:error];
}

- (BOOL)pulseRuleEditViewControllerDidDelete:(JPPulseRule *)rule error:(NSError **)error {
    JPPulseLine *line = [self.pulseConfig lineForRule:rule];
    if (!line) {
        if (error) {
            *error = [NSError errorWithDomain:@"JetPilot"
                                         code:1001
                                     userInfo:@{NSLocalizedDescriptionKey: @"规则已变化，请刷新后重试"}];
        }
        return NO;
    }
    [self.pulseConfig removeLine:line];
    return [self savePulseConfigWithError:error];
}

@end



#pragma mark - Pulse Migration Manager (v0.7.8 C3-4)

@interface JPPulseMigrationManager : NSObject
+ (void)checkAndPerformMigrationWithInstalledApps:(NSArray<JPInstalledApp *> *)installedApps;
@end

@implementation JPPulseMigrationManager

+ (void)checkAndPerformMigrationWithInstalledApps:(NSArray<JPInstalledApp *> *)installedApps {
    NSFileManager *fm = [NSFileManager defaultManager];
    if ([fm fileExistsAtPath:kPulseMigrated]) {
        return;
    }

    BOOL onExists = [fm fileExistsAtPath:kMemPressureConfig];
    BOOL offExists = [fm fileExistsAtPath:kMemPressureConfigOff];

    if (!onExists) {
        if (offExists) {
            // E 的总开关是关的（只有 mempressure.txt.off）就不迁移
            [@"off\n" writeToFile:kPulseMigrated atomically:YES encoding:NSUTF8StringEncoding error:nil];
            NSLog(@"PULSE migrated n=0 skipped=0 (master switch off)");
        } else {
            // 两个文件都不存在
            [@"none\n" writeToFile:kPulseMigrated atomically:YES encoding:NSUTF8StringEncoding error:nil];
            NSLog(@"PULSE migrated n=0 skipped=0 (no config)");
        }
        return;
    }

    JPMemPressureConfig *eConfig = [JPMemPressureConfig loadFromPath:kMemPressureConfig];
    if (!eConfig) {
        // 读取失败，不写标记，下次启动再试
        return;
    }

    NSArray<JPInstalledApp *> *apps = installedApps ?: [JPInstalledApp scanInstalledApps];

    BOOL pulseExists = [fm fileExistsAtPath:kPulseConfig];
    JPPulseConfig *pulseCfg = [JPPulseConfig loadFromPath:kPulseConfig];
    if (pulseExists && !pulseCfg) {
        // pulse.txt 存在但读取失败，不覆盖、不写标记，下次启动再试
        return;
    }
    if (!pulseCfg) {
        pulseCfg = [JPPulseConfig new];
        pulseCfg.lines = [NSMutableArray array];
        pulseCfg.rules = [NSMutableArray array];
        pulseCfg.hasTrailingNewline = YES;
    }

    NSInteger migratedCount = 0;
    NSMutableArray<NSString *> *skippedLines = [NSMutableArray array];

    for (JPMemPressureLine *line in eConfig.lines) {
        if (line.type == JPMemPressureLineTypeComment || line.type == JPMemPressureLineTypeGlobalCooldown) {
            continue;
        }
        if (line.type == JPMemPressureLineTypeUnknown) {
            if (line.rawText.length > 0) {
                [skippedLines addObject:line.rawText];
            }
            continue;
        }
        JPMemPressureRule *rule = line.rule;
        if (!rule) continue;

        NSString *rawText = line.rawText.length > 0 ? line.rawText : rule.frag;
        NSString *frag = rule.frag ?: @"";

        // 不迁移的规则：扩展、Safari WebContent、系统 App
        if ([frag containsString:@".appex"] || [rawText containsString:@".appex"]) {
            [skippedLines addObject:rawText];
            continue;
        }
        if ([frag containsString:@"WebContent"] || [rawText containsString:@"WebContent"] ||
            [frag containsString:@"WebKit"] || [rawText containsString:@"WebKit"] ||
            [frag containsString:@"Safari"] || [rawText containsString:@"Safari"]) {
            [skippedLines addObject:rawText];
            continue;
        }
        if ([frag containsString:@"/Applications/"] || [rawText containsString:@"/Applications/"]) {
            [skippedLines addObject:rawText];
            continue;
        }

        NSString *targetBid = nil;
        if ([frag hasPrefix:@"bid:"]) {
            NSString *b = [frag substringFromIndex:4];
            if ([b hasPrefix:@"com.apple."]) {
                [skippedLines addObject:rawText];
                continue;
            }
            targetBid = b;
        } else {
            // 按 frag 找到对应 App 的 bid：匹配口径必须和守护进程现在的匹配函数一致
            // （路径子串，路径前缀 /var 和 /private/var 都要兼容）
            NSMutableArray<JPInstalledApp *> *matched = [NSMutableArray array];
            for (JPInstalledApp *app in apps) {
                NSString *execName = [app.bundleName stringByDeletingPathExtension];
                NSString *p1 = [app.appDirPath stringByAppendingPathComponent:execName];
                NSString *p2 = [NSString stringWithFormat:@"/private%@", p1];
                NSString *d1 = app.appDirPath;
                NSString *d2 = [NSString stringWithFormat:@"/private%@", d1];
                if ([p1 containsString:frag] || [p2 containsString:frag] ||
                    [d1 containsString:frag] || [d2 containsString:frag] ||
                    [app.pathFragment containsString:frag] || [app.bundleName containsString:frag]) {
                    [matched addObject:app];
                }
            }
            if (matched.count == 1) {
                JPInstalledApp *mApp = matched.firstObject;
                if (mApp.bundleId.length > 0 && ![mApp.bundleId hasPrefix:@"com.apple."]) {
                    targetBid = mApp.bundleId;
                } else {
                    [skippedLines addObject:rawText];
                    continue;
                }
            } else {
                // 匹配到 0 个或者多个，都不迁移，在提示里列出原规则的原文
                [skippedLines addObject:rawText];
                continue;
            }
        }

        if (targetBid.length == 0) {
            [skippedLines addObject:rawText];
            continue;
        }

        // 已经存在的 pulse.txt 行不覆盖（以用户自己设的为准）
        JPPulseRule *existingPulse = [pulseCfg ruleForBundleId:targetBid];
        if (existingPulse != nil) {
            continue;
        }

        // 参数换算
        // target = E 的阈值，低于 300 的按 300 算，上限 1500。微信固定用 550
        NSInteger target = 0;
        if ([targetBid isEqualToString:@"com.tencent.xin"]) {
            target = 550;
        } else {
            target = rule.thresholdMB;
            if (target < 300) target = 300;
            if (target > 1500) target = 1500;
        }

        // n = 冷却时间（秒）÷60，向上取整，最少 5；超过 60 的按 60 算；没写冷却时间时用全局值，全局值也没有就用 n=10
        NSInteger cdSec = 0;
        if (rule.cooldown > 0) {
            cdSec = rule.cooldown;
        } else if (eConfig.globalCooldown > 0) {
            cdSec = eConfig.globalCooldown;
        }
        NSInteger n = 10;
        if (cdSec > 0) {
            n = (NSInteger)ceil((double)cdSec / 60.0);
            if (n < 5) n = 5;
            if (n > 60) n = 60;
        }

        // daymax=30，delay=10
        NSInteger delay = 10;
        NSInteger daymax = 30;

        // 写成 on，默认开启。单条规则本身是 off 的，迁移后也是 off
        BOOL enabled = rule.isEnabled;

        JPPulseRule *newPR = [JPPulseRule new];
        newPR.bid = targetBid;
        newPR.enabled = enabled;
        newPR.target = target;
        newPR.n = n;
        newPR.daymax = daymax;
        newPR.delay = delay;

        [pulseCfg setRule:newPR forBundleId:targetBid];
        migratedCount++;
    }

    NSString *tmpPath = [kPulseConfig stringByAppendingString:@".tmp"];
    NSError *sErr = nil;
    BOOL saveOk = [pulseCfg saveToPath:tmpPath error:&sErr];
    if (!saveOk) {
        unlink([tmpPath fileSystemRepresentation]);
        NSLog(@"PULSE migration save error: %@", sErr);
        return; // 迁移失败时不写标记，下次启动再试
    }

    if (rename([tmpPath fileSystemRepresentation], [kPulseConfig fileSystemRepresentation]) != 0) {
        unlink([tmpPath fileSystemRepresentation]);
        NSLog(@"PULSE migration rename error errno=%d", errno);
        return; // 迁移失败时不写标记，下次启动再试
    }

    // 写入标记文件
    [@"1\n" writeToFile:kPulseMigrated atomically:YES encoding:NSUTF8StringEncoding error:nil];
    NSLog(@"PULSE migrated n=%ld skipped=%ld", (long)migratedCount, (long)skippedLines.count);

    if (skippedLines.count > 0) {
        NSString *items = [skippedLines componentsJoinedByString:@"\n"];
        NSString *msg = [NSString stringWithFormat:@"以下 App 的前台释放规则已停用：\n%@", items];
        dispatch_async(dispatch_get_main_queue(), ^{
            UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"前台释放停用说明"
                                                                           message:msg
                                                                    preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:@"知道了" style:UIAlertActionStyleDefault handler:nil]];
            UIWindow *win = UIApplication.sharedApplication.delegate.window;
            UIViewController *root = win.rootViewController;
            while (root.presentedViewController) {
                root = root.presentedViewController;
            }
            if (root) {
                [root presentViewController:alert animated:YES completion:nil];
            }
        });
    }
}

@end



#pragma mark - App Delegate & Main

@interface AppDelegate : UIResponder <UIApplicationDelegate, UITabBarControllerDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation AppDelegate

- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)opts {
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];

    MainViewController *vc1 = [MainViewController new];
    UINavigationController *nav1 = [[UINavigationController alloc] initWithRootViewController:vc1];
    nav1.tabBarItem = [[UITabBarItem alloc] initWithTitle:@"进程优先级"
                                                    image:[UIImage systemImageNamed:@"slider.horizontal.3"]
                                                      tag:0];

    JPMemPressureViewController *vc2 = [JPMemPressureViewController new];
    UINavigationController *nav2 = [[UINavigationController alloc] initWithRootViewController:vc2];
    nav2.tabBarItem = [[UITabBarItem alloc] initWithTitle:@"内存释放"
                                                    image:[UIImage systemImageNamed:@"memorychip"]
                                                      tag:1];

    JPRestartViewController *vc3 = [JPRestartViewController new];
    UINavigationController *nav3 = [[UINavigationController alloc] initWithRootViewController:vc3];
    nav3.tabBarItem = [[UITabBarItem alloc] initWithTitle:@"超内存重启"
                                                    image:[UIImage systemImageNamed:@"arrow.clockwise"]
                                                      tag:2];

    UITabBarController *tabBar = [UITabBarController new];
    tabBar.viewControllers = @[nav1, nav2, nav3];
    tabBar.delegate = self;

    self.window.rootViewController = tabBar;
    [self.window makeKeyAndVisible];

    dispatch_async(dispatch_get_main_queue(), ^{
        [JPPulseMigrationManager checkAndPerformMigrationWithInstalledApps:nil];
    });

    return YES;
}

@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(AppDelegate.class));
    }
}
