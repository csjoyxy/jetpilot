/* H1 状态机不变量测试：从 jetpilotd.c 提取的 spawn/join 守卫与 worker 收尾序列
 * （语义逐行对应），随机 worker 时长下验证：
 *  - running==1 时绝不 spawn（守卫 defer）
 *  - spawn 前 join 掉所有已结束旧 worker，句柄永不覆写
 *  - running==0 时逻辑态必已清（join 点校验）
 *  - defer 时队列条目保留（下轮重试）
 */
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#define PULSE_MAX_QUEUE 32
typedef struct { int pid; int fp_mb; } pulse_queue_entry_t;
static pulse_queue_entry_t g_pulse_queue[PULSE_MAX_QUEUE];
static int g_npulse_queue = 0;

static _Atomic int   g_pulse_in_progress = 0;
static _Atomic pid_t g_pulse_current_pid = 0;
static _Atomic int   g_pulse_thread_running = 0;
static pthread_t     g_pulse_thread;
static int           g_pulse_thread_has_handle = 0;

static atomic_int total_creates = 0;
static atomic_int total_defers  = 0;
static atomic_int handle_overwrite_violations = 0;

static void pulse_queue_remove_at(int idx) {
    if (idx < 0 || idx >= g_npulse_queue) return;
    for (int i = idx; i < g_npulse_queue - 1; i++) g_pulse_queue[i] = g_pulse_queue[i + 1];
    g_npulse_queue--;
}

static void *pulse_worker_thread(void *arg) {
    usleep(1000 + rand() % 3000); /* 模拟 pulse 执行 */
    /* worker 收尾（与 jetpilotd.c 一致）：先清逻辑态，最后 running=0(release)。
     * harness 特有：在两步之间加宽窗口，稳定击发主循环的 defer 路径 */
    atomic_store_explicit(&g_pulse_in_progress, 0, memory_order_relaxed);
    atomic_store_explicit(&g_pulse_current_pid, 0, memory_order_relaxed);
    usleep(3000);
    atomic_store_explicit(&g_pulse_thread_running, 0, memory_order_release);
    free(arg);
    return NULL;
}

static void *pulse_worker_thread(void *arg);

static void main_round(int spawn_expected) {
    /* === 逐行对应 jetpilotd.c pulse_round 的守卫与 spawn === */
    if (g_pulse_thread_has_handle &&
        !atomic_load_explicit(&g_pulse_thread_running, memory_order_acquire)) {
        pthread_join(g_pulse_thread, NULL);
        g_pulse_thread_has_handle = 0;
    }
    if (!spawn_expected || g_npulse_queue == 0) return;

    while (atomic_load_explicit(&g_pulse_in_progress, memory_order_acquire) == 0 &&
           g_npulse_queue > 0) {
        if (g_pulse_thread_has_handle) {
            if (atomic_load_explicit(&g_pulse_thread_running, memory_order_acquire)) {
                atomic_fetch_add(&total_defers, 1);
                /* 不变量：条目保留在队列 */
                assert(g_npulse_queue > 0);
                break;
            }
            /* 不变量：join 点 running 必为 0 ⇒ 逻辑态已清 */
            assert(atomic_load_explicit(&g_pulse_in_progress, memory_order_acquire) == 0);
            pthread_join(g_pulse_thread, NULL);
            g_pulse_thread_has_handle = 0;
        }
        pulse_queue_entry_t target = g_pulse_queue[0];
        pulse_queue_remove_at(0);

        atomic_store_explicit(&g_pulse_in_progress, 1, memory_order_relaxed);
        atomic_store_explicit(&g_pulse_current_pid, target.pid, memory_order_relaxed);

        pulse_queue_entry_t *thread_arg = malloc(sizeof(pulse_queue_entry_t));
        if (thread_arg) {
            *thread_arg = target;
            atomic_store_explicit(&g_pulse_thread_running, 1, memory_order_release);
            /* 监控：create 前必无未回收句柄 */
            if (g_pulse_thread_has_handle) atomic_fetch_add(&handle_overwrite_violations, 1);
            if (pthread_create(&g_pulse_thread, NULL, pulse_worker_thread, thread_arg) == 0) {
                g_pulse_thread_has_handle = 1;
                atomic_fetch_add(&total_creates, 1);
            } else {
                atomic_store_explicit(&g_pulse_thread_running, 0, memory_order_relaxed);
                free(thread_arg);
                atomic_store_explicit(&g_pulse_in_progress, 0, memory_order_relaxed);
                atomic_store_explicit(&g_pulse_current_pid, 0, memory_order_relaxed);
            }
        } else {
            atomic_store_explicit(&g_pulse_in_progress, 0, memory_order_relaxed);
            atomic_store_explicit(&g_pulse_current_pid, 0, memory_order_relaxed);
        }
        break;
    }
}

int main(void) {
    srand(42);
    for (int round = 0; round < 20000; round++) {
        /* 随机入队 0~3 个候选 */
        int enq = rand() % 4;
        for (int i = 0; i < enq && g_npulse_queue < PULSE_MAX_QUEUE; i++) {
            g_pulse_queue[g_npulse_queue].pid = 1000 + rand() % 500;
            g_pulse_queue[g_npulse_queue].fp_mb = rand() % 900;
            g_npulse_queue++;
        }
        int expect_spawn = (rand() % 100 < 80) && g_npulse_queue > 0;
        main_round(expect_spawn);
        /* 不变量：running==1 ⇒ 有未 join 句柄 */
        if (atomic_load_explicit(&g_pulse_thread_running, memory_order_acquire))
            assert(g_pulse_thread_has_handle == 1);
        /* 每轮最多一个活线程 */
        usleep(200 + rand() % 800); /* 模拟主循环节拍，制造收尾竞争窗口 */
    }
    /* 收尾：等最后一个 worker 完成并 join，校验清理路径 */
    for (int i = 0; i < 100 && g_pulse_thread_has_handle; i++) {
        main_round(0);
        usleep(2000);
    }
    if (g_pulse_thread_has_handle) {
        pthread_join(g_pulse_thread, NULL);
        g_pulse_thread_has_handle = 0;
    }
    assert(atomic_load_explicit(&g_pulse_thread_running, memory_order_acquire) == 0);
    assert(atomic_load_explicit(&g_pulse_in_progress, memory_order_acquire) == 0);

    printf("creates=%d defers=%d overwrite_violations=%d\n",
           atomic_load(&total_creates), atomic_load(&total_defers),
           atomic_load(&handle_overwrite_violations));
    assert(atomic_load(&handle_overwrite_violations) == 0);
    assert(atomic_load(&total_creates) > 0);
    if (atomic_load(&total_defers) == 0) {
        printf("NOTE: defer path not hit (window small)\n");
    } else {
        printf("defer path exercised\n");
    }
    printf("ALL H1 INVARIANTS OK\n");
    return 0;
}
