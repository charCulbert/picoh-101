// Float against fixed point, scenario by scenario: the difference's level below the float render (a null
// test), and the two long-term spectra in third-octave bands. Usage: compare <float dir> <fixed dir>.
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
std::vector<float> load (const std::string& path)
{
    std::vector<float> v;
    if (FILE* f = fopen (path.c_str(), "rb"))
    {
        float x;
        while (fread (&x, sizeof x, 1, f) == 1)
            v.push_back (x);
        fclose (f);
    }
    return v;
}

void fft (std::vector<std::complex<double>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap (a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const std::complex<double> w = std::polar (1.0, -2 * M_PI / double (len));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> u = 1;
            for (size_t k = 0; k < len / 2; ++k, u *= w)
            {
                const auto t = a[i + k + len / 2] * u;
                a[i + k + len / 2] = a[i + k] - t;
                a[i + k] += t;
            }
        }
    }
}

// Energy in third-octave bands from 20 Hz, averaged over Hann windows.
std::vector<double> bands (const std::vector<float>& x)
{
    constexpr size_t n = 8192;
    std::vector<double> power (n / 2, 0.0);
    for (size_t at = 0; at + n <= x.size(); at += n / 2)
    {
        std::vector<std::complex<double>> a (n);
        for (size_t i = 0; i < n; ++i)
            a[i] = x[at + i] * (0.5 - 0.5 * std::cos (2 * M_PI * double (i) / n));
        fft (a);
        for (size_t i = 0; i < n / 2; ++i)
            power[i] += std::norm (a[i]);
    }
    std::vector<double> out;
    for (double low = 20; low < 20000; low *= std::cbrt (2.0))
    {
        double e = 0;
        for (size_t i = 0; i < n / 2; ++i)
            if (const double hz = double (i) * 48000 / n; hz >= low && hz < low * std::cbrt (2.0))
                e += power[i];
        out.push_back (e);
    }
    return out;
}
// Integrated loudness, BS.1770 at 48 kHz: K-weighting, 400 ms blocks every 100 ms, gated at -70 LUFS and
// 10 LU below the ungated mean.
double lufs (const std::vector<float>& x)
{
    auto biquad = [] (std::vector<double> in, const double b[3], const double a[3])
    {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (double& v : in)
        {
            const double y = b[0] * v + b[1] * x1 + b[2] * x2 - a[1] * y1 - a[2] * y2;
            x2 = x1, x1 = v, y2 = y1, y1 = y, v = y;
        }
        return in;
    };
    const double b1[] = { 1.53512485958697, -2.69169618940638, 1.19839281085285 }, a1[] = { 1, -1.69065929318241, 0.73248077421585 };
    const double b2[] = { 1, -2, 1 }, a2[] = { 1, -1.99004745483398, 0.99007225036621 };
    const auto k = biquad (biquad (std::vector<double> (x.begin(), x.end()), b1, a1), b2, a2);
    std::vector<double> blocks;
    for (size_t at = 0; at + 19200 <= k.size(); at += 4800)
    {
        double e = 0;
        for (size_t i = at; i < at + 19200; ++i)
            e += k[i] * k[i];
        blocks.push_back (e / 19200);
    }
    auto gated = [&] (double threshold)
    {
        double sum = 0;
        int n = 0;
        for (double e : blocks)
            if (-0.691 + 10 * std::log10 (e + 1e-30) > threshold)
                sum += e, ++n;
        return n ? -0.691 + 10 * std::log10 (sum / n) : -200.0;
    };
    return gated (gated (-70) - 10);
}

// The strongest peak's frequency, from one long Hann window and a parabola through the peak.
double peakHz (const std::vector<float>& x)
{
    constexpr size_t n = 32768;
    const size_t at = x.size() > n ? x.size() - n : 0;
    std::vector<std::complex<double>> a (n);
    for (size_t i = 0; i < n && at + i < x.size(); ++i)
        a[i] = x[at + i] * (0.5 - 0.5 * std::cos (2 * M_PI * double (i) / n));
    fft (a);
    size_t best = 1;
    for (size_t i = 1; i < n / 2 - 1; ++i)
        if (std::abs (a[i]) > std::abs (a[best]))
            best = i;
    const double l = std::log (std::abs (a[best - 1])), c = std::log (std::abs (a[best])), r = std::log (std::abs (a[best + 1]));
    return (double (best) + 0.5 * (l - r) / (l - 2 * c + r)) * 48000 / n;
}

void writeWav (const std::string& path, const std::vector<float>& x)
{
    FILE* f = fopen (path.c_str(), "wb");
    if (! f)
        return;
    auto u32 = [&] (uint32_t v) { fwrite (&v, 4, 1, f); };
    auto u16 = [&] (uint16_t v) { fwrite (&v, 2, 1, f); };
    const uint32_t bytes = uint32_t (x.size() * 4);
    fwrite ("RIFF", 1, 4, f), u32 (36 + bytes), fwrite ("WAVEfmt ", 1, 8, f), u32 (16), u16 (3), u16 (1);
    u32 (48000), u32 (48000 * 4), u16 (4), u16 (32), fwrite ("data", 1, 4, f), u32 (bytes);
    fwrite (x.data(), 4, x.size(), f);
    fclose (f);
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 3)
        return 2;
    const char* names[] = { "default-melody", "saw-pulse-sub-glide", "pwm-lfo-noise-res08", "sweeps-every-block",
                            "arpeggio", "saw-pulse-full",
                            "selfosc-440", "selfosc-1500", "selfosc-5000", "res095-singing", "top-keys" };
    // The null test applies where the sound is deterministic; a singing resonance diverges, so it is held
    // to loudness, spectra and pitch.
    const bool nulls[] = { true, true, true, true, true, true, false, false, false, false, true };
    const bool pitched[] = { false, false, false, false, false, false, true, true, true, false, false };
    const std::string wavs = argc > 3 ? argv[3] : "";
    int failures = 0;
    printf ("%-22s %8s %9s %9s %9s %10s\n", "scenario", "null dB", "LUFS diff", "band max", "band mean", "pitch c");
    for (size_t s = 0; s < std::size (names); ++s)
    {
        const auto a = load (std::string (argv[1]) + "/" + names[s] + ".f32");
        const auto b = load (std::string (argv[2]) + "/" + names[s] + ".f32");
        if (a.empty() || a.size() != b.size())
        {
            printf ("%-22s missing or different lengths\n", names[s]);
            ++failures;
            continue;
        }
        double e = 0, r = 0;
        for (size_t i = 0; i < a.size(); ++i)
            e += (a[i] - b[i]) * (a[i] - b[i]), r += double (a[i]) * a[i];
        const double null = 10 * std::log10 (e / r + 1e-30);
        const auto ba = bands (a), bb = bands (b);
        double total = 0, worst = 0, mean = 0;
        int counted = 0;
        for (double x : ba)
            total += x;
        for (size_t i = 0; i < ba.size(); ++i)
            // Bands within 60 dB of the whole; for a self-oscillation 40 dB, below which is the noise that
            // started it, shaped differently once the two have drifted apart.
            if (ba[i] > total * (pitched[s] ? 1e-4 : 1e-6))
            {
                const double d = std::abs (10 * std::log10 (bb[i] / ba[i]));
                worst = std::max (worst, d), mean += d, ++counted;
            }
        mean /= std::max (1, counted);
        const double loudness = lufs (b) - lufs (a);
        const double cents = pitched[s] ? 1200 * std::log2 (peakHz (b) / peakHz (a)) : 0;
        const bool ok = (nulls[s] ? null <= -80 : true) && worst <= 0.5 && std::abs (loudness) <= 0.1
                        && std::abs (cents) <= 1;
        failures += ok ? 0 : 1;
        char pitch[16] = "";
        if (pitched[s])
            snprintf (pitch, sizeof pitch, "%.3f", cents);
        printf ("%-22s %8.1f %9.3f %9.3f %9.3f %10s %s\n", names[s], null, loudness, worst, mean, pitch, ok ? "" : "FAIL");
        if (! wavs.empty())
        {
            writeWav (wavs + "/" + names[s] + "-float.wav", a);
            writeWav (wavs + "/" + names[s] + "-fixed.wav", b);
        }
    }
    return failures ? 1 : 0;
}
