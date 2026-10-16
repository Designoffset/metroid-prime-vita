/* Named timing zones for the Vita port's function profile (no-ops elsewhere).
 *   MP_ZONE("name");   at the top of a function or block: its wall time, inclusive
 *                      of what it calls, and how often it ran.
 * vita_prof.cpp logs the costliest zones every few seconds, per frame. */
#pragma once
#ifdef __cplusplus
#include <stdint.h>
#ifdef __vita__
extern "C" uint64_t sceKernelGetProcessTimeWide(void);
struct MpZone {
  const char* name;
  volatile uint64_t us;
  volatile uint32_t calls;
  MpZone* next;
  uint64_t lastUs;
  uint32_t lastCalls;
  explicit MpZone(const char* n);
};
struct MpZoneScope {
  MpZone& z;
  uint64_t t;
  explicit MpZoneScope(MpZone& zone) : z(zone), t(sceKernelGetProcessTimeWide()) {}
  ~MpZoneScope() {
    z.us += sceKernelGetProcessTimeWide() - t;
    z.calls++;
  }
};
#define MP_ZONE_CAT2(a, b) a##b
#define MP_ZONE_CAT(a, b) MP_ZONE_CAT2(a, b)
#define MP_ZONE(nm)                                                                                                    \
  static MpZone MP_ZONE_CAT(_mp_zone_, __LINE__)(nm);                                                                  \
  MpZoneScope MP_ZONE_CAT(_mp_zone_scope_, __LINE__)(MP_ZONE_CAT(_mp_zone_, __LINE__))
#else
#define MP_ZONE(nm) ((void)0)
#endif
#endif
