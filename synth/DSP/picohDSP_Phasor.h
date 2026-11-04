#pragma once

#include <algorithm>
#include <cmath>

namespace picohDSP
{

// A phase rising 0 to 1 at a frequency, wrapping to 0. Says where in the sample each wrap fell.
struct Phasor
{
    // Stopped (frequency 0) until setFrequency.
    void prepare (double sampleRate) noexcept;

    void setPhase (float phase) noexcept; // 0 to 1
    float getPhase() const noexcept { return phase; }

    // Capped below half a cycle per sample, so it wraps at most once a sample. Cheap to call every sample.
    void setFrequency (float hz) noexcept;

    // Slides there across the next frames samples, centred, so the pitch changes
    // without a step and without lagging. Each sample's slide is slideIncrement().
    void rampFrequency (float hz, int frames) noexcept;

    // Cycles per sample, this sample's.
    float getIncrement() const noexcept { return increment; }

    // Moves the increment on during a ramp. advance() does it itself; anything else that moves the phase
    // calls it once a sample, after its last getIncrement().
    void slideIncrement() noexcept { increment += slide; }

    // Moves on a sample. If it wrapped, where in the sample (0 to 1, from
    // the last sample to this one), else -1.
    float advance() noexcept;

    // Moves on part of a sample (under a cycle); true if that wrapped.
    bool advanceAndCheckWrap (float cycles) noexcept;

    // Moves on this many samples at once, in double, at the frequency last set; returns the cycles passed.
    long long skip (int frames) noexcept;

    static constexpr float maxIncrement = 0.49f; // cycles per sample

private:
    float inverseSampleRate = 0.0f;
    float phase = 0.0f;     // 0 to 1
    float increment = 0.0f; // cycles per sample, this sample's
    float slide = 0.0f;     // added to the increment each sample during a ramp
    float target = 0.0f;    // the increment last set: where a ramp is heading

    float incrementFor (float hz) const noexcept;
};

//=====================================================
//  ╷ ╭┬╮ ╭─╮ ╷   ╭─╴ ╭┬╮ ╭─╴ ╭╮╷ ╶┬╴ ╭─╮ ╶┬╴ ╷ ╭─╮ ╭╮╷
//  │ │││ ├─╯ │   ├─  │││ ├─  │││  │  ├─┤  │  │ │ │ │││
//  ╵ ╵ ╵ ╵   ╰─╴ ╰─╴ ╵ ╵ ╰─╴ ╵╰╯  ╵  ╵ ╵  ╵  ╵ ╰─╯ ╵╰╯
//=====================================================

inline void Phasor::prepare (double sampleRate) noexcept
{
    inverseSampleRate = sampleRate > 0.0 ? static_cast<float> (1.0 / sampleRate) : 0.0f;
    increment = slide = target = 0.0f;
}

inline void Phasor::setPhase (float newPhase) noexcept
{
    phase = newPhase - std::floor (newPhase);
}

inline float Phasor::incrementFor (float hz) const noexcept
{
    return std::clamp (hz * inverseSampleRate, 0.0f, maxIncrement);
}

inline void Phasor::setFrequency (float hz) noexcept
{
    increment = target = incrementFor (hz);
    slide = 0.0f;
}

// Sample k of the frames gets newIncrement + slide * (k - (frames - 1) / 2):
// it passes through the new increment half way, so the frames add up to
// newIncrement * frames. The two ends are held no further either side of
// the new increment than it is from 0 and from the cap, so a wide jump at
// a low pitch never runs the phase backwards.
inline void Phasor::rampFrequency (float hz, int frames) noexcept
{
    const float newIncrement = incrementFor (hz);
    const float halfSpan =
        0.5f * (newIncrement - target) * static_cast<float> (frames - 1) / static_cast<float> (std::max (1, frames));
    const float limit = 0.99f * std::min (newIncrement, maxIncrement - newIncrement); // a little inside, for rounding
    const float heldHalfSpan = std::clamp (halfSpan, -limit, limit);

    slide = frames > 1 ? 2.0f * heldHalfSpan / static_cast<float> (frames - 1) : 0.0f;
    increment = newIncrement - heldHalfSpan;
    target = newIncrement;
}

inline float Phasor::advance() noexcept
{
    const float start = phase;
    const float thisIncrement = increment;
    increment += slide;
    phase += thisIncrement;
    if (phase < 1.0f)
        return -1.0f;

    phase -= 1.0f;
    return (1.0f - start) / thisIncrement;
}

inline bool Phasor::advanceAndCheckWrap (float cycles) noexcept
{
    phase += cycles;
    if (phase < 1.0f)
        return false;

    phase -= 1.0f;
    return true;
}

inline long long Phasor::skip (int frames) noexcept
{
    increment = target;
    slide = 0.0f;
    const double total = static_cast<double> (phase) + static_cast<double> (increment) * frames;
    const double cycles = std::floor (total);
    phase = static_cast<float> (total - cycles);
    return static_cast<long long> (cycles);
}

} // namespace picohDSP
