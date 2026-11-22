# tests/ — 守护进程回归 harness（Linux 侧本地验证）

本目录是批次1（T1/T2/T3）与批次2（E1/E4）关键修复的**可复现回归证据**。四个
主 harness 的被测函数/片段逐字节提取自 `src/jetpilotd.c`（提取锚点见文末），
私有/不可移植 API 用可编程 mock 替换，全部断言可审计。

**stub 头声明**：`notify.h`、`CoreFoundation/`、`dispatch/`、`mach/`、`sys/`、
`jp_apple_compat.h` 仅为 Linux 侧 `gcc -fsyntax-only` 语法校验 `src/jetpilotd.c`
提供的 Darwin 头替身。**它们不参与 iOS App / daemon 的任何构建**（iOS 构建走
theos 真实 SDK，见 `.github/workflows/build-jetpilot.yml`），也不进入 tipa 产物。

## 一、harness 一览与覆盖映射

| 文件 | 对应任务 | 覆盖路径 |
|---|---|---|
| `test_h1_invariants.c` | 批次1 T1 · H1 并发原子性 | pulse spawn/join 守卫与 worker 收尾序列（与源码逐行对应）：running(release/acquire) 权威信号、旧 worker 收尾中绝不 spawn（defer）、句柄永不覆写、running==0 ⇒ 逻辑态已清、defer 时队列条目保留 |
| `test_t2_rwlock.c` | 批次1 T2 · H2 规则数组撕裂读 | 重启阈值读锁片段（锁内只读、锁外判断、单次 unlock）三条路径语义；8 reader × 20000 次三路径轮换 + 2 writer 并发无死锁；结束锁平衡（tryrd/trywr 立即成功） |
| `test_m1_commit.c` | 批次1 T3 · M1 原子写统一 | `jp_atomic_commit` 全行为：成功写入（fsync→rename→目录 fsync）、覆盖、rename 失败（tmp 清理+旧文件完整）、空指针防御、目录 fsync 可用性 |
| `test_e1e4.c` | 批次2 E1/E4（+P2/P3 修复） | A applied 语义回归（set/set_ex/find/prune/remove/creates 计数）；B/B2 快轮漂移矩阵（RAISE/DEMOTE/系统抬档 SKIP/无身份只确认存活）与 sys_band 动作前记录；C/C2 cmd3 立即与**部分进度**异常降级（seen 残留清零、不 prune、下一轮死条目剪除）；D listscan（只遍历已知 pid、get_list 失败→SLEEP）；E 启动探测与 JP_E4_PROBE_FAIL / JP_FAST_SCAN 注入；F SLEEP/LEGACY 纯休眠分发；G P2 恢复路径（慢轮成功回升 LISTSCAN、env 强制除外） |

## 二、负向 / 文档性用例

| 文件 | 性质 | 预期 |
|---|---|---|
| `test_t2_oldbug.c` | **负向用例**（批次1 T2 修复前缺陷复现） | 把 reader 换成旧双解锁变体跑同一并发压力。**运行会挂死（writer 在 wrlock 上永久阻塞）= 预期结果**，证明 T2 harness 具备检出该类 bug 的能力（负向验证：harness 对旧缺陷报挂死、对新代码报通过）。请用 `timeout` 包裹运行 |
| `jp_stale_seen.c` | **文档性用例**（批次2 P2 seen 残留缺陷复现） | 自包含迷你模型（非源码提取）：断言 pre-fix 逻辑下「cmd3 部分进度后异常降级、轮间退出 pid 被 listscan+prune KEPT」的泄漏确实发生，以及 seen 清零后确实剪除。运行通过；生产路径回归在 `test_e1e4.c` 场景 C2 |

## 三、怎么跑（Debian/Ubuntu，gcc ≥ 12；任何有 glibc/pthread 的 Linux 均可）

```bash
cd tests
mkdir -p /tmp/jp_test_bin

# --- 批次1 T1：H1 状态机不变量（普通 + TSan） ---
gcc -O2 -std=gnu11 -Wall -Wextra test_h1_invariants.c -o /tmp/jp_test_bin/h1 -lpthread
/tmp/jp_test_bin/h1
gcc -O1 -g -fsanitize=thread -std=gnu11 test_h1_invariants.c -o /tmp/jp_test_bin/h1t -lpthread
setarch $(uname -m) -R /tmp/jp_test_bin/h1t     # TSan 需关闭 ASLR（vm.mmap_rnd_bits 过高的内核）

# --- 批次1 T2：H2 锁平衡（普通 + TSan） ---
gcc -O2 -std=gnu11 -Wall -Wextra test_t2_rwlock.c -o /tmp/jp_test_bin/t2 -lpthread
/tmp/jp_test_bin/t2
gcc -O1 -g -fsanitize=thread -std=gnu11 test_t2_rwlock.c -o /tmp/jp_test_bin/t2t -lpthread
setarch $(uname -m) -R /tmp/jp_test_bin/t2t     # 退出码 0 且无 ThreadSanitizer 警告

# --- 批次1 T2 负向用例（预期挂死，用 timeout 包裹） ---
gcc -O2 -std=gnu11 test_t2_oldbug.c -o /tmp/jp_test_bin/t2old -lpthread
timeout 20 /tmp/jp_test_bin/t2old; echo "exit=$?"   # exit=124（超时杀）= 预期

# --- 批次1 T3：M1 提交助手 ---
gcc -O2 -std=gnu11 -Wall -Wextra test_m1_commit.c -o /tmp/jp_test_bin/m1
/tmp/jp_test_bin/m1

# --- 批次2 E1/E4：applied/快轮/兜底链/恢复 ---
gcc -O2 -std=gnu11 -Wall -Wextra -Wno-unused-variable -Wno-unused-function \
    test_e1e4.c -o /tmp/jp_test_bin/e1e4
/tmp/jp_test_bin/e1e4     # 九场景 A/B/B2/C/C2/D/E/F/G 全过
# （-Wno-unused-*：E1 块里主循环在用的全局在 harness 内不可达，属提取边界，非告警信号）

# --- 批次2 P2 文档性用例 ---
gcc -O2 -std=gnu11 -Wall -Wextra jp_stale_seen.c -o /tmp/jp_test_bin/stale
/tmp/jp_test_bin/stale
```

全部通过的判据：每个程序打印末行 `ALL ... OK` / 明确 OK 行，退出码 0；
TSan 运行除程序自身输出外无任何 `WARNING: ThreadSanitizer` 行。

### src/jetpilotd.c 语法校验（stub 头的用途）

```bash
# 方式一：-include 补 sysctlbyname 等（与 tests/ 同目录的 stub）
gcc -fsyntax-only -std=gnu11 -Wall -Wextra -Itests \
    -include tests/jp_apple_compat.h src/jetpilotd.c
# 方式二：仅 -I（sysctl.h 已含 sysctlbyname）
gcc -fsyntax-only -std=gnu11 -Wall -Wextra -Itests src/jetpilotd.c
```

两种方式均应零输出、退出码 0。

## 四、快照性质与再生成

`test_t2_rwlock.c` / `test_m1_commit.c` / `test_e1e4.c` 的被测片段是从
`src/jetpilotd.c` 逐字节提取的**快照**（用 Python 按下列锚点切片拼接，
mock 段手写）：

| 提取物 | 起始锚点 | 结束锚点 |
|---|---|---|
| test_t2_rwlock | `/* M1: 提交 tmp 内容到 final`（jp_atomic_commit 不需要）→ 重启阈值段 | `/* H2: 重启规则数组读锁` … `if (out_fp) *out_fp = cur_fp;` |
| test_m1_commit | `/* M1: 提交 tmp 内容到 final` | `static void write_status(` |
| test_e1e4 | `#define BAND_IDLE`…`extern int memorystatus_control`；`#define MAX_APPLIED 64`…`#define MAX_OLD_TARGETS`；`proc_label`；`E1 快慢双节奏` 头块 | 各自片段结束标记 |

源码后续改动若触及这些片段，请按相同锚点重新生成快照后再跑——harness 测的是
快照，**不会**随源码漂移自动失败，这是已知限制；互审时请对照锚点确认快照新鲜度。

## 五、与构建/打包的关系（已确认）

- `.github/workflows/build-jetpilot.yml`：打包只取 `src/`（编译产物 + Payload），
  `tests/` 不进 tipa。
- `scripts/check-release.sh`：禁词扫描仅 `src/`，版本/签名检查针对
  `src/app/Info.plist` 与构建产物；`tests/` 不影响任何检查项。
- `scripts/verify-artifacts.py`：验证 tipa 内容，与 `tests/` 无交互。
