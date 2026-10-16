// Vita profiler: section timers (mp_vita_prof.h) plus a sampler thread that, 1000
// times a second, records which phase the game's main thread is in
// (aurora::phase). Every 5 seconds one summary line goes to the log:
//   prof: main cpu 97% | tick 80% draw 5% ... | disc 3.1ms/s(120) thp 0 ...
#include <psp2/kernel/threadmgr.h>

#include <aurora/phase.hpp>

#include <psp2/kernel/sysmem.h>

#include <malloc.h>

#include <cstdio>
#include <cstring>

#include "mp_vita_prof.h"
#include "mp_zone.h"

#include <algorithm>
#include <atomic>

extern "C" unsigned int _newlib_heap_size_user; // vita_main.cpp

// Function zones (mp_zone.h): a lock-free list, built as the zones first run.
std::atomic<MpZone*> g_zones{nullptr};
MpZone::MpZone(const char* n) : name(n), us(0), calls(0), next(nullptr), lastUs(0), lastCalls(0) {
  MpZone* head = g_zones.load();
  do {
    next = head;
  } while (!g_zones.compare_exchange_weak(head, this));
}

extern "C" {
volatile uint64_t mp_prof_us[MP_PROF_COUNT];
volatile uint32_t mp_prof_calls[MP_PROF_COUNT];
volatile int mp_prof_audio_thid = -1;
volatile int mp_prof_keep_thid[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
}

namespace {
const char* const kProfNames[MP_PROF_COUNT] = {"disc", "thp", "ai", "musyx", "irqwait", "thp_hw"};

SceUID g_mainThread = -1;
} // namespace
uint32_t vita_frames_presented(); // vita_main.cpp
namespace {

// Pin threads to cores (the Animal Crossing Vita port does the same): threads made
// with affinity 0 inherit their creator's mask, so without this everything the
// game starts can pile onto one core.
//   core 0: game main thread
//   core 1: audio (MusyX mixer, SDL audio device)
//   core 2: everything else we start (Aurora render worker, disc reader, caches)
// Sony's own threads (Sce*) are left alone.
struct ThreadRow {
  SceUID uid;
  char name[32];
  uint64_t lastRun;
};
ThreadRow g_rows[48];
int g_rowCount = 0;

void pin_threads(bool logTable, double secs) {
  SceKernelThreadInfo info;
  int misses = 0;
  for (SceUID uid = 0x40010001; uid < 0x40020000 && misses < 500; ++uid) {
    if (sceKernelGetThreadmgrUIDClass(uid) != SCE_KERNEL_TMID_Thread) {
      ++misses;
      continue;
    }
    std::memset(&info, 0, sizeof info);
    info.size = sizeof info;
    if (sceKernelGetThreadInfo(uid, &info) != 0) {
      ++misses;
      continue;
    }
    misses = 0;
    int target = 0;
    bool keep = false;
    for (int k = 0; k < 8; ++k) keep |= uid == mp_prof_keep_thid[k];
    if (keep) {
      target = 0; // the movie decoder threads pin themselves to core 1
    } else if (uid == g_mainThread) {
      target = SCE_KERNEL_CPU_MASK_USER_0;
    } else if (std::strcmp(info.name, "MusyX Audio") == 0 || std::strncmp(info.name, "SDLAudio", 8) == 0) {
      target = SCE_KERNEL_CPU_MASK_USER_1;
    } else if (std::strncmp(info.name, "Sce", 3) != 0 && std::strncmp(info.name, "mp_", 3) != 0) {
      target = SCE_KERNEL_CPU_MASK_USER_2;
    }
    if (target != 0 && info.currentCpuAffinityMask != target) sceKernelChangeThreadCpuAffinityMask(uid, target);

    if (logTable) {
      int r = 0;
      while (r < g_rowCount && g_rows[r].uid != uid) ++r;
      if (r == g_rowCount && g_rowCount < 48) {
        g_rows[g_rowCount] = {uid, {}, 0};
        std::strncpy(g_rows[g_rowCount].name, info.name, 31);
        ++g_rowCount;
      }
      double cpu = -1;
      if (r < 48) {
        if (g_rows[r].lastRun != 0 && secs > 0) cpu = (info.runClocks - g_rows[r].lastRun) / 1e4 / secs;
        g_rows[r].lastRun = info.runClocks;
      }
      std::fprintf(stderr, "prof:   thread %-28s core %d mask 0x%05x prio %3d cpu %5.1f%%\n", info.name,
                   info.lastExecutedCpuId, target ? target : info.currentCpuAffinityMask, info.currentPriority, cpu);
    }
  }
}

struct Bucket {
  const char* name;
  uint32_t count;
};

int sampler(SceSize, void*) {
  constexpr int kMax = 24;
  Bucket buckets[kMax] = {};
  uint32_t total = 0;
  uint64_t lastReport = sceKernelGetProcessTimeWide();
  uint64_t lastRun = 0, lastAudioRun = 0;
  bool loggedAffinity = false;
  unsigned reports = 0;
  uint64_t lastProf[MP_PROF_COUNT] = {};
  uint32_t lastCalls[MP_PROF_COUNT] = {};
  for (;;) {
    sceKernelDelayThread(5000); // 200 samples a second: enough, and a fifth of the cost on core 2
    const char* name = aurora::phase::state(aurora::phase::Main).name.load(std::memory_order_relaxed);
    int i = 0;
    for (; i < kMax && buckets[i].name != nullptr && buckets[i].name != name; ++i) {
    }
    if (i < kMax) {
      buckets[i].name = name;
      buckets[i].count++;
    }
    ++total;

    const uint64_t now = sceKernelGetProcessTimeWide();
    if (now - lastReport < 5000000) continue;
    const double secs = (now - lastReport) / 1e6;

    SceKernelThreadInfo info;
    char where[160] = "";
    double mainCpu = -1, audioCpu = -1;
    std::memset(&info, 0, sizeof info);
    info.size = sizeof info;
    if (sceKernelGetThreadInfo(g_mainThread, &info) >= 0) {
      const uint64_t run = info.runClocks;
      if (lastRun != 0) mainCpu = 100.0 * (run - lastRun) / (now - lastReport);
      lastRun = run;
      std::snprintf(where, sizeof where, "main core %d mask 0x%x prio %d", info.lastExecutedCpuId,
                    info.currentCpuAffinityMask, info.currentPriority);
    }
    if (mp_prof_audio_thid >= 0) {
      std::memset(&info, 0, sizeof info);
      info.size = sizeof info;
      if (sceKernelGetThreadInfo(mp_prof_audio_thid, &info) >= 0) {
        const uint64_t run = info.runClocks;
        if (lastAudioRun != 0) audioCpu = 100.0 * (run - lastAudioRun) / (now - lastReport);
        lastAudioRun = run;
        const size_t w = std::strlen(where);
        std::snprintf(where + w, sizeof where - w, ", audio core %d mask 0x%x prio %d", info.lastExecutedCpuId,
                      info.currentCpuAffinityMask, info.currentPriority);
      }
    }
    ++reports;
    const bool table = reports <= 3 || reports % 6 == 0;
    if (table) std::fprintf(stderr, "prof: %s\n", where);
    pin_threads(table, secs);
    char line[1024];
    static uint32_t lastFrames = 0, lastFrames0 = 0;
    const uint32_t frames = vita_frames_presented();
    const double fps = (frames - lastFrames) / secs;
    lastFrames = frames;
    int n = std::snprintf(line, sizeof line, "prof: fps %.1f main cpu %.0f%% audio cpu %.0f%% |", fps, mainCpu, audioCpu);
    for (int b = 0; b < kMax && buckets[b].name != nullptr; ++b) {
      if (buckets[b].count * 100 >= total) // >= 1%
        n += std::snprintf(line + n, sizeof line - n, " %s %u%%,", buckets[b].name, buckets[b].count * 100 / total);
    }
    n += std::snprintf(line + n, sizeof line - n, " |");
    for (int p = 0; p < MP_PROF_COUNT; ++p) {
      const uint64_t us = mp_prof_us[p];
      const uint32_t calls = mp_prof_calls[p];
      n += std::snprintf(line + n, sizeof line - n, " %s %.1fms/s(%u)", kProfNames[p], (us - lastProf[p]) / 1000.0 / secs,
                         calls - lastCalls[p]);
      lastProf[p] = us;
      lastCalls[p] = calls;
    }
    // Memory: the game heap (textures spill into it once vitaGL's pools are full)
    // and what the system still has free.
    {
      const struct mallinfo mi = mallinfo();
      SceKernelFreeMemorySizeInfo fm;
      std::memset(&fm, 0, sizeof fm);
      fm.size = sizeof fm;
      sceKernelGetFreeMemorySize(&fm);
      n += std::snprintf(line + n, sizeof line - n, " | heap %.1f/%.0f MB, free ram %.1f MB cdram %.1f MB",
                         mi.uordblks / 1048576.0, _newlib_heap_size_user / 1048576.0, fm.size_user / 1048576.0,
                         fm.size_cdram / 1048576.0);
    }
    std::fprintf(stderr, "%s\n", line);
    // The costliest function zones since the last report, per presented frame.
    {
      struct Row {
        MpZone* z;
        uint64_t us;
        uint32_t calls;
      } rows[96];
      int nr = 0;
      for (MpZone* z = g_zones.load(); z != nullptr && nr < 96; z = z->next) {
        const uint64_t u = z->us;
        const uint32_t c = z->calls;
        rows[nr++] = {z, u - z->lastUs, c - z->lastCalls};
        z->lastUs = u;
        z->lastCalls = c;
      }
      std::sort(rows, rows + nr, [](const Row& a, const Row& b) { return a.us > b.us; });
      const double perFrame = frames - lastFrames0 > 0 ? 1.0 / (frames - lastFrames0) : 1.0;
      char zl[1400];
      int zn = std::snprintf(zl, sizeof zl, "zones (ms/frame, calls/frame):");
      for (int i = 0; i < nr && i < 28 && rows[i].us > 0; ++i)
        zn += std::snprintf(zl + zn, sizeof zl - zn, " %s %.2f (%.0f),", rows[i].z->name, rows[i].us / 1000.0 * perFrame,
                            rows[i].calls * perFrame);
      if (nr) std::fprintf(stderr, "%s\n", zl);
      lastFrames0 = frames;
    }
    for (auto& b : buckets) b = {};
    total = 0;
    lastReport = now;
  }
  return 0;
}
} // namespace

// Aurora's two busiest threads each get a core of their own: the render worker
// (it plays the frame back through vitaGL) core 2, the GX FIFO processor (it turns
// the game's GX commands into draws) core 1, beside the audio threads, which
// outrank it. Before this both sat on core 2 while core 0 waited for them.
extern "C" void vita_place_named_thread(const char* name) {
  if (name == nullptr) return;
  int mask = 0, slot = -1;
  if (std::strcmp(name, "Aurora render worker") == 0) {
    mask = SCE_KERNEL_CPU_MASK_USER_2;
    slot = 3;
  } else if (std::strcmp(name, "Aurora FIFO processor") == 0) {
    mask = SCE_KERNEL_CPU_MASK_USER_1;
    slot = 4;
  }
  if (mask == 0) return;
  mp_prof_keep_thid[slot] = sceKernelGetThreadId();
  sceKernelChangeThreadCpuAffinityMask(0, mask);
  std::fprintf(stderr, "prof: %s placed on core %d\n", name, mask == SCE_KERNEL_CPU_MASK_USER_2 ? 2 : 1);
}

void vita_prof_start() {
  g_mainThread = sceKernelGetThreadId();
  pin_threads(false, 0);
  // High priority so samples are on time; it sleeps 1 ms between samples. Core 2,
  // away from the main thread's usual core.
  const SceUID th = sceKernelCreateThread("mp_prof", sampler, 0x40, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_2, nullptr);
  if (th >= 0) sceKernelStartThread(th, 0, nullptr);
}
