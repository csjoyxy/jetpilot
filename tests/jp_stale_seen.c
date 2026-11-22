/* jp_stale_seen.c — 批次2 P2（代码审核）seen 残留缺陷的最小复现与修复验证。
 *
 * 缺陷（已修复）：fast_round_cmd3 部分进度（某条 cmd3 成功置 seen=1）后、后续条目
 * 非 ESRCH 失败 return -1 不 prune，已置 seen=1 的条目带陈旧 seen 进下一轮
 * LISTSCAN——轮间退出的 pid 不在全表，listscan 对不在全表的条目不清 seen，
 * prune 把死进程条目多保留一轮（互审员记录的 "listscan KEPT 泄漏"）。
 * 修复：src/jetpilotd.c fast_round_cmd3 降级分支 return -1 前清全表 seen。
 *
 * 本文件是自包含迷你模型（非源码提取）：断言「旧逻辑下泄漏确实发生」+
 * 「清零后确实剪除」，作为该缺陷的回归文档保留。运行它应当通过；
 * 它测的是缺陷模型，不是生产源码——生产路径的回归在 test_e1e4.c 场景 C2。
 */
#include <stdio.h>
#include <assert.h>
#include <string.h>

#define MOCK_ALIVE  0    /* cmd3 成功 */
#define MOCK_EINVAL 1    /* 非 ESRCH 异常失败 */
#define MOCK_ESRCH  2    /* 进程退出 */

#define MAX_APPLIED 4
typedef struct {
    int pid;
    int seen;
} applied_t;
static applied_t g_applied[MAX_APPLIED];
static int g_napplied = 0;

static int mock_prio_rc[4];      /* 按 pid 序返回 MOCK_* */
static int mock_list_pid[4];     /* 下一轮 get_list 全表内容 */
static int mock_nlist = 0;

/* 迷你 cmd3 快轮：仅模拟 seen 生命周期。clear_seen_on_error=0 为 pre-fix 逻辑。 */
static int mini_cmd3_round(int clear_seen_on_error) {
    for (int i = 0; i < g_napplied; i++) {
        applied_t *a = &g_applied[i];
        int rc = mock_prio_rc[i];
        if (rc == MOCK_ESRCH) continue;          /* 退出：不置 seen */
        if (rc == MOCK_EINVAL) {
            if (clear_seen_on_error)             /* 修复：清本轮部分观察 */
                for (int k = 0; k < g_napplied; k++) g_applied[k].seen = 0;
            return -1;                            /* pre-fix 与修复版在此分叉 */
        }
        a->seen = 1;                              /* 部分进度：成功条目已置 seen */
    }
    return 0;
}

/* 迷你 listscan 快轮：只对在全表中的条目置 seen（不在全表 = 轮间退出）。 */
static void mini_listscan_round(void) {
    for (int i = 0; i < g_napplied; i++) {
        applied_t *a = &g_applied[i];
        int in_list = 0;
        for (int j = 0; j < mock_nlist; j++)
            if (mock_list_pid[j] == a->pid) { in_list = 1; break; }
        if (!in_list) continue;                   /* 缺陷放大点：不清 seen */
        a->seen = 1;
    }
}

/* 与生产 applied_prune 同语义：seen=0 剪除并压实。 */
static void mini_prune(void) {
    int j = 0;
    for (int i = 0; i < g_napplied; i++) {
        if (g_applied[i].seen) {
            g_applied[i].seen = 0;
            g_applied[j++] = g_applied[i];
        }
    }
    g_napplied = j;
}

static void setup(int pid_a, int pid_b) {
    g_napplied = 0;
    /* 创建时 seen=1（同生产 applied_set_ex），当轮末 prune 重置为 0 */
    g_applied[g_napplied].pid = pid_a; g_applied[g_napplied].seen = 1; g_napplied++;
    g_applied[g_napplied].pid = pid_b; g_applied[g_napplied].seen = 1; g_napplied++;
    mini_prune();
}

int main(void) {
    /* --- pre-fix 行为：部分进度 + 不清 seen → 死条目 KEPT 泄漏 --- */
    setup(100, 200);
    mock_prio_rc[0] = MOCK_ALIVE;    /* pid100 cmd3 成功 → seen=1 */
    mock_prio_rc[1] = MOCK_EINVAL;   /* pid200 异常失败 → 降级 return -1 */
    assert(mini_cmd3_round(0) == -1);
    assert(g_applied[0].seen == 1 && g_applied[1].seen == 0);
    /* 轮间 pid100 退出：下一轮全表只剩 200 */
    mock_nlist = 1; mock_list_pid[0] = 200;
    mini_listscan_round();
    mini_prune();
    int kept_dead = 0;
    for (int i = 0; i < g_napplied; i++) if (g_applied[i].pid == 100) kept_dead = 1;
    assert(kept_dead && g_napplied == 2);   /* 缺陷：死 pid100 被 KEPT */
    printf("[pre-fix] dead pid 100 KEPT after listscan prune (defect reproduced)\n");

    /* --- 修复后行为：降级前清 seen → 下一轮死条目剪除 --- */
    setup(100, 200);
    mock_prio_rc[0] = MOCK_ALIVE;
    mock_prio_rc[1] = MOCK_EINVAL;
    assert(mini_cmd3_round(1) == -1);
    assert(g_applied[0].seen == 0 && g_applied[1].seen == 0);   /* 残留清零 */
    mock_nlist = 1; mock_list_pid[0] = 200;
    mini_listscan_round();
    mini_prune();
    assert(g_napplied == 1 && g_applied[0].pid == 200);   /* 死 pid100 剪除 */
    printf("[fixed]   dead pid 100 PRUNED (seen-clear verified)\n");

    printf("jp_stale_seen: defect model documented, fix behavior verified\n");
    return 0;
}
