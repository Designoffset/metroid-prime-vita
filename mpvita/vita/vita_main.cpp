// PS Vita entry point for the Metroid Prime port (headless test build).
// Sets up memory, paths and an on-screen status display, then runs the port's
// normal main() (renamed aurora_main by Aurora).
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <exception>
#include <string>
#include <system_error>
#include <typeinfo>
#include <unistd.h>
#include <cxxabi.h>

extern "C" {
#include "debugScreen.h"
}

// Large newlib heap: the game, Aurora and MusyX allocate from it.
// The app is built with extended memory (ATTRIBUTE2), so there is room for this plus code and stacks.
extern "C" {
unsigned int _newlib_heap_size_user = 260u * 1024u * 1024u;
int sceUserMainThreadStackSize = 8 * 1024 * 1024;
// Stack of every pthread (std::thread, Aurora's workers) that doesn't ask for a size:
// the Vita's default is 32 KB, and the runtime shader compiler, which runs on the
// render worker, overflows that compiling the bigger GameCube shaders.
unsigned int _pthread_stack_default_user = 1024 * 1024;
}

// The port's main() is renamed aurora_main by <aurora/main.h>.
extern "C" int aurora_main(int argc, char** argv);
extern "C" const char mp_build_id[]; // early_boot.c
void vita_prof_start();               // vita_prof.cpp
extern "C" void SDL_SetMainReady(void);

static const char* kDataDir = "ux0:data/MetroidPrime";

static bool file_exists(const std::string& p) {
  SceIoStat st;
  return sceIoGetstat(p.c_str(), &st) >= 0;
}

static std::string find_disc_in(const std::string& dir) {
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      std::string n = e->d_name;
      auto dot = n.rfind('.');
      if (dot == std::string::npos) continue;
      std::string ext = n.substr(dot + 1);
      for (auto& c : ext) c = (char)tolower((unsigned char)c);
      if (ext == "iso" || ext == "gcm" || ext == "ciso") {
        closedir(d);
        return dir + "/" + n;
      }
    }
    closedir(d);
  }
  return {};
}

// Disc image: ux0:data/MetroidPrime/rom/<any>.iso|.gcm|.ciso (same layout as the
// Animal Crossing Vita port), or directly in ux0:data/MetroidPrime/.
static std::string find_disc() {
  std::string p = find_disc_in(std::string(kDataDir) + "/rom");
  return p.empty() ? find_disc_in(kDataDir) : p;
}

// Uncaught C++ exceptions: show the message on screen and in the log instead of
// aborting silently.
static void on_terminate() {
  const char* type = "unknown";
  std::string what = "(no message)";
  int code = 0;
  if (std::exception_ptr e = std::current_exception()) {
    try {
      std::rethrow_exception(e);
    } catch (const std::system_error& ex) {
      type = "std::system_error";
      what = ex.what();
      code = ex.code().value();
    } catch (const std::exception& ex) {
      type = typeid(ex).name();
      what = ex.what();
    } catch (...) {
    }
  } else {
    what = "std::terminate called without an exception";
  }
  std::fprintf(stderr, "\nvita: FATAL uncaught exception %s (code %d): %s\n", type, code, what.c_str());
  std::fflush(stderr);
  psvDebugScreenPrintf("\n*** FATAL ERROR ***\n%s (code %d)\n%s\n", type, code, what.c_str());
  psvDebugScreenPrintf("\nPlease send ux0:data/MetroidPrime/error.log\n");
  SceCtrlData pad;
  for (;;) {
    sceCtrlPeekBufferPositive(0, &pad, 1);
    if (pad.buttons & SCE_CTRL_START) break;
    sceKernelDelayThread(50000);
  }
  sceKernelExitProcess(0);
  for (;;) {}
}

// abort() (Aurora's fatal errors, failed asserts): show the end of the log on
// screen and wait for START instead of dropping straight to a crash report.
static void on_abort(int) {
  std::fflush(stderr);
  std::fflush(stdout);
  psvDebugScreenPrintf("\n*** FATAL ERROR (abort) ***\nLast lines of ux0:data/MetroidPrime/error.log:\n\n");
  SceUID fd = sceIoOpen("ux0:data/MetroidPrime/error.log", SCE_O_RDONLY, 0);
  if (fd >= 0) {
    const SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    const SceOff start = size > 900 ? size - 900 : 0;
    sceIoLseek(fd, start, SCE_SEEK_SET);
    static char buf[901];
    const int n = sceIoRead(fd, buf, 900);
    sceIoClose(fd);
    if (n > 0) {
      buf[n] = 0;
      const char* p = buf;
      if (start > 0) // skip the partial first line
        if (const char* nl = std::strchr(buf, '\n')) p = nl + 1;
      psvDebugScreenPrintf("%s\n", p);
    }
  }
  psvDebugScreenPrintf("\nPress START to exit.\n");
  SceCtrlData pad;
  for (;;) {
    sceCtrlPeekBufferPositive(0, &pad, 1);
    if (pad.buttons & SCE_CTRL_START) break;
    sceKernelDelayThread(50000);
  }
  sceKernelExitProcess(0);
  for (;;) {}
}

static std::atomic<uint32_t> g_frames{0};
uint32_t vita_frames_presented() { return g_frames.load(std::memory_order_relaxed); }
static uint64_t g_startUs = 0;

// Called by the null WebGPU layer on every presented frame.
namespace wgnull {
void on_present() {
  const uint32_t n = ++g_frames;
  if (n % 30 == 0) {
    const uint64_t now = sceKernelGetProcessTimeWide();
    const double secs = (now - g_startUs) / 1e6;
    int x = 0, y = 400;
    psvDebugScreenSetCoordsXY(&x, &y);
    psvDebugScreenPrintf("Frames: %-8u  time: %6.1fs  avg fps: %5.1f    \n", n, secs, n / (secs > 0 ? secs : 1));
  }
}
} // namespace wgnull

static void wait_for_exit_button() {
  psvDebugScreenPrintf("\nPress START to exit.\n");
  SceCtrlData pad;
  for (;;) {
    sceCtrlPeekBufferPositive(0, &pad, 1);
    if (pad.buttons & SCE_CTRL_START) break;
    sceKernelDelayThread(50000);
  }
}

int main(int, char**) {
  // Fastest clocks; the user's overclock plugin can raise these further.
  scePowerSetArmClockFrequency(444);
  scePowerSetBusClockFrequency(222);
  scePowerSetGpuClockFrequency(222);
  scePowerSetGpuXbarClockFrequency(166);

  psvDebugScreenInit();
  std::set_terminate(on_terminate);
  std::signal(SIGABRT, on_abort);
  psvDebugScreenPrintf("Metroid Prime - PS Vita port (headless test build)\n%s\n", mp_build_id);
  psvDebugScreenPrintf("No graphics yet: this build checks that the game runs.\n\n");

  // Folders were already created by early_boot.c; repeat in case that failed.
  sceIoMkdir(kDataDir, 0777);
  const std::string user = std::string(kDataDir) + "/saves";
  const std::string cache = std::string(kDataDir) + "/cache";
  sceIoMkdir(user.c_str(), 0777);
  sceIoMkdir(cache.c_str(), 0777);
  sceIoMkdir((std::string(kDataDir) + "/rom").c_str(), 0777);
  const std::string logPath = std::string(kDataDir) + "/error.log";

  const std::string disc = find_disc();
  if (disc.empty()) {
    psvDebugScreenPrintf("No disc image found.\nCopy your Metroid Prime disc image (.iso/.gcm/.ciso) into:\n  %s/rom/\n", kDataDir);
    wait_for_exit_button();
    sceKernelExitProcess(0);
    return 0;
  }
  psvDebugScreenPrintf("Disc: %s\n", disc.c_str());
  // Everything the port, Aurora and SDL print is appended to error.log (like the
  // Animal Crossing port); each run starts with a "--- launch ---" line written by
  // early_boot.c. Start over once it gets large.
  {
    SceIoStat st;
    if (sceIoGetstat(logPath.c_str(), &st) >= 0 && st.st_size > 4 * 1024 * 1024) sceIoRemove(logPath.c_str());
  }
  if (std::freopen(logPath.c_str(), "a", stderr)) {
    std::setvbuf(stderr, nullptr, _IONBF, 0);
  }
  if (std::freopen(logPath.c_str(), "a", stdout)) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
  }
  std::fprintf(stderr, "vita: disc %s\n", disc.c_str());
  psvDebugScreenPrintf("Log:  %s\n\n", logPath.c_str());

  // std::filesystem treats "ux0:..." as a relative path (it does not start with
  // '/'), and the Vita's working directory is "app0:", so anything it resolves
  // ends up as "app0:/ux0:...". Work on ux0: and hand the port Unix-style paths;
  // newlib maps "/data/..." to "ux0:/data/...".
  chdir("ux0:/");
  auto unixPath = [](const std::string& p) { return p.rfind("ux0:", 0) == 0 ? (p[4] == '/' ? p.substr(4) : "/" + p.substr(4)) : p; };
  const std::string discUnix = unixPath(disc);
  const std::string userUnix = unixPath(user);
  std::fprintf(stderr, "vita: cwd ux0:/, disc %s, user %s\n", discUnix.c_str(), userUnix.c_str());
  setenv("MP_DISC", discUnix.c_str(), 1);
  setenv("MP_USER_PATH", userUnix.c_str(), 1);
  setenv("MP_CACHE_PATH", unixPath(cache).c_str(), 1);
  // Button prompts (PlayStation icons) and other texture packs: ux0:data/MetroidPrime/textures.
  setenv("MP_TEXTURES", "/data/MetroidPrime/textures", 1);
  // PC-only extras the Vita renderer cannot draw (it reproduces the GameCube's GX): left
  // on, they cost the game thread and Aurora their recording and are then dropped, and the
  // Remastered visor/liquid/effect replacements would stand in for retail versions that do
  // draw. The original game's look throughout. (Not overwritten: an MP_* set before wins.)
  static const char* const kOff[] = {"MP_SHADOWS", "MP_BLOOM", "MP_VOLFOG", "MP_COLOR_GRADE", "MP_THERMAL_POST",
                                     "MP_XRAY_POST", "MP_ROOM_ENV", "MP_ROOM_LIGHTS", "MP_ROOM_LIQUID",
                                     "MP_REMASTERED_EFFECTS", "MP_REMASTERED_BALL_LIGHT", "MP_HDR_SCENE"};
  for (const char* name : kOff) setenv(name, "0", 0);
  setenv("MP_NO_DISC_DIALOG", "1", 1);
  setenv("MP_LOG_FILE", "0", 1); // stderr already goes to the log file
  setenv("HOME", userUnix.c_str(), 1);

  vita_prof_start(); // one "prof:" line in the log every 5 s
  SDL_SetMainReady(); // we provide main() ourselves
  g_startUs = sceKernelGetProcessTimeWide();
  char arg0[] = "app0:/eboot.bin";
  char* argv[] = {arg0, nullptr};
  psvDebugScreenPrintf("Starting game...\n");
  const int rc = aurora_main(1, argv);
  psvDebugScreenPrintf("\nGame exited with code %d after %u frames.\n", rc, g_frames.load());
  wait_for_exit_button();
  sceKernelExitProcess(0);
  return rc;
}
