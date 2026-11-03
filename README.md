# JetsamPilot

TrollStore 应用 + root 常驻进程：周期性把受保护 App 的 Jetsam 档位抬到目标值，让系统在内存紧张时先杀别的进程。默认规则把 Reynard 保在 90 档。

目标设备：iPhone 11 / iOS 16.4.1（档位数值自 iOS 16 起整体 ×10：后台 30，前台 100）。

## 仓库结构

```
src/
  app/main.m                # 巨魔 App 界面：编辑规则、启动/停止常驻进程、看状态
  app/Info.plist            # App 信息（com.lili.jetsampilot）
  jetsampilotd.c            # 常驻进程：每 5–10 秒按规则重设 jetsam 档位
  entitlements-app.plist    # App 权限（persona-mgmt 拉起 root 子进程）
  entitlements-daemon.plist # 守护进程权限（memorystatus / jetsam.modify-priority）
.github/workflows/
  build-jetsampilot.yml     # Actions：macos-latest 编译 → 打包 .tipa → 上传产物
```

## 开发者流程

1. 改代码后直接 `push` 到 `main`（或在 Actions 页手动 Run workflow）。
2. `build-jetsampilot` 自动跑：编译守护进程 → 编译 App → `ldid` 签名 → 打包 `JetsamPilot.tipa` → 上传到 Artifacts（保留 90 天）。
3. 发版：`git tag v0.x.y && git push origin v0.x.y`（先 push 代码再打 tag）→ Actions 自动把 `JetsamPilot.tipa` 挂到对应 Release 下，仓库首页 Releases 栏直接下载，无需手动传。
4. 手机上从 Release 下载 `.tipa`，用 TrollStore 安装，打开点「启动」。
5. 版本历史见下方。

## 规则配置

App 内「保存规则」写入 `/var/mobile/Library/JetsamPilot/config.txt`，常驻进程 5–10 秒内生效：

```
# protect <路径片段> <档位，最高 95>
# demote  <路径片段>   （不在前台时降到 30）
protect Reynard.app/ 90
```

守护进程只碰 `/var/containers/Bundle/Application/` 下的用户 App，绝不动系统进程；档位封顶 95，永不触及前台 100。

## 版本历史

- **v0.5.0**（2026-11-03）：被杀统计；提权失败原因红字显示。
- **v0.4.0**（2026-11-03）：「保护中」可点开看名单；锁屏省电改事件驱动。
- **v0.3.0**（2026-11-03）：锁屏后降低轮询频率省电。
- **v0.2.0**（2026-11-03）：UI 重做；新增 App 图标。
- **v0.1.0**（2026-11-03）：首版。

## 已知未做

- 锁屏省电逻辑（state=1=锁屏）按社区常用语义实现，未在真机逐行核实；若行为反了，看 README 版本历史回退。
- 守护进程部分权限需求按 XNU 一般规则推断，未在 16.4.1 上逐行核实；若 `memorystatus_control` 报 EPERM，看守护进程 stderr 排查。
