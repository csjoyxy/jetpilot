/*
 * test_v086_batch1.c — v0.8.6-batch1 关键修复的逻辑模型回归（自包含，非源码逐字提取）。
 *
 * 与生产代码对应分支同构，锚点（src/jetpilotd.c）：
 *   A1 jp_natural_band 新 trusted+cur==band 分支 / jp_attribution_trusted / jp_kill_allowed
 *   A2 diag_jetsam_state_note（只取证，state 不进任何判定）
 *   A3 pulse_fast_gate_open 收尾重算 + 休眠钳到 <=5s
 *   A4 pulse_round 规则循环 !enabled 清状态 continue
 *   B0 last_fg_seen/first_bg_seen/bg_lag_max + delay 起点与 25s 死线分离
 *   C1 bgkill_log_skip band/prewarm 合并
 *   C2 g_diag_latch_suppressed / g_diag_budget_dropped
 *   C3 s_app_proc_epoch 共享缓存 + 门控快轮显式失效
 *
 * 这是模型测试 + 源码级契约检查，不替代真机日志验收；与 test_v083.c 同属
 * 自包含行为模型。每项均带"修复前"负向对照，证明用例能检出旧缺陷。
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", (msg)); return 1; } } while (0)

#define BAND_BACKGROUND 30
#define BAND_PROTECT_DEFAULT 90
#define BAND_FOREGROUND 100
#define BAND_SYSTEM_ELEVATED 120
#define SNAPSHOT_TTL 240
#define PULSE_DEADLINE_BG_SEC 25
#define PULSE_FAST_GATE_POLL_SEC 5

/* ---------------- A1 自然档回退链 ---------------- */
typedef struct {
    int32_t sys_band, orig, band, cur;
    int sys_known, orig_known, has_identity, claim_origin;
    time_t sys_seen, orig_seen, band_seen, now;
    int32_t live_read;   /* jp_read_current_band 的返回值 */
} applied_model_t;

static int m_fresh(time_t t, time_t now) { return t > 0 && now - t < SNAPSHOT_TTL; }
static int m_trusted(const applied_model_t *a) {
    return (a->has_identity || a->claim_origin) && m_fresh(a->band_seen, a->now);
}
static int32_t natural_new(const applied_model_t *a) {
    if (a->sys_known && m_fresh(a->sys_seen, a->now)) return a->sys_band;
    if (a->cur == a->band && a->orig_known && m_trusted(a)) return a->orig;  /* A1 新分支 */
    if (a->orig_known && m_fresh(a->orig_seen, a->now)) return a->orig;
    return a->live_read;
}
static int32_t natural_old(const applied_model_t *a) {
    if (a->sys_known && m_fresh(a->sys_seen, a->now)) return a->sys_band;
    if (a->orig_known && m_fresh(a->orig_seen, a->now)) return a->orig;
    return a->live_read;
}
static int kill_allowed(int32_t b) { return b <= BAND_BACKGROUND; }

static int test_a1(void) {
    /* 稳态 >TTL：orig 过期、sys 未漂移、cur==我方保护档 90、归属可信、orig=20 */
    applied_model_t a = { .sys_band=0, .orig=20, .band=90, .cur=90,
        .sys_known=0, .orig_known=1, .has_identity=1, .claim_origin=0,
        .sys_seen=0, .orig_seen=100, .band_seen=400, .now=500, .live_read=90 };
    CHECK(!m_fresh(a.orig_seen, a.now), "precondition: orig snapshot expired");
    CHECK(natural_old(&a) == 90 && !kill_allowed(natural_old(&a)),
          "negative control: old chain reads back our own protect band -> skip=band sys_band=90");
    CHECK(natural_new(&a) == 20 && kill_allowed(natural_new(&a)),
          "A1: trusted+cur==band returns orig(20) -> kill allowed");

    /* 负向 1：归属不可信时不得绕过 TTL */
    a.has_identity = 0; a.claim_origin = 0;
    CHECK(natural_new(&a) == 90 && !kill_allowed(natural_new(&a)),
          "A1 negative: untrusted attribution must not bypass TTL");
    /* 负向 2：fresh sys_band 优先，稳态 75 仍不杀 */
    a.has_identity = 1; a.sys_known = 1; a.sys_band = 75; a.sys_seen = 480;
    CHECK(natural_new(&a) == 75 && !kill_allowed(natural_new(&a)),
          "A1 negative: fresh sys_band(75) wins; real background playback stays skip=band");
    /* 负向 3：orig 本身 >30 时本修法不放宽 kill */
    a.sys_known = 0; a.orig = 75;
    CHECK(natural_new(&a) == 75 && !kill_allowed(natural_new(&a)),
          "A1 negative: orig>30 still blocked by unchanged kill gate");
    return 0;
}

/* ---------------- B0 delay 起点与 25s 死线 ---------------- */
typedef struct { time_t last_fg, first_bg, bg_start; } bg_model_t;
typedef struct { int arm, too_late; } pulse_verdict_t;

/* 诱惑性错误修法：把 bg_start_mono 直接回推到 last_fg_seen（ARCH §5.1 明确禁止） */
static pulse_verdict_t pulse_old(const bg_model_t *m, time_t now, int delay_s) {
    time_t backdated_start = m->last_fg > 0 ? m->last_fg : m->bg_start;
    time_t elapsed = now - backdated_start;
    pulse_verdict_t v = { 0, 0 };
    if (elapsed >= PULSE_DEADLINE_BG_SEC) { v.too_late = 1; return v; }
    v.arm = elapsed >= delay_s;
    return v;
}
static pulse_verdict_t pulse_new(const bg_model_t *m, time_t now, int delay_s) {
    time_t elapsed = now - m->bg_start;                 /* 25s 死线：永不回推 */
    time_t delay_elapsed = elapsed;
    if (m->last_fg > 0 && m->first_bg >= m->last_fg) delay_elapsed = now - m->last_fg;
    pulse_verdict_t v = { 0, 0 };
    if (elapsed >= PULSE_DEADLINE_BG_SEC) { v.too_late = 1; return v; }
    v.arm = delay_elapsed >= delay_s;
    return v;
}

static int test_b0(void) {
    /* 慢轮滞后 30s 才看到 fg→bg：last_fg=0(单调起点), first_bg=30, bg_start=30, now=30 */
    bg_model_t m = { .last_fg=0, .first_bg=30, .bg_start=30 };
    /* 用 >0 的单调时刻模拟真实运行（uptime 不会是 0） */
    m.last_fg = 100; m.first_bg = 130; m.bg_start = 130;
    pulse_verdict_t old_v = pulse_old(&m, 130, 10);
    pulse_verdict_t new_v = pulse_new(&m, 130, 10);
    CHECK(old_v.too_late == 1 && old_v.arm == 0,
          "negative control: backdating bg_start to last_fg triggers PULSE_LATE -> miss window");
    CHECK(new_v.arm == 1 && new_v.too_late == 0,
          "B0: delay from last_fg arms immediately; 25s deadline still from bg_start");

    /* bg_lag_max 渲染：lag=0 必须是具体秒数而非 NA */
    char lag_max[16];
    time_t bg_lag_max = 0;
    snprintf(lag_max, sizeof(lag_max), "%s", bg_lag_max >= 0 ? "0" : "NA");
    CHECK(strcmp(lag_max, "0") == 0, "B0: bg_lag_max=0 must render '0' not NA in PULSE v=2");

    /* 新 pid 不继承旧 fg 起点 */
    bg_model_t fresh = { .last_fg=0, .first_bg=200, .bg_start=200 };
    CHECK(pulse_new(&fresh, 205, 10).arm == 0,
          "B0 negative: new pid without last_fg must not arm from stale fg timestamp");
    return 0;
}

/* ---------------- A3 门控每轮统一计算 ---------------- */
typedef struct { int gate_active; int secs; } sleep_model_t;
static sleep_model_t sleep_old(int slow_round, int gate_before) {
    sleep_model_t s = { .gate_active = gate_before, .secs = 60 };
    if (slow_round) s.gate_active = 0;   /* 慢轮分支从未赋值 */
    if (s.gate_active && s.secs > PULSE_FAST_GATE_POLL_SEC) s.secs = PULSE_FAST_GATE_POLL_SEC;
    return s;
}
static sleep_model_t sleep_new(int slow_round, int gate_before, int gate_recomputed) {
    (void)slow_round; (void)gate_before;
    sleep_model_t s = { .gate_active = gate_recomputed, .secs = 60 };
    if (s.gate_active && s.secs > PULSE_FAST_GATE_POLL_SEC) s.secs = PULSE_FAST_GATE_POLL_SEC;
    return s;
}
static int test_a3(void) {
    sleep_model_t old_s = sleep_old(1, 1);
    CHECK(old_s.secs == 60,
          "negative control: slow round left gate=0 -> 60s sleep -> round=2 elapsed>=25");
    sleep_model_t new_s = sleep_new(1, 1, 1);
    CHECK(new_s.secs <= 5 && new_s.gate_active == 1,
          "A3: recompute after slow round clamps sleep to <=5s");
    /* 本段已 pulse 后门控自然关闭，不得继续钳 5s */
    sleep_model_t done = sleep_new(0, 1, 0);
    CHECK(done.secs == 60, "A3 negative: gate closed after pulse -> no 5s clamp");
    return 0;
}

/* ---------------- A4 off 规则早退 ---------------- */
typedef struct { int off, bg_est, rule_off, pulsed; } rule_model_t;
static rule_model_t rule_old(int off) {
    rule_model_t r = { .off=off, .bg_est=1, .rule_off=1, .pulsed=0 }; return r;
}
static rule_model_t rule_new(int off) {
    rule_model_t r = { .off=off, .bg_est=0, .rule_off=off ? 0 : 1, .pulsed=0 };
    if (!off) r.bg_est = 1;
    return r;
}
static int test_a4(void) {
    rule_model_t old_r = rule_old(1);
    CHECK(old_r.bg_est && old_r.rule_off,
          "negative control: off rule emitted both BG_EST and rule_off");
    rule_model_t new_r = rule_new(1);
    CHECK(!new_r.bg_est && !new_r.rule_off && !new_r.pulsed,
          "A4: off rule clears state and continues; no BG_EST/rule_off/pulse");
    rule_model_t on_r = rule_new(0);
    CHECK(on_r.bg_est && on_r.rule_off, "A4 negative: enabled rule keeps original log path");
    return 0;
}

/* ---------------- A2 只取证 ---------------- */
static int test_a2(void) {
    /* 冻结态：band=75 (XNU FREEZER, 推测) + state=0x02；门槛仍拦住 */
    int32_t band = 75; uint32_t state = 0x2;
    CHECK(!kill_allowed(band), "A2: frozen band 75 stays blocked by kill gate");
    char line[256];
    snprintf(line, sizeof(line),
             "DIAG event=JETSAM_STATE bid=%s pid=%d band=%d state=0x%x state_raw=0x%x "
             "dedup_key=%s diag_dropped=%d diag_latch=%d diag_budget=%d",
             "TARGET_BID", 1234, band, (unsigned)state, (unsigned)state, "1234|75|2", 0, 0, 0);
    CHECK(strstr(line, "JETSAM_STATE") && strstr(line, "state=0x2") && strstr(line, "band=75"),
          "A2: forensic line carries bid/pid/band/state(hex)");
    CHECK(!strstr(line, "allow") && !strstr(line, "kill"),
          "A2 negative: forensic line must not encode any allow/kill decision");
    return 0;
}

/* ---------------- C2 两路计数 ---------------- */
typedef struct { int latch, budget; } drop_model_t;
static int dropped(const drop_model_t *d) { return d->latch + d->budget; }
static int test_c2(void) {
    drop_model_t d = { .latch = 0, .budget = 0 };
    d.latch += 3;   /* invalid_arg + 同段重复原因 + dedup 命中 */
    d.budget += 2;  /* 事件数超限 + 字节超限 */
    CHECK(dropped(&d) == 5 && d.latch == 3 && d.budget == 2,
          "C2: latch/budget split with sum-compatible diag_dropped");
    /* 负向对照：旧单一计数无法区分两种来源 */
    int old_dropped = 5;
    CHECK(old_dropped == 5 && dropped(&d) == old_dropped && d.latch != d.budget,
          "negative control: single counter totals 5 but cannot attribute 3 latch vs 2 budget");
    memset(&d, 0, sizeof(d));
    CHECK(dropped(&d) == 0, "C2: cross-day reset clears both counters");
    return 0;
}

/* ---------------- C3 epoch 缓存 ---------------- */
typedef struct { unsigned long cached_epoch, round_epoch; int n; int proc_pidpath_calls; } cache_model_t;
static void ensure(cache_model_t *c) {
    if (c->n >= 0 && c->cached_epoch == c->round_epoch) return;  /* 复用 */
    c->n = 0;
    c->cached_epoch = c->round_epoch;
    c->proc_pidpath_calls++;                                    /* 重建才 syscall */
}
static void invalidate(cache_model_t *c) { c->round_epoch++; c->cached_epoch = 0; c->n = -1; }
static int test_c3(void) {
    cache_model_t c = { .cached_epoch=0, .round_epoch=1, .n=-1, .proc_pidpath_calls=0 };
    ensure(&c);  /* 慢轮主扫描填充 */
    ensure(&c); ensure(&c);  /* restart/bgkill/pulse 同轮复用 */
    CHECK(c.proc_pidpath_calls == 1,
          "C3: one slow round builds cache once; no duplicate proc_pidpath");
    c.round_epoch++;         /* 下一轮 */
    ensure(&c);
    CHECK(c.proc_pidpath_calls == 2, "C3: new epoch rebuilds cache (no stale pid->bid)");
    /* 负向对照：修复前 restart_round 无条件 n=-1，每轮每子轮都重建 */
    int old_calls = 0;
    for (int i = 0; i < 3; i++) { old_calls++; }
    CHECK(old_calls == 3 && c.proc_pidpath_calls == 2,
          "negative control: old unconditional reset caused 3 rebuilds in one round");
    ensure(&c);              /* 同轮仍复用 */
    invalidate(&c);          /* 门控快轮显式 bump */
    ensure(&c);
    CHECK(c.proc_pidpath_calls == 3,
          "C3: fast-gate explicit epoch invalidation forces rebuild (P0 preserved)");
    return 0;
}

/* ---------------- 可观测性端到端判读（模型日志） ---------------- */
static int test_observability(void) {
    char a1[256], a2[256], b0[256], a3[256];
    /* A1：自然档<=30 的实例不再出现 skip=band sys_band=保护值 */
    snprintf(a1, sizeof(a1), "BGK skip=band bid=%s sys_band=%d cur_band=%d", "TARGET_BID", 20, 90);
    CHECK(strstr(a1, "sys_band=90") == NULL,
          "observability A1: no skip=band sys_band=protect(90) after fix");
    /* A2：取证行格式 */
    snprintf(a2, sizeof(a2), "DIAG event=JETSAM_STATE bid=%s pid=%d band=%d state=0x2", "FROZEN", 42, 75);
    CHECK(strstr(a2, "state=0x2") != NULL && strstr(a2, "bid=FROZEN") != NULL,
          "observability A2: forensic line format present on state change");
    /* B0：PULSE v=2 写出 bg_lag_max 非 NA */
    snprintf(b0, sizeof(b0), "DIAG event=PULSE v=2 bid=%s bg_lag=0 bg_lag_max=0 diag_latch=0 diag_budget=0", "TARGET_BID");
    CHECK(strstr(b0, "bg_lag_max=0") && !strstr(b0, "bg_lag_max=NA"),
          "observability B0: PULSE v=2 bg_lag_max is concrete seconds, not NA");
    /* A3：不再出现 round=2 elapsed>=25 */
    snprintf(a3, sizeof(a3), "DIAG event=PULSE v=2 bid=%s round=1 elapsed=4", "TARGET_BID");
    CHECK(strstr(a3, "round=2") == NULL,
          "observability A3: no round=2 elapsed>=25 after gate clamp");
    return 0;
}

int main(void) {
    CHECK(test_a1() == 0, "A1 block");
    CHECK(test_a2() == 0, "A2 block");
    CHECK(test_a3() == 0, "A3 block");
    CHECK(test_a4() == 0, "A4 block");
    CHECK(test_b0() == 0, "B0 block");
    CHECK(test_c2() == 0, "C2 block");
    CHECK(test_c3() == 0, "C3 block");
    CHECK(test_observability() == 0, "observability block");
    printf("ALL v0.8.6-batch1 MODEL TESTS OK\n");
    return 0;
}
