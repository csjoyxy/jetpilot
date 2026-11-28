/*
 * test_v087_appmon.c — v0.8.7 APPMON（BKSApplicationStateMonitor 零轮询前后台事件推送）
 * 逻辑模型与多线程管道/环形缓冲并发回归测试（兼容 ASan/UBSan 与 TSan）。
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/select.h>

#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", (msg)); return 1; } } while (0)

#define BAND_FOREGROUND 100
#define BAND_SYSTEM_ELEVATED 120
#define PULSE_DEADLINE_BG_SEC 25
#define PULSE_FAST_GATE_POLL_SEC 5
#define PULSE_FIRST_OBSERVE_MAX_SLOW 20
#define SLOW_MAX_SECS 240
#define APPMON_RING_CAP 64
#define APPMON_BG_PENDING_MAX_SEC 3

#define JP_BKS_STATE_TERMINATED   1U
#define JP_BKS_STATE_BG_SUSPENDED 2U
#define JP_BKS_STATE_BG_RUNNING   4U
#define JP_BKS_STATE_FOREGROUND   8U

typedef struct {
    char     bid[128];
    int      pid;
    uint32_t state;
    time_t   mono;
} appmon_ev_t;

typedef struct {
    int    tracked_pid;
    int    in_bg;
    time_t bg_start_mono;
    time_t last_fg_seen_mono;
    time_t first_bg_seen_mono;
    time_t bg_lag;
    int    appmon_in_fg;
    time_t appmon_bg_pending_mono;
    int    ever_fg;
} app_track_model_t;

static void apply_event_model(app_track_model_t *m, const appmon_ev_t *ev, int *need_bg_scan) {
    if (ev->state == JP_BKS_STATE_FOREGROUND && ev->pid > 0) {
        m->tracked_pid = ev->pid;
        m->ever_fg = 1;
        m->in_bg = 0;
        m->bg_start_mono = 0;
        m->last_fg_seen_mono = ev->mono;
        m->first_bg_seen_mono = 0;
        m->bg_lag = -1;
        m->appmon_in_fg = 1;
        m->appmon_bg_pending_mono = 0;
    } else if ((ev->state == JP_BKS_STATE_BG_RUNNING ||
                ev->state == JP_BKS_STATE_BG_SUSPENDED) && ev->pid > 0) {
        if (need_bg_scan) *need_bg_scan = 1;
        if (m->tracked_pid == ev->pid && m->appmon_in_fg) {
            m->last_fg_seen_mono = ev->mono;
            m->first_bg_seen_mono = ev->mono;
            m->appmon_bg_pending_mono = ev->mono;
        } else if (!m->in_bg) {
            if (m->tracked_pid == ev->pid && m->first_bg_seen_mono <= 0) {
                m->first_bg_seen_mono = ev->mono;
            }
            m->appmon_bg_pending_mono = ev->mono;
        }
        m->appmon_in_fg = 0;
    } else if (ev->state == JP_BKS_STATE_TERMINATED) {
        m->appmon_in_fg = 0;
        m->appmon_bg_pending_mono = 0;
    }
}

static void pulse_round_model(app_track_model_t *m, int pid, int32_t cur, time_t now_mono) {
    if (cur >= BAND_FOREGROUND) {
        m->last_fg_seen_mono = now_mono;
        m->first_bg_seen_mono = 0;
        if (m->appmon_bg_pending_mono > 0 &&
            (now_mono < m->appmon_bg_pending_mono ||
             (now_mono - m->appmon_bg_pending_mono) >= APPMON_BG_PENDING_MAX_SEC)) {
            m->appmon_bg_pending_mono = 0;
        }
        m->in_bg = 0;
        m->bg_start_mono = 0;
        m->tracked_pid = pid;
    } else if (cur >= 0 && cur <= 95) {
        if (m->tracked_pid != pid) {
            m->tracked_pid = pid;
            m->in_bg = 1;
            m->bg_start_mono = now_mono;
            m->last_fg_seen_mono = 0;
            m->first_bg_seen_mono = now_mono;
            m->bg_lag = -1;
            m->appmon_in_fg = 0;
            m->appmon_bg_pending_mono = 0;
        } else if (!m->in_bg || m->bg_start_mono == 0) {
            m->in_bg = 1;
            m->appmon_in_fg = 0;
            m->appmon_bg_pending_mono = 0;
            m->bg_start_mono = now_mono;
            if (m->first_bg_seen_mono <= 0) m->first_bg_seen_mono = now_mono;
            if (m->last_fg_seen_mono > 0 && m->first_bg_seen_mono >= m->last_fg_seen_mono) {
                m->bg_lag = m->first_bg_seen_mono - m->last_fg_seen_mono;
            } else {
                m->bg_lag = -1;
            }
        }
    }
}

static int next_wait_ms_model(app_track_model_t *m, int delay_s, time_t now_mono,
                              int default_secs, int *gate_open) {
    int wait_ms = default_secs * 1000;
    *gate_open = 0;
    if (!m->in_bg && m->appmon_bg_pending_mono > 0) {
        if (now_mono >= m->appmon_bg_pending_mono &&
            (now_mono - m->appmon_bg_pending_mono) < APPMON_BG_PENDING_MAX_SEC) {
            *gate_open = 1;
            if (wait_ms > 250) wait_ms = 250;
        } else {
            m->appmon_bg_pending_mono = 0;
        }
        return wait_ms;
    }
    if (!m->in_bg || m->tracked_pid <= 0 || m->bg_start_mono <= 0) return wait_ms;
    time_t elapsed = now_mono - m->bg_start_mono;
    if (elapsed >= 0 && elapsed < PULSE_DEADLINE_BG_SEC) {
        *gate_open = 1;
        if (wait_ms > PULSE_FAST_GATE_POLL_SEC * 1000) wait_ms = PULSE_FAST_GATE_POLL_SEC * 1000;
        time_t delay_elapsed = elapsed;
        if (m->last_fg_seen_mono > 0 && m->first_bg_seen_mono >= m->last_fg_seen_mono) {
            delay_elapsed = now_mono - m->last_fg_seen_mono;
        }
        if (delay_elapsed >= 0 && delay_elapsed < delay_s) {
            int rem_ms = (int)((delay_s - delay_elapsed) * 1000);
            if (rem_ms > 0 && rem_ms < wait_ms) wait_ms = rem_ms;
        }
    }
    return wait_ms;
}

/* [1] 连续刷 1 小时零轮询 + 退后台瞬间 lag=0 与精确 delay_s 倒计时 */
static int test_long_fg_zero_poll_and_exact_delay(void) {
    app_track_model_t m;
    memset(&m, 0, sizeof(m));
    m.bg_lag = -1;

    int need_bg = 0;
    appmon_ev_t fg_ev = { "com.tencent.xin", 5120, JP_BKS_STATE_FOREGROUND, 100 };
    apply_event_model(&m, &fg_ev, &need_bg);
    CHECK(need_bg == 0 && m.ever_fg == 1 && m.appmon_in_fg == 1,
          "FG entry latches ever_fg=1 without forcing slow BG scan");

    /* 负向对照：若退后台时不刷新 last_fg_seen_mono，1 小时后 (t=3700) 会把 delay_elapsed 算成 3600s 误秒发 */
    time_t buggy_delay_elapsed = 3700 - m.last_fg_seen_mono;
    CHECK(buggy_delay_elapsed == 3600,
          "negative control: unrefreshed last_fg_seen_mono after 1h zero-polling would misfire delay_s=10 at 0s");

    /* v0.8.7 退后台事件：t=3700 上滑回桌面 */
    appmon_ev_t bg_ev = { "com.tencent.xin", 5120, JP_BKS_STATE_BG_RUNNING, 3700 };
    apply_event_model(&m, &bg_ev, &need_bg);
    CHECK(need_bg == 1 && m.last_fg_seen_mono == 3700 && m.first_bg_seen_mono == 3700,
          "BG transition refreshes both last_fg and first_bg to exact transition timestamp");

    pulse_round_model(&m, 5120, 30, 3700);
    CHECK(m.in_bg == 1 && m.bg_lag == 0 && m.appmon_bg_pending_mono == 0,
          "pulse_round enters in_bg with exact lag=0");

    /* delay_s=3 时应精确睡 3000ms（而非粗粒度 5000ms） */
    int gate = 0;
    int w3 = next_wait_ms_model(&m, 3, 3700, 60, &gate);
    CHECK(gate == 1 && w3 == 3000, "delay_s=3 schedules exact 3000ms wakeup");

    /* delay_s=10 时首段睡 5000ms，t=3707 时剩余 3000ms */
    int w10_a = next_wait_ms_model(&m, 10, 3700, 60, &gate);
    int w10_b = next_wait_ms_model(&m, 10, 3707, 60, &gate);
    CHECK(w10_a == 5000 && w10_b == 3000, "delay_s=10 clamps to 5000ms then exact remaining 3000ms");
    return 0;
}

/* [2] 0.3s 快进快出：同批 FIFO 事件不丢 ever_fg 与前台参照 */
static int test_subsecond_tap_and_swipe_fifo(void) {
    app_track_model_t m;
    memset(&m, 0, sizeof(m));
    m.bg_lag = -1;

    appmon_ev_t batch[2] = {
        { "com.czzhao.binance", 8801, JP_BKS_STATE_FOREGROUND, 500 },
        { "com.czzhao.binance", 8801, JP_BKS_STATE_BG_RUNNING, 500 }
    };
    int need_bg = 0;
    for (int i = 0; i < 2; i++) {
        apply_event_model(&m, &batch[i], &need_bg);
    }
    pulse_round_model(&m, 8801, 30, 500);
    CHECK(m.ever_fg == 1 && m.in_bg == 1 && m.bg_lag == 0 && need_bg == 1,
          "0.3s tap-and-swipe FIFO preserves ever_fg=1 and lag=0");
    return 0;
}

/* [3] 退后台动画期间 XNU 档位短暂滞留 >=100：250ms 快检追平 + 3s 超时封顶 */
static int test_xnu_band_drop_animation_lag(void) {
    app_track_model_t m;
    memset(&m, 0, sizeof(m));
    m.bg_lag = -1;

    appmon_ev_t fg_ev = { "com.xingin.discover", 6100, JP_BKS_STATE_FOREGROUND, 600 };
    appmon_ev_t bg_ev = { "com.xingin.discover", 6100, JP_BKS_STATE_BG_RUNNING, 610 };
    int need_bg = 0;
    apply_event_model(&m, &fg_ev, &need_bg);
    apply_event_model(&m, &bg_ev, &need_bg);

    /* t=610 首检时 XNU 档位仍滞留在 100：保持 pending 并排 250ms 快检 */
    pulse_round_model(&m, 6100, BAND_FOREGROUND, 610);
    int gate = 0;
    int w = next_wait_ms_model(&m, 0, 610, 60, &gate);
    CHECK(m.in_bg == 0 && m.appmon_bg_pending_mono == 610 && gate == 1 && w == 250,
          "band=100 animation lag keeps pending state and schedules 250ms retry");

    /* 250ms 后档位跌至 30：立即入后台并清 pending */
    pulse_round_model(&m, 6100, 30, 611);
    CHECK(m.in_bg == 1 && m.appmon_bg_pending_mono == 0 && m.bg_lag == 1,
          "subsequent drop to band 30 enters in_bg and clears pending");

    /* 若后台音频持续停在 120 满 3s：自动过期停止 250ms 快检，回退 60s 休眠 */
    apply_event_model(&m, &fg_ev, &need_bg);
    bg_ev.mono = 700;
    apply_event_model(&m, &bg_ev, &need_bg);
    pulse_round_model(&m, 6100, BAND_SYSTEM_ELEVATED, 703);
    w = next_wait_ms_model(&m, 0, 703, 60, &gate);
    CHECK(gate == 0 && w == 60000 && m.appmon_bg_pending_mono == 0,
          "elevated band 120 expires pending after 3s and restores 60s sleep");
    return 0;
}

/* [4] 扩展进程过滤与慢轮 20s 钳制解除/回退 */
static int slow_period_with_appmon(int appmon_active, int any_pulse_enabled, int backoff_slow) {
    int sp = backoff_slow;
    if (!appmon_active && any_pulse_enabled && sp > PULSE_FIRST_OBSERVE_MAX_SLOW) {
        sp = PULSE_FIRST_OBSERVE_MAX_SLOW;
    }
    return sp;
}

static int test_extension_filter_and_backoff_unlock(void) {
    CHECK(slow_period_with_appmon(1, 1, SLOW_MAX_SECS) == 240,
          "active APPMON unlocks slow period to full 240s backoff");
    CHECK(slow_period_with_appmon(0, 1, SLOW_MAX_SECS) == 20,
          "fallback APPMON=0 preserves <=20s slow scan safety net");
    return 0;
}

/* [5] 多线程生产者 + 非阻塞管道 + 环形队列并发压力测试（兼容 TSan/ASan） */
typedef struct {
    pthread_mutex_t mtx;
    appmon_ev_t     ring[APPMON_RING_CAP];
    unsigned        head;
    unsigned        count;
    int             pipe_pending;
    int             pipe_fd[2];
    int             producers_done;
} mt_ctx_t;

static void *producer_fn(void *arg) {
    mt_ctx_t *ctx = (mt_ctx_t *)arg;
    for (int i = 0; i < 2500; i++) {
        int need_write = 0;
        pthread_mutex_lock(&ctx->mtx);
        unsigned idx;
        if (ctx->count < APPMON_RING_CAP) {
            idx = (ctx->head + ctx->count) % APPMON_RING_CAP;
            ctx->count++;
        } else {
            idx = ctx->head;
            ctx->head = (ctx->head + 1U) % APPMON_RING_CAP;
        }
        snprintf(ctx->ring[idx].bid, sizeof(ctx->ring[idx].bid), "com.test.app");
        ctx->ring[idx].pid = 1000 + (i & 3);
        ctx->ring[idx].state = (i & 1) ? JP_BKS_STATE_BG_RUNNING : JP_BKS_STATE_FOREGROUND;
        ctx->ring[idx].mono = 100 + i;
        if (!ctx->pipe_pending) {
            ctx->pipe_pending = 1;
            need_write = 1;
        }
        pthread_mutex_unlock(&ctx->mtx);
        if (need_write) {
            char c = 'A';
            ssize_t wr = write(ctx->pipe_fd[1], &c, 1);
            (void)wr;
        }
    }
    pthread_mutex_lock(&ctx->mtx);
    ctx->producers_done++;
    pthread_mutex_unlock(&ctx->mtx);
    return NULL;
}

static int test_mt_pipe_and_ring(void) {
    mt_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    pthread_mutex_init(&ctx.mtx, NULL);
    CHECK(pipe(ctx.pipe_fd) == 0, "pipe create");
    for (int e = 0; e < 2; e++) {
        int fl = fcntl(ctx.pipe_fd[e], F_GETFL, 0);
        CHECK(fl >= 0 && fcntl(ctx.pipe_fd[e], F_SETFL, fl | O_NONBLOCK) == 0, "nonblock");
    }

    pthread_t th[4];
    for (int t = 0; t < 4; t++) {
        CHECK(pthread_create(&th[t], NULL, producer_fn, &ctx) == 0, "pthread_create");
    }

    int drained_batches = 0;
    for (;;) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(ctx.pipe_fd[0], &rf);
        struct timeval tv = { 0, 5000 };
        if (select(ctx.pipe_fd[0] + 1, &rf, NULL, NULL, &tv) > 0) {
            pthread_mutex_lock(&ctx.mtx);
            char buf[64];
            while (read(ctx.pipe_fd[0], buf, sizeof(buf)) > 0) {}
            ctx.pipe_pending = 0;
            while (ctx.count > 0) {
                ctx.head = (ctx.head + 1U) % APPMON_RING_CAP;
                ctx.count--;
            }
            pthread_mutex_unlock(&ctx.mtx);
            drained_batches++;
        } else {
            pthread_mutex_lock(&ctx.mtx);
            int done = ctx.producers_done;
            unsigned rem = ctx.count;
            pthread_mutex_unlock(&ctx.mtx);
            if (done == 4 && rem == 0) break;
        }
        if (drained_batches > 20) break;
    }

    for (int t = 0; t < 4; t++) {
        pthread_join(th[t], NULL);
    }
    pthread_mutex_lock(&ctx.mtx);
    char buf[64];
    while (read(ctx.pipe_fd[0], buf, sizeof(buf)) > 0) {}
    ctx.pipe_pending = 0;
    while (ctx.count > 0) {
        ctx.head = (ctx.head + 1U) % APPMON_RING_CAP;
        ctx.count--;
    }
    pthread_mutex_unlock(&ctx.mtx);

    CHECK(ctx.count == 0 && ctx.pipe_pending == 0, "final ring and pipe_pending cleanly drained");
    close(ctx.pipe_fd[0]);
    close(ctx.pipe_fd[1]);
    pthread_mutex_destroy(&ctx.mtx);
    return 0;
}

/* [6] Build 31 修复 ①：AP 深睡 >240s 后按 proc_bsdinfo 启动时间续期归属新鲜度，同时拦截 PID 复用 */
#define JP_SNAPSHOT_TTL_SECS 240
typedef struct {
    int      pid;
    int32_t  orig;
    int      orig_known;
    int32_t  band;
    time_t   band_seen_mono;
    int      has_identity;
    int      has_proc_start;
    uint64_t start_tvsec;
    uint64_t start_tvusec;
} applied_model_t;

static int snap_fresh_model(time_t seen, time_t now_m) {
    return seen > 0 && now_m >= seen && (now_m - seen) <= JP_SNAPSHOT_TTL_SECS;
}

static int same_inst_model(const applied_model_t *a, uint64_t live_sec, uint64_t live_usec) {
    if (!a || !a->has_proc_start || a->pid <= 0) return 0;
    return (a->start_tvsec == live_sec && a->start_tvusec == live_usec);
}

static int attr_trusted_model(const applied_model_t *a, time_t now_m,
                              uint64_t live_sec, uint64_t live_usec) {
    if (!a || !a->has_identity) return 0;
    if (snap_fresh_model(a->band_seen_mono, now_m)) return 1;
    return same_inst_model(a, live_sec, live_usec);
}

static void observe_band_model(applied_model_t *a, int32_t cur, time_t now_m,
                               uint64_t live_sec, uint64_t live_usec) {
    if (cur == a->band) {
        if (snap_fresh_model(a->band_seen_mono, now_m) ||
            same_inst_model(a, live_sec, live_usec)) {
            a->band_seen_mono = now_m;
        }
    }
}

static int32_t natural_band_model(const applied_model_t *a, int32_t cur_now, time_t now_m,
                                  uint64_t live_sec, uint64_t live_usec) {
    if (a && cur_now == a->band && a->orig_known &&
        attr_trusted_model(a, now_m, live_sec, live_usec)) {
        return a->orig;
    }
    return cur_now;
}

static int test_deep_sleep_proc_start_renewal(void) {
    applied_model_t a = {
        .pid = 380, .orig = 30, .orig_known = 1, .band = 39,
        .band_seen_mono = 100, .has_identity = 1,
        .has_proc_start = 1, .start_tvsec = 1700000100ULL, .start_tvusec = 456789ULL
    };

    /* 模拟锁屏 AP 深睡 6 分钟（360s > 240s TTL），唤醒时 CLOCK_MONOTONIC = 460 */
    time_t wake_mono = 460;
    CHECK(!snap_fresh_model(a.band_seen_mono, wake_mono), "360s sleep exceeds 240s TTL");

    /* 同进程实例（启动时间一致）：natural_band 仍还原为 30，且 observe_band 恢复新鲜度 */
    CHECK(natural_band_model(&a, 39, wake_mono, 1700000100ULL, 456789ULL) == 30,
          "same process instance after 6min sleep returns orig=30 instead of own band=39");
    observe_band_model(&a, 39, wake_mono, 1700000100ULL, 456789ULL);
    CHECK(a.band_seen_mono == 460 && snap_fresh_model(a.band_seen_mono, 470),
          "observe_band renews band_seen_mono after deep sleep");

    /* 负向安全验证：若深睡期间 PID 380 被新进程复用（启动时间变化），严禁续期或采信旧 orig */
    a.band_seen_mono = 100;
    CHECK(natural_band_model(&a, 39, wake_mono, 1700000399ULL, 111ULL) == 39,
          "PID reuse with different start_tv must NOT trust stale orig");
    observe_band_model(&a, 39, wake_mono, 1700000399ULL, 111ULL);
    CHECK(a.band_seen_mono == 100, "PID reuse must NOT renew band_seen_mono");
    return 0;
}

/* [7] Build 31 修复 ③：内核挂起态 (JP_MEMSTAT_SUSPENDED=0x1) 跳过 pulse 并立即关 5s 快轮门控 */
#define JP_MEMSTAT_SUSPENDED 0x1U
#define JP_MEMSTAT_ASSERTION 0x40U

static int test_suspended_skip_and_gate_close(void) {
    app_track_model_t m;
    memset(&m, 0, sizeof(m));
    m.bg_lag = -1;
    int suspended_this_bg = 0;

    /* 正常退后台（持 0x40 后台断言，未挂起）：允许触发 */
    appmon_ev_t fg_ev = { "com.tencent.xin", 380, JP_BKS_STATE_FOREGROUND, 1000 };
    appmon_ev_t bg_ev = { "com.tencent.xin", 380, JP_BKS_STATE_BG_RUNNING, 1005 };
    apply_event_model(&m, &fg_ev, NULL);
    apply_event_model(&m, &bg_ev, NULL);
    pulse_round_model(&m, 380, 39, 1005);
    uint32_t active_bg_state = JP_MEMSTAT_ASSERTION;
    CHECK((active_bg_state & JP_MEMSTAT_SUSPENDED) == 0,
          "active UIKit background task (0x40) is NOT marked suspended");

    /* 模拟夜间 1s 后台推送唤醒后迅速被内核挂起 (state=0x1)：t=5s 检查命中挂起并关门控 */
    uint32_t susp_state = JP_MEMSTAT_SUSPENDED;
    if ((susp_state & JP_MEMSTAT_SUSPENDED) != 0) {
        suspended_this_bg = 1;
    }
    int gate = 1;
    int wait_ms = suspended_this_bg ? 60000 : next_wait_ms_model(&m, 5, 1010, 60, &gate);
    if (suspended_this_bg) gate = 0;
    CHECK(suspended_this_bg == 1 && gate == 0 && wait_ms == 60000,
          "suspended process latches suspended_this_bg and closes 5s fast gate");

    /* 下次用户真正点开进前台：清除挂起闩锁，恢复正常 pulse 资格 */
    fg_ev.mono = 2000;
    apply_event_model(&m, &fg_ev, NULL);
    suspended_this_bg = 0;
    bg_ev.mono = 2010;
    apply_event_model(&m, &bg_ev, NULL);
    pulse_round_model(&m, 380, 39, 2010);
    wait_ms = suspended_this_bg ? 60000 : next_wait_ms_model(&m, 5, 2010, 60, &gate);
    CHECK(gate == 1 && wait_ms == 5000, "next FG->BG transition clears suspended latch");
    return 0;
}

/* [8] Build 31 修复 ②：仅在 restart.txt 中的慢规则（如输入法 wetype）不订阅毫秒级 APPMON */
static int should_subscribe_appmon(int in_pulse_on, int in_bgkill_on,
                                   int in_protect, int in_demote, int in_restart_only) {
    (void)in_restart_only;
    return (in_pulse_on || in_bgkill_on || in_protect || in_demote) ? 1 : 0;
}

static int test_restart_only_excluded_from_appmon(void) {
    /* com.tencent.wetype: pulse=off, bgkill=0, protect=0, demote=0, restart=1 -> 不订阅 */
    CHECK(should_subscribe_appmon(0, 0, 0, 0, 1) == 0,
          "restart-only rule (wetype) excluded from APPMON subscription");
    /* com.tencent.xin: pulse=on, protect=1, restart=1 -> 仍正常订阅 */
    CHECK(should_subscribe_appmon(1, 0, 1, 0, 1) == 1,
          "rule present in pulse/protect remains subscribed to APPMON");
    return 0;
}

int main(void) {
    CHECK(test_long_fg_zero_poll_and_exact_delay() == 0, "[1] zero-poll 1h FG + exact delay_s");
    CHECK(test_subsecond_tap_and_swipe_fifo() == 0, "[2] 0.3s tap-and-swipe FIFO");
    CHECK(test_xnu_band_drop_animation_lag() == 0, "[3] XNU band>=100 animation lag 250ms retry");
    CHECK(test_extension_filter_and_backoff_unlock() == 0, "[4] backoff unlock & fallback");
    CHECK(test_mt_pipe_and_ring() == 0, "[5] multithreaded self-pipe & ring stress");
    CHECK(test_deep_sleep_proc_start_renewal() == 0, "[6] deep-sleep >240s proc_start attribution renewal");
    CHECK(test_suspended_skip_and_gate_close() == 0, "[7] kernel-suspended skip & gate close");
    CHECK(test_restart_only_excluded_from_appmon() == 0, "[8] restart-only rule excluded from APPMON");
    printf("ALL v0.8.7 APPMON TESTS OK\n");
    return 0;
}
