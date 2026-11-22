/* Linux syntax-check stub for Darwin notify.h */
#ifndef STUB_NOTIFY_H
#define STUB_NOTIFY_H
#include <stdint.h>
#define NOTIFY_STATUS_OK 0u
typedef uint32_t notify_status_t; 
extern uint32_t notify_register_file_descriptor(const char *name, int *notify_fd, int flags, int *notify_token);
extern uint32_t notify_get_state(int token, uint64_t *state);
#endif
