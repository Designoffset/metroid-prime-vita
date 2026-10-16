// Background THP (title-screen / cutscene movie) video decoding for the Vita.
//
// The front end plays several movies at once (3-4 at 30 fps, 640x480); a software
// decode costs ~13 ms a frame, more than one Vita core can sustain. So:
//  1. decoding runs on worker threads and the movie player only waits for a
//     frame right before drawing it;
//  2. the workers use the Vita's hardware JPEG decoder (SceJpeg, part of the
//     codec engine) when it works, with the software decoder as the fallback.
//
// Two stages, so the codec engine and the CPU work at the same time:
//   "mp_thp"    rebuilds the JPEG and runs the hardware decode into one of
//               kSlots output buffers (or decodes in software);
//   readback   reads a finished buffer back and re-tiles it into the textures;
//   helper     decodes in software (core 0, low priority) while the hardware is busy.
//
// THP frames are baseline JPEG with the 0xFF00 byte stuffing removed, so the
// stuffing is put back before handing a frame to the hardware. The hardware
// writes planar Y/Cb/Cr; the game wants GameCube I8 textures (8x4 tiles), so
// the planes are re-tiled. The first few hardware frames are checked against
// the software decoder; on a mismatch or error the hardware path is turned off
// for the rest of the run and the reason is logged.
//
// API (C linkage, used by CCubeMoviePlayer.cpp under __vita__):
//   vita_thp_decode_async(data, size, y, u, v)  copy `data` and queue a decode
//   vita_thp_wait(y)                            wait for queued decodes into y
//   vita_thp_wait_all()                         wait for everything queued
#include <psp2/jpeg.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <pthread.h>

#include <arm_neon.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include "mp_vita_prof.h"

extern "C" int32_t THPVideoDecode(void* file, void* tileY, void* tileU, void* tileV, void* work);

namespace {
struct Job {
  std::vector<uint8_t> data;
  uint32_t size;
  void* y;
  void* u;
  void* v;
};

constexpr int kSlots = 2; // hardware output buffers

struct Staged {
  Job job;
  int slot;
  SceJpegOutputInfo info;
};

std::mutex g_mutex;
std::condition_variable g_wake;     // decode stage: new job
std::condition_variable g_stageCv;  // readback stage: new staged frame; decode stage: a slot came free
std::condition_variable g_finished; // waiters: a job finished
std::deque<Job> g_queue;
std::deque<Staged> g_staged;
std::vector<const void*> g_inFlight; // y of every job taken from g_queue and not finished
int g_freeSlots = kSlots;
bool g_hwBusy = false;  // the decode stage is working on a job
size_t g_maxQueue = 0;  // deepest queue since the last log line
bool g_started = false;

alignas(32) uint8_t g_work[4096 + 32];   // software decoder scratch (decode stage)
alignas(32) uint8_t g_workRb[4096 + 32]; // software decoder scratch (readback stage self-test)

uint64_t now_us() { return sceKernelGetProcessTimeWide(); }

// Per-stage timing, logged every kLogEvery frames.
constexpr uint32_t kLogEvery = 600;
std::atomic<uint64_t> g_tPrep{0}, g_tDecode{0}, g_tDecodeCpu{0}, g_tRetile{0}, g_tSoft{0}, g_tWaitSlot{0};
std::atomic<uint32_t> g_hwFrames{0}, g_swFrames{0}, g_helperFrames{0};
// CPU time (us) the calling thread has used so far.
uint64_t thread_cpu_us() {
  SceKernelThreadInfo info;
  std::memset(&info, 0, sizeof info);
  info.size = sizeof info;
  if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0) return 0;
  return info.runClocks;
}

void finish_job(const void* y) {
  {
    std::lock_guard lock(g_mutex);
    for (size_t i = 0; i < g_inFlight.size(); ++i)
      if (g_inFlight[i] == y) {
        g_inFlight.erase(g_inFlight.begin() + i);
        break;
      }
  }
  g_finished.notify_all();
}

void maybe_log() {
  const uint32_t hw = g_hwFrames.load(), sw = g_swFrames.load();
  const uint32_t n = hw + sw;
  if (n == 0 || n % kLogEvery != 0) return;
  const double hwN = hw ? hw : 1, swN = sw ? sw : 1;
  size_t maxQueue;
  {
    std::lock_guard lock(g_mutex);
    maxQueue = g_maxQueue;
    g_maxQueue = 0;
  }
  std::fprintf(stderr,
               "thp: %u frames by hardware, %u by software (%u by the helper) | per hardware frame: prep %.2f ms, "
               "decode %.2f ms (%.2f ms CPU), readback+retile %.2f ms, slot wait %.2f ms | software %.2f ms | "
               "queue up to %u\n",
               hw, sw, g_helperFrames.load(), g_tPrep / hwN / 1000, g_tDecode / hwN / 1000, g_tDecodeCpu / hwN / 1000,
               g_tRetile / hwN / 1000, g_tWaitSlot / hwN / 1000, g_tSoft / swN / 1000, static_cast<unsigned>(maxQueue));
}

// ---------------------------------------------------------------- hardware path
constexpr int kColorSpaceYuv420 = 0x20202;
constexpr int kNoCscOutput = -1;
constexpr int kModeWithDht = 0;

// The vitasdk header declares sceJpegDecodeMJpegYCbCr as (jpeg, size, mode, out,
// outSize, buf, bufSize), but the firmware takes (jpeg, size, out, outSize, mode,
// coefBuf, coefSize) - the order Vita3K implements. With the header's order the
// mode (0) lands in the output pointer: SCE_JPEG_ERROR_INVALID_POINTER.
using DecodeYCbCrFn = int (*)(const SceUInt8* jpeg, SceSize jpegSize, SceUInt8* out, SceSize outSize, SceInt32 mode,
                              void* coefBuffer, SceSize coefBufferSize);
inline int decode_ycbcr(const SceUInt8* jpeg, SceSize jpegSize, SceUInt8* out, SceSize outSize, SceInt32 mode,
                        void* coef, SceSize coefSize) {
  return reinterpret_cast<DecodeYCbCrFn>(&sceJpegDecodeMJpegYCbCr)(jpeg, jpegSize, out, outSize, mode, coef, coefSize);
}
constexpr int kSelfTestFrames = 3;

enum class Hw { Untried, On, Off };
std::atomic<Hw> g_hw{Hw::Untried};
int g_selfTestsLeft = kSelfTestFrames; // readback stage only
// The codec engine needs physically contiguous memory for everything it touches
// (error 0x80650053 otherwise). Memory setups tried in order; the first that
// works is kept. Physically contiguous blocks come in whole megabytes.
struct HwConfig {
  const char* name;
  SceKernelMemBlockType type;
};
const HwConfig kHwConfigs[] = {
    {"main memory, physically contiguous", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW},
    {"video memory (CDRAM)", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW},
};
constexpr int kHwConfigCount = sizeof(kHwConfigs) / sizeof(kHwConfigs[0]);
constexpr int kErrNotPhysicallyContinuous = static_cast<int>(0x80650053);
int g_hwConfig = -1;
SceUID g_hwInBlock = -1, g_hwCoefBlock = -1, g_hwOutBlock[kSlots] = {-1, -1};
uint8_t* g_hwCoef = nullptr; // coefficient scratch, if the decoder asks for one
constexpr uint32_t kHwCoefSize = 1u << 20;
uint8_t* g_hwIn = nullptr;            // JPEG data (the codec engine reads it)
uint8_t* g_hwOut[kSlots] = {};        // decoded planes (the codec engine writes them)
SceJpegOutputInfo* g_hwInfo = nullptr; // inside the input block, after the data
constexpr uint32_t kHwInSize = 1u << 20;
constexpr uint32_t kHwInDataCap = kHwInSize - 4096;
constexpr uint32_t kHwOutSize = 2u << 20;

void hw_off(const char* why, int code) {
  std::fprintf(stderr, "thp: hardware JPEG decoder off (%s, 0x%08x); using the software decoder\n", why,
               static_cast<unsigned>(code));
  g_hw = Hw::Off;
}

uint8_t* alloc_block(const char* name, SceKernelMemBlockType type, uint32_t size, SceUID& block) {
  block = sceKernelAllocMemBlock(name, type, size, nullptr);
  if (block < 0) return nullptr;
  void* base = nullptr;
  if (sceKernelGetMemBlockBase(block, &base) < 0) return nullptr;
  return static_cast<uint8_t*>(base);
}

void free_block(SceUID& b) {
  if (b >= 0) sceKernelFreeMemBlock(b);
  b = -1;
}

void free_buffers() {
  free_block(g_hwInBlock);
  free_block(g_hwCoefBlock);
  for (int s = 0; s < kSlots; ++s) {
    free_block(g_hwOutBlock[s]);
    g_hwOut[s] = nullptr;
  }
  g_hwIn = g_hwCoef = nullptr;
  g_hwInfo = nullptr;
}

// Switches to the next memory setup that can be allocated. False when none is left.
// Only called by the decode stage while every slot is free.
bool next_config() {
  free_buffers();
  while (++g_hwConfig < kHwConfigCount) {
    const HwConfig& c = kHwConfigs[g_hwConfig];
    g_hwIn = alloc_block("mp_thp_in", c.type, kHwInSize, g_hwInBlock);
    g_hwCoef = alloc_block("mp_thp_coef", c.type, kHwCoefSize, g_hwCoefBlock);
    bool ok = g_hwIn != nullptr && g_hwCoef != nullptr;
    for (int s = 0; s < kSlots && ok; ++s) {
      g_hwOut[s] = alloc_block("mp_thp_out", c.type, kHwOutSize, g_hwOutBlock[s]);
      ok = g_hwOut[s] != nullptr;
    }
    if (ok) {
      g_hwInfo = reinterpret_cast<SceJpegOutputInfo*>(g_hwIn + kHwInDataCap);
      std::fprintf(stderr, "thp: trying the hardware decoder with %s\n", c.name);
      return true;
    }
    std::fprintf(stderr, "thp: could not allocate %s (in 0x%08x coef 0x%08x out 0x%08x/0x%08x)\n", c.name,
                 static_cast<unsigned>(g_hwInBlock), static_cast<unsigned>(g_hwCoefBlock),
                 static_cast<unsigned>(g_hwOutBlock[0]), static_cast<unsigned>(g_hwOutBlock[1]));
    free_buffers();
  }
  return false;
}

bool hw_init() {
  int r = sceJpegInitMJpeg(1);
  if (r < 0) {
    sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC);
    r = sceJpegInitMJpeg(1);
  }
  if (r < 0) {
    hw_off("sceJpegInitMJpeg failed", r);
    return false;
  }
  if (!next_config()) {
    hw_off("no usable memory for the decoder", 0);
    return false;
  }
  std::fprintf(stderr, "thp: hardware JPEG decoder initialised\n");
  g_hw = Hw::On;
  return true;
}

inline uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }

// Finds the start of the entropy-coded data and the restart interval.
bool scan_headers(const uint8_t* d, uint32_t size, uint32_t& scanStart, uint32_t& restart, uint32_t& dims) {
  restart = 0;
  dims = 0;
  uint32_t pos = 0;
  while (pos + 4 <= size) {
    if (d[pos] != 0xFF) return false;
    while (pos < size && d[pos] == 0xFF) ++pos;
    if (pos >= size) return false;
    const uint8_t marker = d[pos++];
    if (marker == 0xD8) continue;
    if (pos + 2 > size) return false;
    const uint16_t len = be16(d + pos);
    if (len < 2 || pos + len > size) return false;
    if (marker == 0xDD && len == 4) restart = be16(d + pos + 2);
    if (marker == 0xC0 && len >= 7) dims = uint32_t(be16(d + pos + 3)) << 16 | be16(d + pos + 5); // height, width
    pos += len;
    if (marker == 0xDA) {
      scanStart = pos;
      return true;
    }
  }
  return false;
}

// THP -> standard JPEG: headers as-is, 0xFF in the entropy data stuffed with 0x00, EOI appended.
uint32_t make_standard_jpeg(const uint8_t* d, uint32_t size, uint32_t scanStart, uint8_t* out, uint32_t cap) {
  if (scanStart + 2 > cap) return 0;
  std::memcpy(out, d, scanStart);
  uint32_t o = scanStart;
  uint32_t i = scanStart;
  while (i < size) {
    // Copy runs without 0xFF in one go.
    const void* ff = std::memchr(d + i, 0xFF, size - i);
    const uint32_t end = ff ? static_cast<uint32_t>(static_cast<const uint8_t*>(ff) - d) : size;
    const uint32_t run = end - i;
    if (o + run + 4 > cap) return 0;
    std::memcpy(out + o, d + i, run);
    o += run;
    i = end;
    if (ff) {
      out[o++] = 0xFF;
      out[o++] = 0x00;
      ++i;
    }
  }
  out[o++] = 0xFF;
  out[o++] = 0xD9;
  return o;
}

// Planar 8-bit plane -> GameCube I8 (8x4 tiles), the layout the software decoder writes.
void retile(const uint8_t* src, uint32_t pitch, uint32_t w, uint32_t h, uint8_t* dst) {
  const uint32_t tilesPerRow = (w + 7) / 8;
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* row = src + static_cast<size_t>(y) * pitch;
    uint8_t* base = dst + static_cast<size_t>(y / 4) * tilesPerRow * 32 + (y & 3) * 8;
    for (uint32_t t = 0; t < tilesPerRow; ++t) std::memcpy(base + t * 32, row + t * 8, 8);
  }
}

// The same, reading the source in 64-byte bursts per row (it is uncached memory
// written by the codec engine) and writing whole tiles with NEON.
void retile_fast(const uint8_t* src, uint32_t pitch, uint32_t w, uint32_t h, uint8_t* dst) {
  if (w % 64 != 0 || h % 4 != 0 || pitch % 16 != 0) {
    retile(src, pitch, w, h, dst);
    return;
  }
  const uint32_t tilesPerRow = w / 8;
  for (uint32_t y = 0; y < h; y += 4) {
    const uint8_t* r0 = src + static_cast<size_t>(y) * pitch;
    const uint8_t* r1 = r0 + pitch;
    const uint8_t* r2 = r1 + pitch;
    const uint8_t* r3 = r2 + pitch;
    uint8_t* out = dst + static_cast<size_t>(y / 4) * tilesPerRow * 32;
    for (uint32_t x = 0; x < w; x += 64) {
      const uint8x16x4_t a = vld1q_u8_x4(r0 + x), b = vld1q_u8_x4(r1 + x);
      const uint8x16x4_t c = vld1q_u8_x4(r2 + x), d = vld1q_u8_x4(r3 + x);
      uint8_t* o = out + (x / 8) * 32;
      for (int k = 0; k < 4; ++k) {
        // 16 bytes of each row = two tiles: tile 2k, then tile 2k+1.
        vst1q_u8(o + k * 64 + 0, vcombine_u8(vget_low_u8(a.val[k]), vget_low_u8(b.val[k])));
        vst1q_u8(o + k * 64 + 16, vcombine_u8(vget_low_u8(c.val[k]), vget_low_u8(d.val[k])));
        vst1q_u8(o + k * 64 + 32, vcombine_u8(vget_high_u8(a.val[k]), vget_high_u8(b.val[k])));
        vst1q_u8(o + k * 64 + 48, vcombine_u8(vget_high_u8(c.val[k]), vget_high_u8(d.val[k])));
      }
    }
  }
}

inline size_t tiled_index(uint32_t x, uint32_t y, uint32_t w) {
  return static_cast<size_t>(y / 4) * ((w + 7) / 8) * 32 + (x / 8) * 32 + (y & 3) * 8 + (x & 7);
}

// Compares a hardware-decoded plane with the software one. Small differences
// are normal (the two IDCTs round differently).
bool planes_match(const uint8_t* a, const uint8_t* b, uint32_t w, uint32_t h, double& meanDiff, double& bigShare) {
  uint64_t sum = 0, big = 0, n = 0;
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      const size_t i = tiled_index(x, y, w);
      const int d = std::abs(int(a[i]) - int(b[i]));
      sum += d;
      big += d > 16;
      ++n;
    }
  meanDiff = n ? double(sum) / n : 0;
  bigShare = n ? double(big) / n : 0;
  return meanDiff < 3.0 && bigShare < 0.005;
}

// Decode stage: hardware decode of one frame into g_hwOut[slot]. False if the
// hardware can't do this frame (g_hw turned off where appropriate).
bool hw_submit(const Job& job, int slot, SceJpegOutputInfo& infoOut) {
  uint32_t scanStart = 0, restart = 0, dims = 0;
  if (!scan_headers(job.data.data(), job.size, scanStart, restart, dims)) {
    hw_off("unexpected THP frame header", 0);
    return false;
  }
  if (restart != 0) {
    // Restart intervals without RST markers can't be fed to the hardware cheaply.
    hw_off("movie uses restart intervals", restart);
    return false;
  }
  const uint64_t t0 = now_us();
  // Build the JPEG in cached memory and copy it over in one go: byte writes
  // straight into uncached memory cost several ms a frame.
  static std::vector<uint8_t> jpeg(kHwInDataCap);
  const uint32_t jpegSize = make_standard_jpeg(job.data.data(), job.size, scanStart, jpeg.data(), kHwInDataCap);
  if (jpegSize == 0) {
    hw_off("frame too large", job.size);
    return false;
  }
  std::memcpy(g_hwIn, jpeg.data(), (jpegSize + 31) & ~31u);
  const uint64_t t1 = now_us();
  g_tPrep += t1 - t0;
  // The output layout only depends on the frame size; ask once per size.
  static uint32_t infoDims = 0;
  int r = 0;
  if (dims == 0 || dims != infoDims) {
    std::memset(g_hwInfo, 0, sizeof *g_hwInfo);
    r = sceJpegGetOutputInfo(g_hwIn, jpegSize, kNoCscOutput, kModeWithDht, g_hwInfo);
    infoDims = r >= 0 ? dims : 0;
    if (r < 0) {
      std::fprintf(stderr, "thp: sceJpegGetOutputInfo: 0x%08x with %s\n", static_cast<unsigned>(r),
                   kHwConfigs[g_hwConfig].name);
      return false;
    }
    if (g_hwInfo->colorSpace != kColorSpaceYuv420 || g_hwInfo->outputSize > kHwOutSize) {
      hw_off("unsupported frame format", g_hwInfo->colorSpace);
      return false;
    }
    // unk_0x10 is the coefficient buffer size the decoder wants (0 = none).
    if (g_hwInfo->unk_0x10 > kHwCoefSize) {
      hw_off("decoder wants a larger coefficient buffer", static_cast<int>(g_hwInfo->unk_0x10));
      return false;
    }
  }
  const uint32_t coefSize = g_hwInfo->unk_0x10;
  const uint64_t cpu0 = thread_cpu_us();
  r = decode_ycbcr(g_hwIn, jpegSize, g_hwOut[slot], g_hwInfo->outputSize, kModeWithDht,
                   coefSize ? g_hwCoef : nullptr, coefSize);
  g_tDecode += now_us() - t1;
  g_tDecodeCpu += thread_cpu_us() - cpu0;
  if (r < 0) {
    std::fprintf(stderr, "thp: sceJpegDecodeMJpegYCbCr: 0x%08x with %s\n", static_cast<unsigned>(r),
                 kHwConfigs[g_hwConfig].name);
    if (r != kErrNotPhysicallyContinuous) hw_off("decoder call failed", r);
    return false;
  }
  infoOut = *g_hwInfo;
  return true;
}

// Readback stage: hardware output in g_hwOut[slot] -> the job's textures.
void hw_finish(const Job& job, int slot, const SceJpegOutputInfo& info) {
  const uint64_t t0 = now_us();
  const uint32_t w = info.width, h = info.height;
  const uint32_t cw = (w + 1) / 2, ch = (h + 1) / 2;
  const uint8_t* yPlane = g_hwOut[slot];
  const uint8_t* cbPlane = yPlane + static_cast<size_t>(info.pitch[0].x) * info.pitch[0].y;
  const uint8_t* crPlane = cbPlane + static_cast<size_t>(info.pitch[1].x) * info.pitch[1].y;

  if (g_selfTestsLeft > 0) {
    // Check against the software decoder, which writes the real textures meanwhile.
    const size_t ySize = static_cast<size_t>((w + 7) & ~7u) * ((h + 3) & ~3u);
    const size_t cSize = static_cast<size_t>((cw + 7) & ~7u) * ((ch + 3) & ~3u);
    std::vector<uint8_t> ty(ySize), tu(cSize), tv(cSize);
    retile_fast(yPlane, info.pitch[0].x, w, h, ty.data());
    retile_fast(cbPlane, info.pitch[1].x, cw, ch, tu.data());
    retile_fast(crPlane, info.pitch[2].x, cw, ch, tv.data());
    THPVideoDecode(const_cast<uint8_t*>(job.data.data()), job.y, job.u, job.v,
                   reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(g_workRb) + 31) & ~uintptr_t(31)));
    double my, by, mu, bu, mv, bv;
    const bool ok = planes_match(ty.data(), static_cast<uint8_t*>(job.y), w, h, my, by) &&
                    planes_match(tu.data(), static_cast<uint8_t*>(job.u), cw, ch, mu, bu) &&
                    planes_match(tv.data(), static_cast<uint8_t*>(job.v), cw, ch, mv, bv);
    std::fprintf(stderr, "thp: self-test %dx%d pitch %ux%u/%ux%u: Y diff %.2f (%.3f%% big) %s\n", w, h,
                 info.pitch[0].x, info.pitch[0].y, info.pitch[1].x, info.pitch[1].y, my, by * 100,
                 ok ? "ok" : "MISMATCH");
    if (!ok) {
      hw_off("hardware output differs from the software decoder", 0);
      return; // the software result is already in place
    }
    if (--g_selfTestsLeft == 0) std::fprintf(stderr, "thp: hardware JPEG decoder verified, now in use\n");
    return;
  }

  retile_fast(yPlane, info.pitch[0].x, w, h, static_cast<uint8_t*>(job.y));
  retile_fast(cbPlane, info.pitch[1].x, cw, ch, static_cast<uint8_t*>(job.u));
  retile_fast(crPlane, info.pitch[2].x, cw, ch, static_cast<uint8_t*>(job.v));
  g_tRetile += now_us() - t0;
}

void software_decode(Job& job) {
  const uint64_t t0 = now_us();
  // (THPVideoDecode times itself as MP_PROF_THP_VIDEO.)
  THPVideoDecode(job.data.data(), job.y, job.u, job.v,
                 reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(g_work) + 31) & ~uintptr_t(31)));
  g_tSoft += now_us() - t0;
  ++g_swFrames;
  maybe_log();
}

// Pins the calling worker to core 1 (audio's core, mostly idle), below the audio
// threads (MusyX 112, SDL 159): on core 2 the decoder kept losing the CPU to the
// render worker. vita_prof.cpp leaves threads in mp_prof_keep_thid alone.
void pin_self(int prio, int index, int cpuMask = SCE_KERNEL_CPU_MASK_USER_1) {
  mp_prof_keep_thid[index] = sceKernelGetThreadId();
  sceKernelChangeThreadPriority(0, prio);
  sceKernelChangeThreadCpuAffinityMask(0, cpuMask);
}

// ---------------------------------------------------------------- stages
// Pthreads: std::condition_variable needs pthread's per-thread data.
void* decode_stage(void*) {
  pin_self(165, 0);
  for (;;) {
    Job job;
    {
      std::unique_lock lock(g_mutex);
      g_wake.wait(lock, [] { return !g_queue.empty(); });
      job = std::move(g_queue.front());
      g_queue.pop_front();
      g_inFlight.push_back(job.y);
      g_hwBusy = true;
    }
    if (g_hw == Hw::Untried) hw_init();
    bool staged = false;
    while (g_hw == Hw::On) {
      int slot = -1;
      {
        const uint64_t t0 = now_us();
        std::unique_lock lock(g_mutex);
        g_stageCv.wait(lock, [] { return g_freeSlots > 0; });
        --g_freeSlots;
        // Slots are used round-robin: the readback stage finishes them in order.
        static int nextSlot = 0;
        slot = nextSlot;
        nextSlot = (nextSlot + 1) % kSlots;
        g_tWaitSlot += now_us() - t0;
      }
      MP_PROF_BEGIN(MP_PROF_THP_HW);
      Staged s{std::move(job), slot, {}};
      const bool ok = hw_submit(s.job, slot, s.info);
      MP_PROF_END(MP_PROF_THP_HW);
      if (ok) {
        {
          std::lock_guard lock(g_mutex);
          g_staged.push_back(std::move(s));
        }
        g_stageCv.notify_all();
        staged = true;
        break;
      }
      job = std::move(s.job);
      {
        std::unique_lock lock(g_mutex);
        ++g_freeSlots;
        if (g_hw != Hw::On) break;
        // Not physically contiguous enough: try the next memory setup once every
        // buffer is idle.
        g_stageCv.wait(lock, [] { return g_freeSlots == kSlots; });
      }
      if (!next_config()) hw_off("no memory setup works with the decoder", 0);
    }
    if (!staged) {
      software_decode(job);
      finish_job(job.y);
    }
    {
      std::lock_guard lock(g_mutex);
      g_hwBusy = false;
    }
    g_wake.notify_all(); // the helper only takes jobs while this stage is busy
  }
  return nullptr;
}

// Software helper: takes queued frames while the hardware stage is busy (or the
// hardware is off), so the two decoders share the load. On core 0 below the game's
// main thread (160): it runs while the main thread waits, e.g. for a movie frame.
void* helper_stage(void*) {
  pin_self(170, 2, SCE_KERNEL_CPU_MASK_USER_0);
  for (;;) {
    Job job;
    {
      std::unique_lock lock(g_mutex);
      g_wake.wait(lock, [] { return !g_queue.empty() && (g_hwBusy || g_hw == Hw::Off); });
      job = std::move(g_queue.front());
      g_queue.pop_front();
      g_inFlight.push_back(job.y);
    }
    software_decode(job);
    ++g_helperFrames;
    finish_job(job.y);
  }
  return nullptr;
}

void* readback_stage(void*) {
  pin_self(166, 1);
  for (;;) {
    Staged s;
    {
      std::unique_lock lock(g_mutex);
      g_stageCv.wait(lock, [] { return !g_staged.empty(); });
      s = std::move(g_staged.front());
      g_staged.pop_front();
    }
    MP_PROF_BEGIN(MP_PROF_THP_HW);
    hw_finish(s.job, s.slot, s.info);
    MP_PROF_END(MP_PROF_THP_HW);
    {
      std::lock_guard lock(g_mutex);
      ++g_freeSlots;
    }
    g_stageCv.notify_all();
    ++g_hwFrames;
    maybe_log();
    finish_job(s.job.y);
  }
  return nullptr;
}

void start_thread(void* (*fn)(void*)) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 0x10000);
  pthread_t th;
  if (pthread_create(&th, &attr, fn, nullptr) == 0) pthread_detach(th);
  pthread_attr_destroy(&attr);
}

bool pending_for(const void* y) {
  if (y == nullptr) return !g_inFlight.empty() || !g_queue.empty();
  for (const void* p : g_inFlight)
    if (p == y) return true;
  for (const Job& j : g_queue)
    if (j.y == y) return true;
  return false;
}
} // namespace

extern "C" void vita_thp_decode_async(const void* data, uint32_t size, void* y, void* u, void* v) {
  Job job;
  // The software decoder's bit reader reads up to 4 bytes past the last bit it uses.
  job.data.resize(size + 16);
  std::memcpy(job.data.data(), data, size);
  std::memset(job.data.data() + size, 0, 16);
  job.size = size;
  job.y = y;
  job.u = u;
  job.v = v;
  {
    std::lock_guard lock(g_mutex);
    if (!g_started) {
      g_started = true;
      start_thread(decode_stage);
      start_thread(readback_stage);
      start_thread(helper_stage);
    }
    g_queue.push_back(std::move(job));
    if (g_queue.size() > g_maxQueue) g_maxQueue = g_queue.size();
  }
  g_wake.notify_all();
}

extern "C" void vita_thp_wait(const void* y) {
  std::unique_lock lock(g_mutex);
  g_finished.wait(lock, [y] { return !pending_for(y); });
}

extern "C" void vita_thp_wait_all() { vita_thp_wait(nullptr); }
