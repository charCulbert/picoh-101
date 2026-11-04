#pragma once

// Trace points inside the synth's render, for timing it on hardware. PICOH_TRACE_POINT (point, arg) is
// ((void)0) unless a build defines it before this header (or with -include). arg keeps 16 bits, so scale a
// fraction first (level * 1000). PICOH_TRACE_POINTS is the one list, so ids and names can't drift apart:
//
//   enum class Point { PICOH_TRACE_POINTS (PICOH_TRACE_ENUM) };
//
// New points go at the end, which keeps the old ids. In the order a render reaches them:
//   synthBegin       Synth::render starts; arg: frames
//   monoCleared      a piece's mono buffer is cleared; arg: the piece's frames
//   voiceBegin       the voice is next; arg: 0
//   voiceStarted     the voice's VCO has started the block; arg: frames
//   oscillatorsDone  a chunk's oscillator pass is done; arg: the chunk's first sample
//   filterDone       the chunk's filter and envelope pass is done; arg: its first sample
//   voiceEnd         the voice rendered; arg: 0
//   voiceSkipped     the voice was idle and only skipped the block; arg: 0
//   outputCopied     a piece is scaled into left and right; arg: the piece's frames
//
// Points sit between passes, never inside a per-sample loop: Voice::render is sensitive to register pressure.

#define PICOH_TRACE_POINTS(X)                                                                                          \
    X (synthBegin)                                                                                                     \
    X (monoCleared)                                                                                                    \
    X (voiceBegin)                                                                                                     \
    X (voiceStarted)                                                                                                   \
    X (oscillatorsDone)                                                                                                \
    X (filterDone)                                                                                                     \
    X (voiceEnd)                                                                                                       \
    X (voiceSkipped)                                                                                                   \
    X (outputCopied)

#define PICOH_TRACE_ENUM(point) point,
#define PICOH_TRACE_NAME(point) #point,

#ifndef PICOH_TRACE_POINT
#define PICOH_TRACE_POINT(point, arg) ((void) 0)
#endif
