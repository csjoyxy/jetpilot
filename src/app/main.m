/*
 * JetPilot v0.6.0 — iOS 15+ TrollStore 守护进程管理与规则编辑
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

#define POSIX_SPAWN_PERSONA_FLAGS_OVERRIDE 1
extern int posix_spawnattr_set_persona_np(const posix_spawnattr_t *, uid_t, uint32_t);
extern int posix_spawnattr_set_persona_uid_np(const posix_spawnattr_t *, uid_t);
extern int posix_spawnattr_set_persona_gid_np(const posix_spawnattr_t *, uid_t);
extern char **environ;

static NSString *const kDir        = @"/var/mobile/Library/JetPilot";
static NSString *const kConfig     = @"/var/mobile/Library/JetPilot/config.txt";
static NSString *const kConfigBackup = @"/var/mobile/Library/JetPilot/config.txt.pre-0.5.6";
static NSString *const kMigrationDeclined = @"/var/mobile/Library/JetPilot/migration.declined";
static NSString *const kFlag       = @"/var/mobile/Library/JetPilot/running.flag";
static NSString *const kStatus     = @"/var/mobile/Library/JetPilot/status.txt";
static NSString *const kPidFile    = @"/var/mobile/Library/JetPilot/daemon.pid";
static NSString *const kLogFile    = @"/var/mobile/Library/JetPilot/daemon.log";
static NSString *const kLogOldFile = @"/var/mobile/Library/JetPilot/daemon.log.1";

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
           @"# demote  <路径片段>   （不在前台时降到 30）\n"
           @"protect Reynard.app/ 90\n";
}

static NSString *BandDescription(NSInteger band) {
    if (band >= 95) return [NSString stringWithFormat:@"%ld · 最高保护", (long)band];
    if (band >= 90) return [NSString stringWithFormat:@"%ld · 接近前台", (long)band];
    if (band >= 80) return [NSString stringWithFormat:@"%ld · 高优先级后台", (long)band];
    if (band >= 60) return [NSString stringWithFormat:@"%ld · 中优先级后台", (long)band];
    if (band >= 40) return [NSString stringWithFormat:@"%ld · 低优先级后台", (long)band];
    if (band > 30)  return [NSString stringWithFormat:@"%ld · 普通后台", (long)band];
    return @"30 · 系统默认后台";
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

@interface JPStatusInfo : NSObject
@property (nonatomic, copy) NSString *updated;
@property (nonatomic, assign) NSInteger protectRules;
@property (nonatomic, assign) NSInteger demoteRules;
@property (nonatomic, assign) NSInteger protectedPids;
@property (nonatomic, assign) NSInteger demotedPids;
@property (nonatomic, assign) unsigned long reassertsTotal;
@property (nonatomic, copy) NSArray<JPProtectedApp *> *protectedApps;
@property (nonatomic, assign) BOOL hasProtectedAppLines;
@property (nonatomic, assign) BOOL bidSupport;      /* 守护进程是否支持 bid: 规则 */
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
@end

@implementation JPStatusInfo

+ (JPProtectedApp *)parseProtectedAppLine:(NSString *)line {
    if (!line || ![line hasPrefix:@"protected_app="]) return nil;
    NSString *raw = [line substringFromIndex:14];
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

+ (JPStatusInfo *)loadCurrentStatus {
    JPStatusInfo *st = [JPStatusInfo new];
    st.updated = @"";
    st.protectedApps = @[];
    st.hasProtectedAppLines = NO;
    st.bidSupport = NO;
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
    NSMutableDictionary<NSNumber *, NSString *> *labels = [NSMutableDictionary dictionary];
    NSMutableDictionary<NSNumber *, NSString *> *bids = [NSMutableDictionary dictionary];
    NSArray<NSString *> *lines = [raw componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]];
    for (NSString *line in lines) {
        if ([line hasPrefix:@"updated="]) {
            st.updated = [line substringFromIndex:8];
        } else if ([line hasPrefix:@"bid_support="]) {
            st.bidSupport = ([[line substringFromIndex:12] intValue] == 1);
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
            JPProtectedApp *app = [self parseProtectedAppLine:line];
            if (app) {
                [apps addObject:app];
            }
        } else if ([line hasPrefix:@"protected_proc="]) {
            NSString *rest = [line substringFromIndex:15];
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
    st.protectedApps = [apps copy];
    return st;
}
@end

@interface JPInstalledApp : NSObject
@property (nonatomic, copy) NSString *displayName;
@property (nonatomic, copy) NSString *bundleName;
@property (nonatomic, copy) NSString *bundleId;
@property (nonatomic, copy) NSString *pathFragment;
+ (NSArray<JPInstalledApp *> *)scanInstalledApps;
@end

@implementation JPInstalledApp
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
    if ([frag hasPrefix:@"bid:"]) {
        NSString *targetBid = [frag substringFromIndex:4];
        for (JPInstalledApp *app in apps) {
            if (app.bundleId.length > 0 && [app.bundleId isEqualToString:targetBid]) {
                return app.displayName;
            }
        }
        return frag;
    }
    for (JPInstalledApp *app in apps) {
        if ([frag containsString:app.bundleName] || [app.bundleName containsString:frag]) {
            return app.displayName;
        }
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
+ (JPKilledStatsInfo *)loadStatsForRules:(NSArray<JPRule *> *)rules installedApps:(NSArray<JPInstalledApp *> *)apps;
@end

@implementation JPKilledStatsInfo

+ (JPKilledStatsInfo *)loadStatsForRules:(NSArray<JPRule *> *)rules installedApps:(NSArray<JPInstalledApp *> *)apps {
    JPKilledStatsInfo *result = [JPKilledStatsInfo new];
    result.totalKills = 0;
    result.daySummaries = @[];
    result.skippedFilesCount = 0;

    @try {
        NSMutableArray<JPRule *> *protectRules = [NSMutableArray array];
        for (JPRule *r in rules) {
            if (r.type == JPRuleTypeProtect && r.pathFragment.length > 0) {
                [protectRules addObject:r];
            }
        }
        if (protectRules.count == 0) {
            return result;
        }

        NSFileManager *fm = NSFileManager.defaultManager;
        BOOL isDir = NO;
        if (![fm fileExistsAtPath:kCrashReporterDir isDirectory:&isDir] || !isDir) {
            return result;
        }

        NSError *dirErr = nil;
        NSArray<NSString *> *allFiles = [fm contentsOfDirectoryAtPath:kCrashReporterDir error:&dirErr];
        if (!allFiles || allFiles.count == 0) {
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

- (void)updateWithStatus:(JPStatusInfo *)status isRunning:(BOOL)isRunning;
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

        [self.actionButton.topAnchor constraintEqualToAnchor:div2.bottomAnchor constant:12],
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
    self.summaryLabel.textColor = UIColor.systemGreenColor;
    self.summaryLabel.text = @"无被杀记录";

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

    if (stats.totalKills == 0) {
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
- (void)ruleCardView:(JPRuleCardView *)card didChangeBand:(NSInteger)band;
- (void)ruleCardViewDidFinishBandChange:(JPRuleCardView *)card;
- (void)ruleCardViewDidTap:(JPRuleCardView *)card;
@end

@interface JPRuleCardView : UIView
@property (nonatomic, strong) JPRule *rule;
@property (nonatomic, weak) id<JPRuleCardViewDelegate> delegate;
@property (nonatomic, strong) UILabel *titleLabel;
@property (nonatomic, strong) UILabel *pathLabel;
@property (nonatomic, strong) UILabel *statusLabel;
@property (nonatomic, strong) UILabel *bandLabel;
@property (nonatomic, strong) UISlider *slider;
@property (nonatomic, strong) UIButton *deleteButton;

- (instancetype)initWithRule:(JPRule *)rule delegate:(id<JPRuleCardViewDelegate>)delegate;
- (void)updateStatusWithInfo:(JPStatusInfo *)status daemonRunning:(BOOL)daemonRunning;
@end

@implementation JPRuleCardView

- (instancetype)initWithRule:(JPRule *)rule delegate:(id<JPRuleCardViewDelegate>)delegate {
    self = [super initWithFrame:CGRectZero];
    if (self) {
        _rule = rule;
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
    self.titleLabel.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold];
    self.titleLabel.textColor = UIColor.labelColor;
    self.titleLabel.text = self.rule.displayName.length > 0 ? self.rule.displayName : self.rule.pathFragment;

    self.pathLabel = [UILabel new];
    self.pathLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.pathLabel.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
    self.pathLabel.textColor = UIColor.secondaryLabelColor;
    self.pathLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
    self.pathLabel.text = self.rule.pathFragment;

    self.statusLabel = [UILabel new];
    self.statusLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.statusLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightMedium];
    self.statusLabel.textColor = UIColor.secondaryLabelColor;
    self.statusLabel.numberOfLines = 0;

    self.deleteButton = [UIButton buttonWithType:UIButtonTypeSystem];
    self.deleteButton.translatesAutoresizingMaskIntoConstraints = NO;
    [self.deleteButton setImage:[UIImage systemImageNamed:@"trash"] forState:UIControlStateNormal];
    self.deleteButton.tintColor = UIColor.systemRedColor;
    [self.deleteButton addTarget:self action:@selector(deleteTapped) forControlEvents:UIControlEventTouchUpInside];

    UIImageView *chevron = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"chevron.right"]];
    chevron.translatesAutoresizingMaskIntoConstraints = NO;
    chevron.tintColor = UIColor.tertiaryLabelColor;
    chevron.contentMode = UIViewContentModeScaleAspectFit;

    [self addSubview:self.titleLabel];
    [self addSubview:self.pathLabel];
    [self addSubview:self.statusLabel];
    [self addSubview:self.deleteButton];
    [self addSubview:chevron];

    [NSLayoutConstraint activateConstraints:@[
        [chevron.centerYAnchor constraintEqualToAnchor:self.titleLabel.centerYAnchor],
        [chevron.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-14],
        [chevron.widthAnchor constraintEqualToConstant:10],
        [chevron.heightAnchor constraintEqualToConstant:14],

        [self.deleteButton.centerYAnchor constraintEqualToAnchor:self.titleLabel.centerYAnchor],
        [self.deleteButton.trailingAnchor constraintEqualToAnchor:chevron.leadingAnchor constant:-6],
        [self.deleteButton.widthAnchor constraintEqualToConstant:32],
        [self.deleteButton.heightAnchor constraintEqualToConstant:32],

        [self.titleLabel.topAnchor constraintEqualToAnchor:self.topAnchor constant:14],
        [self.titleLabel.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:16],
        [self.titleLabel.trailingAnchor constraintLessThanOrEqualToAnchor:self.deleteButton.leadingAnchor constant:-8],

        [self.pathLabel.topAnchor constraintEqualToAnchor:self.titleLabel.bottomAnchor constant:3],
        [self.pathLabel.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:16],
        [self.pathLabel.trailingAnchor constraintLessThanOrEqualToAnchor:self.deleteButton.leadingAnchor constant:-8],

        [self.statusLabel.topAnchor constraintEqualToAnchor:self.pathLabel.bottomAnchor constant:5],
        [self.statusLabel.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:16],
        [self.statusLabel.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-16]
    ]];

    if (self.rule.type == JPRuleTypeProtect) {
        self.bandLabel = [UILabel new];
        self.bandLabel.translatesAutoresizingMaskIntoConstraints = NO;
        self.bandLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightMedium];
        self.bandLabel.textColor = UIColor.systemBlueColor;
        self.bandLabel.text = BandDescription(self.rule.targetBand);

        self.slider = [UISlider new];
        self.slider.translatesAutoresizingMaskIntoConstraints = NO;
        self.slider.minimumValue = 30.0f;
        self.slider.maximumValue = 95.0f;
        self.slider.value = (float)self.rule.targetBand;
        self.slider.tintColor = UIColor.systemBlueColor;
        [self.slider addTarget:self action:@selector(sliderValueChanged:) forControlEvents:UIControlEventValueChanged];
        [self.slider addTarget:self action:@selector(sliderTouchEnded:) forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside | UIControlEventTouchCancel];

        UILabel *minHint = [UILabel new];
        minHint.translatesAutoresizingMaskIntoConstraints = NO;
        minHint.font = [UIFont systemFontOfSize:11 weight:UIFontWeightRegular];
        minHint.textColor = UIColor.tertiaryLabelColor;
        minHint.text = @"30 后台";

        UILabel *maxHint = [UILabel new];
        maxHint.translatesAutoresizingMaskIntoConstraints = NO;
        maxHint.font = [UIFont systemFontOfSize:11 weight:UIFontWeightRegular];
        maxHint.textColor = UIColor.tertiaryLabelColor;
        maxHint.text = @"95 顶配";
        maxHint.textAlignment = NSTextAlignmentRight;

        [self addSubview:self.bandLabel];
        [self addSubview:self.slider];
        [self addSubview:minHint];
        [self addSubview:maxHint];

        [NSLayoutConstraint activateConstraints:@[
            [self.bandLabel.topAnchor constraintEqualToAnchor:self.statusLabel.bottomAnchor constant:10],
            [self.bandLabel.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:16],
            [self.bandLabel.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-16],

            [self.slider.topAnchor constraintEqualToAnchor:self.bandLabel.bottomAnchor constant:6],
            [self.slider.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:16],
            [self.slider.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-16],

            [minHint.topAnchor constraintEqualToAnchor:self.slider.bottomAnchor constant:2],
            [minHint.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:16],
            [minHint.bottomAnchor constraintEqualToAnchor:self.bottomAnchor constant:-14],

            [maxHint.topAnchor constraintEqualToAnchor:self.slider.bottomAnchor constant:2],
            [maxHint.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-16],
            [maxHint.bottomAnchor constraintEqualToAnchor:self.bottomAnchor constant:-14]
        ]];
    } else {
        UIView *badge = [UIView new];
        badge.translatesAutoresizingMaskIntoConstraints = NO;
        badge.backgroundColor = [UIColor.systemOrangeColor colorWithAlphaComponent:0.12];
        badge.layer.cornerRadius = 6;

        UIImageView *badgeIcon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"arrow.down.circle.fill"]];
        badgeIcon.translatesAutoresizingMaskIntoConstraints = NO;
        badgeIcon.tintColor = UIColor.systemOrangeColor;

        UILabel *badgeText = [UILabel new];
        badgeText.translatesAutoresizingMaskIntoConstraints = NO;
        badgeText.font = [UIFont systemFontOfSize:13 weight:UIFontWeightMedium];
        badgeText.textColor = UIColor.systemOrangeColor;
        badgeText.text = @"降级到后台 30";

        [badge addSubview:badgeIcon];
        [badge addSubview:badgeText];

        [NSLayoutConstraint activateConstraints:@[
            [badgeIcon.leadingAnchor constraintEqualToAnchor:badge.leadingAnchor constant:8],
            [badgeIcon.centerYAnchor constraintEqualToAnchor:badge.centerYAnchor],
            [badgeIcon.widthAnchor constraintEqualToConstant:16],
            [badgeIcon.heightAnchor constraintEqualToConstant:16],

            [badgeText.leadingAnchor constraintEqualToAnchor:badgeIcon.trailingAnchor constant:6],
            [badgeText.trailingAnchor constraintEqualToAnchor:badge.trailingAnchor constant:-8],
            [badgeText.centerYAnchor constraintEqualToAnchor:badge.centerYAnchor],
            [badge.heightAnchor constraintEqualToConstant:28]
        ]];

        [self addSubview:badge];
        [NSLayoutConstraint activateConstraints:@[
            [badge.topAnchor constraintEqualToAnchor:self.statusLabel.bottomAnchor constant:10],
            [badge.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:16],
            [badge.bottomAnchor constraintEqualToAnchor:self.bottomAnchor constant:-14]
        ]];
    }

    UITapGestureRecognizer *tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(cardTapped:)];
    [self addGestureRecognizer:tap];
}

- (void)cardTapped:(UITapGestureRecognizer *)gesture {
    CGPoint pt = [gesture locationInView:self];
    if (CGRectContainsPoint(self.deleteButton.frame, pt)) return;
    if (self.slider && CGRectContainsPoint(self.slider.frame, pt)) return;
    if ([self.delegate respondsToSelector:@selector(ruleCardViewDidTap:)]) {
        [self.delegate ruleCardViewDidTap:self];
    }
}

- (void)updateStatusWithInfo:(JPStatusInfo *)status daemonRunning:(BOOL)daemonRunning {
    if (!daemonRunning) {
        self.statusLabel.text = @"守护进程未运行";
        self.statusLabel.textColor = UIColor.secondaryLabelColor;
        return;
    }
    if ([self.rule.pathFragment hasPrefix:@"bid:"] && (!status || !status.bidSupport)) {
        self.statusLabel.text = @"守护进程版本过低，此规则不生效";
        self.statusLabel.textColor = UIColor.systemRedColor;
        return;
    }
    // 必改 4-3: 降级规则卡片状态只走前两级（未运行/版本低），否则固定显示说明（status.txt 仅记录保护规则）
    if (self.rule.type == JPRuleTypeDemote) {
        self.statusLabel.text = @"不在前台时降到 30";
        self.statusLabel.textColor = UIColor.secondaryLabelColor;
        return;
    }
    if (self.rule.isConflict) {
        NSString *names = [self.rule.conflictAppNames componentsJoinedByString:@"、"] ?: @"";
        self.statusLabel.text = [NSString stringWithFormat:@"会命中多个 App：%@，点此重选", names];
        self.statusLabel.textColor = UIColor.systemOrangeColor;
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
                // 小问题: 路径匹配使用 isEqualToString，避免 Runner.app/ 把 xRunner.app/ 算成自己的
                if ([pApp.pathFragment isEqualToString:self.rule.pathFragment]) {
                    [matchedPids addObject:@(pApp.pid)];
                }
            }
        }
    }

    if (matchedPids.count == 1) {
        self.statusLabel.text = [NSString stringWithFormat:@"保护中 · pid %d", [matchedPids.firstObject intValue]];
        self.statusLabel.textColor = UIColor.systemGreenColor;
    } else if (matchedPids.count > 1) {
        self.statusLabel.text = [NSString stringWithFormat:@"保护中 · %lu 个进程", (unsigned long)matchedPids.count];
        self.statusLabel.textColor = UIColor.systemGreenColor;
    } else {
        self.statusLabel.text = @"没在运行 · 打开后自动保护";
        self.statusLabel.textColor = UIColor.secondaryLabelColor;
    }
}

- (void)sliderValueChanged:(UISlider *)slider {
    NSInteger band = (NSInteger)lroundf(slider.value);
    self.rule.targetBand = band;
    self.bandLabel.text = BandDescription(band);
    if ([self.delegate respondsToSelector:@selector(ruleCardView:didChangeBand:)]) {
        [self.delegate ruleCardView:self didChangeBand:band];
    }
}

- (void)sliderTouchEnded:(UISlider *)slider {
    NSInteger band = (NSInteger)lroundf(slider.value);
    self.rule.targetBand = band;
    if ([self.delegate respondsToSelector:@selector(ruleCardViewDidFinishBandChange:)]) {
        [self.delegate ruleCardViewDidFinishBandChange:self];
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
@property (nonatomic, strong) UITableView *tableView;
@property (nonatomic, strong) UISearchController *searchController;
@property (nonatomic, strong) UIView *emptyView;
@property (nonatomic, copy) void (^onSelectApp)(NSString *pathFragment, NSString *displayName);
@property (nonatomic, copy) void (^onManualAdd)(NSString *pathFragment, JPRuleType type, NSInteger band);
@end

@implementation JPAppPickerViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"添加规则";
    self.view.backgroundColor = UIColor.systemGroupedBackgroundColor;

    self.filteredApps = [NSMutableArray arrayWithArray:self.allApps];

    self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"手动输入"
                                                                             style:UIBarButtonItemStylePlain
                                                                            target:self
                                                                            action:@selector(showManualInputDialog)];
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
    [manualBtn setTitle:@"手动输入路径片段" forState:UIControlStateNormal];
    manualBtn.titleLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightMedium];
    [manualBtn addTarget:self action:@selector(showManualInputDialog) forControlEvents:UIControlEventTouchUpInside];

    [self.emptyView addSubview:icon];
    [self.emptyView addSubview:tip];
    [self.emptyView addSubview:manualBtn];

    [NSLayoutConstraint activateConstraints:@[
        [icon.centerXAnchor constraintEqualToAnchor:self.emptyView.centerXAnchor],
        [icon.topAnchor constraintEqualToAnchor:self.emptyView.topAnchor],
        [icon.widthAnchor constraintEqualToConstant:44],
        [icon.heightAnchor constraintEqualToConstant:44],

        [tip.topAnchor constraintEqualToAnchor:icon.bottomAnchor constant:12],
        [tip.leadingAnchor constraintEqualToAnchor:self.emptyView.leadingAnchor constant:24],
        [tip.trailingAnchor constraintEqualToAnchor:self.emptyView.trailingAnchor constant:-24],

        [manualBtn.topAnchor constraintEqualToAnchor:tip.bottomAnchor constant:16],
        [manualBtn.centerXAnchor constraintEqualToAnchor:self.emptyView.centerXAnchor],
        [manualBtn.bottomAnchor constraintEqualToAnchor:self.emptyView.bottomAnchor]
    ]];

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

- (void)cancelTapped {
    [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)updateSearchResultsForSearchController:(UISearchController *)searchController {
    NSString *term = [searchController.searchBar.text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (term.length == 0) {
        self.filteredApps = [NSMutableArray arrayWithArray:self.allApps];
    } else {
        [self.filteredApps removeAllObjects];
        for (JPInstalledApp *app in self.allApps) {
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

    BOOL added = [self isAppAlreadyAdded:app];
    if (added) {
        cell.detailTextLabel.text = [NSString stringWithFormat:@"%@ · 已在规则中", app.pathFragment];
        cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
        cell.accessoryType = UITableViewCellAccessoryCheckmark;
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
    } else {
        cell.detailTextLabel.text = app.pathFragment;
        cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
        cell.accessoryType = UITableViewCellAccessoryNone;
        cell.selectionStyle = UITableViewCellSelectionStyleDefault;
    }
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    JPInstalledApp *app = self.filteredApps[indexPath.row];
    if ([self isAppAlreadyAdded:app]) {
        UIAlertController *notice = [UIAlertController alertControllerWithTitle:@"规则已存在"
                                                                        message:[NSString stringWithFormat:@"「%@」已在规则列表中", app.displayName]
                                                                 preferredStyle:UIAlertControllerStyleAlert];
        [notice addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:notice animated:YES completion:nil];
        return;
    }

    NSString *bid = app.bundleId;
    if (bid && bid.length > 0) {
        if (self.onSelectApp) {
            self.onSelectApp([NSString stringWithFormat:@"bid:%@", bid], app.displayName);
        }
        [self dismissViewControllerAnimated:YES completion:nil];
    } else {
        __weak typeof(self) weakSelf = self;
        UIAlertController *warn = [UIAlertController alertControllerWithTitle:@"提示"
                                                                      message:@"这个 App 读不到标识，按目录名匹配，可能会误伤同名 App"
                                                               preferredStyle:UIAlertControllerStyleAlert];
        [warn addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:^(UIAlertAction * _Nonnull action) {
            if (weakSelf.onSelectApp) {
                weakSelf.onSelectApp(app.pathFragment, app.displayName);
            }
            [weakSelf dismissViewControllerAnimated:YES completion:nil];
        }]];
        // 提交 2 小 1: 选 App 读不到 bundleId 时的提示框加「取消」选项
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
            if (weakSelf.onManualAdd) {
                weakSelf.onManualAdd(text, type, band);
            }
            [weakSelf dismissViewControllerAnimated:YES completion:nil];
        }]];
        [warn addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
        [self presentViewController:warn animated:YES completion:nil];
    } else {
        if (weakSelf.onManualAdd) {
            weakSelf.onManualAdd(text, type, band);
        }
        [weakSelf dismissViewControllerAnimated:YES completion:nil];
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
    [alert addAction:[UIAlertAction actionWithTitle:@"添加为保护 (90 档)" style:UIAlertActionStyleDefault handler:^(UIAlertAction * _Nonnull action) {
        NSString *text = [weakAlert.textFields.firstObject.text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        if (text.length == 0) return;
        if (![text containsString:@"/"] && ![text hasSuffix:@".app"]) {
            text = [text stringByAppendingString:@".app/"];
        }
        [weakSelf promptManualAddWithText:text type:JPRuleTypeProtect band:90];
    }]];

    [alert addAction:[UIAlertAction actionWithTitle:@"添加为降级 (30 档)" style:UIAlertActionStyleDefault handler:^(UIAlertAction * _Nonnull action) {
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

@interface JPLogViewController : UIViewController
@property (nonatomic, strong) UITextView *textView;
@property (nonatomic, strong) UIBarButtonItem *clearItem;
@end

@implementation JPLogViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"调试日志";
    self.view.backgroundColor = UIColor.systemGroupedBackgroundColor;

    UIBarButtonItem *copyBtn = [[UIBarButtonItem alloc] initWithTitle:@"复制"
                                                                style:UIBarButtonItemStylePlain
                                                               target:self
                                                               action:@selector(copyLog)];
    self.clearItem = [[UIBarButtonItem alloc] initWithTitle:@"清空"
                                                      style:UIBarButtonItemStylePlain
                                                     target:self
                                                     action:@selector(confirmClearLog)];
    self.navigationItem.rightBarButtonItems = @[copyBtn, self.clearItem];

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
        [self.textView.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
        [self.textView.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [self.textView.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [self.textView.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor]
    ]];
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
@end

@implementation JPRuleEditViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"编辑规则";
    self.view.backgroundColor = UIColor.systemGroupedBackgroundColor;

    self.currentBand = self.rule.targetBand;
    if (self.currentBand < 30) self.currentBand = 30;
    if (self.currentBand > 95) self.currentBand = 95;
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
    return (self.rule.type == JPRuleTypeProtect) ? 3 : 2;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    if (section == 0) return 3;
    if (self.rule.type == JPRuleTypeProtect && section == 1) return 1;
    return 1;
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    if (section == 0) return @"应用信息";
    if (self.rule.type == JPRuleTypeProtect && section == 1) return @"目标保护档位（步进 5）";
    return nil;
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
        } else {
            UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
            cell.textLabel.text = @"重新选择 App";
            cell.textLabel.textColor = UIColor.systemBlueColor;
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
            return cell;
        }
    } else if (self.rule.type == JPRuleTypeProtect && indexPath.section == 1) {
        UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
        cell.selectionStyle = UITableViewCellSelectionStyleNone;

        self.sliderValLabel = [UILabel new];
        self.sliderValLabel.translatesAutoresizingMaskIntoConstraints = NO;
        self.sliderValLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightMedium];
        self.sliderValLabel.textColor = UIColor.systemBlueColor;
        self.sliderValLabel.text = BandDescription(self.currentBand);

        UISlider *slider = [UISlider new];
        slider.translatesAutoresizingMaskIntoConstraints = NO;
        slider.minimumValue = 30.0f;
        slider.maximumValue = 95.0f;
        slider.value = (float)self.currentBand;
        [slider addTarget:self action:@selector(sliderMoved:) forControlEvents:UIControlEventValueChanged];

        [cell.contentView addSubview:self.sliderValLabel];
        [cell.contentView addSubview:slider];

        [NSLayoutConstraint activateConstraints:@[
            [self.sliderValLabel.topAnchor constraintEqualToAnchor:cell.contentView.topAnchor constant:12],
            [self.sliderValLabel.leadingAnchor constraintEqualToAnchor:cell.contentView.leadingAnchor constant:16],
            [self.sliderValLabel.trailingAnchor constraintEqualToAnchor:cell.contentView.trailingAnchor constant:-16],

            [slider.topAnchor constraintEqualToAnchor:self.sliderValLabel.bottomAnchor constant:10],
            [slider.leadingAnchor constraintEqualToAnchor:cell.contentView.leadingAnchor constant:16],
            [slider.trailingAnchor constraintEqualToAnchor:cell.contentView.trailingAnchor constant:-16],
            [slider.bottomAnchor constraintEqualToAnchor:cell.contentView.bottomAnchor constant:-14]
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
    NSInteger stepped = (NSInteger)lroundf(slider.value / 5.0f) * 5;
    if (stepped < 30) stepped = 30;
    if (stepped > 95) stepped = 95;
    self.currentBand = stepped;
    self.sliderValLabel.text = BandDescription(stepped);
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    if (indexPath.section == 0 && indexPath.row == 2) {
        [self reselectApp];
    } else if ((self.rule.type == JPRuleTypeProtect && indexPath.section == 2) ||
               (self.rule.type != JPRuleTypeProtect && indexPath.section == 1)) {
        [self confirmDelete];
    }
}

@end

#pragma mark - Controllers: Main ViewController

@interface MainViewController : UIViewController <JPRuleCardViewDelegate, JPRuleEditViewControllerDelegate>
@property (nonatomic, strong) NSMutableArray<JPRule *> *rules;
@property (nonatomic, copy) NSArray<JPInstalledApp *> *installedApps;

@property (nonatomic, strong) UIScrollView *scrollView;
@property (nonatomic, strong) UIStackView *contentStackView;

@property (nonatomic, strong) JPStatusCardView *statusCard;
@property (nonatomic, strong) JPKilledStatsCardView *killedStatsCard;
// 被杀统计的后台串行队列 + 世代计数：解析放后台，过期结果丢弃
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
@end

@implementation MainViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"JetPilot";
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
    // 1. Status Section
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
    // 提交 2 小 2: 文案和点击逻辑改为停止后自动重启守护进程
    [self.versionWarningBanner setTitle:@"守护进程是旧版本，按 App 添加的规则不会生效。点击重启守护进程。" forState:UIControlStateNormal];
    [self.versionWarningBanner setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    self.versionWarningBanner.titleLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightMedium];
    self.versionWarningBanner.titleLabel.numberOfLines = 0;
    self.versionWarningBanner.titleLabel.textAlignment = NSTextAlignmentCenter;
    self.versionWarningBanner.contentEdgeInsets = UIEdgeInsetsMake(10, 14, 10, 14);
    [self.versionWarningBanner addTarget:self action:@selector(restartDaemon) forControlEvents:UIControlEventTouchUpInside];
    self.versionWarningBanner.hidden = YES;
    [self.contentStackView addArrangedSubview:self.versionWarningBanner];

    UILabel *statusHeader = [self createSectionHeader:@"运行状态"];
    self.statusCard = [JPStatusCardView new];
    __weak typeof(self) weakSelf = self;
    self.statusCard.onActionToggle = ^{
        [weakSelf toggleDaemon];
    };
    self.statusCard.onTapProtected = ^{
        [weakSelf showProtectedList];
    };

    [self.contentStackView addArrangedSubview:statusHeader];
    [self.contentStackView addArrangedSubview:self.statusCard];

    // 2. Killed Stats Section
    self.killedStatsCard = [JPKilledStatsCardView new];
    [self.contentStackView addArrangedSubview:self.killedStatsCard];

    // 3. Rules Section
    self.rulesSectionHeaderLabel = [self createSectionHeader:@"保护规则"];
    self.rulesStackView = [UIStackView new];
    self.rulesStackView.translatesAutoresizingMaskIntoConstraints = NO;
    self.rulesStackView.axis = UILayoutConstraintAxisVertical;
    self.rulesStackView.spacing = 12;

    [self.contentStackView addArrangedSubview:self.rulesSectionHeaderLabel];
    [self.contentStackView addArrangedSubview:self.rulesStackView];

    // 3. Add Rule Card
    self.addRuleCard = [self createAddRuleCard];
    [self.contentStackView addArrangedSubview:self.addRuleCard];

    // 4. About Section
    UILabel *aboutHeader = [self createSectionHeader:@"关于"];
    [self.contentStackView addArrangedSubview:aboutHeader];
    UIView *aboutCard = [self createAboutCard];
    [self.contentStackView addArrangedSubview:aboutCard];

    // 5. Footer Note
    UILabel *footer = [UILabel new];
    footer.translatesAutoresizingMaskIntoConstraints = NO;
    footer.font = [UIFont systemFontOfSize:12 weight:UIFontWeightRegular];
    footer.textColor = UIColor.tertiaryLabelColor;
    footer.textAlignment = NSTextAlignmentCenter;
    footer.numberOfLines = 0;
    footer.text = @"改动实时自动保存 · 守护进程每 5–10 秒轮询生效";
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
    versionLabel.text = [NSString stringWithFormat:@"JetPilot v%@", version];

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
    [logButton setTitleColor:UIColor.systemBlueColor forState:UIControlStateNormal];
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
    lbl.text = @"已保存，守护进程 5–10 秒内生效";

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
        if (cleanTokens.count < 2) continue;

        NSString *verb = cleanTokens[0];
        NSString *frag = cleanTokens[1];

        if ([verb isEqualToString:@"protect"]) {
            NSInteger band = 90;
            if (cleanTokens.count >= 3) {
                band = [cleanTokens[2] integerValue];
            }
            if (band <= 0) band = 90;
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
            JPRule *r = [JPRule new];
            r.type = JPRuleTypeDemote;
            r.pathFragment = frag;
            r.targetBand = 30;
            r.displayName = [self resolveDisplayNameForFragment:frag];
            [self evaluateRuleConflict:r];
            [self.rules addObject:r];
        }
    }
}

- (BOOL)saveConfigWithError:(NSError **)errorOut {
    NSMutableString *outStr = [NSMutableString string];
    [outStr appendString:@"# JetPilot 规则（iOS 16 档位：后台 30，前台 100）\n"];
    [outStr appendString:@"# protect <路径片段> <档位，最高 95>\n"];
    [outStr appendString:@"# demote  <路径片段>   （不在前台时降到 30）\n"];

    for (JPRule *r in self.rules) {
        if (r.type == JPRuleTypeProtect) {
            NSInteger band = r.targetBand;
            if (band < 30) band = 30;
            if (band > 95) band = 95;
            [outStr appendFormat:@"protect %@ %ld\n", r.pathFragment, (long)band];
        } else if (r.type == JPRuleTypeDemote) {
            [outStr appendFormat:@"demote %@\n", r.pathFragment];
        }
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

    // 必改 3-4: 撤销持久化标记检查，若用户此前撤销过则不再自动迁移
    NSFileManager *fm = NSFileManager.defaultManager;
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

    // 必改 3-2: 备份失败则不迁移，直接 return（本次不再尝试）
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
        // 小问题: 与 refreshStatus 统一使用 [self daemonState] == JPDaemonStateRunning
        BOOL isRunning = ([self daemonState] == JPDaemonStateRunning);
        for (JPRule *rule in self.rules) {
            JPRuleCardView *card = [[JPRuleCardView alloc] initWithRule:rule delegate:self];
            [card updateStatusWithInfo:st daemonRunning:isRunning];
            [self.rulesStackView addArrangedSubview:card];
        }
    }
    self.rulesSectionHeaderLabel.text = [NSString stringWithFormat:@"保护规则 (%lu)", (unsigned long)self.rules.count];
    if (self.killedStatsCard) {
        [self refreshKilledStatsIfNeeded:YES];
    }
}

#pragma mark - Daemon Control

- (JPDaemonState)daemonState {
    NSFileManager *fm = NSFileManager.defaultManager;
    if (![fm fileExistsAtPath:kFlag]) return JPDaemonStateStopped;

    /* 刚启动的 10 秒宽限：守护进程可能还没写出第一份 status.txt */
    if (self.lastSpawnTime > 0 && CFAbsoluteTimeGetCurrent() - self.lastSpawnTime < 10.0)
        return JPDaemonStateRunning;

    /* status.txt 早于本次开机 = 开机后没跑过（不受 pid 复用影响） */
    NSDate *mtime = [[fm attributesOfItemAtPath:kStatus error:nil] fileModificationDate];
    time_t boot = JPBootTime();
    if (!mtime) return JPDaemonStateDead;
    if (boot > 0 && (time_t)mtime.timeIntervalSince1970 < boot) return JPDaemonStateDead;

    NSString *pidStr = [NSString stringWithContentsOfFile:kPidFile encoding:NSUTF8StringEncoding error:nil];
    pid_t pid = (pid_t)[pidStr intValue];
    if (pid <= 0) return JPDaemonStateDead;
    if (kill(pid, 0) != 0 && errno == ESRCH) return JPDaemonStateDead;
    return JPDaemonStateRunning;   /* 0 或 EPERM（root 进程）都算活着 */
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

// 提交 2 小 2: 停止后自动启动守护进程（复用现有启动流程）
- (void)restartDaemon {
    [self stopDaemon];
    NSString *pidStr = [NSString stringWithContentsOfFile:kPidFile encoding:NSUTF8StringEncoding error:nil];
    pid_t pid = (pid_t)[pidStr intValue];
    if (pid > 0) {
        kill(pid, SIGTERM);
    }
    [NSFileManager.defaultManager removeItemAtPath:kPidFile error:nil];
    __weak typeof(self) weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.3 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        [weakSelf startDaemon];
    });
}

- (void)startDaemon {
    /* 异常未运行时，daemon.pid 里的旧 pid 可能已被别的进程复用，
     * 会骗过守护进程自己的防重复检查，先删掉 */
    if ([self daemonState] == JPDaemonStateDead)
        [NSFileManager.defaultManager removeItemAtPath:kPidFile error:nil];
    [self saveConfig];
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
    if (rc == 0) self.lastSpawnTime = CFAbsoluteTimeGetCurrent();

    if (rc != 0) {
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"启动失败"
                                                                       message:[NSString stringWithFormat:@"posix_spawn 错误: %s", strerror(rc)]
                                                                preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:alert animated:YES completion:nil];
    }
    [self refreshStatus];
}

- (void)stopDaemon {
    [NSFileManager.defaultManager removeItemAtPath:kFlag error:nil];
    [self refreshStatus];
}

// 被杀统计：只在 JetsamEvent 目录或规则变化时才重新解析（签名比对，3 秒轮询不再空转），
// 解析放后台串行队列（队列本身串行，无需 busy 标志），世代计数丢弃过期结果。
- (void)refreshKilledStatsIfNeeded:(BOOL)force {
    if (!self.killedStatsCard) return;

    // 签名 = 当天日期 + JetsamEvent 文件数 + 最新文件名 + 规则数；只列目录，不读文件，开销很小
    // 日期保证半夜无新日志时 7 天窗口也能往前滚（每天最多因此多解析一次）
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
    self.versionWarningBanner.hidden = !(running && [self hasBidRules] && !st.bidSupport);
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

- (void)ruleCardView:(JPRuleCardView *)card didChangeBand:(NSInteger)band {
    card.rule.targetBand = band;
}

- (void)ruleCardViewDidFinishBandChange:(JPRuleCardView *)card {
    [self saveConfig];
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
            // 小问题: 路径匹配仅使用 isEqualToString
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
        [self showAddResultAlert:@"已添加。守护进程没在运行，启动守护进程后才会保护。"];
        return;
    }

    JPStatusInfo *initSt = self.lastLoadedStatus ?: [JPStatusInfo loadCurrentStatus];
    if ([rule.pathFragment hasPrefix:@"bid:"] && (!initSt || !initSt.bidSupport)) {
        [self showAddResultAlert:@"已添加，但守护进程是旧版本，这条规则不会生效。请停止守护进程再启动。"];
        return;
    }

    // 必改 4-3: 降级规则固定提示，不轮询（status.txt 仅记录保护规则）
    if (type == JPRuleTypeDemote) {
        NSString *msg = [NSString stringWithFormat:@"已添加。%@ 不在前台时会降到 30。", name];
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

        if (mtime && [mtime compare:t0] == NSOrderedDescending) {
            // mtime > t0，说明守护进程已在规则保存后写出新的 status.txt
            [t invalidate];
            [strongSelf refreshStatus];
            JPStatusInfo *currentSt = strongSelf.lastLoadedStatus ?: [JPStatusInfo loadCurrentStatus];
            if ([strongSelf status:currentSt hasMatchingAppForRule:rule]) {
                NSString *msg = [NSString stringWithFormat:@"已添加，%@ 正在运行，已立即保护到 %ld。", name, (long)band];
                [strongSelf showAddResultAlert:msg];
            } else {
                NSString *msg = [NSString stringWithFormat:@"已添加。%@ 现在没运行，你打开它时会自动保护（不会替你打开）。", name];
                [strongSelf showAddResultAlert:msg];
            }
            return;
        }

        checksRemaining--;
        if (checksRemaining <= 0) {
            [t invalidate];
            [strongSelf refreshStatus];
            NSString *msg = @"已添加，守护进程还没处理到这条规则，稍后在规则卡片上看状态";
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
    JPAppPickerViewController *picker = [JPAppPickerViewController new];
    picker.allApps = self.installedApps;
    picker.existingRules = self.rules;

    __weak typeof(self) weakSelf = self;
    picker.onSelectApp = ^(NSString *pathFragment, NSString *displayName) {
        JPRule *r = [JPRule new];
        r.type = JPRuleTypeProtect;
        r.pathFragment = pathFragment;
        r.targetBand = 90;
        r.displayName = displayName;
        [weakSelf evaluateRuleConflict:r];
        [weakSelf.rules addObject:r];
        [weakSelf rebuildRulesCards];
        NSDate *t0 = [NSDate date];
        [weakSelf saveConfig];
        [weakSelf checkRuleRunStateAfterAdd:r appName:displayName targetBand:90 type:JPRuleTypeProtect saveTime:t0];
    };

    picker.onManualAdd = ^(NSString *pathFragment, JPRuleType type, NSInteger band) {
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

#pragma mark - App Delegate & Main

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation AppDelegate

- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)opts {
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    MainViewController *vc = [MainViewController new];
    UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:vc];
    self.window.rootViewController = nav;
    [self.window makeKeyAndVisible];
    return YES;
}

@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(AppDelegate.class));
    }
}
