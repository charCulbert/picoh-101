#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include "picohDSP_Discontinuities.h"
#include "picohDSP_Drift.h"
#include "picohDSP_EllipticBlep.h"
#include "picohDSP_Phasor.h"
#include "picohDSP_SmoothedValue.h"

namespace picohDSP
{

// The VCO and source mixer: saw, pulse and sub oscillator from one phase, and white noise, each
// at its own level, summed into one corrected sample. The sub is divided from the phase's wraps, so its
// steps fall exactly on the saw's and the same elliptic BLEP smooths them all.
struct Vco
{
    enum class Source
    {
        pulse,
        saw,
        sub,
        noise
    };

    enum class SubWave
    {
        octaveDown,         // square, -1 oct: flips every cycle
        twoOctavesDown,     // square, -2 oct: flips every 2 cycles
        twoOctavesDownPulse // pulse, -2 oct, 25%: high for 1 cycle in 4
    };

    static constexpr float levelGlideSeconds = 0.02f;
    // How far the pitch wanders (see Drift), 0 to 1: about 0.04 cents either way.
    static constexpr float driftAmount = 0.1f;
    // The pulse's width, from square (0) to its narrowest (1).
    static constexpr float squareWidth = 0.5f, narrowestWidth = 0.05f;

    // index seeds the phase, the drift and the noise.
    void prepare (double sampleRate, size_t index) noexcept;

    // Built for this sample rate.
    void setCorrectionTable (const EllipticBlep::Table& table) noexcept;

    // In MIDI notes (0.01 is a cent). Retunes at once, without a slide.
    void setNote (float key) noexcept;
    // The same, reached by sliding across the next block (startBlock).
    void moveNote (float key) noexcept;

    // Cents from the note: TUNE, RANGE and TRANSPOSE.
    void setTune (float cents) noexcept;

    // Once a block, before startBlock(): the LFO's share of the pitch, in cents.
    void setModulation (float cents) noexcept;

    // 0 to 1, gliding over levelGlideSeconds.
    void setLevel (Source, float level) noexcept;

    // Takes effect at the sub's next step, so it never jumps mid-cycle.
    void setSubWave (SubWave) noexcept;

    // The PULSE WIDTH slider, 0 to 1. On MAN it is the width (square to narrowest); with a PWM source
    // it is the depth that source moves the width by.
    void setPulseWidth (float amount) noexcept;

    // Once a block, before render(): steps the drift and slides to the new pitch across the block.
    void startBlock (int frames) noexcept;

    // count samples of the corrected mix. widthMod is the PWM source (the LFO or the ADSR, 0 to 1), one
    // value per sample, or nullptr on MAN.
    void render (float* __restrict out, int count, const float* __restrict widthMod) noexcept;

    // Moves on this many samples without making any.
    void skip (int frames) noexcept;

private:
    Phasor phasor;
    Drift drift;
    EllipticBlep correction;
    Discontinuities steps;
    std::array<SmoothedValue<float>, 4> levels;
    SmoothedValue<float> width;
    SubWave subWave = SubWave::octaveDown, nextSubWave = SubWave::octaveDown;
    int subCount = 0; // the phase's wraps, counted for the divider
    uint32_t noiseState = 1;
    float key = 60.0f, tuneCents = 0.0f, modulationCents = 0.0f, sampleRate = 0.0f;
    bool rampPitch = false;

    float frequency() const noexcept;
    float subValue() const noexcept; // the sub's level now, -1 or 1
    float nextNoise() noexcept;
};

//=====================================================
//  ╷ ╭┬╮ ╭─╮ ╷   ╭─╴ ╭┬╮ ╭─╴ ╭╮╷ ╶┬╴ ╭─╮ ╶┬╴ ╷ ╭─╮ ╭╮╷
//  │ │││ ├─╯ │   ├─  │││ ├─  │││  │  ├─┤  │  │ │ │ │││
//  ╵ ╵ ╵ ╵   ╰─╴ ╰─╴ ╵ ╵ ╰─╴ ╵╰╯  ╵  ╵ ╵  ╵  ╵ ╰─╯ ╵╰╯
//=====================================================

inline void Vco::prepare (double newSampleRate, size_t index) noexcept
{
    sampleRate = static_cast<float> (newSampleRate);
    phasor.prepare (newSampleRate);
    correction.prepare (newSampleRate);
    const float start = static_cast<float> (index) * 0.618034f;
    phasor.setPhase (start - std::floor (start));
    drift.prepare (newSampleRate, static_cast<uint32_t> (index));
    drift.setAmount (driftAmount);
    noiseState = static_cast<uint32_t> (index) * 2654435761u + 1u;
    for (auto& level : levels)
        level.reset (level.getTarget());
    width.reset (width.getTarget());
    subCount = 0;
    subWave = nextSubWave;
    steps.clear();
    rampPitch = false;
}

inline void Vco::setCorrectionTable (const EllipticBlep::Table& table) noexcept
{
    correction.setTable (table);
}

inline void Vco::setNote (float newKey) noexcept
{
    key = newKey;
    phasor.setFrequency (frequency());
    rampPitch = false;
}

inline void Vco::moveNote (float newKey) noexcept
{
    key = newKey;
}

inline void Vco::setTune (float cents) noexcept
{
    tuneCents = cents;
    phasor.setFrequency (frequency());
}

inline void Vco::setModulation (float cents) noexcept
{
    modulationCents = cents;
}

inline void Vco::setLevel (Source source, float level) noexcept
{
    levels[static_cast<size_t> (source)].setTarget (std::clamp (level, 0.0f, 1.0f),
                                                    static_cast<int> (levelGlideSeconds * sampleRate));
}

inline void Vco::setSubWave (SubWave wave) noexcept
{
    nextSubWave = wave;
}

// 5 ms, so moving the slider doesn't click.
inline void Vco::setPulseWidth (float amount) noexcept
{
    width.setTarget (std::clamp (amount, 0.0f, 1.0f), static_cast<int> (0.005f * sampleRate));
}

// Retuned every block, moving or not, so the cost doesn't depend on modulation.
inline void Vco::startBlock (int frames) noexcept
{
    drift.advance (frames);
    if (! rampPitch)
    {
        phasor.setFrequency (frequency());
        rampPitch = true;
        return;
    }
    phasor.rampFrequency (frequency(), frames);
}

// Each wave jumps where the cycle restarts (the saw and pulse up by 2, the sub by its divider's step), and
// the pulse falls by 2 at its width; all are timed within the sample from the phase, scaled by their
// levels, and handed to the one correction. Noise needs none.
inline void Vco::render (float* __restrict out, int count, const float* __restrict widthMod) noexcept
{
    for (int i = 0; i < count; ++i)
    {
        const float pulseLevel = levels[0].getNext(), sawLevel = levels[1].getNext();
        const float subLevel = levels[2].getNext(), noiseLevel = levels[3].getNext();
        const float amount = width.getNext() * (widthMod ? widthMod[i] : 1.0f);
        const float pulseWidth = squareWidth - (squareWidth - narrowestWidth) * amount;
        const float increment = phasor.getIncrement();
        const float start = phasor.getPhase();
        const auto fallBetween = [&] (float from, float to, float offset)
        {
            if (pulseWidth > from && pulseWidth <= to)
                steps.addStep (offset + (pulseWidth - from) / increment, -2.0f * pulseLevel);
        };

        steps.clear();
        if (! phasor.advanceAndCheckWrap (increment))
            fallBetween (start, phasor.getPhase(), 0.0f);
        else
        {
            const float wrapOffset = (1.0f - start) / increment;
            fallBetween (start, 1.0f, 0.0f);
            const float subBefore = subValue();
            subCount = (subCount + 1) & 3;
            subWave = nextSubWave;
            steps.addStep (wrapOffset, 2.0f * (sawLevel + pulseLevel) + (subValue() - subBefore) * subLevel);
            fallBetween (0.0f, phasor.getPhase(), wrapOffset);
        }
        phasor.slideIncrement();

        for (const Discontinuity& d : steps)
            correction.addStep (d.time, d.height);
        const float p = phasor.getPhase();
        const float mix =
            sawLevel * (1.0f - 2.0f * p) + pulseLevel * (p < pulseWidth ? 1.0f : -1.0f) + subLevel * subValue();
        out[i] = correction.process (mix) + noiseLevel * nextNoise();
    }
}

inline void Vco::skip (int frames) noexcept
{
    subCount = static_cast<int> ((subCount + phasor.skip (frames)) & 3);
    subWave = nextSubWave;
    drift.advance (frames);
    for (auto& level : levels)
        level.reset (level.getTarget());
    width.reset (width.getTarget());
    steps.clear();
    correction.reset();
}

inline float Vco::frequency() const noexcept
{
    const float semitones = key - 69.0f + (tuneCents + modulationCents + drift.getCents()) / 100.0f;
    return 440.0f * std::exp2 (semitones / 12.0f);
}

// subCount counts the cycles 0-3: -1 oct flips every cycle, -2 oct every 2, the pulse is high for 1 in 4.
inline float Vco::subValue() const noexcept
{
    switch (subWave)
    {
        case SubWave::octaveDown: return subCount & 1 ? -1.0f : 1.0f;
        case SubWave::twoOctavesDown: return subCount & 2 ? -1.0f : 1.0f;
        case SubWave::twoOctavesDownPulse: return subCount == 0 ? 1.0f : -1.0f;
    }
    return 0.0f;
}

// White, from a linear congruential generator, -1 to 1.
inline float Vco::nextNoise() noexcept
{
    noiseState = 1664525u * noiseState + 1013904223u;
    return 4.656613e-10f * static_cast<float> (static_cast<int32_t> (noiseState));
}

} // namespace picohDSP
