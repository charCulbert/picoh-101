#pragma once

#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

#include "picohDSP_SmoothedValue.h"

namespace picohDSP
{

// The coefficients for every cutoff in 1/16-semitone steps, so hold() looks them up instead of computing
// an exp2, a tan and a division.
struct CoefficientTable
{
    struct Coefficients
    {
        float g; // tan (pi * hz / sampleRate)
        float p; // 1 / (1 + g), for the 24 dB filter
    };

    // Allocates, so not on the audio thread.
    void build (double sampleRate);

    // 20 Hz to 20 kHz (or 0.45 x the sample rate if lower), as notes.
    float getLowestNote() const noexcept;
    float getHighestNote() const noexcept;

    // Blended between the two nearest steps.
    Coefficients lookup (float note) const noexcept;

private:
    static constexpr float stepsPerSemitone = 16.0f;

    float lowestNote = 0.0f, highestNote = 0.0f;
    std::vector<Coefficients> steps; // g and p side by side, so one lookup reads both
};

// The VCF: a 24 dB low-pass of four one-pole stages in a resonance loop, each stage and the loop with a
// soft clip, the way the IR3109's OTAs saturate. Near full resonance it self-oscillates.
struct Filter
{
    void prepare (double sampleRate) noexcept;
    void reset() noexcept; // silent, the cutoff already at its target

    // Must be set, and built for this sample rate, before process().
    void setTable (const CoefficientTable& table) noexcept;

    // The cutoff as a note (cutoffToNote() takes the log). Glides there over 20 ms.
    static float cutoffToNote (float hz) noexcept;
    void setCutoffNote (float note) noexcept;

    // 0 to 1: up to 0.8 a peak Q 0.5 to 4.5 above the passband, with the bass kept level; above, on to
    // self-oscillation.
    void setResonance (float amount) noexcept;

    // Works the cutoff out once at the middle of the next samples, modulation (semitones) included, and holds
    // it for them. Over 16 samples a fast resonant sweep stays 42 dB or more below the sound.
    void hold (float modulationSemitones, int samples) noexcept;

    float process (float input) noexcept;

private:
    // The soft clip's limit, set by ear, and the gain that brings a clipped saw back to its level.
    static constexpr float clipLevel = 1.42f, clipMakeUp = 1.1f;
    static float clip (float x) noexcept; // tanh-like, to +-clipLevel

    float sampleRate = 0.0f;
    const CoefficientTable* table = nullptr;

    SmoothedValue<float> cutoffNote { 135.08f }; // the cutoff as a note, gliding over 20 ms

    float loopGain = 0.0f;   // k: how much of the output is fed back, 0 to past 4
    float outputGain = 1.0f; // 1 + k, so the bass holds its level
    float G = 0.0f;          // a stage's g / (1 + g), held with the cutoff
    float singing = 0.0f;    // 0 up to RES 0.8, 1 at full: how much of the pitch correction applies
    // Singing, the clips and the one-pass loop pull the pitch flat, more as it rises: up to 0.2 semitone at
    // 3.3 kHz. This many semitones per unit of g puts it back.
    static constexpr float pitchCorrection = 0.93f;
    float stages[4] = {};
};

//=====================================================
//  ╷ ╭┬╮ ╭─╮ ╷   ╭─╴ ╭┬╮ ╭─╴ ╭╮╷ ╶┬╴ ╭─╮ ╶┬╴ ╷ ╭─╮ ╭╮╷
//  │ │││ ├─╯ │   ├─  │││ ├─  │││  │  ├─┤  │  │ │ │ │││
//  ╵ ╵ ╵ ╵   ╰─╴ ╰─╴ ╵ ╵ ╰─╴ ╵╰╯  ╵  ╵ ╵  ╵  ╵ ╰─╯ ╵╰╯
//=====================================================

namespace filter_detail
{
inline constexpr float minimumHz = 20.0f, maximumHz = 20000.0f;
inline constexpr float maximumOfSampleRate = 0.45f; // below Nyquist, where tan blows up
inline constexpr float cutoffSmoothingSeconds = 0.02f;

inline float hzToNote (float hz) noexcept
{
    return 69.0f + 12.0f * std::log2 (hz / 440.0f);
}
} // namespace filter_detail

// Filled in double, so the steps' rounding doesn't add up.
inline void CoefficientTable::build (double sampleRate)
{
    using namespace filter_detail;
    lowestNote = hzToNote (minimumHz);
    highestNote = hzToNote (std::min (maximumHz, maximumOfSampleRate * static_cast<float> (sampleRate)));

    // One step past the highest note, so lookup() always has a pair to blend.
    steps.resize (static_cast<size_t> (std::ceil ((highestNote - lowestNote) * stepsPerSemitone)) + 2);
    for (size_t i = 0; i < steps.size(); ++i)
    {
        const double note = lowestNote + static_cast<double> (i) / stepsPerSemitone;
        const double hz = 440.0 * std::exp2 ((note - 69.0) / 12.0);
        const double g = std::tan (3.14159265358979 * hz / sampleRate);
        steps[i] = { static_cast<float> (g), static_cast<float> (1.0 / (1.0 + g)) };
    }
}

inline float CoefficientTable::getLowestNote() const noexcept
{
    return lowestNote;
}
inline float CoefficientTable::getHighestNote() const noexcept
{
    return highestNote;
}

inline CoefficientTable::Coefficients CoefficientTable::lookup (float note) const noexcept
{
    const float position = (note - lowestNote) * stepsPerSemitone;
    const size_t index = std::min (static_cast<size_t> (position), steps.size() - 2);
    const float fraction = position - static_cast<float> (index);
    const Coefficients& below = steps[index];
    const Coefficients& above = steps[index + 1];
    return { below.g + (above.g - below.g) * fraction, below.p + (above.p - below.p) * fraction };
}

inline void Filter::prepare (double newSampleRate) noexcept
{
    sampleRate = static_cast<float> (newSampleRate);
    reset();
}

inline void Filter::reset() noexcept
{
    for (float& state : stages)
        state = 0.0f;
    cutoffNote.reset (cutoffNote.getTarget());
}

inline void Filter::setTable (const CoefficientTable& newTable) noexcept
{
    table = &newTable;
}

inline float Filter::cutoffToNote (float hz) noexcept
{
    using namespace filter_detail;
    return hzToNote (std::clamp (hz, minimumHz, maximumHz));
}

inline void Filter::setCutoffNote (float note) noexcept
{
    cutoffNote.setTarget (note, static_cast<int> (filter_detail::cutoffSmoothingSeconds * sampleRate));
}

// A ladder's peak above the passband is (1 + k) / (4 - k) at the cutoff. Up to 0.8 that peak is q = 0.5 to
// 4.5; over the last fifth k goes on to 4.4, past 4, where the loop sings and the soft clips hold it.
inline void Filter::setResonance (float amount) noexcept
{
    const auto kForQ = [] (float q) { return (4.0f * q - 1.0f) / (1.0f + q); };
    const float r = std::clamp (amount, 0.0f, 1.0f);
    constexpr float knee = 0.8f, top = 4.4f;
    const float atKnee = kForQ (0.5f + 5.0f * knee);
    singing = r <= knee ? 0.0f : (r - knee) / (1.0f - knee);
    loopGain = atKnee + (top - atKnee) * singing;
    if (r <= knee)
        loopGain = kForQ (0.5f + 5.0f * r);
    outputGain = (1.0f + loopGain) * clipMakeUp;
}

inline void Filter::hold (float modulationSemitones, int samples) noexcept
{
    assert (table != nullptr && samples > 0);
    cutoffNote.skip (samples / 2);
    const float note =
        std::clamp (cutoffNote.getCurrent() + modulationSemitones, table->getLowestNote(), table->getHighestNote());
    cutoffNote.skip (samples - samples / 2);
    auto [g, p] = table->lookup (note);
    if (singing > 0.0f)
    {
        const auto corrected = table->lookup (std::min (note + singing * pitchCorrection * g, table->getHighestNote()));
        g = corrected.g;
        p = corrected.p;
    }
    G = g * p;
}

inline float Filter::clip (float x) noexcept
{
    const float t = std::clamp (x / clipLevel, -3.0f, 3.0f);
    return clipLevel * t * (27.0f + t * t) / (27.0f + 9.0f * t * t);
}

// Each stage is a trapezoidal one-pole, y = G x + (1 - G) s. The loop is solved in closed form as if the
// stages were linear, and that estimate feeds the clipped stages.
inline float Filter::process (float x) noexcept
{
    const float b = 1.0f - G, G2 = G * G, G4 = G2 * G2;
    const float fromStates = b * (G2 * G * stages[0] + G2 * stages[1] + G * stages[2] + stages[3]);
    const float estimate = (G4 * x + fromStates) / (1.0f + loopGain * G4);
    float u = clip (x - loopGain * estimate);
    for (float& state : stages)
    {
        const float v = G * clip (u - state);
        u = v + state;
        state = u + v;
    }
    return u * outputGain;
}

} // namespace picohDSP
