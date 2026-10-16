#pragma once
/* PS Vita: no ioctl(); sqlite only uses it for Linux-specific features. */
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
static inline int ioctl(int fd, unsigned long req, ...) { (void)fd; (void)req; return -1; }
#ifdef __cplusplus
}
#endif
