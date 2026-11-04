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
// correction starts at its exact time, with no delay. For speed the two real poles are filtered as real
// numbers, the output is read and the poles moved in one pass, and only steps are corrected.
struct EllipticBlep
{
    // A step's correction at each part of a sample, and each pole's power for one sample.
    struct Table
    {
        // Allocates, so not on the audio thread.
        void build (double sampleRate);

    private:
        friend struct EllipticBlep;
        using Coeffs = signalsmith::blep::EllipticBlepCoeffs<double>;
        static constexpr size_t realCount = Coeffs::realCount, complexCount = Coeffs::complexCount;
        static constexpr size_t partialSteps = 127; // Signalsmith's default

        // A step's coefficient times each pole to the power s / partialSteps, so a step can start part
        // way into a sample.
        std::vector<std::array<float, realCount>> realSteps;
        std::vector<std::array<float, complexCount>> reSteps, imSteps;
        std::array<float, realCount> realPower {};
        std::array<float, complexCount> rePower {}, imPower {};
    };

    void prepare (double sampleRate) noexcept;
    void reset() noexcept;

    // Built for this sample rate, before anything else.
    void setTable (const Table& table) noexcept;

    // A jump during the coming sample, offset 0 to 1 from its start.
    void addStep (float offset, float height) noexcept;

    float process (float sample) noexcept;

private:
    const Table* table = nullptr;

    // Each one-pole filter's state; a real pole's stays real.
    std::array<float, Table::realCount> real {};
    std::array<float, Table::complexCount> re {}, im {};
};

//=====================================================
//  ╷ ╭┬╮ ╭─╮ ╷   ╭─╴ ╭┬╮ ╭─╴ ╭╮╷ ╶┬╴ ╭─╮ ╶┬╴ ╷ ╭─╮ ╭╮╷
//  │ │││ ├─╯ │   ├─  │││ ├─  │││  │  ├─┤  │  │ │ │ │││
//  ╵ ╵ ╵ ╵   ╰─╴ ╰─╴ ╵ ╵ ╰─╴ ╵╰╯  ╵  ╵ ╵  ╵  ╵ ╰─╯ ╵╰╯
//=====================================================

// As the library's constructor. The cutoff is 20 kHz at any sample rate. A step's coefficient is the
// pole's impulse coefficient over the pole.
inline void EllipticBlep::Table::build (double sampleRate)
{
    realSteps.resize (partialSteps + 1);
    reSteps.resize (partialSteps + 1);
    imSteps.resize (partialSteps + 1);

    const Coeffs coeffs;
    const double hzToAngular = 2 * M_PI / sampleRate;

    for (size_t i = 0; i < realCount; ++i)
    {
        const double pole = coeffs.realPoles[i], coeff = coeffs.realCoeffsBlep[i] / pole;
        for (size_t s = 0; s <= partialSteps; ++s)
            realSteps[s][i] = static_cast<float> (coeff * std::exp (double (s) / partialSteps * pole * hzToAngular));
        realPower[i] = static_cast<float> (std::exp (pole * hzToAngular));
    }

    for (size_t i = 0; i < complexCount; ++i)
    {
        const std::complex<double> pole = coeffs.complexPoles[i], coeff = coeffs.complexCoeffsBlep[i] / pole;
        for (size_t s = 0; s <= partialSteps; ++s)
        {
            const auto step = coeff * std::exp (double (s) / partialSteps * pole * hzToAngular);
            reSteps[s][i] = static_cast<float> (step.real());
            imSteps[s][i] = static_cast<float> (step.imag());
        }
        const auto power = std::exp (pole * hzToAngular);
        rePower[i] = static_cast<float> (power.real());
        imPower[i] = static_cast<float> (power.imag());
    }
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

// The step's correction, moved on to the coming sample the way process() moves the state: the table's
// entry for how long before the coming sample the step was.
inline void EllipticBlep::addStep (float offset, float height) noexcept
{
    assert (table != nullptr);
    constexpr size_t partialSteps = Table::partialSteps;
    const float position = (1.0f - std::clamp (offset, 0.0f, 1.0f)) * partialSteps;
    size_t low = static_cast<size_t> (position);
    float fraction = position - static_cast<float> (low);
    if (low > partialSteps - 1)
        low = partialSteps - 1, fraction = 1.0f;

    const auto& realLow = table->realSteps[low];
    const auto& realHigh = table->realSteps[low + 1];
    for (size_t i = 0; i < Table::realCount; ++i)
        real[i] += (realLow[i] + (realHigh[i] - realLow[i]) * fraction) * height;

    const auto& reLow = table->reSteps[low];
    const auto& reHigh = table->reSteps[low + 1];
    const auto& imLow = table->imSteps[low];
    const auto& imHigh = table->imSteps[low + 1];
    for (size_t i = 0; i < Table::complexCount; ++i)
    {
        re[i] += (reLow[i] + (reHigh[i] - reLow[i]) * fraction) * height;
        im[i] += (imLow[i] + (imHigh[i] - imLow[i]) * fraction) * height;
    }
}

// The filters' sum is the correction now; each then moves on a sample, in the same pass.
inline float EllipticBlep::process (float sample) noexcept
{
    assert (table != nullptr);
    float sum = 0.0f;

    for (size_t i = 0; i < Table::realCount; ++i)
    {
        sum += real[i];
        real[i] *= table->realPower[i];
    }

    for (size_t i = 0; i < Table::complexCount; ++i)
    {
        sum += re[i];
        const float nextRe = re[i] * table->rePower[i] - im[i] * table->imPower[i];
        im[i] = re[i] * table->imPower[i] + im[i] * table->rePower[i];
        re[i] = nextRe;
    }

    return sample + sum;
}

} // namespace picohDSP
