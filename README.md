# JetPilot

原名 JetsamPilot，v0.6.0 起改名。从旧版升级请看下方《从 JetsamPilot 升级》。

TrollStore 应用 + root 常驻进程：周期性把受保护 App 的 Jetsam 档位抬到目标值，让系统在内存紧张时先杀别的进程。默认规则把 Reynard 保在 90 档。

目标设备：iPhone 11 / iOS 16.4.1（档位数值自 iOS 16 起整体 ×10：后台 30，前台 100）。

## 仓库结构

```
src/
  app/main.m                # 巨魔 App 界面：编辑规则、启动/停止常驻进程、看状态
  app/Info.plist            # App 信息（com.lili.jetpilot）
  jetpilotd.c               # 常驻进程：每 5–10 秒按规则重设 jetsam 档位
  entitlements-app.plist    # App 权限（persona-mgmt 拉起 root 子进程）
  entitlements-daemon.plist # 守护进程权限（memorystatus / jetsam.modify-priority）
.github/workflows/
  build-jetpilot.yml        # Actions：macos-latest 编译 → 打包 .tipa → 上传产物
```

## 开发者流程

1. 从 `origin/main` 开 `review/<版本>` 分支改代码，用推送脚本推到 GitHub；
2. CI 出包，真机验证通过；
3. `PATCH git/refs/heads/main`（`force: false`，只允许快进）把 main 快进到验证过的提交，再经 API 在同一提交建 tag `v<版本>`；
4. Actions 自动把 JetPilot.tipa 挂到 Release；
5. 手机上从 Release 下载 .tipa，用 TrollStore 安装，打开点「启动」。

## 规则配置

App 内「保存规则」写入 `/var/mobile/Library/JetPilot/config.txt`，常驻进程 5–10 秒内生效：

```
# protect <路径片段 或 bid:Bundle ID> <档位，最高 95>
# demote  <路径片段>   （不在前台时降到 30）
protect Reynard.app/ 90
protect bid:<目标 App 的 Bundle ID> 90
```

Bundle ID 可在 Filza 里打开该 App 的 Info.plist，看 CFBundleIdentifier。

守护进程只碰 `/var/containers/Bundle/Application/` 下的用户 App，绝不动系统进程；档位封顶 95，永不触及前台 100。

## 从 JetsamPilot 升级

1. 在 TrollStore 删除 JetsamPilot；
2. 安装 JetPilot.tipa，打开后重新添加规则并启动。

新版是新的 Bundle ID 和配置目录，不导入旧规则。
删除旧 App 不会删掉 `/var/mobile/Library/JetsamPilot/`，留着不影响新版，想清理可用 Filza 删除。

## 版本历史

- **v0.7.1**（2026-11-06）：只读记录目标应用内存限额诊断。
- **v0.7.0**（2026-11-06）：支持前台 App 内存压力触发，由 mempressure.txt 独立配置。
- **v0.6.0**（2026-11-05）：改名 JetPilot（Bundle ID、配置目录变更，需删除旧版重装，规则需重新添加）。
- **v0.5.6**（2026-11-05）：规则按 Bundle ID 精确匹配，避免误伤同名应用；支持规则编辑与自动迁移。
- **v0.5.5**（2026-11-04）：状态文件按需写入，更省电；守护进程未运行时顶部红条提醒，一键启动；受保护进程标出主程序或扩展；新增调试日志。
- **v0.5.4**（2026-11-04）：重启后不再误调系统给的档位；扩展进程的档位也会随规则下调；保护中状态显示更准确。
- **v0.5.3**（2026-11-04）：重启/升级后可降级旧档位；保护中三态显示；关于页加版本号与仓库链接。
- **v0.5.2**（2026-11-04）：保护档位可下调；删除规则后档位自动恢复。
- **v0.5.1**（2026-11-03）：提权失败显示错误文本；status.txt 原子写入；被杀统计放后台线程解析；删掉进程名反向匹配。
- **v0.5.0**（2026-11-03）：被杀统计；提权失败原因红字显示。
- **v0.4.0**（2026-11-03）：「保护中」可点开看名单；锁屏省电改事件驱动。
- **v0.3.0**（2026-11-03）：锁屏后降低轮询频率省电。
- **v0.2.0**（2026-11-03）：UI 重做；新增 App 图标。
- **v0.1.0**（2026-11-03）：首版。

## 已知未做

- 锁屏省电逻辑（state=1=锁屏）按社区常用语义实现，未在真机逐行核实；若行为反了，看 README 版本历史回退。
- 守护进程部分权限需求按 XNU 一般规则推断，未在 16.4.1 上逐行核实；若 `memorystatus_control` 报 EPERM，看守护进程 stderr 排查。
