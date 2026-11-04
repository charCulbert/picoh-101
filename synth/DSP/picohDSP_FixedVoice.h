// The voice's per-sample parts in fixed point, for chips without an FPU (the RP2040): the VCO with its
// elliptic BLEP, the VCF, the ADSR and the gate. Same maths and the same interfaces as the float ones;
// what runs once a block or once a control change stays float. Signals Q27, levels and gains Q30, the
// phase Q32.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <vector>
#include "elliptic-blep.h"
#include "picohDSP_Drift.h"
#include "picohDSP_Exp2.h"
#include "picohDSP_Envelope.h"
#include "picohDSP_Filter.h"
#include "picohDSP_Gate.h"
#include "picohDSP_Phasor.h"
#include "picohDSP_SmoothedValue.h"
#include "picohDSP_Vco.h"

namespace picohDSP::fixed
{
// (a * b) >> shift for shift 16 to 31 from four 16 x 16 multiplies: the M0+ has no 32 x 32 -> 64.
// Off by a few units in the last place, and the same on every computer.
inline int32_t mul (int32_t a, int32_t b, int shift) noexcept
{
    const int32_t ah = a >> 16, bh = b >> 16;
    const uint32_t al = uint32_t (a) & 0xFFFF, bl = uint32_t (b) & 0xFFFF;
    return ((ah * bh) << (32 - shift)) + ((ah * int32_t (bl)) >> (shift - 16)) + ((int32_t (al) * bh) >> (shift - 16))
           + int32_t ((al * bl) >> shift);
}
constexpr int signalBits = 27; // signals Q27: +-16


struct Blep
{
    static constexpr int realCount = 2, complexCount = 6, partialSteps = 127, powerBits = 30;
    struct Table
    {
        // A step's coefficient times each pole to the power s / partialSteps.
        std::vector<std::array<int32_t, realCount>> realSteps;
        std::vector<std::array<int32_t, complexCount>> reSteps, imSteps;
        // A sample's powers: the real poles', the complex poles' re, re + im and im - re.
        std::array<int32_t, realCount> realPower {};
        std::array<int32_t, complexCount> rePower {}, sumPower {}, differencePower {};
        void build (double sampleRate)
        {
            signalsmith::blep::EllipticBlepCoeffs<double> c;
            realSteps.resize (partialSteps + 1);
            reSteps.resize (partialSteps + 1);
            imSteps.resize (partialSteps + 1);
            const double w = 2 * M_PI / sampleRate;
            auto q = [] (double v, int bits) { return int32_t (std::lrint (v * double (1 << bits))); };
            for (int i = 0; i < realCount; ++i)
            {
                const double k = c.realCoeffsBlep[i] / c.realPoles[i];
                for (int s = 0; s <= partialSteps; ++s)
                    realSteps[s][i] = q (k * std::exp (double (s) / partialSteps * c.realPoles[i] * w), signalBits);
                realPower[i] = q (std::exp (c.realPoles[i] * w), powerBits);
            }
            for (int i = 0; i < complexCount; ++i)
            {
                const std::complex<double> k = c.complexCoeffsBlep[i] / c.complexPoles[i];
                for (int s = 0; s <= partialSteps; ++s)
                {
                    const auto step = k * std::exp (double (s) / partialSteps * c.complexPoles[i] * w);
                    reSteps[s][i] = q (step.real(), signalBits);
                    imSteps[s][i] = q (step.imag(), signalBits);
                }
                const auto z = std::exp (c.complexPoles[i] * w);
                rePower[i] = q (z.real(), powerBits);
                sumPower[i] = q (z.real(), powerBits) + q (z.imag(), powerBits);
                differencePower[i] = q (z.imag(), powerBits) - q (z.real(), powerBits);
            }
        }
    };
    const Table* table = nullptr;
    int32_t real[realCount] {}, re[complexCount] {}, im[complexCount] {};

    void reset() noexcept
    {
        for (auto& x : real) x = 0;
        for (auto& x : re) x = 0;
        for (auto& x : im) x = 0;
    }

    // offset 0 to 1 (Q16) into the coming sample, as EllipticBlep::addStep; height Q27.
    void addStep (uint32_t offset, int32_t height) noexcept
    {
        const uint32_t position = (65536u - std::min (offset, uint32_t (65536))) * partialSteps; // Q16
        int low = int (position >> 16);
        int32_t fraction = int32_t (position & 0xFFFF);
        if (low > partialSteps - 1)
            low = partialSteps - 1, fraction = 65536;
        const auto &realLow = table->realSteps[low], &realHigh = table->realSteps[low + 1];
        for (int i = 0; i < realCount; ++i)
            real[i] += mul (realLow[i] + mul (realHigh[i] - realLow[i], fraction, 16), height, signalBits);
        const auto &reLow = table->reSteps[low], &reHigh = table->reSteps[low + 1];
        const auto &imLow = table->imSteps[low], &imHigh = table->imSteps[low + 1];
        for (int i = 0; i < complexCount; ++i)
        {
            re[i] += mul (reLow[i] + mul (reHigh[i] - reLow[i], fraction, 16), height, signalBits);
            im[i] += mul (imLow[i] + mul (imHigh[i] - imLow[i], fraction, 16), height, signalBits);
        }
    }

    int32_t process (int32_t sample) noexcept
    {
        int32_t sum = 0;
        for (int i = 0; i < realCount; ++i)
        {
            sum += real[i];
            real[i] = mul (real[i], table->realPower[i], powerBits);
        }
        // Three multiplies a pole instead of four.
        for (int i = 0; i < complexCount; ++i)
        {
            sum += re[i];
            const int32_t k1 = mul (re[i] + im[i], table->rePower[i], powerBits);
            const int32_t k2 = mul (re[i], table->differencePower[i], powerBits);
            const int32_t k3 = mul (im[i], table->sumPower[i], powerBits);
            re[i] = k1 - k3;
            im[i] = k1 + k2;
        }
        return sample + sum;
    }
};

// Both toward zero.
inline int32_t toQ (float x) noexcept { return int32_t (x * float (1 << signalBits)); }
inline int32_t toQ30 (float x) noexcept { return int32_t (x * 1073741824.0f); }

// SmoothedValue, Q30.
struct Ramp
{
    int32_t value = 0, target = 0, step = 0;
    int framesLeft = 0;
    void resetToTarget() noexcept { value = target, step = 0, framesLeft = 0; }
    void setTarget (float t, int frames) noexcept
    {
        target = toQ30 (t);
        framesLeft = std::max (1, frames);
        step = (target - value) / framesLeft;
    }
    int32_t next() noexcept
    {
        if (framesLeft > 0)
            value = --framesLeft == 0 ? target : value + step;
        return value;
    }
};

// The VCF (Filter): process() in Q27, the clip from a table; hold() works its coefficients out in float.
struct Filter
{
    void prepare (double newSampleRate) noexcept
    {
        sampleRate = float (newSampleRate);
        if (clipTable[clipEntries] == 0)
            buildClip();
        reset();
    }
    void reset() noexcept
    {
        for (auto& x : s) x = 0;
        cutoffNote.reset (cutoffNote.getTarget());
    }
    void setTable (const CoefficientTable& t) noexcept { table = &t; }
    void setCutoffNote (float note) noexcept
    {
        cutoffNote.setTarget (note, int (filter_detail::cutoffSmoothingSeconds * sampleRate));
    }
    void setResonance (float amount) noexcept
    {
        const auto resonance = Float::resonanceFor (amount);
        loopGain = resonance.loopGain;
        singing = resonance.singing;
        k = toQ (loopGain);
        outGain = toQ ((1.0f + loopGain) * Float::clipMakeUp);
        updateCoefficients();
    }
    void hold (float modulationSemitones, int samples) noexcept
    {
        cutoffNote.skip (samples / 2);
        const float note = cutoffNote.getCurrent() + modulationSemitones;
        cutoffNote.skip (samples - samples / 2);
        heldG = Float::stageGain (*table, note, singing);
        updateCoefficients();
    }
    int32_t process (int32_t x) noexcept
    {
        const int32_t estimate = mul (c4, x, 30) + mul (c3, s[0], 30) + mul (c2, s[1], 30) + mul (c1, s[2], 30)
                                 + mul (c0, s[3], 30);
        int32_t u = clip (x - mul (k, estimate, signalBits));
        for (int32_t& state : s)
        {
            const int32_t v = mul (G, clip (u - state), 30);
            u = v + state;
            state = u + v;
        }
        return mul (u, outGain, signalBits);
    }

private:
    using Float = picohDSP::Filter;
    static constexpr float clipLevel = Float::clipLevel;
    static constexpr int clipEntries = 1024; // |x| 0 to 8 in Q27: x >> 20
    static inline int32_t clipTable[clipEntries + 1];
    static void buildClip()
    {
        for (int i = 0; i <= clipEntries; ++i)
        {
            double x = 8.0 * i / clipEntries, t = std::min (x / clipLevel, 3.0);
            clipTable[i] = int32_t (std::lrint (clipLevel * t * (27 + t * t) / (27 + 9 * t * t) * (1 << signalBits)));
        }
    }
    void updateCoefficients() noexcept
    {
        const float b = 1 - heldG, G2 = heldG * heldG, G4 = G2 * G2, inverse = 1 / (1 + loopGain * G4);
        const float bInverse = b * inverse;
        G = toQ30 (heldG);
        c4 = toQ30 (G4 * inverse), c3 = toQ30 (bInverse * G2 * heldG), c2 = toQ30 (bInverse * G2);
        c1 = toQ30 (bInverse * heldG), c0 = toQ30 (bInverse);
    }
    static int32_t clip (int32_t x) noexcept
    {
        const uint32_t a = uint32_t (x < 0 ? -x : x);
        if (a >= (8u << signalBits))
            return x < 0 ? -clipTable[clipEntries] : clipTable[clipEntries];
        const uint32_t i = a >> 20, f = (a >> 9) & 0x7FF;
        const int32_t d = clipTable[i + 1] - clipTable[i]; // 2^20 at most, so d * f fits
        const int32_t y = clipTable[i] + (d * int32_t (f) >> 11);
        return x < 0 ? -y : y;
    }

    float sampleRate = 0, loopGain = 0, singing = 0, heldG = 0;
    const CoefficientTable* table = nullptr;
    SmoothedValue<float> cutoffNote { 135.08f };
    int32_t G = 0, c4 = 0, c3 = 0, c2 = 0, c1 = 0, c0 = 0, k = 0, outGain = 0; // G, cN Q30; k, gain Q27
    int32_t s[4] {};
};

// The ADSR (Envelope), level Q30; the same Settings.
struct Envelope
{
    static constexpr float overshoot = picohDSP::Envelope::overshoot;
    void prepare (double sr) noexcept { sampleRate = float (sr); stop(); }
    void setSettings (const picohDSP::Envelope::Settings& s) noexcept { settings = &s; }
    void settingsChanged() noexcept
    {
        if (stage == Stage::decay && level < toQ30 (settings->sustainLevel))
            stage = Stage::sustain;
        refresh();
    }
    float getAttack() const noexcept { return settings->getAttack(); }
    void start() noexcept { if (sampleRate > 0) enter (Stage::attack); }
    void release() noexcept { if (stage != Stage::idle) enter (Stage::release); }
    void stop() noexcept { stage = Stage::idle, level = 0; }
    bool isActive() const noexcept { return stage != Stage::idle; }
    int32_t next() noexcept
    {
        constexpr int32_t one = 1 << 30;
        switch (stage)
        {
            case Stage::idle: break;
            case Stage::attack:
                level += mul (aim - level, coefficient, 30);
                if (level >= one)
                {
                    level = one;
                    enter (Stage::decay);
                }
                break;
            case Stage::decay:
                level += mul (aim - level, coefficient, 30);
                if (level <= sustain)
                {
                    level = sustain;
                    stage = Stage::sustain;
                }
                break;
            case Stage::sustain: level += mul (sustain - level, coefficient, 30); break;
            case Stage::release:
                level += mul (aim - level, coefficient, 30);
                if (level <= 0)
                    stop();
                break;
        }
        return level;
    }

private:
    enum class Stage { idle, attack, decay, sustain, release };
    Stage stage = Stage::idle;
    int32_t level = 0, aim = 0, coefficient = 0, sustain = 0;
    float sampleRate = 0;
    const picohDSP::Envelope::Settings* settings = nullptr;
    void enter (Stage s) noexcept { stage = s; refresh(); }
    void refresh() noexcept
    {
        const float sus = settings->sustainLevel;
        sustain = toQ30 (sus);
        switch (stage)
        {
            case Stage::attack: aim = toQ30 (1 + overshoot), coefficient = toQ30 (settings->attackCoefficient); break;
            case Stage::decay:
                aim = toQ30 (sus - overshoot * (1 - sus)), coefficient = toQ30 (settings->decayCoefficient);
                break;
            case Stage::sustain: coefficient = toQ30 (settings->decayCoefficient); break;
            case Stage::release: aim = toQ30 (-overshoot), coefficient = toQ30 (settings->releaseCoefficient); break;
            case Stage::idle: break;
        }
    }
};

// The VCA's gate (Gate), Q30.
struct Gate
{
    static constexpr float openSeconds = picohDSP::Gate::openSeconds, closeSeconds = picohDSP::Gate::closeSeconds;
    void prepare (double sr) noexcept
    {
        openStep = toQ30 (1.0f / (openSeconds * float (sr)));
        closeStep = toQ30 (1.0f / (closeSeconds * float (sr)));
        stop();
    }
    void start() noexcept { step = openStep; }
    void release() noexcept { step = -closeStep; }
    void stop() noexcept { level = step = 0; }
    bool isActive() const noexcept { return level > 0 || step > 0; }
    int32_t next() noexcept
    {
        level = std::clamp (level + step, int32_t (0), int32_t (1 << 30));
        if (level <= 0)
            step = 0;
        return level;
    }

private:
    int32_t level = 0, step = 0, openStep = 0, closeStep = 0;
};

// The VCO and source mixer (Vco): the phase a 32-bit integer that wraps by itself, each step's time within
// the sample a Q16 fraction, the mix Q27 into the fixed BLEP. Pitch is worked out once a block, in float.
struct Vco
{
    using Source = picohDSP::Vco::Source;
    using SubWave = picohDSP::Vco::SubWave;
    static constexpr float levelGlideSeconds = picohDSP::Vco::levelGlideSeconds, driftAmount = picohDSP::Vco::driftAmount;
    static constexpr float widthGlideSeconds = picohDSP::Vco::widthGlideSeconds, phaseSpread = picohDSP::Vco::phaseSpread;
    static constexpr float squareWidth = picohDSP::Vco::squareWidth, narrowestWidth = picohDSP::Vco::narrowestWidth;
    static constexpr float maxIncrement = Phasor::maxIncrement;

    void prepare (double newSampleRate, size_t index) noexcept
    {
        sampleRate = float (newSampleRate);
        inverseSampleRate = newSampleRate > 0.0 ? float (1.0 / newSampleRate) : 0.0f;
        correction.reset();
        const float start = float (index) * phaseSpread;
        phase = uint32_t (double (start - std::floor (start)) * 4294967296.0);
        increment = slide = 0;
        target = 0;
        drift.prepare (newSampleRate, uint32_t (index));
        drift.setAmount (driftAmount);
        noiseState = uint32_t (index) * 2654435761u + 1u;
        for (auto& level : levels)
            level.resetToTarget();
        width.resetToTarget();
        subCount = 0;
        subWave = nextSubWave;
        pulseHigh = phase < 0x80000000u;
        rampPitch = false;
    }
    void setCorrectionTable (const Blep::Table& table) noexcept { correction.table = &table; }
    void setNote (float newKey) noexcept
    {
        key = newKey;
        setIncrement (frequency());
        rampPitch = false;
    }
    void moveNote (float newKey) noexcept { key = newKey; }
    void setTune (float cents) noexcept
    {
        tuneCents = cents;
        setIncrement (frequency());
    }
    void setModulation (float cents) noexcept { modulationCents = cents; }
    void setLevel (Source source, float level) noexcept
    {
        levels[size_t (source)].setTarget (std::clamp (level, 0.0f, 1.0f), int (levelGlideSeconds * sampleRate));
    }
    void setSubWave (SubWave wave) noexcept { nextSubWave = wave; }
    void setPulseWidth (float amount) noexcept
    {
        width.setTarget (std::clamp (amount, 0.0f, 1.0f), int (widthGlideSeconds * sampleRate));
    }

    // As Phasor::rampFrequency, in float; the increments then go to Q32.
    void startBlock (int frames) noexcept
    {
        drift.advance (frames);
        if (! rampPitch)
        {
            setIncrement (frequency());
            rampPitch = true;
            return;
        }
        const float newIncrement = incrementFor (frequency());
        const float halfSpan = 0.5f * (newIncrement - target) * float (frames - 1) / float (std::max (1, frames));
        const float limit = 0.99f * std::min (newIncrement, maxIncrement - newIncrement);
        const float held = std::clamp (halfSpan, -limit, limit);
        const float slideF = frames > 1 ? 2.0f * held / float (frames - 1) : 0.0f;
        increment = toQ40 (newIncrement - held);
        slide = q40 (slideF);
        target = newIncrement;
    }

    // widthMod Q30, or nullptr. out Q27.
    void render (int32_t* __restrict out, int count, const int32_t* __restrict widthMod) noexcept
    {
        for (int i = 0; i < count; ++i)
        {
            const int32_t pulseLevel = levels[0].next(), sawLevel = levels[1].next();
            const int32_t subLevel = levels[2].next(), noiseLevel = levels[3].next();
            int32_t amount = width.next();
            if (widthMod)
                amount = mul (amount, widthMod[i], 30);
            const uint32_t pulseWidth = pulseWidthAt (amount);
            const uint32_t inc = uint32_t (increment >> 8), start = phase;
            phase = advance (start, increment);
            const int32_t step = pulseLevel >> 2; // 2 x the level, in Q27
            // As the float Vco: the pulse steps only when it changes, mid-way if the width passed the phase.
            const auto pulseOver = [&] (uint32_t from, uint64_t to, uint32_t offset)
            {
                const bool high = to < pulseWidth;
                if (high == pulseHigh)
                    return;
                const bool crossed = pulseWidth > from && pulseWidth <= to;
                const uint32_t part = crossed ? pulseWidth - from : uint32_t ((to - from) >> 1);
                correction.addStep (offset + fractionOf (part, inc), high ? step : -step);
                pulseHigh = high;
            };
            if (phase >= start) // no wrap
                pulseOver (start, phase, 0);
            else
            {
                const uint32_t wrapOffset = fractionOf (0u - start, inc);
                pulseOver (start, uint64_t (1) << 32, 0);
                const int subBefore = subValue();
                subCount = (subCount + 1) & 3;
                subWave = nextSubWave;
                const int32_t sawAndPulse = (sawLevel >> 2) + (pulseLevel >> 2); // each 2^30 at most
                correction.addStep (wrapOffset, sawAndPulse + (subValue() - subBefore) * (subLevel >> 3));
                pulseHigh = true;
                pulseOver (0, phase, wrapOffset);
            }
            increment = roundAsFloat (increment + uint64_t (slide));

            const int32_t saw = mul (sawLevel, int32_t (0x7FFFFFFFu - phase) >> 4, 30);
            const int32_t pulse = pulseHigh ? pulseLevel >> 3 : -(pulseLevel >> 3);
            const int32_t sub = subValue() * (subLevel >> 3);
            noiseState = 1664525u * noiseState + 1013904223u;
            const int32_t noise = mul (noiseLevel, int32_t (noiseState) >> 4, 30);
            out[i] = correction.process (saw + pulse + sub) + noise;
        }
    }

    void skip (int frames) noexcept
    {
        increment = toQ40 (target);
        slide = 0;
        const uint64_t total = uint64_t (phase) + uint64_t (increment >> 8) * uint32_t (frames);
        phase = uint32_t (total);
        subCount = int ((uint32_t (subCount) + uint32_t (total >> 32)) & 3);
        subWave = nextSubWave;
        drift.advance (frames);
        for (auto& level : levels)
            level.resetToTarget();
        width.resetToTarget();
        pulseHigh = phase < pulseWidthAt (width.value);
        correction.reset();
    }

private:
    Blep correction;
    Drift drift;
    std::array<Ramp, 4> levels;
    Ramp width;
    SubWave subWave = SubWave::octaveDown, nextSubWave = SubWave::octaveDown;
    int subCount = 0;
    bool pulseHigh = true; // the pulse at the end of the last sample
    uint32_t noiseState = 1, phase = 0;
    uint64_t increment = 0; // Q40
    int64_t slide = 0;      // Q40
    float target = 0, key = 60, tuneCents = 0, modulationCents = 0, sampleRate = 0;
    bool rampPitch = false;

    float inverseSampleRate = 0;
    float incrementFor (float hz) const noexcept { return std::clamp (hz * inverseSampleRate, 0.0f, maxIncrement); }
    void setIncrement (float hz) noexcept
    {
        target = incrementFor (hz);
        increment = toQ40 (target);
        slide = 0;
    }
    float frequency() const noexcept
    {
        const float semitones = key - 69.0f + (tuneCents + modulationCents + drift.getCents()) / 100.0f;
        return 440.0f * exp2 (semitones / 12.0f);
    }
    int subValue() const noexcept
    {
        switch (subWave)
        {
            case SubWave::octaveDown: return subCount & 1 ? -1 : 1;
            case SubWave::twoOctavesDown: return subCount & 2 ? -1 : 1;
            case SubWave::twoOctavesDownPulse: return subCount == 0 ? 1 : -1;
        }
        return 0;
    }
    static uint64_t toQ40 (float x) noexcept { return uint64_t (q40 (x)); }
    // x (under 2 either way) times 2^40, toward zero, as through a double but from the float's bits.
    static int64_t q40 (float x) noexcept
    {
        uint32_t b;
        std::memcpy (&b, &x, 4);
        const int e = int ((b >> 23) & 255);
        if (e == 0)
            return 0;
        const uint64_t m = (b & 0x7FFFFFu) | 0x800000u;
        const int shift = e - 110;
        const uint64_t v = shift >= 0 ? m << shift : (shift > -64 ? m >> -shift : 0);
        return b >> 31 ? -int64_t (v) : int64_t (v);
    }

    // x (Q40, under 2) rounded as a float holding it would be: to 24 significant bits, half to even. So the
    // phase and the ramping increment add up exactly as the float Phasor's, and the pitch matches it.
    static uint64_t roundAsFloat (uint64_t x) noexcept
    {
        const uint32_t high = uint32_t (x >> 32);
        const int bits = 64 - (high ? __builtin_clz (high) : 32 + __builtin_clz (uint32_t (x) | 1u));
        if (bits <= 24)
            return x;
        const int drop = bits - 24;
        const uint64_t half = uint64_t (1) << (drop - 1), mask = (uint64_t (1) << drop) - 1;
        const uint64_t rest = x & mask;
        x &= ~mask;
        if (rest > half || (rest == half && (x >> drop & 1)))
            x += mask + 1;
        return x;
    }
    // The phase (Q32) moved on by the increment (Q40), rounded as the float Phasor rounds phase + increment.
    static uint32_t advance (uint32_t phase, uint64_t increment) noexcept
    {
        return uint32_t (roundAsFloat ((uint64_t (phase) << 8) + increment) >> 8);
    }

    static uint32_t pulseWidthAt (int32_t amount) noexcept // amount Q30, width in Q32 phase
    {
        constexpr int32_t widthSpan = int32_t ((squareWidth - narrowestWidth) * 4294967296.0);
        return 0x80000000u - uint32_t (mul (amount, widthSpan, 30));
    }
    // part / whole (part <= whole) as a Q16 fraction, the divisor kept to its top 16 bits.
    static uint32_t fractionOf (uint32_t part, uint32_t whole) noexcept
    {
        if (whole == 0)
            return 0;
        const int n = __builtin_clz (whole);
        return std::min (uint32_t ((part << n) / ((whole << n) >> 16)), uint32_t (65536));
    }
};
} // namespace picohDSP::fixed
