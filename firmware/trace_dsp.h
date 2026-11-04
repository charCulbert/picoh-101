#pragma once
// TRACE_DSP: -include'd into Synth.cpp, so the synth's trace points stamp into the audio core's ring.
// dspRing is set only for recorded blocks, so a point elsewhere costs a load and a test.
#define PICOH_TRACE_POINT(point, arg)                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        if (trace::Ring* ring = trace::dspRing)                                                                        \
            ring->stamp (trace::dsp::point, uint16_t (arg));                                                           \
    } while (0)
// After the macro: picohDSP_TraceHook.h, which trace.h includes, defines its default only if it's missing.
#include "trace.h"
