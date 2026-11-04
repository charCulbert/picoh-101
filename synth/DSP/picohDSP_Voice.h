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
// is the gate. The LFO is read once per render() call.
struct Voice
{
    static constexpr float keyTrackPivot = 35.0f;      // the note key tracking leaves alone (B1)
    static constexpr float envelopeSemitones = 192.0f; // Env Amount 1 moves the cutoff this far

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
    float key = 60.0f; // the gliding key, this block
    Envelope envelope;
    Gate gate;
    Filter filter;
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
    const Settings& s = *settings;
    key = glide.next (frames);
    vco.moveNote (key);
    const float lfoValue = lfo.next (frames);
    const float triangle = lfo.getTriangle();
    vco.setModulation (lfoValue * s.lfoToPitchCents + triangle * s.wheelToPitchCents + s.bendCents);

    vco.startBlock (frames);
    PICOH_TRACE_POINT (voiceStarted, frames);
    const float cutoffSemitones = (key - keyTrackPivot) * s.keyTrack + lfoValue * s.lfoToCutoffSemitones
                                 + triangle * s.wheelToCutoffSemitones + s.bendCutoffSemitones;

    // Per chunk: the ADSR and the gate first (ENV can move the pulse's width), then the VCO, then the filter
    // and VCA. In one loop they don't fit the registers. The ADSR and the gate both run, so switching the
    // VCA mid-note doesn't jump.
    const float sweepScale = s.envAmount * envelopeSemitones;
    constexpr int chunk = 32, holdLength = 16;
    float sound[chunk], adsr[chunk], open[chunk], lfoWidth[chunk];
    for (int start = 0; start < frames; start += chunk)
    {
        const int count = std::min (chunk, frames - start);
        for (int i = 0; i < count; ++i)
        {
            adsr[i] = envelope.next();
            open[i] = gate.next();
        }
        const float* widthMod = nullptr;
        if (s.pwmSource == PwmSource::env)
            widthMod = adsr;
        else if (s.pwmSource == PwmSource::lfo)
        {
            std::fill (lfoWidth, lfoWidth + count, 0.5f * (lfoValue + 1.0f));
            widthMod = lfoWidth;
        }
        vco.render (sound, count, widthMod);
        PICOH_TRACE_POINT (oscillatorsDone, start);

        // The cutoff is held for holdLength samples at the ADSR's middle value (Filter::hold).
        for (int from = 0; from < count; from += holdLength)
        {
            const int length = std::min (holdLength, count - from);
            const float middle = 0.5f * (adsr[from + (length - 1) / 2] + adsr[from + length / 2]);
            filter.hold (middle * sweepScale + cutoffSemitones, length);
            const float* level = s.vcaGate ? open : adsr;
            for (int i = from; i < from + length; ++i)
                out[start + i] += filter.process (sound[i]) * level[i];
        }
        PICOH_TRACE_POINT (filterDone, start);
    }
}

inline void Voice::skip (int frames) noexcept
{
    lfo.next (frames); // idle, it still runs
    vco.skip (frames);
    filter.reset();
}

} // namespace picohDSP
