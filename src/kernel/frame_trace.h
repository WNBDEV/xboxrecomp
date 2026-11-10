/* Frame profiler (frame_trace.c): RECOMP_FRAME_STATS, RECOMP_TRACE. */
#ifndef XBOX_FRAME_TRACE_H
#define XBOX_FRAME_TRACE_H

#include <stdint.h>

enum { FTR_EXEC_CPU, FTR_EXEC_IDLE, FTR_PACING, FTR_PRESENT };

int xbox_FrameTraceOn(void);
double xbox_FrameTraceNowUs(void);
void xbox_FrameTraceTitleThread(void);
void xbox_FrameTraceSpan(const char *name, double t0_us, double t1_us, uint32_t a, uint32_t b);
void xbox_FrameTraceAdd(int what, double ms);
void xbox_FrameTraceDraws(unsigned n);
void xbox_FrameTraceKernelCall(uint32_t ordinal, uint32_t caller, uint32_t arg,
                               double t0_us, double t1_us);
void xbox_FrameTraceFrame(void);

#endif
