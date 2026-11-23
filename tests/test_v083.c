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

/* ---------- 1. Ltarget 窗口守卫 ---------- */
#define GROWTH_CAP_MB 128

typedef struct {
    const char *end;
    int  t_reach;
    int  early_abort_assert;
} win_t;

/* 与 jetpilotd.c 窗口内守卫同构：绝对涨幅保护 + target 收敛 + 60s 定时。
 * old_guard=1 时模拟返工前的 95%L 比例守卫（负向对照）。 */
static win_t window_step(uint64_t fp0, uint64_t target, uint64_t cur, int sec, int old_guard) {
    win_t r = { "CONTINUE", 0, 0 };
    if (old_guard) {
        if (cur * 100 >= 95 * target) { r.end = "ABORT"; r.t_reach = sec; return r; }
        if (cur * 100 < 80 * target)  { r.end = "TARGET_REACHED"; r.t_reach = sec; return r; }
        if (sec == 60) { r.end = "WINDOW_END"; return r; }
        return r;
    }
    uint64_t growth = (fp0 / 4 > (uint64_t)GROWTH_CAP_MB) ? fp0 / 4 : (uint64_t)GROWTH_CAP_MB;
    uint64_t cap = fp0 + growth;
    if (cur > cap) {
        if (sec <= 1 && fp0 > target) r.early_abort_assert = 1;
        r.end = "ABORT"; r.t_reach = sec; return r;
    }
    if (cur <= target) { r.end = "TARGET_REACHED"; r.t_reach = sec; return r; }
    if (sec == 60) { r.end = "WINDOW_END"; return r; }
    return r;
}

static int test_window(void) {
    /* 负向对照：入队保证 fp>target=L 时旧守卫首秒必 ABORT */
    win_t old = window_step(713, 400, 713, 1, 1);
    CHECK(strcmp(old.end, "ABORT") == 0 && old.t_reach == 1,
          "old 95%%L guard must abort at t=1 (negative control)");

    /* 新守卫：fp 恒定 > target，60s 内不得出现 t_reach<=1 的 ABORT，须活到 WINDOW_END */
    for (int sec = 1; sec <= 59; sec++) {
        win_t r = window_step(713, 400, 713, sec, 0);
        CHECK(!(strcmp(r.end, "ABORT") == 0 && r.t_reach <= 1),
              "no t<=1 ABORT when fp>target (Ltarget window)");
        CHECK(strcmp(r.end, "CONTINUE") == 0, "window must not end before 60s");
        CHECK(r.early_abort_assert == 0, "no early_abort assertion on steady fp");
    }
    win_t last = window_step(713, 400, 713, 60, 0);
    CHECK(strcmp(last.end, "WINDOW_END") == 0, "60s timed restore still active");

    /* TARGET_REACHED 重定义：fp 降到 target 以下才提前结束 */
    win_t reached = window_step(713, 400, 380, 10, 0);
    CHECK(strcmp(reached.end, "TARGET_REACHED") == 0 && reached.t_reach == 10,
          "TARGET_REACHED at fp<=target, not unreachable 0.8*L");

    /* 真暴涨仍熔断（相对 fp0 的 cap = max(128, fp0/4) = 178, cap=891） */
    win_t runaway = window_step(713, 400, 900, 1, 0);
    CHECK(strcmp(runaway.end, "ABORT") == 0 && runaway.early_abort_assert == 1,
          "runaway growth still aborts and logs assertion");
    printf("[1] Ltarget window guards OK (old guard negative control included)\n");
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

int main(void) {
    if (test_window() != 0) return 1;
    if (test_self_written() != 0) return 1;
    if (test_delay_zero() != 0) return 1;
    if (test_restore_streak() != 0) return 1;
    if (test_gate() != 0) return 1;
    printf("ALL V0.8.3 REWORK TESTS PASS\n");
    return 0;
}
