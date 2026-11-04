#pragma once

#include <algorithm>
#include "picohDSP_Envelope.h"
#include "picohDSP_Filter.h"
#include "picohDSP_Gate.h"
#include "picohDSP_Glide.h"
#include "picohDSP_LFO.h"
#include "picohDSP_TraceHook.h"
#include "picohDSP_Vco.h"

namespace picohDSP
{

// One note: VCO, VCF, VCA. The ADSR sweeps the cutoff and is the VCA on ENV; on GATE the VCA
// is the gate.
//
// The controls (glide, LFO, the modulation sums, the VCO's pitch and the filter's coefficients) are worked
// out on a clock: every tickLength samples counted from prepare(), for the tickLength to come, however
// render() calls split the samples. Notes, gates and the ADSR still move on their own sample; a new key's
// pitch is set at once, its key tracking and glide wait for the next tick. After skip() the first render()
// works them out at once.
struct Voice
{
    static constexpr float keyTrackPivot = 35.0f;      // the note key tracking leaves alone (B1)
    static constexpr float envelopeSemitones = 192.0f; // Env Amount 1 moves the cutoff this far
    static constexpr int tickLength = 16;

    // What moves the pulse's width: the PULSE WIDTH slider alone, or the LFO or the ADSR by that much.
    enum class PwmSource
    {
        manual,
        lfo,
        env
    };

    // The controls, read at each render.
    struct Settings
    {
        float lfoToPitchCents = 0.0f;       // the LFO at full swing moves the pitch this far
        float lfoToCutoffSemitones = 0.0f;  // and the cutoff this far
        float wheelToPitchCents = 0.0f;      // the bender pushed: the LFO's triangle at full swing moves the pitch
        float wheelToCutoffSemitones = 0.0f; // and the cutoff this far
        float bendCents = 0.0f;              // the bender sideways, now
        float bendCutoffSemitones = 0.0f;
        float envAmount = 0.0f;             // -1 to 1: the ADSR's sweep, envelopeSemitones at 1
        float keyTrack = 0.0f;              // -1 to 1: at 1 the cutoff follows the note a semitone per semitone
        bool vcaGate = false;               // the VCA on GATE instead of the ADSR
        PwmSource pwmSource = PwmSource::manual;
    };

    void prepare (double sampleRate, size_t index) noexcept;
    void start (int key, bool glide) noexcept; // plays key, and the ADSR attacks from where it is
    void setKey (int key, bool glide) noexcept;
    void attack() noexcept; // the ADSR and the gate again from where they are, the key unchanged
    void release() noexcept;
    void stop() noexcept;
    bool isActive() const noexcept;
    void render (float* __restrict out, int frames) noexcept; // adds into out
    void skip (int frames) noexcept;

    // Before start().
    void setSettings (const Settings& newSettings) noexcept { settings = &newSettings; }

    Envelope& getEnvelope() noexcept { return envelope; }
    Filter& getFilter() noexcept { return filter; }
    Vco& getVco() noexcept { return vco; }
    LFO& getLfo() noexcept { return lfo; }
    Glide& getGlide() noexcept { return glide; }

private:
    Vco vco;
    LFO lfo;
    Glide glide;
    const Settings* settings = nullptr;
    float key = 60.0f; // the gliding key, this tick
    Envelope envelope;
    Gate gate;
    Filter filter;

    int sinceTick = 0;
    bool late = true; // the controls are due before the next tick: after prepare() or skip()
    float cutoffSemitones = 0.0f, halfSweepScale = 0.0f; // the cutoff with the ADSR at 0, and per ADSR unit, halved
    float lfoWidth = 0.0f;                                 // the PWM on LFO

    void control (int frames) noexcept;               // the controls, for the next frames samples
    void renderRun (float* __restrict out, int frames) noexcept; // within a tick
    void advanceClock (int frames) noexcept;
};

//=====================================================
//  ╷ ╭┬╮ ╭─╮ ╷   ╭─╴ ╭┬╮ ╭─╴ ╭╮╷ ╶┬╴ ╭─╮ ╶┬╴ ╷ ╭─╮ ╭╮╷
//  │ │││ ├─╯ │   ├─  │││ ├─  │││  │  ├─┤  │  │ │ │ │││
//  ╵ ╵ ╵ ╵   ╰─╴ ╰─╴ ╵ ╵ ╰─╴ ╵╰╯  ╵  ╵ ╵  ╵  ╵ ╰─╯ ╵╰╯
//=====================================================

inline void Voice::prepare (double sampleRate, size_t index) noexcept
{
    envelope.prepare (sampleRate);
    gate.prepare (sampleRate);
    filter.prepare (sampleRate);
    vco.prepare (sampleRate, index);
    lfo.prepare (sampleRate, static_cast<uint32_t> (index));
    glide.prepare (sampleRate);
    sinceTick = 0;
    late = true;
}

inline void Voice::start (int newKey, bool slide) noexcept
{
    setKey (newKey, slide);
    attack();
}

inline void Voice::attack() noexcept
{
    envelope.start();
    gate.start();
}

inline void Voice::setKey (int newKey, bool slide) noexcept
{
    if (slide && glide.moveTo (static_cast<float> (newKey)))
        return;
    key = static_cast<float> (newKey);
    glide.jump (key);
    vco.setNote (key);
}

inline void Voice::release() noexcept
{
    envelope.release();
    gate.release();
}

inline void Voice::stop() noexcept
{
    envelope.stop();
    gate.stop();
}

inline bool Voice::isActive() const noexcept
{
    return settings->vcaGate ? gate.isActive() : envelope.isActive();
}

inline void Voice::render (float* __restrict out, int frames) noexcept
{
    for (int done = 0; done < frames;)
    {
        if (sinceTick == 0)
            lfo.tick (tickLength);
        if (sinceTick == 0 || late)
            control (tickLength - sinceTick);
        const int length = std::min (frames - done, tickLength - sinceTick);
        renderRun (out + done, length);
        advanceClock (length);
        done += length;
    }
}

inline void Voice::skip (int frames) noexcept
{
    for (int done = 0; done < frames;) // idle, the LFO still runs
    {
        if (sinceTick == 0)
            lfo.tick (tickLength);
        const int length = std::min (frames - done, tickLength - sinceTick);
        advanceClock (length);
        done += length;
    }
    vco.skip (frames);
    filter.reset();
    late = true;
}

inline void Voice::advanceClock (int frames) noexcept
{
    lfo.advance (frames);
    sinceTick = (sinceTick + frames) % tickLength;
}

// The filter's cutoff is held over the frames at the ADSR's middle value (Filter::hold), read ahead on a
// copy so the ADSR itself stays on this sample.
inline void Voice::control (int frames) noexcept
{
    const Settings& s = *settings;
    key = glide.next (frames);
    vco.moveNote (key);
    const float lfoValue = lfo.getValue();
    const float triangle = lfo.getTriangle();
    vco.setModulation (lfoValue * s.lfoToPitchCents + triangle * s.wheelToPitchCents + s.bendCents);
    vco.startBlock (frames);
    cutoffSemitones = (key - keyTrackPivot) * s.keyTrack + lfoValue * s.lfoToCutoffSemitones
                    + triangle * s.wheelToCutoffSemitones + s.bendCutoffSemitones;
    halfSweepScale = s.envAmount * envelopeSemitones * 0.5f;
    lfoWidth = 0.5f * (lfoValue + 1.0f);

    Envelope ahead = envelope;
    const int below = (frames - 1) / 2, above = frames / 2;
    float twiceMiddle = 0.0f;
    for (int i = 0; i <= above; ++i)
    {
        const float level = ahead.next();
        if (i == below)
            twiceMiddle = level;
        if (i == above)
            twiceMiddle += level;
    }
    filter.hold (twiceMiddle * halfSweepScale + cutoffSemitones, frames);
    late = false;
    PICOH_TRACE_POINT (voiceStarted, frames);
}

// The ADSR and the gate first (ENV can move the pulse's width), then the VCO, then the filter and VCA. In one
// loop they don't fit the registers. The ADSR and the gate both run, so switching the VCA mid-note doesn't
// jump.
inline void Voice::renderRun (float* __restrict out, int frames) noexcept
{
    const Settings& s = *settings;
    float sound[tickLength], adsr[tickLength], open[tickLength], width[tickLength];
    for (int i = 0; i < frames; ++i)
    {
        adsr[i] = envelope.next();
        open[i] = gate.next();
    }
    const float* widthMod = nullptr;
    if (s.pwmSource == PwmSource::env)
        widthMod = adsr;
    else if (s.pwmSource == PwmSource::lfo)
    {
        std::fill (width, width + frames, lfoWidth);
        widthMod = width;
    }
    vco.render (sound, frames, widthMod);
    PICOH_TRACE_POINT (oscillatorsDone, frames);

    const float* level = s.vcaGate ? open : adsr;
    for (int i = 0; i < frames; ++i)
        out[i] += filter.process (sound[i]) * level[i];
    PICOH_TRACE_POINT (filterDone, frames);
}

} // namespace picohDSP
