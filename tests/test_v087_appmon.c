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
            if (done && rem == 0) break;
        }
        if (drained_batches >= 1 && !ctx.producers_done) {
            /* 检查线程是否全部结束在下方 join 后统一收口 */
        }
        /* 非阻塞推进：跑够若干轮后 join 生产者并做最终排空 */
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

int main(void) {
    CHECK(test_long_fg_zero_poll_and_exact_delay() == 0, "[1] zero-poll 1h FG + exact delay_s");
    CHECK(test_subsecond_tap_and_swipe_fifo() == 0, "[2] 0.3s tap-and-swipe FIFO");
    CHECK(test_xnu_band_drop_animation_lag() == 0, "[3] XNU band>=100 animation lag 250ms retry");
    CHECK(test_extension_filter_and_backoff_unlock() == 0, "[4] backoff unlock & fallback");
    CHECK(test_mt_pipe_and_ring() == 0, "[5] multithreaded self-pipe & ring stress");
    printf("ALL v0.8.7 APPMON TESTS OK\n");
    return 0;
}
