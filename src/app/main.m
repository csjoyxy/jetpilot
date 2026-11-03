/*
 * JetsamPilot — 巨魔 App 界面
 * 编辑规则、以 root 拉起 / 停止常驻进程 jetsampilotd、查看状态。
 *
 * 编译：
 *   xcrun -sdk iphoneos clang -arch arm64 -O2 -fobjc-arc -miphoneos-version-min=15.0 \
 *     -framework UIKit -framework Foundation main.m -o JetsamPilot
 *   ldid -Sentitlements-app.plist JetsamPilot
 */
#import <UIKit/UIKit.h>
#include <spawn.h>
#include <signal.h>
#include <string.h>   /* strerror */
#include <sys/stat.h>

/* TrollStore 拉起 root 子进程用的私有 persona 接口 */
#define POSIX_SPAWN_PERSONA_FLAGS_OVERRIDE 1
extern int posix_spawnattr_set_persona_np(const posix_spawnattr_t *, uid_t, uint32_t);
extern int posix_spawnattr_set_persona_uid_np(const posix_spawnattr_t *, uid_t);
extern int posix_spawnattr_set_persona_gid_np(const posix_spawnattr_t *, uid_t);
extern char **environ;

static NSString *const kDir    = @"/var/mobile/Library/JetsamPilot";
static NSString *const kConfig = @"/var/mobile/Library/JetsamPilot/config.txt";
static NSString *const kFlag   = @"/var/mobile/Library/JetsamPilot/running.flag";
static NSString *const kStatus = @"/var/mobile/Library/JetsamPilot/status.txt";

static NSString *DefaultConfig(void) {
    return @"# JetsamPilot 规则（iOS 16 档位：后台 30，前台 100）\n"
           @"# protect <路径片段> <档位，最高 95>\n"
           @"# demote  <路径片段>   （不在前台时降到 30）\n"
           @"protect Reynard.app/ 90\n";
}

@interface VC : UIViewController
@property (nonatomic, strong) UITextView *editor;
@property (nonatomic, strong) UILabel *status;
@end

@implementation VC

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;

    NSFileManager *fm = NSFileManager.defaultManager;
    [fm createDirectoryAtPath:kDir withIntermediateDirectories:YES
                   attributes:@{NSFilePosixPermissions: @0777} error:nil];
    if (![fm fileExistsAtPath:kConfig]) {
        [DefaultConfig() writeToFile:kConfig atomically:YES encoding:NSUTF8StringEncoding error:nil];
    }

    self.editor = [UITextView new];
    self.editor.font = [UIFont monospacedSystemFontOfSize:14 weight:UIFontWeightRegular];
    self.editor.text = [NSString stringWithContentsOfFile:kConfig encoding:NSUTF8StringEncoding error:nil];
    self.editor.layer.borderWidth = 1;
    self.editor.layer.borderColor = UIColor.separatorColor.CGColor;
    self.editor.autocorrectionType = UITextAutocorrectionTypeNo;
    self.editor.autocapitalizationType = UITextAutocapitalizationTypeNone;

    self.status = [UILabel new];
    self.status.numberOfLines = 0;
    self.status.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];

    UIButton *save  = [self button:@"保存规则" action:@selector(saveConfig)];
    UIButton *start = [self button:@"启动"     action:@selector(startDaemon)];
    UIButton *stop  = [self button:@"停止"     action:@selector(stopDaemon)];
    UIButton *ref   = [self button:@"刷新状态" action:@selector(refreshStatus)];

    UIStackView *buttons = [[UIStackView alloc] initWithArrangedSubviews:@[save, start, stop, ref]];
    buttons.distribution = UIStackViewDistributionFillEqually;
    buttons.spacing = 8;

    UIStackView *root = [[UIStackView alloc] initWithArrangedSubviews:@[self.editor, buttons, self.status]];
    root.axis = UILayoutConstraintAxisVertical;
    root.spacing = 12;
    root.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:root];

    UILayoutGuide *g = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [root.leadingAnchor constraintEqualToAnchor:g.leadingAnchor constant:16],
        [root.trailingAnchor constraintEqualToAnchor:g.trailingAnchor constant:-16],
        [root.topAnchor constraintEqualToAnchor:g.topAnchor constant:16],
        [root.bottomAnchor constraintLessThanOrEqualToAnchor:g.bottomAnchor constant:-16],
        [self.editor.heightAnchor constraintEqualToConstant:260],
        [buttons.heightAnchor constraintEqualToConstant:44],
    ]];
    [self refreshStatus];
}

- (UIButton *)button:(NSString *)title action:(SEL)sel {
    UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
    [b setTitle:title forState:UIControlStateNormal];
    b.backgroundColor = UIColor.secondarySystemBackgroundColor;
    b.layer.cornerRadius = 8;
    [b addTarget:self action:sel forControlEvents:UIControlEventTouchUpInside];
    return b;
}

- (void)saveConfig {
    NSError *err = nil;
    [self.editor.text writeToFile:kConfig atomically:YES encoding:NSUTF8StringEncoding error:&err];
    [self.view endEditing:YES];
    self.status.text = err ? [NSString stringWithFormat:@"保存失败：%@", err.localizedDescription]
                           : @"已保存，常驻进程下一轮（5–10 秒内）生效";
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

    self.status.text = rc == 0 ? [NSString stringWithFormat:@"已启动，pid %d", pid]
                               : [NSString stringWithFormat:@"启动失败：%s", strerror(rc)];
}

- (void)stopDaemon {
    [NSFileManager.defaultManager removeItemAtPath:kFlag error:nil];
    self.status.text = @"已发出停止信号，常驻进程会在 10 秒内退出";
}

- (void)refreshStatus {
    NSString *s = [NSString stringWithContentsOfFile:kStatus encoding:NSUTF8StringEncoding error:nil];
    BOOL flag = [NSFileManager.defaultManager fileExistsAtPath:kFlag];
    self.status.text = [NSString stringWithFormat:@"运行标记：%@\n%@",
                        flag ? @"开" : @"关", s ?: @"（还没有状态，先点「启动」）"];
}
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation AppDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)opts {
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController = [VC new];
    [self.window makeKeyAndVisible];
    return YES;
}
@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(AppDelegate.class));
    }
}
