#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include "picohDSP_SmoothedValue.h"

namespace picohDSP
{

// The LFO: free-running, restarted only by restart(). Worked out once a control tick (tick()), for the
// value at the tick's end; rate glides over 20 ms. Between ticks the phase moves on in a straight line, for
// isHigh() and samplesToEdge() at the sample reached (advance()).
struct LFO
{
    // The SH-101's.
    enum class Waveform
    {
        triangle,   // from 0 up to 1, down to -1 and back
        square,     // 1 for the first half, then -1
        sampleHold, // RANDOM: a new random value from -1 to 1 each cycle
        noise       // NOISE: a new random value each tick, whatever the rate
    };

    static constexpr float minRateHz = 0.01f, maxRateHz = 40.0f;

    // seed sets its starting phase and its random values, so a render is the same every time.
    void prepare (double sampleRate, uint32_t seed) noexcept;

    void setRate (float hz) noexcept; // minRateHz to maxRateHz
    void setWaveform (Waveform waveform) noexcept { this->waveform = waveform; }

    // From the last tick's end, the next frames samples: their value at the end comes from getValue().
    void tick (int frames) noexcept;
    void advance (int frames) noexcept { at += frames; } // samples rendered since the tick, up to its frames

    float getValue() const noexcept { return value; }                // -1 to 1, at the tick's end
    float getTriangle() const noexcept { return triangleAt (end); } // the triangle there, whatever the waveform: the bender's push
    void restart() noexcept;            // back to its head here: the square going high, the triangle up from 0
    bool isHigh() const noexcept;       // the square, here
    int samplesToEdge() const noexcept; // until the square next changes, at the tick's rate, at most a tick on

private:
    static constexpr float glideSeconds = 0.02f;

    SmoothedValue<float> rate { 0.5f };
    Waveform waveform = Waveform::triangle;
    float inverseSampleRate = 0.0f, sampleRate = 0.0f;
    float start = 0.0f, end = 0.0f, increment = 0.0f; // the phase at the tick and its end, and per sample
    int at = 0, length = 0;                           // samples since the tick, and in it
    float value = 0.0f;
    float held = 0.0f; // sample and hold's value this cycle
    uint32_t randomState = 1;

    // The phase samples after the tick: the same sum for the tick's end, so the edges agree with it.
    float phaseAt (int samples) const noexcept
    {
        const float phase = start + increment * static_cast<float> (samples);
        return phase < 1.0f ? phase : phase - 1.0f;
    }
    float valueAt (float phase) const noexcept; // -1 to 1
    static float triangleAt (float phase) noexcept;
    float nextRandom() noexcept;                // -1 to 1
};

//=====================================================
//  ╷ ╭┬╮ ╭─╮ ╷   ╭─╴ ╭┬╮ ╭─╴ ╭╮╷ ╶┬╴ ╭─╮ ╶┬╴ ╷ ╭─╮ ╭╮╷
//  │ │││ ├─╯ │   ├─  │││ ├─  │││  │  ├─┤  │  │ │ │ │││
//  ╵ ╵ ╵ ╵   ╰─╴ ╰─╴ ╵ ╵ ╰─╴ ╵╰╯  ╵  ╵ ╵  ╵  ╵ ╰─╯ ╵╰╯
//=====================================================

inline void LFO::prepare (double newSampleRate, uint32_t seed) noexcept
{
    sampleRate = static_cast<float> (newSampleRate);
    inverseSampleRate = 1.0f / sampleRate;
    const float phase = static_cast<float> (seed) * 0.618034f;
    start = end = phase - std::floor (phase);
    at = length = 0;
    rate.reset (rate.getTarget());
    increment = rate.getCurrent() * inverseSampleRate;
    randomState = seed * 2654435761u + 1u; // spread nearby seeds apart
    held = nextRandom();
    value = valueAt (end);
}

inline void LFO::setRate (float hz) noexcept
{
    rate.setTarget (std::clamp (hz, minRateHz, maxRateHz), static_cast<int> (glideSeconds * sampleRate));
}

inline void LFO::tick (int frames) noexcept
{
    start = end;
    at = 0;
    length = frames;
    rate.skip (frames);
    increment = rate.getCurrent() * inverseSampleRate;
    if (start + increment * static_cast<float> (frames) >= 1.0f)
        held = nextRandom();
    end = phaseAt (frames);
    value = waveform == Waveform::noise ? nextRandom() : valueAt (end);
}

inline float LFO::valueAt (float phase) const noexcept
{
    switch (waveform)
    {
        case Waveform::triangle: return triangleAt (phase);
        case Waveform::square: return phase < 0.5f ? 1.0f : -1.0f;
        case Waveform::sampleHold: return held;
        case Waveform::noise: break;
    }
    return 0.0f;
}

inline void LFO::restart() noexcept
{
    start = -(increment * static_cast<float> (at));
    end = phaseAt (length);
    held = nextRandom();
    if (waveform != Waveform::noise)
        value = valueAt (end);
}

inline bool LFO::isHigh() const noexcept
{
    return phaseAt (at) < 0.5f;
}

inline int LFO::samplesToEdge() const noexcept
{
    const float phase = phaseAt (at);
    const float distance = (phase < 0.5f ? 0.5f : 1.0f) - phase;
    const int edge = std::max (1, static_cast<int> (std::ceil (distance / increment)));
    return std::min (edge, at < length ? length - at : std::max (1, length));
}

// A quarter on, so it starts at 0 going up.
inline float LFO::triangleAt (float phase) noexcept
{
    const float shifted = phase < 0.75f ? phase + 0.25f : phase - 0.75f;
    return 1.0f - 4.0f * std::abs (shifted - 0.5f);
}

// A linear congruential generator, scaled to -1 to 1.
inline float LFO::nextRandom() noexcept
{
    randomState = 1664525u * randomState + 1013904223u;
    return 4.656613e-10f * static_cast<float> (static_cast<int32_t> (randomState));
}

} // namespace picohDSP
