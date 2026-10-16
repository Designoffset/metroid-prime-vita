/* Lightweight profiling counters for the Vita port (no-ops elsewhere).
 * MP_PROF_BEGIN(id) / MP_PROF_END(id) accumulate wall time spent in a section;
 * vita_prof.cpp prints the totals to the log every few seconds. */
#pragma once
#include <stdint.h>

enum {
  MP_PROF_DISC_READ = 0,
  MP_PROF_THP_VIDEO,
  MP_PROF_AUDIO_MIX,
  MP_PROF_MUSYX_FRAME,
  MP_PROF_IRQ_LOCK,
  MP_PROF_THP_HW,
  MP_PROF_COUNT
};

#ifdef __vita__
#ifdef __cplusplus
extern "C" {
#endif
extern volatile uint64_t mp_prof_us[MP_PROF_COUNT];
extern volatile uint32_t mp_prof_calls[MP_PROF_COUNT];
extern volatile int mp_prof_audio_thid;
// Threads that place themselves; vita_prof.cpp does not move them.
extern volatile int mp_prof_keep_thid[8];
int sceKernelGetThreadId(void);
#define MP_PROF_SET_AUDIO_THREAD() (mp_prof_audio_thid = sceKernelGetThreadId())
uint64_t sceKernelGetProcessTimeWide(void);
#ifdef __cplusplus
}
#endif
#define MP_PROF_BEGIN(id) const uint64_t _mp_prof_t_##id = sceKernelGetProcessTimeWide()
#define MP_PROF_END(id)                                                                                                \
  do {                                                                                                                 \
    mp_prof_us[id] += sceKernelGetProcessTimeWide() - _mp_prof_t_##id;                                                 \
    mp_prof_calls[id]++;                                                                                               \
  } while (0)
#else
#define MP_PROF_BEGIN(id) ((void)0)
#define MP_PROF_END(id) ((void)0)
#define MP_PROF_SET_AUDIO_THREAD() ((void)0)
#endif
