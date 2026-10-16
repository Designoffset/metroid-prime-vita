// Runs before every C++ static constructor in the game (priority 101 is the
// earliest user priority), using only kernel I/O calls. Creates the data folder
// layout and stamps error.log, so the folder exists and the log shows the build
// even if something crashes before main().
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/rtc.h>
#include <string.h>

#define MP_DATA "ux0:data/MetroidPrime"
const char mp_build_id[] = "MP-Vita build 49 (" __DATE__ " " __TIME__ ")";

static void put(SceUID fd, const char* s) { sceIoWrite(fd, s, strlen(s)); }
static void putn(SceUID fd, unsigned v, int w, char sep) {
  char b[8]; int n = 0;
  b[n++] = sep;
  char d[6]; int k = 0;
  do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 6);
  while (k < w) d[k++] = '0';
  while (k) b[n++] = d[--k];
  sceIoWrite(fd, b, n);
}

__attribute__((constructor(101))) static void mp_early_boot(void) {
  sceIoMkdir("ux0:data", 0777);
  sceIoMkdir(MP_DATA, 0777);
  sceIoMkdir(MP_DATA "/rom", 0777);
  sceIoMkdir(MP_DATA "/saves", 0777);
  sceIoMkdir(MP_DATA "/cache", 0777);
  SceUID fd = sceIoOpen(MP_DATA "/error.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
  if (fd >= 0) {
    SceDateTime t;
    memset(&t, 0, sizeof t);
    sceRtcGetCurrentClockLocalTime(&t);
    put(fd, "\n--- launch");
    putn(fd, t.year, 4, ' '); putn(fd, t.month, 2, '-'); putn(fd, t.day, 2, '-');
    putn(fd, t.hour, 2, ' '); putn(fd, t.minute, 2, ':'); putn(fd, t.second, 2, ':');
    put(fd, " ---\n");
    put(fd, mp_build_id);
    put(fd, "\n");
    sceIoClose(fd);
  }
}
