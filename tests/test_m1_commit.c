#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <assert.h>

static char CONFIG_DIR_BUF[512];
#define CONFIG_DIR CONFIG_DIR_BUF

/* M1: 提交 tmp 内容到 final——fflush → ferror → fsync(数据) → fclose →
 * rename → fsync(CONFIG_DIR 目录)。rename 前数据已落盘、rename 后目录项
 * 已落盘，崩溃/掉电不会再出现「旧文件被空/截断文件替换」；任一步失败清
 * 理 tmp 并返回 -1，旧版本目标文件保持完整。所有状态文件都在 CONFIG_DIR，
 * 目录 fsync 只对该目录做一次。 */
static int jp_atomic_commit(FILE *f, const char *tmp_path, const char *final_path) {
    if (!f || !tmp_path || !final_path) {
        if (f) fclose(f);
        if (tmp_path) unlink(tmp_path);
        return -1;
    }
    if (fflush(f) != 0 || ferror(f)) {
        fclose(f);
        unlink(tmp_path);
        return -1;
    }
    if (fsync(fileno(f)) != 0) {
        fclose(f);
        unlink(tmp_path);
        return -1;
    }
    fclose(f);
    if (rename(tmp_path, final_path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    int dfd = open(CONFIG_DIR, O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        (void)fsync(dfd);
        close(dfd);
    }
    return 0;
}

static char  *g_last_body = NULL;   /* 上次成功写盘的内容（不含 updated= 行） */
static size_t g_last_body_len = 0;
static time_t g_last_write = 0;     /* 上次成功写盘的 CLOCK_MONOTONIC 秒 */
static int    g_restart_cfg_bad_count = 0; /* restart.txt 无效行数 (v0.7.8 C1-N6) */
static void pulse_write_status(FILE *m);
static void bgkill_write_status(FILE *m);

static char *read_file(const char *p) {
    static char buf[4096];
    FILE *f = fopen(p, "r");
    if (!f) return NULL;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0; fclose(f);
    return buf;
}

int main(void) {
    char td[] = "/tmp/jp_commit_test_XXXXXX";
    assert(mkdtemp(td) != NULL);
    snprintf(CONFIG_DIR_BUF, sizeof(CONFIG_DIR_BUF), "%s", td);
    char tmp[600], final[600];
    snprintf(tmp, sizeof(tmp), "%s/state.tmp", td);
    snprintf(final, sizeof(final), "%s/state.txt", td);

    /* 1) 成功路径：内容落盘、tmp 不残留 */
    int fd = open(tmp, O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW, 0644);
    assert(fd >= 0);
    FILE *f = fdopen(fd, "w");
    fprintf(f, "hello=1\n");
    assert(jp_atomic_commit(f, tmp, final) == 0);
    assert(strcmp(read_file(final), "hello=1\n") == 0);
    assert(access(tmp, F_OK) != 0);
    printf("[1] success path OK (data persisted, tmp removed)\n");

    /* 2) 覆盖已有文件：新内容替换旧内容 */
    fd = open(tmp, O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW, 0644);
    assert(fd >= 0);
    f = fdopen(fd, "w");
    fprintf(f, "hello=2\n");
    assert(jp_atomic_commit(f, tmp, final) == 0);
    assert(strcmp(read_file(final), "hello=2\n") == 0);
    printf("[2] overwrite path OK\n");

    /* 3) rename 失败（目标目录不存在）：返回 -1、tmp 清理、旧文件保持完整 */
    char bad_final[700];
    snprintf(bad_final, sizeof(bad_final), "%s/no_such_dir/state.txt", td);
    fd = open(tmp, O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW, 0644);
    assert(fd >= 0);
    f = fdopen(fd, "w");
    fprintf(f, "corrupt=1\n");
    assert(jp_atomic_commit(f, tmp, bad_final) == -1);
    assert(access(tmp, F_OK) != 0);               /* tmp 已清理 */
    assert(strcmp(read_file(final), "hello=2\n") == 0); /* 旧版本文件完整 */
    printf("[3] rename-failure path OK (tmp cleaned, old file intact)\n");

    /* 4) 空 FILE* 防御：返回 -1 且不崩溃 */
    assert(jp_atomic_commit(NULL, tmp, final) == -1);
    printf("[4] null-arg guard OK\n");

    /* 5) 目录 fsync 生效性（Linux 上 open(O_DIRECTORY)+fsync 不报错即视为通过） */
    int dfd = open(td, O_RDONLY|O_DIRECTORY);
    assert(dfd >= 0);
    assert(fsync(dfd) == 0);
    close(dfd);
    printf("[5] directory fsync OK\n");

    unlink(final);
    rmdir(td);
    printf("ALL M1 HELPER TESTS OK\n");
    return 0;
}
