/* The Vita's newlib is built without C99 printf sizes: "%zu" prints "zu" and
 * leaves the argument in place, so the next "%s" reads a number as a string
 * pointer and crashes. Every printf-family call ends in one of the functions
 * wrapped here (-Wl,--wrap): the format is rewritten first, dropping the z and t
 * sizes (size_t and ptrdiff_t are 32 bits here) and turning j into ll. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct _reent;

static const char* fix_format(const char* fmt, char* buf, size_t cap) {
  if (fmt == NULL) return fmt;
  const char* p = strchr(fmt, '%');
  if (p == NULL) return fmt;
  int needed = 0;
  for (const char* q = p; *q; ++q)
    if (*q == 'z' || *q == 't' || *q == 'j') {
      needed = 1;
      break;
    }
  if (!needed) return fmt;
  size_t o = 0;
  for (const char* s = fmt; *s;) {
    if (o + 4 >= cap) return fmt; /* too long: leave it as it was */
    if (*s != '%') {
      buf[o++] = *s++;
      continue;
    }
    buf[o++] = *s++;
    if (*s == '%') {
      buf[o++] = *s++;
      continue;
    }
    /* flags, width, precision */
    while (*s && strchr("-+ #0123456789.*'", *s)) {
      if (o + 4 >= cap) return fmt;
      buf[o++] = *s++;
    }
    if (*s == 'z' || *s == 't') {
      ++s;
    } else if (*s == 'j') {
      buf[o++] = 'l';
      buf[o++] = 'l';
      ++s;
    }
  }
  buf[o] = '\0';
  return buf;
}

#define WRAP(name)                                                                                                    \
  int __real_##name(struct _reent* r, FILE* f, const char* fmt, va_list ap);                                         \
  int __wrap_##name(struct _reent* r, FILE* f, const char* fmt, va_list ap) {                                        \
    char buf[1024];                                                                                                   \
    return __real_##name(r, f, fix_format(fmt, buf, sizeof buf), ap);                                                 \
  }

WRAP(_vfprintf_r)
WRAP(_svfprintf_r)
WRAP(_vfiprintf_r)
WRAP(_svfiprintf_r)
