#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <complex>
#include <vector>
#include "elliptic-blep.h"

namespace picohDSP
{

// Smooths the oscillators' jumps with Signalsmith's elliptic BLEP (Geraint Luff, 2024): the wave as if
// through an 11th-order elliptic low-pass at 20 kHz, then sampled. A sum of one-pole filters, so each jump's
// correction starts at its exact time, with no delay. Changed for speed: the two real poles stay real, and
// the output is read and the poles moved in one pass.
struct EllipticBlep
{
    // The poles' powers for one sample rate, and the step and corner coefficients.
    struct Table
    {
        // Allocates, so not on the audio thread.
        void build (double sampleRate);

    private:
        friend struct EllipticBlep;
        using Coeffs = signalsmith::blep::EllipticBlepCoeffs<float>;
        static constexpr size_t realCount = Coeffs::realCount, complexCount = Coeffs::complexCount;
        static constexpr size_t partialSteps = 127; // Signalsmith's default

        // Each pole to the power s / partialSteps, so an event can start part way into a sample.
        std::vector<std::array<float, realCount>> realPowers;
        std::vector<std::array<float, complexCount>> rePowers, imPowers;
        // By order: 1 a step, 2 a corner (0 unused).
        std::array<std::array<float, realCount>, 3> realCoeffs {};
        std::array<std::array<float, complexCount>, 3> reCoeffs {}, imCoeffs {};
    };

    void prepare (double sampleRate) noexcept;
    void reset() noexcept;

    // Built for this sample rate, before anything else.
    void setTable (const Table& table) noexcept;

    // A jump during the coming sample, offset 0 to 1 from its start.
    void addStep (float offset, float height) noexcept;

    // A change of slope per sample during the coming sample, offset as for addStep.
    void addCorner (float offset, float slopeChange) noexcept;

    float process (float sample) noexcept;

private:
    const Table* table = nullptr;

    // Each one-pole filter's state; a real pole's stays real.
    std::array<float, Table::realCount> real {};
    std::array<float, Table::complexCount> re {}, im {};

    void add (float amount, size_t order, float samplesInPast) noexcept;
};

//=====================================================
//  ╷ ╭┬╮ ╭─╮ ╷   ╭─╴ ╭┬╮ ╭─╴ ╭╮╷ ╶┬╴ ╭─╮ ╶┬╴ ╷ ╭─╮ ╭╮╷
//  │ │││ ├─╯ │   ├─  │││ ├─  │││  │  ├─┤  │  │ │ │ │││
//  ╵ ╵ ╵ ╵   ╰─╴ ╰─╴ ╵ ╵ ╰─╴ ╵╰╯  ╵  ╵ ╵  ╵  ╵ ╰─╯ ╵╰╯
//=====================================================

// As the library's constructor, in float. The cutoff is 20 kHz at any sample rate.
inline void EllipticBlep::Table::build (double sampleRate)
{
    realPowers.resize (partialSteps + 1);
    rePowers.resize (partialSteps + 1);
    imPowers.resize (partialSteps + 1);

    using Complex = std::complex<float>;
    const Coeffs coeffs;
    const float hzToAngular = static_cast<float> ((2 * M_PI) / static_cast<float> (sampleRate));

    // Hands each power (what 0, at s) and coefficient (what 1, at its order) to store.
    const auto pole = [&] (Complex p, Complex coeff, auto&& store)
    {
        for (size_t s = 0; s <= partialSteps; ++s)
            store (s, 0, std::exp (static_cast<float> (s) / partialSteps * p * hzToAngular));

        Complex blepCoeff = coeff * hzToAngular;
        for (size_t order = 1; order <= 2; ++order)
        {
            blepCoeff /= p * hzToAngular; // a step's is the impulse's integral, a corner's its second
            store (order, 1, blepCoeff);
        }
    };

    for (size_t i = 0; i < realCount; ++i)
        pole (coeffs.realPoles[i], coeffs.realCoeffsBlep[i], [&] (size_t at, int what, Complex value)
              { (what == 0 ? realPowers[at][i] : realCoeffs[at][i]) = value.real(); });

    for (size_t i = 0; i < complexCount; ++i)
        pole (coeffs.complexPoles[i], coeffs.complexCoeffsBlep[i],
              [&] (size_t at, int what, Complex value)
              {
                  (what == 0 ? rePowers[at][i] : reCoeffs[at][i]) = value.real();
                  (what == 0 ? imPowers[at][i] : imCoeffs[at][i]) = value.imag();
              });
}

inline void EllipticBlep::prepare (double) noexcept
{
    reset();
}

inline void EllipticBlep::reset() noexcept
{
    real = {};
    re = {};
    im = {};
}

inline void EllipticBlep::setTable (const Table& newTable) noexcept
{
    table = &newTable;
}

// The library's orders: 1 a step, 2 a change of slope. Its time is how long
// before the coming sample the event was.
inline void EllipticBlep::addStep (float offset, float height) noexcept
{
    add (height, 1, 1.0f - offset);
}

inline void EllipticBlep::addCorner (float offset, float slopeChange) noexcept
{
    add (slopeChange, 2, 1.0f - offset);
}

// The event's correction, moved on to the coming sample the way process()
// moves the state, by the pole's power for that part of a sample.
inline void EllipticBlep::add (float amount, size_t order, float samplesInPast) noexcept
{
    assert (table != nullptr);
    constexpr size_t partialSteps = Table::partialSteps;
    const float position = samplesInPast * partialSteps;
    const size_t low = std::min (static_cast<size_t> (std::floor (position)), partialSteps - 1);
    const float fraction = position - std::floor (position);

    const auto& lowReal = table->realPowers[low];
    const auto& highReal = table->realPowers[low + 1];
    for (size_t i = 0; i < Table::realCount; ++i)
    {
        const float power = lowReal[i] + (highReal[i] - lowReal[i]) * fraction;
        real[i] += table->realCoeffs[order][i] * power * amount;
    }

    const auto& lowRe = table->rePowers[low];
    const auto& highRe = table->rePowers[low + 1];
    const auto& lowIm = table->imPowers[low];
    const auto& highIm = table->imPowers[low + 1];
    for (size_t i = 0; i < Table::complexCount; ++i)
    {
        const float powerRe = lowRe[i] + (highRe[i] - lowRe[i]) * fraction;
        const float powerIm = lowIm[i] + (highIm[i] - lowIm[i]) * fraction;
        const float coeffRe = table->reCoeffs[order][i], coeffIm = table->imCoeffs[order][i];
        re[i] += (coeffRe * powerRe - coeffIm * powerIm) * amount;
        im[i] += (coeffRe * powerIm + coeffIm * powerRe) * amount;
    }
}

// The filters' sum is the correction now; each then moves on a sample, in the same pass.
inline float EllipticBlep::process (float sample) noexcept
{
    assert (table != nullptr);
    float sum = 0.0f;

    const auto& realPower = table->realPowers[Table::partialSteps];
    for (size_t i = 0; i < Table::realCount; ++i)
    {
        sum += real[i];
        real[i] *= realPower[i];
    }

    const auto& powerRe = table->rePowers[Table::partialSteps];
    const auto& powerIm = table->imPowers[Table::partialSteps];
    for (size_t i = 0; i < Table::complexCount; ++i)
    {
        sum += re[i];
        const float nextRe = re[i] * powerRe[i] - im[i] * powerIm[i];
        im[i] = re[i] * powerIm[i] + im[i] * powerRe[i];
        re[i] = nextRe;
    }

    return sample + sum;
}

} // namespace picohDSP
