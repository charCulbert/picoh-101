#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace picohDSP
{

// One oscillator's slow pitch wander in cents. The amount sets a ceiling (about
// 4 cents at most); a wider one wanders slower. The same seed always drifts the same way.
struct Drift
{
    static constexpr float maxCents = 3.9375f;
    static constexpr int stepFrames = 256; // far finer than the wander moves

    void prepare (double sampleRate, uint32_t seed) noexcept;

    // 0 (stable) to 1. Takes effect at once.
    void setAmount (float amount) noexcept;

    // Steps every stepFrames samples whatever the block size, so a render drifts the same at any buffer
    // size. True if it stepped.
    bool advance (int frames) noexcept;

    // Worked out only when it steps or the amount changes.
    float getCents() const noexcept;

private:
    float sampleRate = 0.0f;
    uint32_t randomState = 1;
    float walk = 0.0f;             // the wander, spread about 1 either side of 0
    float limitCents = 0.0f;       // the most the pitch can be off
    float timeConstant = 2.0f;     // seconds
    float keep = 1.0f, fresh = 0.0f; // the walk's step: walk = keep*walk + fresh*gaussian
    int framesToStep = stepFrames;
    float cents = 0.0f;

    void updateStep() noexcept;
    void updateCents() noexcept;
    float nextUniform() noexcept;  // 0 to 1, never 0
    float nextGaussian() noexcept; // mean 0, standard deviation 1
};

//=====================================================
//  ╷ ╭┬╮ ╭─╮ ╷   ╭─╴ ╭┬╮ ╭─╴ ╭╮╷ ╶┬╴ ╭─╮ ╶┬╴ ╷ ╭─╮ ╭╮╷
//  │ │││ ├─╯ │   ├─  │││ ├─  │││  │  ├─┤  │  │ │ │ │││
//  ╵ ╵ ╵ ╵   ╰─╴ ╰─╴ ╵ ╵ ╰─╴ ╵╰╯  ╵  ╵ ╵  ╵  ╵ ╰─╯ ╵╰╯
//=====================================================

inline void Drift::prepare (double newSampleRate, uint32_t seed) noexcept
{
    sampleRate = static_cast<float> (newSampleRate);
    randomState = seed * 2654435761u + 0x9E3779B9u; // spread nearby seeds apart
    walk = nextGaussian(); // somewhere in its range already, as if long running
    // The first step falls somewhere in the first stepFrames, by seed (159 / 256 is near the golden ratio),
    // so the oscillators don't all step in the same block.
    framesToStep = 1 + static_cast<int> ((seed * 159u) % static_cast<uint32_t> (stepFrames));
    updateStep();
    updateCents();
}

// Squared, so the knob's lower half holds the subtle settings. At 0.3 cents typical or less the time
// constant is 2 s; above, it grows with the size.
inline void Drift::setAmount (float amount) noexcept
{
    const float clamped = std::clamp (amount, 0.0f, 1.0f);
    limitCents = maxCents * clamped * clamped;
    timeConstant = 2.0f * std::max (1.0f, 0.43f * limitCents / 0.3f);
    updateStep();
    updateCents();
}

// An Ornstein-Uhlenbeck process: a random walk pulled back toward 0. Each
// step keeps a share of where it was and takes the rest at random, in
// proportions that keep its spread at 1.
inline void Drift::updateStep() noexcept
{
    if (sampleRate <= 0.0f)
        return;

    keep = std::exp (-static_cast<float> (stepFrames) / (timeConstant * sampleRate));
    fresh = std::sqrt (1.0f - keep * keep);
}

inline bool Drift::advance (int frames) noexcept
{
    bool stepped = false;
    for (framesToStep -= frames; framesToStep <= 0; framesToStep += stepFrames)
    {
        walk = keep * walk + fresh * nextGaussian();
        stepped = true;
    }

    if (stepped)
        updateCents();

    return stepped;
}

inline float Drift::getCents() const noexcept
{
    return cents;
}

// tanh eases toward the limit and never passes it, without sticking there.
inline void Drift::updateCents() noexcept
{
    cents = limitCents * std::tanh (0.5f * walk);
}

inline float Drift::nextUniform() noexcept
{
    randomState = randomState * 1664525u + 1013904223u;
    return (static_cast<float> (randomState >> 8) + 0.5f) / 16777216.0f;
}

// Box-Muller: two uniforms make one Gaussian (its pair is thrown away).
inline float Drift::nextGaussian() noexcept
{
    const float u = nextUniform(), v = nextUniform();
    return std::sqrt (-2.0f * std::log (u)) * std::cos (6.2831853f * v);
}

} // namespace picohDSP
