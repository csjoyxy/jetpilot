#ifndef STUB_MACH_TIME_H
#define STUB_MACH_TIME_H
#include <stdint.h>
struct mach_timebase_info { uint32_t numer; uint32_t denom; };
typedef struct mach_timebase_info mach_timebase_info_data_t;
extern int mach_timebase_info(mach_timebase_info_data_t *info);
#endif
