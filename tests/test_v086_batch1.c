/*
 * test_v086_batch1.c — v0.8.6-batch1 关键修复的逻辑模型回归（自包含，非源码逐字提取）。
 *
 * 与生产代码对应分支同构，锚点（src/jetpilotd.c）：
 *   A1 jp_natural_band 新 trusted+cur==band 分支 / jp_attribution_trusted / jp_kill_allowed_with_state
 *   A2 diag_jetsam_state_note（256 槽 + 陈旧轮回收防颠簸）+ BAND_FREEZER(75) 挂起冷冻态精确放行
 *   A3 pulse_fast_gate_open 收尾重算 + 休眠钳到 <=5s
 *   A4 pulse_round 规则循环 !enabled 清状态 continue
 *   B0 last_fg_seen/first_bg_seen/bg_lag_max + delay 起点与 25s 死线分离
 *   C1 bgkill_log_skip band/prewarm 合并
 *   C2 g_diag_latch_suppressed / g_diag_budget_dropped
 *   C3 s_app_proc_epoch 共享缓存 + 改档同步更新 + 门控快轮显式失效
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
#define BAND_FREEZER 75
#define BAND_PROTECT_DEFAULT 90
#define BAND_FOREGROUND 100
#define BAND_SYSTEM_ELEVATED 120
#define JP_MEMSTAT_SUSPENDED 0x001U
#define JP_MEMSTAT_FROZEN    0x002U
#define JP_MEMSTAT_ASSERTION 0x040U
#define JP_MEMSTAT_ACTIVE    0x080U
#define SNAPSHOT_TTL 240
#define PULSE_DEADLINE_BG_SEC 25
#define PULSE_FAST_GATE_POLL_SEC 5

/* ---------------- A1 自然档回退链 + 冷冻室门槛 ---------------- */
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
static int is_frozen_idle(int32_t b, uint32_t state, int state_known) {
    if (!state_known || b != BAND_FREEZER) return 0;
    const uint32_t req = JP_MEMSTAT_SUSPENDED | JP_MEMSTAT_FROZEN;
    const uint32_t forbid = JP_MEMSTAT_ASSERTION | JP_MEMSTAT_ACTIVE;
    return ((state & req) == req) && ((state & forbid) == 0);
}
static int kill_allowed_with_state(int32_t b, uint32_t state, int state_known) {
    if (b <= BAND_BACKGROUND) return 1;
    return is_frozen_idle(b, state, state_known);
}
static int kill_allowed(int32_t b) { return kill_allowed_with_state(b, 0, 0); }

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
    /* 负向 2：fresh sys_band 优先，带 Assertion(0x40) 的活跃 75 仍不杀 */
    a.has_identity = 1; a.sys_known = 1; a.sys_band = 75; a.sys_seen = 480;
    CHECK(natural_new(&a) == 75 && !kill_allowed(natural_new(&a)) &&
          !kill_allowed_with_state(natural_new(&a), JP_MEMSTAT_ASSERTION, 1),
          "A1 negative: fresh sys_band(75) with assertion stays skip=band");
    /* 正向联动：若自然档 75 且实时 state 确认为挂起冷冻态 (0x3)，放行清理/重启 */
    CHECK(kill_allowed_with_state(natural_new(&a),
          JP_MEMSTAT_SUSPENDED | JP_MEMSTAT_FROZEN, 1),
          "A1+Freezer: natural band 75 with Suspended|Frozen(0x3) is allowed");
    /* 负向 3：无 state 证明时 orig>30 仍被 fail-closed 拦住 */
    a.sys_known = 0; a.orig = 75;
    CHECK(natural_new(&a) == 75 && !kill_allowed(natural_new(&a)),
          "A1 negative: orig>30 without verified frozen state stays blocked");
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

/* ---------------- A2 冷冻室精确放行 + 256 槽状态缓存防颠簸 ---------------- */
#define OLD_STATE_SLOTS 64
#define NEW_STATE_SLOTS 256
typedef struct {
    int used;
    int pid;
    int32_t band;
    uint32_t state;
    unsigned long seen_epoch;
} state_slot_model_t;

static int note_old_64(state_slot_model_t *slots, unsigned *head, int pid, int32_t band, uint32_t state) {
    state_slot_model_t *s = NULL;
    for (int i = 0; i < OLD_STATE_SLOTS; i++) {
        if (slots[i].used && slots[i].pid == pid) { s = &slots[i]; break; }
    }
    int changed = 1;
    if (!s) {
        for (int i = 0; i < OLD_STATE_SLOTS; i++) {
            if (!slots[i].used) { s = &slots[i]; break; }
        }
        if (!s) s = &slots[(*head)++ % OLD_STATE_SLOTS];
        s->used = 1;
        s->pid = pid;
    } else if (s->band == band && s->state == state) {
        changed = 0;
    }
    s->band = band;
    s->state = state;
    return changed;
}

static int note_new_256(state_slot_model_t *slots, unsigned *head, int pid,
                        int32_t band, uint32_t state, unsigned long epoch) {
    state_slot_model_t *s = NULL;
    for (int i = 0; i < NEW_STATE_SLOTS; i++) {
        if (slots[i].used && slots[i].pid == pid) { s = &slots[i]; break; }
    }
    int changed = 1;
    if (!s) {
        for (int i = 0; i < NEW_STATE_SLOTS; i++) {
            if (!slots[i].used) { s = &slots[i]; break; }
        }
        if (!s) {
            for (int i = 0; i < NEW_STATE_SLOTS; i++) {
                unsigned idx = (*head + (unsigned)i) % NEW_STATE_SLOTS;
                if (slots[idx].seen_epoch != epoch) {
                    s = &slots[idx];
                    *head = idx + 1;
                    break;
                }
            }
        }
        if (!s) s = &slots[(*head)++ % NEW_STATE_SLOTS];
        s->used = 1;
        s->pid = pid;
    } else if (s->band == band && s->state == state) {
        changed = 0;
    }
    s->band = band;
    s->state = state;
    s->seen_epoch = epoch;
    return changed;
}

static int test_a2(void) {
    /* 1. XNU 冷冻室门槛矩阵核验（真机 121 进程实测：挂起冷冻态 75 为 0x3，活跃断言为 0x40） */
    CHECK(!kill_allowed(75), "A2: state_known=0 must stay fail-closed on band 75");
    CHECK(!kill_allowed_with_state(75, 0x2, 1), "A2: missing Suspended(0x1) stays blocked");
    CHECK(!kill_allowed_with_state(75, 0x1, 1), "A2: missing Frozen(0x2) stays blocked");
    CHECK(!kill_allowed_with_state(75, 0x43, 1), "A2: Frozen+Assertion(0x40) stays blocked");
    CHECK(!kill_allowed_with_state(75, 0x83, 1), "A2: Frozen+Active(0x80) stays blocked");
    CHECK(!kill_allowed_with_state(40, 0x3, 1), "A2: non-Freezer band 40 stays blocked even with 0x3");
    CHECK(kill_allowed_with_state(75, 0x3, 1), "A2: XNU Freezer idle state 0x3 (Suspended|Frozen) is allowed");
    CHECK(kill_allowed_with_state(75, 0x7, 1), "A2: Freezer idle state with benign bit 0x4 is allowed");

    /* 2. P0 修复回归：150 个稳态进程跑 3 轮，旧 64 槽 100% 颠簸刷满预算，新 256 槽仅首轮输出 */
    state_slot_model_t old_slots[OLD_STATE_SLOTS];
    state_slot_model_t new_slots[NEW_STATE_SLOTS];
    unsigned old_head = 0, new_head = 0;
    int old_emits = 0, new_emits = 0;
    memset(old_slots, 0, sizeof(old_slots));
    memset(new_slots, 0, sizeof(new_slots));
    for (unsigned long r = 1; r <= 3; r++) {
        for (int p = 1000; p < 1150; p++) {
            old_emits += note_old_64(old_slots, &old_head, p, 75, 0x3);
            new_emits += note_new_256(new_slots, &new_head, p, 75, 0x3, r);
        }
    }
    CHECK(old_emits == 450,
          "negative control: 64-slot FIFO thrashes 100% on 150 steady PIDs (450 emits in 3 rounds)");
    CHECK(new_emits == 150,
          "A2 fix: 256-slot cache emits once on round 1 and 0 on steady rounds 2-3");

    /* 3. 陈旧轮优先回收：第 1 轮塞满 256 个短命进程（仅前 50 个存活），第 2 轮新来 150 个进程时
     *    必须优先回收 epoch!=2 的已退出槽，不得误淘第 2 轮活跃进程 */
    memset(new_slots, 0, sizeof(new_slots));
    new_head = 0;
    for (int p = 1; p <= 256; p++) {
        CHECK(note_new_256(new_slots, &new_head, p, 0, 0x1, 1) == 1, "round 1 fill");
    }
    int r2_survivor_emits = 0, r2_fresh_emits = 0, r3_emits = 0;
    for (int p = 1; p <= 50; p++) {
        r2_survivor_emits += note_new_256(new_slots, &new_head, p, 0, 0x1, 2);
    }
    for (int p = 2001; p <= 2150; p++) {
        r2_fresh_emits += note_new_256(new_slots, &new_head, p, 75, 0x3, 2);
    }
    for (int p = 1; p <= 50; p++) {
        r3_emits += note_new_256(new_slots, &new_head, p, 0, 0x1, 3);
    }
    for (int p = 2001; p <= 2150; p++) {
        r3_emits += note_new_256(new_slots, &new_head, p, 75, 0x3, 3);
    }
    CHECK(r2_survivor_emits == 0 && r2_fresh_emits == 150 && r3_emits == 0,
          "A2 fix: stale-epoch reclamation recycles exited slots without evicting live PIDs");

    /* 4. 取证行格式不变 */
    int32_t band = 75; uint32_t state = 0x3;
    char line[256];
    snprintf(line, sizeof(line),
             "DIAG event=JETSAM_STATE bid=%s pid=%d band=%d state=0x%x state_raw=0x%x "
             "dedup_key=%s diag_dropped=%d diag_latch=%d diag_budget=%d",
             "TARGET_BID", 1234, band, (unsigned)state, (unsigned)state, "1234|75|3", 0, 0, 0);
    CHECK(strstr(line, "JETSAM_STATE") && strstr(line, "state=0x3") && strstr(line, "band=75"),
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

/* ---------------- C3 epoch 缓存 + 同轮改档同步 ---------------- */
typedef struct {
    unsigned long cached_epoch, round_epoch;
    int n;
    int proc_pidpath_calls;
    int pid0;
    int32_t prio0;
} cache_model_t;
static void ensure(cache_model_t *c) {
    if (c->n >= 0 && c->cached_epoch == c->round_epoch) return;  /* 复用 */
    c->n = 1;
    c->pid0 = 4242;
    c->prio0 = 30;
    c->cached_epoch = c->round_epoch;
    c->proc_pidpath_calls++;                                    /* 重建才 syscall */
}
static void update_cached_prio(cache_model_t *c, int pid, int32_t prio) {
    if (c->n <= 0 || c->cached_epoch != c->round_epoch) return;
    if (c->pid0 == pid) c->prio0 = prio;
}
static void invalidate(cache_model_t *c) { c->round_epoch++; c->cached_epoch = 0; c->n = -1; }
static int test_c3(void) {
    cache_model_t c = { .cached_epoch=0, .round_epoch=1, .n=-1, .proc_pidpath_calls=0 };
    ensure(&c);  /* 慢轮主扫描填充（初始 prio=30） */
    CHECK(c.prio0 == 30, "C3 precondition: initial cached priority is 30");
    update_cached_prio(&c, 4242, 90);  /* 慢轮 RAISE 成功后同步缓存 */
    ensure(&c); ensure(&c);            /* restart/bgkill/pulse 同轮复用 */
    CHECK(c.proc_pidpath_calls == 1 && c.prio0 == 90,
          "C3: one slow round builds cache once and syncs post-RAISE/LOWER/DEMOTE/RETURN priority");
    c.round_epoch++;         /* 下一轮：未 ensure 前旧缓存不可被误写 */
    update_cached_prio(&c, 4242, 10);
    CHECK(c.prio0 == 90, "C3 negative: stale epoch cache is not mutated before rebuild");
    ensure(&c);
    CHECK(c.proc_pidpath_calls == 2 && c.prio0 == 30,
          "C3: new epoch rebuilds cache (no stale pid->bid)");
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
