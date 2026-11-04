#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include "picohDSP_Phasor.h"
#include "picohDSP_SmoothedValue.h"

namespace picohDSP
{

// The LFO: free-running, restarted only by restart(). Worked out once a block; rate glides over 20 ms.
struct LFO
{
    // The SH-101's.
    enum class Waveform
    {
        triangle,   // from 0 up to 1, down to -1 and back
        square,     // 1 for the first half, then -1
        sampleHold, // RANDOM: a new random value from -1 to 1 each cycle
        noise       // NOISE: a new random value each call to next(), whatever the rate
    };

    static constexpr float minRateHz = 0.01f, maxRateHz = 40.0f;

    // seed sets its starting phase and its random values, so a render is the same every time.
    void prepare (double sampleRate, uint32_t seed) noexcept;

    void setRate (float hz) noexcept; // minRateHz to maxRateHz
    void setWaveform (Waveform waveform) noexcept { this->waveform = waveform; }

    // Moves on this many samples; the value there, -1 to 1.
    float next (int frames) noexcept;
    float getTriangle() const noexcept; // the triangle at the same point, whatever the waveform: the bender's push
    void restart() noexcept;            // back to its head: the square going high, the triangle up from 0
    bool isHigh() const noexcept;       // the square, now
    int samplesToEdge() const noexcept; // until the square next changes, at the current rate

private:
    static constexpr float glideSeconds = 0.02f;

    Phasor phasor;
    SmoothedValue<float> rate { 0.5f };
    Waveform waveform = Waveform::triangle;
    float sampleRate = 0.0f;
    float held = 0.0f; // sample and hold's value this cycle
    uint32_t randomState = 1;

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
    phasor.prepare (newSampleRate);
    const float start = static_cast<float> (seed) * 0.618034f;
    phasor.setPhase (start - std::floor (start));
    rate.reset (rate.getTarget());
    phasor.setFrequency (rate.getCurrent());
    randomState = seed * 2654435761u + 1u; // spread nearby seeds apart
    held = nextRandom();
}

inline void LFO::setRate (float hz) noexcept
{
    rate.setTarget (std::clamp (hz, minRateHz, maxRateHz), static_cast<int> (glideSeconds * sampleRate));
}

inline float LFO::next (int frames) noexcept
{
    rate.skip (frames);
    phasor.setFrequency (rate.getCurrent());
    if (phasor.skip (frames) > 0)
        held = nextRandom();
    if (waveform == Waveform::noise)
        return nextRandom();

    return valueAt (phasor.getPhase());
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
    phasor.setPhase (0.0f);
    held = nextRandom();
}

inline bool LFO::isHigh() const noexcept
{
    return phasor.getPhase() < 0.5f;
}

inline int LFO::samplesToEdge() const noexcept
{
    const float phase = phasor.getPhase();
    const float distance = (phase < 0.5f ? 0.5f : 1.0f) - phase;
    return std::max (1, static_cast<int> (std::ceil (distance / phasor.getIncrement())));
}

inline float LFO::getTriangle() const noexcept
{
    return triangleAt (phasor.getPhase());
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
