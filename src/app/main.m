/*
 * JetsamPilot — iOS 15+ TrollStore 守护进程管理与规则编辑
 * 单文件 UIKit 纯代码实现，深色模式自适应，Auto Layout 布局。
 *
 * 编译命令：
 *   xcrun -sdk iphoneos clang -arch arm64 -O2 -Wall -fobjc-arc \
 *     -miphoneos-version-min=15.0 -framework UIKit -framework Foundation \
 *     app/main.m -o JetsamPilot
 *   ldid -Sentitlements-app.plist JetsamPilot
 */

#import <UIKit/UIKit.h>
#include <spawn.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#define POSIX_SPAWN_PERSONA_FLAGS_OVERRIDE 1
extern int posix_spawnattr_set_persona_np(const posix_spawnattr_t *, uid_t, uint32_t);
extern int posix_spawnattr_set_persona_uid_np(const posix_spawnattr_t *, uid_t);
extern int posix_spawnattr_set_persona_gid_np(const posix_spawnattr_t *, uid_t);
extern char **environ;

static NSString *const kDir     = @"/var/mobile/Library/JetsamPilot";
static NSString *const kConfig  = @"/var/mobile/Library/JetsamPilot/config.txt";
static NSString *const kFlag    = @"/var/mobile/Library/JetsamPilot/running.flag";
static NSString *const kStatus  = @"/var/mobile/Library/JetsamPilot/status.txt";
static NSString *const kPidFile = @"/var/mobile/Library/JetsamPilot/daemon.pid";

static NSString *DefaultConfig(void) {
    return @"# JetsamPilot 规则（iOS 16 档位：后台 30，前台 100）\n"
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
@end

@implementation JPRule
@end

@interface JPStatusInfo : NSObject
@property (nonatomic, copy) NSString *updated;
@property (nonatomic, assign) NSInteger protectRules;
@property (nonatomic, assign) NSInteger demoteRules;
@property (nonatomic, assign) NSInteger protectedPids;
@property (nonatomic, assign) NSInteger demotedPids;
@property (nonatomic, assign) unsigned long reassertsTotal;
+ (JPStatusInfo *)loadCurrentStatus;
@end

@implementation JPStatusInfo
+ (JPStatusInfo *)loadCurrentStatus {
    JPStatusInfo *st = [JPStatusInfo new];
    st.updated = @"";
    NSString *raw = [NSString stringWithContentsOfFile:kStatus encoding:NSUTF8StringEncoding error:nil];
    if (!raw || raw.length == 0) return st;

    NSArray<NSString *> *lines = [raw componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]];
    for (NSString *line in lines) {
        if ([line hasPrefix:@"updated="]) {
            st.updated = [line substringFromIndex:8];
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
        }
    }
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

#pragma mark - Views: Status Card

@interface JPStatusCardView : UIView
@property (nonatomic, strong) UIView *statusDot;
@property (nonatomic, strong) UILabel *statusTitleLabel;
@property (nonatomic, strong) UILabel *updatedLabel;
@property (nonatomic, strong) UILabel *protectedCountLabel;
@property (nonatomic, strong) UILabel *demotedCountLabel;
@property (nonatomic, strong) UILabel *reassertCountLabel;
@property (nonatomic, strong) UIButton *actionButton;
@property (nonatomic, copy) void (^onActionToggle)(void);

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
    UIView *col1 = [self createColumnWithTitle:@"保护中进程" numberLabel:&pLabel tintColor:UIColor.systemGreenColor];
    self.protectedCountLabel = pLabel;

    UILabel *dLabel = nil;
    UIView *col2 = [self createColumnWithTitle:@"降级中进程" numberLabel:&dLabel tintColor:UIColor.systemOrangeColor];
    self.demotedCountLabel = dLabel;

    UILabel *rLabel = nil;
    UIView *col3 = [self createColumnWithTitle:@"累计生效" numberLabel:&rLabel tintColor:UIColor.systemBlueColor];
    self.reassertCountLabel = rLabel;

    UIStackView *metricsStack = [[UIStackView alloc] initWithArrangedSubviews:@[col1, col2, col3]];
    metricsStack.translatesAutoresizingMaskIntoConstraints = NO;
    metricsStack.axis = UILayoutConstraintAxisHorizontal;
    metricsStack.distribution = UIStackViewDistributionFillEqually;
    metricsStack.alignment = UIStackViewAlignmentCenter;

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
    [self addSubview:metricsStack];
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

        [metricsStack.topAnchor constraintEqualToAnchor:div1.bottomAnchor constant:12],
        [metricsStack.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:16],
        [metricsStack.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-16],

        [div2.topAnchor constraintEqualToAnchor:metricsStack.bottomAnchor constant:12],
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

- (UIView *)createColumnWithTitle:(NSString *)title numberLabel:(UILabel **)labelOut tintColor:(UIColor *)tint {
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
    [box addSubview:txt];

    [NSLayoutConstraint activateConstraints:@[
        [num.topAnchor constraintEqualToAnchor:box.topAnchor],
        [num.leadingAnchor constraintEqualToAnchor:box.leadingAnchor],
        [num.trailingAnchor constraintEqualToAnchor:box.trailingAnchor],

        [txt.topAnchor constraintEqualToAnchor:num.bottomAnchor constant:4],
        [txt.leadingAnchor constraintEqualToAnchor:box.leadingAnchor],
        [txt.trailingAnchor constraintEqualToAnchor:box.trailingAnchor],
        [txt.bottomAnchor constraintEqualToAnchor:box.bottomAnchor]
    ]];

    if (labelOut) *labelOut = num;
    return box;
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
}

@end

#pragma mark - Views: Rule Card

@class JPRuleCardView;

@protocol JPRuleCardViewDelegate <NSObject>
- (void)ruleCardViewDidRequestDelete:(JPRuleCardView *)card;
- (void)ruleCardView:(JPRuleCardView *)card didChangeBand:(NSInteger)band;
- (void)ruleCardViewDidFinishBandChange:(JPRuleCardView *)card;
@end

@interface JPRuleCardView : UIView
@property (nonatomic, strong) JPRule *rule;
@property (nonatomic, weak) id<JPRuleCardViewDelegate> delegate;
@property (nonatomic, strong) UILabel *titleLabel;
@property (nonatomic, strong) UILabel *pathLabel;
@property (nonatomic, strong) UILabel *bandLabel;
@property (nonatomic, strong) UISlider *slider;
@property (nonatomic, strong) UIButton *deleteButton;

- (instancetype)initWithRule:(JPRule *)rule delegate:(id<JPRuleCardViewDelegate>)delegate;
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

    self.deleteButton = [UIButton buttonWithType:UIButtonTypeSystem];
    self.deleteButton.translatesAutoresizingMaskIntoConstraints = NO;
    [self.deleteButton setImage:[UIImage systemImageNamed:@"trash"] forState:UIControlStateNormal];
    self.deleteButton.tintColor = UIColor.systemRedColor;
    [self.deleteButton addTarget:self action:@selector(deleteTapped) forControlEvents:UIControlEventTouchUpInside];

    [self addSubview:self.titleLabel];
    [self addSubview:self.pathLabel];
    [self addSubview:self.deleteButton];

    [NSLayoutConstraint activateConstraints:@[
        [self.deleteButton.topAnchor constraintEqualToAnchor:self.topAnchor constant:12],
        [self.deleteButton.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-12],
        [self.deleteButton.widthAnchor constraintEqualToConstant:36],
        [self.deleteButton.heightAnchor constraintEqualToConstant:36],

        [self.titleLabel.topAnchor constraintEqualToAnchor:self.topAnchor constant:14],
        [self.titleLabel.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:16],
        [self.titleLabel.trailingAnchor constraintLessThanOrEqualToAnchor:self.deleteButton.leadingAnchor constant:-8],

        [self.pathLabel.topAnchor constraintEqualToAnchor:self.titleLabel.bottomAnchor constant:3],
        [self.pathLabel.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:16],
        [self.pathLabel.trailingAnchor constraintLessThanOrEqualToAnchor:self.deleteButton.leadingAnchor constant:-8]
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
            [self.bandLabel.topAnchor constraintEqualToAnchor:self.pathLabel.bottomAnchor constant:12],
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
            [badge.topAnchor constraintEqualToAnchor:self.pathLabel.bottomAnchor constant:12],
            [badge.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:16],
            [badge.bottomAnchor constraintEqualToAnchor:self.bottomAnchor constant:-14]
        ]];
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
        if ([r.pathFragment isEqualToString:app.pathFragment] ||
            [r.pathFragment containsString:app.bundleName] ||
            [app.bundleName containsString:r.pathFragment]) {
            return YES;
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

    if (self.onSelectApp) {
        self.onSelectApp(app.pathFragment, app.displayName);
    }
    [self dismissViewControllerAnimated:YES completion:nil];
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
        if (weakSelf.onManualAdd) {
            weakSelf.onManualAdd(text, JPRuleTypeProtect, 90);
        }
        [weakSelf dismissViewControllerAnimated:YES completion:nil];
    }]];

    [alert addAction:[UIAlertAction actionWithTitle:@"添加为降级 (30 档)" style:UIAlertActionStyleDefault handler:^(UIAlertAction * _Nonnull action) {
        NSString *text = [weakAlert.textFields.firstObject.text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        if (text.length == 0) return;
        if (![text containsString:@"/"] && ![text hasSuffix:@".app"]) {
            text = [text stringByAppendingString:@".app/"];
        }
        if (weakSelf.onManualAdd) {
            weakSelf.onManualAdd(text, JPRuleTypeDemote, 30);
        }
        [weakSelf dismissViewControllerAnimated:YES completion:nil];
    }]];

    [alert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

@end

#pragma mark - Controllers: Main ViewController

@interface MainViewController : UIViewController <JPRuleCardViewDelegate>
@property (nonatomic, strong) NSMutableArray<JPRule *> *rules;
@property (nonatomic, copy) NSArray<JPInstalledApp *> *installedApps;

@property (nonatomic, strong) UIScrollView *scrollView;
@property (nonatomic, strong) UIStackView *contentStackView;

@property (nonatomic, strong) JPStatusCardView *statusCard;
@property (nonatomic, strong) UILabel *rulesSectionHeaderLabel;
@property (nonatomic, strong) UIStackView *rulesStackView;
@property (nonatomic, strong) UIView *addRuleCard;

@property (nonatomic, strong) UIView *saveNoticeToast;
@property (nonatomic, strong) NSTimer *saveNoticeTimer;
@property (nonatomic, strong) NSTimer *pollingTimer;
@end

@implementation MainViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"JetsamPilot";
    self.view.backgroundColor = UIColor.systemGroupedBackgroundColor;
    self.navigationController.navigationBar.prefersLargeTitles = YES;

    self.rules = [NSMutableArray array];

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

    // 1. Status Section
    UILabel *statusHeader = [self createSectionHeader:@"运行状态"];
    self.statusCard = [JPStatusCardView new];
    __weak typeof(self) weakSelf = self;
    self.statusCard.onActionToggle = ^{
        [weakSelf toggleDaemon];
    };

    [self.contentStackView addArrangedSubview:statusHeader];
    [self.contentStackView addArrangedSubview:self.statusCard];

    // 2. Rules Section
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

    // 4. Footer Note
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
    if (!frag || frag.length == 0) return @"";
    for (JPInstalledApp *app in self.installedApps) {
        if ([frag containsString:app.bundleName] || [app.bundleName containsString:frag]) {
            return app.displayName;
        }
    }
    return frag;
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
            [self.rules addObject:r];
        } else if ([verb isEqualToString:@"demote"]) {
            JPRule *r = [JPRule new];
            r.type = JPRuleTypeDemote;
            r.pathFragment = frag;
            r.targetBand = 30;
            r.displayName = [self resolveDisplayNameForFragment:frag];
            [self.rules addObject:r];
        }
    }
}

- (void)saveConfig {
    NSMutableString *outStr = [NSMutableString string];
    [outStr appendString:@"# JetsamPilot 规则（iOS 16 档位：后台 30，前台 100）\n"];
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
    if (ok) {
        [self showSaveNotice];
    }
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
        for (JPRule *rule in self.rules) {
            JPRuleCardView *card = [[JPRuleCardView alloc] initWithRule:rule delegate:self];
            [self.rulesStackView addArrangedSubview:card];
        }
    }
    self.rulesSectionHeaderLabel.text = [NSString stringWithFormat:@"保护规则 (%lu)", (unsigned long)self.rules.count];
}

#pragma mark - Daemon Control

- (BOOL)isDaemonRunning {
    NSFileManager *fm = NSFileManager.defaultManager;
    if (![fm fileExistsAtPath:kFlag]) {
        return NO;
    }
    NSString *pidStr = [NSString stringWithContentsOfFile:kPidFile encoding:NSUTF8StringEncoding error:nil];
    if (pidStr && pidStr.length > 0) {
        pid_t pid = (pid_t)[pidStr intValue];
        if (pid > 0) {
            if (kill(pid, 0) != 0 && errno == ESRCH) {
                return NO;
            }
        }
    }
    return YES;
}

- (void)toggleDaemon {
    if ([self isDaemonRunning]) {
        [self stopDaemon];
    } else {
        [self startDaemon];
    }
}

- (void)startDaemon {
    [self saveConfig];
    [@"run" writeToFile:kFlag atomically:YES encoding:NSUTF8StringEncoding error:nil];

    NSString *bin = [NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:@"jetsampilotd"];
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

- (void)refreshStatus {
    BOOL running = [self isDaemonRunning];
    JPStatusInfo *st = [JPStatusInfo loadCurrentStatus];
    [self.statusCard updateWithStatus:st isRunning:running];
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

#pragma mark - Add Rule Action

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
        [weakSelf.rules addObject:r];
        [weakSelf rebuildRulesCards];
        [weakSelf saveConfig];
    };

    picker.onManualAdd = ^(NSString *pathFragment, JPRuleType type, NSInteger band) {
        JPRule *r = [JPRule new];
        r.type = type;
        r.pathFragment = pathFragment;
        r.targetBand = band;
        r.displayName = [weakSelf resolveDisplayNameForFragment:pathFragment];
        [weakSelf.rules addObject:r];
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
