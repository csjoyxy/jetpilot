/* 【负向用例】test_t2_oldbug.c：把 reader 换成旧双解锁变体（check_restart_thr_OLD_BUG），
 * 复现批次1 T2 修复前的缺陷。运行它会挂死（writer 在 wrlock 上永久阻塞，需 timeout 杀），
 * 挂起 = 预期结果，证明 T2 harness 具备检出该类 bug 的能力。正常 harness 是 test_t2_rwlock.c。
 * 原始说明：从 jetpilotd.c 提取修复后的重启阈值读锁片段（check_restart_thr），
 * 覆盖三条路径（无匹配 / 匹配且 fp<=thr / 匹配且 fp>thr），
 * 并发 writer 压力下验证锁平衡；另含旧双解锁变体（check_restart_thr_OLD_BUG）
 * 用于负向验证 harness 的检出能力（默认不调用）。 */
#include <stdio.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <assert.h>
#include <unistd.h>

#define RESTART_MAX_RULES 64
typedef struct {
    char    bid[128];
    int32_t thr_mb;
    int32_t bg_sec;
    int32_t max_count;
    int32_t relaunch;
} restart_rule_t;
static restart_rule_t g_restart_rules[RESTART_MAX_RULES];
static int g_nrestart_rules = 0;
static pthread_rwlock_t s_rule_lock = PTHREAD_RWLOCK_INITIALIZER;

/* === 提取自 src/jetpilotd.c pulse_revalidate_conditions（修复后） === */
static int check_restart_thr(const char *bid, uint64_t cur_fp) {
/* H2: 重启规则数组读锁——锁内只读值、锁外判断，保证单次 unlock。
     * （复审修复：原写法在「匹配且 fp ≤ thr_mb」路径下循环内 unlock 后
     * break 又落到循环外 unlock，双重 unlock 属 POSIX 未定义行为） */
    pthread_rwlock_rdlock(&s_rule_lock);
    int over_thr = 0;
    for (int r = 0; r < g_nrestart_rules; r++) {
        if (strcmp(g_restart_rules[r].bid, bid) == 0) {
            over_thr = (cur_fp > (uint64_t)g_restart_rules[r].thr_mb);
            break;
        }
    }
    pthread_rwlock_unlock(&s_rule_lock);
    if (over_thr) return 1;  /* over_thr -> 被阈值拦截，包装为 0/1：1=拦截 0=放行 */
    return 0;
}

/* === 旧（双解锁 bug）变体，仅用于负向验证，不被调用 === */
__attribute__((unused)) static int check_restart_thr_OLD_BUG(const char *bid, uint64_t cur_fp) {
    pthread_rwlock_rdlock(&s_rule_lock);
    for (int r = 0; r < g_nrestart_rules; r++) {
        if (strcmp(g_restart_rules[r].bid, bid) == 0) {
            int over_thr = (cur_fp > (uint64_t)g_restart_rules[r].thr_mb);
            pthread_rwlock_unlock(&s_rule_lock); /* 循环内解锁 */
            if (over_thr) return 1;
            break;                                /* 落到循环外再解锁一次 */
        }
    }
    pthread_rwlock_unlock(&s_rule_lock);
    return 0;
}

static _Atomic int writer_stop = 0;  /* P2 修复：volatile 改 _Atomic，TSan 数据竞争归零 */
static void *writer_fn(void *arg) {
    (void)arg;
    while (!atomic_load_explicit(&writer_stop, memory_order_acquire)) {
        pthread_rwlock_wrlock(&s_rule_lock);
        g_restart_rules[0].thr_mb++;
        pthread_rwlock_unlock(&s_rule_lock);
        usleep(100);
    }
    return NULL;
}

static void *reader_fn(void *arg) {
    long n = (long)arg;
    for (long i = 0; i < n; i++) {
        const char *bids[3] = {"com.a.one", "com.b.two", "com.c.none"};
        uint64_t fps[3]    = {50, 300, 99999};
        check_restart_thr_OLD_BUG(bids[i % 3], fps[i % 3]); /* 负向：用双解锁变体跑同一压力，预期挂死 */
    }
    return NULL;
}

int main(void) {
    strcpy(g_restart_rules[0].bid, "com.a.one");
    g_restart_rules[0].thr_mb = 200;
    strcpy(g_restart_rules[1].bid, "com.b.two");
    g_restart_rules[1].thr_mb = 200;
    g_nrestart_rules = 2;

    /* 三路径单次语义检查 */
    assert(check_restart_thr("com.a.one", 50)    == 0); /* 匹配且 fp<=thr：放行 */
    assert(check_restart_thr("com.a.one", 99999) == 1); /* 匹配且 fp>thr：拦截 */
    assert(check_restart_thr("com.c.none", 50)   == 0); /* 无匹配：放行 */
    printf("[1] three paths semantics OK\n");

    /* 并发压力：8 reader × 20000 次（三路径轮换）+ writer 线程 */
    pthread_t writers[2], readers[8];
    for (int i = 0; i < 2; i++) pthread_create(&writers[i], NULL, writer_fn, NULL);
    for (int i = 0; i < 8; i++) pthread_create(&readers[i], NULL, reader_fn, (void *)20000L);
    for (int i = 0; i < 8; i++) pthread_join(readers[i], NULL);
    atomic_store_explicit(&writer_stop, 1, memory_order_release);
    for (int i = 0; i < 2; i++) pthread_join(writers[i], NULL);
    printf("[2] 160k concurrent reads + writers survived (no deadlock)\n");

    /* 锁平衡检查：reader/writer 计数必须归零，try 系列应立即成功 */
    assert(pthread_rwlock_tryrdlock(&s_rule_lock) == 0);
    pthread_rwlock_unlock(&s_rule_lock);
    assert(pthread_rwlock_trywrlock(&s_rule_lock) == 0);
    pthread_rwlock_unlock(&s_rule_lock);
    printf("[3] lock balance OK (tryrd/trywr both immediate)\n");

    printf("ALL T2 LOCK-BALANCE TESTS OK\n");
    return 0;
}
