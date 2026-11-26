/*
 * test_v083.c — v0.8.3 返工项的逻辑模型回归（自包含，非源码逐字提取）。
 *
 * 与生产代码对应分支同构，锚点：
 *   - Ltarget 窗口守卫            src/jetpilotd.c: PULSE_LTARGET_GROWTH_CAP_MB / 窗口 for 循环
 *   - self_written 魔数可信恢复    pulse_saved_state_t / pulse_load_trusted_state_file / pulse_do_restore
 *   - delay_s=0 判定               pulse_check_candidate 条件 2
 *   - 按 bid 恢复熔断              pulse_note_restore_failed / PULSE_RESTORE_CIRCUIT_N
 *   - 门控快轮开合                pulse_fast_gate_open
 *
 * 这是模型测试，不替代真机验收；与 jp_stale_seen.c 同属自包含行为模型。
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", (msg)); return 1; } } while (0)

/* ---------- 1. XNU [80%,100%) 黄金警告带与 95%×L 越界前守卫 ---------- */
#define PULSE_GUARD_PCT 95

static inline uint64_t calc_hard_guard_limit(uint64_t fp_mb) {
    return (fp_mb * 11 + 9) / 10;   /* ceil(fp * 1.10) */
}

static inline uint64_t calc_pulse_limit(uint64_t fp_mb) {
    uint64_t L = (fp_mb * 100 + 84) / 85; /* ceil(fp / 0.85) */
    uint64_t g = calc_hard_guard_limit(fp_mb);
    return (L < g) ? g : L;
}

typedef struct {
    const char *end;
    int  t_reach;
    int  early_abort_assert;
} win_t;

/* 与 jetpilotd.c 窗口内守卫同构：95%×L 越界前恢复 + target 收敛 + 60s 定时。 */
static win_t window_step(uint64_t fp0, uint64_t target, uint64_t L, uint64_t cur, int sec) {
    (void)fp0;
    win_t r = { "CONTINUE", 0, 0 };
    if (cur * 100 >= (uint64_t)PULSE_GUARD_PCT * L) {
        if (sec <= 1) r.early_abort_assert = 1;
        r.end = "ABORT"; r.t_reach = sec;
        return r;
    }
    if (cur <= target) { r.end = "TARGET_REACHED"; r.t_reach = sec; return r; }
    if (sec == 60) { r.end = "WINDOW_END"; return r; }
    return r;
}

static int test_window(void) {
    /* 负向对照：旧 L=target (400 < fp0=537) 直接使 fp0 >= L (limit_exceeded=TRUE)，
     * 错失 UIKit PRESSURE_WARN/CRITICAL 且暴露于内核 highwater 击杀 */
    CHECK(537 > 400, "negative control: L=target(400) puts fp0(537) into limit_exceeded highwater zone");

    /* 验证全量典型占用 (101 / 441 / 537 / 713 MB) 均满足：
     * 1) 80%×L < fp0 < 100%×L（命中 XNU !limit_exceeded UIKit 警告带且免疫 highwater）
     * 2) fp0 < 95%×L（恒定占用绝不在首秒误触发 ABORT，稳态跑满 60s） */
    const uint64_t samples[] = { 101, 441, 537, 713 };
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        uint64_t fp0 = samples[i];
        uint64_t L = calc_pulse_limit(fp0);
        CHECK(fp0 * 100 > 80 * L, "fp0 must exceed 80%*L warning threshold to trigger UIKit warning");
        CHECK(fp0 < L, "fp0 must stay strictly below 100%*L to prevent XNU highwater kill");
        CHECK(fp0 * 100 < (uint64_t)PULSE_GUARD_PCT * L, "fp0 must stay below 95%*L guard");
    }

    uint64_t L537 = calc_pulse_limit(537); /* 632 MB */
    for (int sec = 1; sec <= 59; sec++) {
        win_t r = window_step(537, 400, L537, 537, sec);
        CHECK(strcmp(r.end, "CONTINUE") == 0 && r.early_abort_assert == 0,
              "steady fp0=537 must run smoothly without early abort");
    }
    win_t last = window_step(537, 400, L537, 537, 60);
    CHECK(strcmp(last.end, "WINDOW_END") == 0, "60s timed restore still active");

    /* TARGET_REACHED：占用回落至 target 以下提前恢复 */
    win_t reached = window_step(537, 400, L537, 380, 10);
    CHECK(strcmp(reached.end, "TARGET_REACHED") == 0 && reached.t_reach == 10,
          "TARGET_REACHED when cur_fp <= target");

    /* 95%×L 越界前保护：632*0.95 = 600.4 -> 涨到 601 MB (< 632 MB) 时立即提前恢复，
     * 绝不等涨穿 632 MB 被内核 highwater 点杀 */
    win_t guard95 = window_step(537, 400, L537, 601, 8);
    CHECK(601 < L537 && strcmp(guard95.end, "ABORT") == 0 && guard95.t_reach == 8,
          "95%*L guard aborts and restores BEFORE reaching 100%*L highwater limit");
    printf("[1] XNU [80%%,100%%) warning-band & 95%%*L pre-highwater guard OK\n");
    return 0;
}

/* ---------- 2. self_written 魔数与非法值拒绝 ---------- */
#define FMT_MAGIC 0x4A505331U
#define RESTORE_FLOOR(fp) (((fp) * 12 + 9) / 10)  /* ceil(fp*1.2) */

typedef struct {
    uint32_t magic;
    int      self_written;
    int32_t  orig_inactive;
    uint64_t fp;
} snapshot_t;

static int snapshot_is_trusted(const snapshot_t *s) {
    if (s->magic != FMT_MAGIC) return 0;
    if (s->self_written != 0 && s->self_written != 1) return 0;
    if (!s->self_written) return 0;
    return s->orig_inactive >= 0;
}

/* 返回 1 表示按写前快照原样恢复；0 表示回退 active 镜像 */
static int restore_uses_snapshot(const snapshot_t *s) {
    return snapshot_is_trusted(s);
}

static int test_self_written(void) {
    snapshot_t good = { FMT_MAGIC, 1, 0, 713 };
    /* 旧 fp 合理性判定会把 orig_inactive=0 < ceil(713*1.2) 误判不可信；
     * 魔数+self_written 合法时应强制原样恢复。 */
    CHECK((uint64_t)good.orig_inactive < RESTORE_FLOOR(good.fp), "precondition: old heuristic would reject");
    CHECK(restore_uses_snapshot(&good) == 1, "valid self_written snapshot restored verbatim");

    snapshot_t no_magic = { 0, 1, 400, 713 };
    CHECK(restore_uses_snapshot(&no_magic) == 0, "missing magic -> not trusted");

    snapshot_t bad_magic = { 0xDEADBEEFU, 1, 400, 713 };
    CHECK(restore_uses_snapshot(&bad_magic) == 0, "wrong magic -> not trusted");

    snapshot_t bad_flag = { FMT_MAGIC, 2, 400, 713 };
    CHECK(restore_uses_snapshot(&bad_flag) == 0, "illegal self_written value -> not trusted");

    snapshot_t neg_inact = { FMT_MAGIC, 1, -1, 713 };
    CHECK(restore_uses_snapshot(&neg_inact) == 0, "negative orig_inactive -> not trusted");

    snapshot_t old = { FMT_MAGIC, 0, 400, 713 };
    CHECK(restore_uses_snapshot(&old) == 0, "self_written=0 -> old plausibility path");
    printf("[2] self_written magic / illegal-value rejection OK\n");
    return 0;
}

/* ---------- 3. delay_s=0 ---------- */
static int candidate_delay_ok(int delay_s, time_t elapsed) {
    return elapsed >= (time_t)delay_s;
}

static int test_delay_zero(void) {
    CHECK(candidate_delay_ok(0, 0) == 1, "delay_s=0 eligible immediately on entering bg");
    CHECK(candidate_delay_ok(3, 0) == 0, "delay_s=3 not eligible at t=0");
    CHECK(candidate_delay_ok(3, 3) == 1, "delay_s=3 eligible at t=3");
    CHECK(candidate_delay_ok(20, 19) == 0, "upper bound 20 still enforced");
    printf("[3] delay_s=0 eligibility OK\n");
    return 0;
}

/* ---------- 4. 按 bid 的恢复失败熔断 streak ---------- */
#define CIRCUIT_N 3
typedef struct { char bid[64]; int streak; int tripped; } guard_t;
static guard_t guards[8];
static int n_guards = 0;

static guard_t *guard_get(const char *bid) {
    for (int i = 0; i < n_guards; i++) if (strcmp(guards[i].bid, bid) == 0) return &guards[i];
    if (n_guards < 8) {
        guard_t *g = &guards[n_guards++];
        memset(g, 0, sizeof(*g));
        snprintf(g->bid, sizeof(g->bid), "%s", bid);
        return g;
    }
    return NULL;
}

static void note_failed(const char *bid) {
    guard_t *g = guard_get(bid);
    if (!g || g->tripped) return;
    g->streak++;
    if (g->streak >= CIRCUIT_N) g->tripped = 1;
}
static void note_ok(const char *bid) {
    guard_t *g = guard_get(bid);
    if (g) g->streak = 0; /* 当日 tripped 保持 */
}
static void new_day(void) {
    for (int i = 0; i < n_guards; i++) { guards[i].streak = 0; guards[i].tripped = 0; }
}

static int test_restore_streak(void) {
    CHECK(guard_get("bid.A") != NULL, "guard entry created");
    note_failed("bid.A");
    note_failed("bid.A");
    CHECK(guard_get("bid.A")->streak == 2 && !guard_get("bid.A")->tripped, "below threshold: warn only");
    note_failed("bid.A");
    CHECK(guard_get("bid.A")->tripped == 1, "third failure opens per-bid circuit");
    note_failed("bid.A");
    CHECK(guard_get("bid.A")->streak == 3, "tripped bid stops counting (no new watermark)");
    note_ok("bid.A");
    CHECK(guard_get("bid.A")->streak == 0 && guard_get("bid.A")->tripped == 1,
          "success clears streak but circuit stays open for the day");
    CHECK(!guard_get("bid.B")->tripped, "other bid unaffected");
    new_day();
    CHECK(!guard_get("bid.A")->tripped && guard_get("bid.A")->streak == 0, "cross-day reset clears circuit");
    printf("[4] per-bid restore streak / circuit OK\n");
    return 0;
}

/* ---------- 5. 门控快轮开合 ---------- */
typedef struct {
    int enabled;
    int in_bg;
    int tracked_pid;
    time_t bg_start;
    int pulsed, ctrl, late;
} gate_state_t;

static int gate_open(const gate_state_t *states, int n, time_t now) {
    for (int i = 0; i < n; i++) {
        const gate_state_t *s = &states[i];
        if (!s->enabled || !s->in_bg || s->tracked_pid <= 0 || s->bg_start <= 0) continue;
        if (s->pulsed || s->ctrl || s->late) continue;
        time_t elapsed = now - s->bg_start;
        if (elapsed >= 0 && elapsed < 25) return 1;
    }
    return 0;
}

static int test_gate(void) {
    gate_state_t s[3] = {
        { 1, 1, 100, 1000, 0, 0, 0 },   /* 打开 */
        { 1, 1, 101, 1000, 0, 0, 1 },   /* late */
        { 0, 1, 102, 1000, 0, 0, 0 },   /* 规则关闭 */
    };
    CHECK(gate_open(s, 3, 1010) == 1, "gate opens for enabled fresh candidate");
    CHECK(gate_open(s, 3, 1025) == 0, "gate closes at 25s deadline");
    gate_state_t late_only[1] = { { 1, 1, 101, 1000, 0, 0, 1 } };
    CHECK(gate_open(late_only, 1, 1010) == 0, "late candidate does not open gate");
    gate_state_t pulsed[1] = { { 1, 1, 100, 1000, 1, 0, 0 } };
    CHECK(gate_open(pulsed, 1, 1010) == 0, "pulsed candidate does not open gate");
    gate_state_t none[1] = { { 0, 0, 0, 0, 0, 0, 0 } };
    CHECK(gate_open(none, 1, 1010) == 0, "no candidate -> power-saving skip");
    printf("[5] fast-round pulse gate OK\n");
    return 0;
}

/* ---------- 6. bgkill 系统预热空壳识别状态机 ---------- */
#define BGKILL_PREWARM_MAX_FP_MB 40
#define BAND_PROTECT_DEFAULT     32
#define BAND_FOREGROUND          100

typedef struct {
    int    tracked_pid;
    int    in_bg;
    time_t bg_start_mono;
    int    bg_rounds;
    int    ever_fg;
    int    prewarm_hold;
    int    prewarm_logged;
    int    kills;
    int    prewarm_logs;
} bgk_sim_t;

/* 返回: 0=未到期/忽略, 1=prewarm_skip, 2=执行 kill */
static int bgkill_sim_step(bgk_sim_t *st, int pid, int cur_band, uint64_t fp_mb,
                           int min_minutes, time_t now_mono) {
    if (pid <= 0) {
        int kills = st->kills, plogs = st->prewarm_logs;
        memset(st, 0, sizeof(*st));
        st->kills = kills; st->prewarm_logs = plogs;
        return 0;
    }
    if (pid != st->tracked_pid) {
        st->tracked_pid = pid;
        st->ever_fg = (cur_band >= BAND_FOREGROUND) ? 1 : 0;
        st->prewarm_logged = 0;
        if (cur_band < BAND_PROTECT_DEFAULT) {
            st->in_bg = 1;
            st->bg_start_mono = now_mono;
            st->bg_rounds = 1;
            st->prewarm_hold = (!st->ever_fg && fp_mb < BGKILL_PREWARM_MAX_FP_MB) ? 1 : 0;
        } else {
            st->in_bg = 0;
            st->bg_start_mono = 0;
            st->bg_rounds = 0;
            st->prewarm_hold = 0;
        }
    } else {
        if (cur_band >= BAND_FOREGROUND) {
            st->ever_fg = 1;
            st->prewarm_hold = 0;
            st->prewarm_logged = 0;
        }
        if (cur_band >= BAND_PROTECT_DEFAULT) {
            st->in_bg = 0;
            st->bg_start_mono = 0;
            st->bg_rounds = 0;
        } else {
            if (!st->in_bg) {
                st->in_bg = 1;
                st->bg_start_mono = now_mono;
                st->bg_rounds = 1;
            } else {
                st->bg_rounds++;
            }
            if (st->prewarm_hold && fp_mb >= BGKILL_PREWARM_MAX_FP_MB) {
                st->prewarm_hold = 0;
                st->ever_fg = 1;
                st->prewarm_logged = 0;
                st->bg_start_mono = now_mono;
                st->bg_rounds = 1;
            }
        }
    }

    if (cur_band >= BAND_PROTECT_DEFAULT || !st->in_bg) return 0;
    if ((int)(now_mono - st->bg_start_mono) < min_minutes * 60) return 0;

    if (!st->ever_fg) {
        if (fp_mb < BGKILL_PREWARM_MAX_FP_MB) {
            st->prewarm_hold = 1;
            if (!st->prewarm_logged) {
                st->prewarm_logged = 1;
                st->prewarm_logs++;
            }
            return 1;
        }
        if (st->prewarm_hold && fp_mb >= BGKILL_PREWARM_MAX_FP_MB) {
            st->prewarm_hold = 0;
            st->ever_fg = 1;
            st->prewarm_logged = 0;
            st->bg_start_mono = now_mono;
            st->bg_rounds = 1;
            return 0;
        }
    }

    st->kills++;
    int kills = st->kills, plogs = st->prewarm_logs;
    memset(st, 0, sizeof(*st));
    st->kills = kills; st->prewarm_logs = plogs;
    return 2;
}

static int test_bgkill_prewarm(void) {
    /* 2A: dasd 预热空壳 (fp=0MB, 从未进前台) 满 20 分钟不杀、只记 1 次 prewarm_skip */
    bgk_sim_t s = {0};
    CHECK(bgkill_sim_step(&s, 12189, 30, 0, 20, 1000) == 0, "2A: t=0 tracked as prewarm");
    CHECK(s.prewarm_hold == 1 && s.ever_fg == 0, "2A: prewarm_hold set at birth");
    CHECK(bgkill_sim_step(&s, 12189, 30, 0, 20, 2205) == 1, "2A: skipped at 20m");
    CHECK(bgkill_sim_step(&s, 12189, 30, 0, 20, 5000) == 1, "2A: still skipped 1h later");
    CHECK(s.kills == 0 && s.prewarm_logs == 1, "2A: zero kills & single latched prewarm log");

    /* 2B: 用户随后点开该预热进程进前台 -> 退后台满 20 分钟正常 kill */
    CHECK(bgkill_sim_step(&s, 12189, 100, 180, 20, 5100) == 0, "2B: foreground visit");
    CHECK(s.ever_fg == 1 && s.prewarm_hold == 0, "2B: ever_fg latched");
    CHECK(bgkill_sim_step(&s, 12189, 30, 180, 20, 5110) == 0, "2B: back to bg at 5110");
    CHECK(bgkill_sim_step(&s, 12189, 30, 180, 20, 6200) == 0, "2B: not yet 20m since 5110");
    CHECK(bgkill_sim_step(&s, 12189, 30, 180, 20, 6315) == 2 && s.kills == 1, "2B: killed after 20m");

    /* 2C: 预热 2 小时后用户在慢轮间隙闪开 3s 退后台 (错过 band=100，但 fp 跳到 210MB)
     * 必须从发现时刻重计 20 分钟，绝不把预热时长算进来秒杀 */
    bgk_sim_t gap = {0};
    bgkill_sim_step(&gap, 13322, 30, 0, 20, 1000);
    bgkill_sim_step(&gap, 13322, 30, 0, 20, 8200); /* 已预热 2 小时 */
    CHECK(gap.kills == 0 && gap.prewarm_hold == 1, "2C: still holding prewarm at 2h");
    CHECK(bgkill_sim_step(&gap, 13322, 30, 210, 20, 8260) == 0,
          "2C: brief fg peek resets timer at 8260 instead of immediate kill");
    CHECK(bgkill_sim_step(&gap, 13322, 30, 210, 20, 9400) == 0, "2C: 19m after peek -> wait");
    CHECK(bgkill_sim_step(&gap, 13322, 30, 210, 20, 9465) == 2 && gap.kills == 1,
          "2C: killed 20m after peek");

    /* 2D: 守护进程重启时已在后台占 180MB 的存量进程 -> 满 20 分钟照常清理 */
    bgk_sim_t legacy = {0};
    bgkill_sim_step(&legacy, 9001, 30, 180, 20, 1000);
    CHECK(legacy.prewarm_hold == 0, "2D: >=40MB existing bg app not treated as prewarm");
    CHECK(bgkill_sim_step(&legacy, 9001, 30, 180, 20, 2205) == 2 && legacy.kills == 1,
          "2D: existing used bg app cleaned normally");

    /* 2E: 用户真点开过的超轻量小工具 (fp=15MB < 40MB) -> 保持不看内存、用过必清性质 */
    bgk_sim_t tiny = {0};
    bgkill_sim_step(&tiny, 9002, 100, 15, 10, 1000);
    bgkill_sim_step(&tiny, 9002, 30, 15, 10, 1010);
    CHECK(bgkill_sim_step(&tiny, 9002, 30, 15, 10, 1615) == 2 && tiny.kills == 1,
          "2E: fg-visited <40MB app still killed on schedule");

    printf("[6] bgkill unlaunched prewarm stub filter OK\n");
    return 0;
}

int main(void) {
    if (test_window() != 0) return 1;
    if (test_self_written() != 0) return 1;
    if (test_delay_zero() != 0) return 1;
    if (test_restore_streak() != 0) return 1;
    if (test_gate() != 0) return 1;
    if (test_bgkill_prewarm() != 0) return 1;
    printf("ALL V0.8.3+V0.8.5 MODEL TESTS PASS\n");
    return 0;
}
