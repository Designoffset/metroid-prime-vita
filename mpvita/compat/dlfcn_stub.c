#include "dlfcn.h"
void* dlopen(const char* f, int m) { (void)f; (void)m; return 0; }
void* dlsym(void* h, const char* n) { (void)h; (void)n; return 0; }
int dlclose(void* h) { (void)h; return 0; }
char* dlerror(void) { static char msg[] = "dynamic loading is not supported on PS Vita"; return msg; }
int dladdr(const void* a, Dl_info* i) { (void)a; (void)i; return 0; }
