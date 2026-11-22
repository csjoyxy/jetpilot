#ifndef STUB_SYS_SYSCTL_H
#define STUB_SYS_SYSCTL_H
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>
#define CTL_KERN 1
#define KERN_BOOTTIME 21
struct kinfo_proc_stub { int filler; };
extern int sysctl(const int *name, unsigned namelen, void *oldp, size_t *oldlenp, const void *newp, size_t newlen);
/* Darwin sysctlbyname：让仅 -I stub 的语法校验也可复现零告警 */
extern int sysctlbyname(const char *name, void *oldp, size_t *oldlenp, const void *newp, size_t newlen);
#endif
