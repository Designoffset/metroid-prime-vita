/* POSIX functions missing from the PS Vita newlib. */
#include <errno.h>
#include <malloc.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
#include <pthread.h>

/* Stream locking: one recursive lock shared by all streams (enough for fmt's
   whole-line printing). */
static pthread_mutex_t g_stream_lock;
static pthread_once_t g_stream_once = PTHREAD_ONCE_INIT;
static void stream_lock_init(void) {
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&g_stream_lock, &a);
  pthread_mutexattr_destroy(&a);
}
void flockfile(FILE* f) { (void)f; pthread_once(&g_stream_once, stream_lock_init); pthread_mutex_lock(&g_stream_lock); }
void funlockfile(FILE* f) { (void)f; pthread_mutex_unlock(&g_stream_lock); }
int ftrylockfile(FILE* f) { (void)f; pthread_once(&g_stream_once, stream_lock_init); return pthread_mutex_trylock(&g_stream_lock) == 0 ? 0 : 1; }

int posix_memalign(void** out, size_t alignment, size_t size) {
  if (alignment < sizeof(void*) || (alignment & (alignment - 1))) return EINVAL;
  void* p = memalign(alignment, size ? size : 1);
  if (!p) return ENOMEM;
  *out = p;
  return 0;
}

/* No processes, terminals, symlinks or user database on the Vita. */
int dup2(int a, int b) { (void)a; (void)b; errno = ENOSYS; return -1; }
pid_t waitpid(pid_t p, int* s, int o) { (void)p; (void)s; (void)o; errno = ECHILD; return -1; }
int execvp(const char* f, char* const a[]) { (void)f; (void)a; errno = ENOSYS; return -1; }
char* ttyname(int fd) { (void)fd; errno = ENOTTY; return NULL; }
ssize_t readlink(const char* p, char* b, size_t n) { (void)p; (void)b; (void)n; errno = EINVAL; return -1; }
struct passwd;
struct passwd* getpwuid(uid_t u) { (void)u; return NULL; }
int fchown(int fd, uid_t u, gid_t g) { (void)fd; (void)u; (void)g; return 0; }
