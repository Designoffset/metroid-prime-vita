#pragma once
/* PS Vita: only the thread-id query the port uses. */
#include <psp2/kernel/threadmgr.h>
#define SYS_gettid 1
#define SYS_tgkill 2
#ifdef __cplusplus
extern "C" {
#endif
static inline long syscall(long number, ...) { return number == SYS_gettid ? (long)sceKernelGetThreadId() : -1; }
#ifdef __cplusplus
}
#endif
