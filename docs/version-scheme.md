# JetPilot 版本号单一事实源方案

定稿：2026-11-22。适用：v0.8.1 及其后所有发版。本文是版本号方案的权威说明；具体代码改动由 VER2（jpengineer）按本文结论落地，架构师（VER1）只出方案与文档、不改码。

## 1. 三处版本字段与 v0.8.1 规范值

| 字段 | 位置 (file:line) | v0.8.1 值 | 角色 |
|---|---|---|---|
| `CFBundleShortVersionString` | `src/app/Info.plist:10` | `0.8.1` | App 展示版本 / tag `v<版本>` 的依据 |
| `CFBundleVersion` | `src/app/Info.plist:11` | `25` | build 号，正整数，逐发版 +1 |
| `JP_VERSION` | `src/jetpilotd.c:219` | `"0.8.1"` | 守护进程自报版本（status / 启动日志） |

约束（单一事实源的三条不变量）：

1. 两个版本**字符串**必须逐字节相等：`ShortVersion == JP_VERSION`（`0.8.1`）。
2. `CFBundleVersion` 必须是正整数（正则 `^[1-9][0-9]*$`），每发版单调递增：v0.8.0 = `24` → v0.8.1 = `25`。
3. 版本三元组由 VER1 架构师**定**，VER2 工程师**只改上述三行**，任何其它版本字样（代码注释、UI 兜底串）都不是事实源，不得据此改版本号。

## 2. 门禁如何强制（单一事实源的校验端）

`scripts/check-release.sh` 的 Check 1（`scripts/check-release.sh:62-107`）是唯一校验点：

- 用 `python3 plistlib` 读 `Info.plist` 两字段（无 python3 时 `sed` 兜底）；
- 用 `sed` 抽 `#define JP_VERSION "..."`；
- `short_ver != jp_ver` → `[FAIL] Version mismatch`；`bundle_ver` 非正整数 → `[FAIL]`；
- 任一 FAIL 置 `FAILED=1`，脚本结尾 `exit 1`，CI 变红。

即：三处只要有一处没同步到同一个值，门禁必挂。不存在第二套权威版本来源；tag 名只取 `ShortVersion`。

## 3. 每版更新流程

1. **VER1（架构师）**：定版本三元组，写本方案 + README 第 10 节版本历史；**不改码**。
2. **VER2（工程师）**：只改 `Info.plist:10` / `:11` 与 `jetpilotd.c:219`，本地跑 `bash scripts/check-release.sh --skip-artifact`，Check 1 三处一致且全门禁 `exit 0`，再送审。
3. **审核 / 合入**：review 分支 CI 出包 → 真机验收 PASS → lead 在 REL1 合 main 打 tag `v<ShortVersion>`。
4. **硬约束**：VER1/VER2 不 push、不打 tag、不动 main；只有 lead 在 REL1 执行发版动作。

## 4. v0.8.1 落地核对（2026-11-22）

- 定稿时源码仍为 `0.8.0 / 24 / 0.8.0`（三处已互相一致，Check 1 通过但为旧值）。
- 目标值 `0.8.1 / 25 / 0.8.1` 已在隔离副本中验证：套用后 `bash scripts/check-release.sh --skip-artifact` 的 Check 1 显示 `0.8.1 / 25 / 0.8.1`，`[OK] Version strings match` + `[OK] CFBundleVersion is a positive integer`，全门禁 `exit 0`。
- 实际改码由 VER2（t_2824b061）在 `review/v0.8.1-integrate`（HEAD `339104c`）执行；本方案未触碰任何源码文件。
