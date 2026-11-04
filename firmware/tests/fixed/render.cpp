// Renders the same note sequences and patches through the synth; built twice, float and fixed point
// (PICOH_FIXED_POINT), each writing <dir>/<scenario>.f32 for compare.cpp.
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
#include "Synth.h"
#include "params.h"

namespace
{
constexpr int block = 64;
constexpr double rate = 48000;

struct Scenario
{
    const char* name;
    std::function<void (Synth&)> patch;
    // Called before each block with its index: notes and parameter moves.
    std::function<void (Synth&, int)> play;
    int blocks;
};

void note (Synth& s, int key, bool on)
{
    picohDSP::Note n;
    n.key = key;
    on ? s.noteOn (n) : s.noteOff (n);
}

// Notes of `length` blocks, one after another, each released `gap` blocks before the next.
std::function<void (Synth&, int)> melody (std::vector<int> keys, int length, int gap)
{
    return [=] (Synth& s, int b)
    {
        const int i = b / length, at = b % length;
        if (i >= int (keys.size()))
            return;
        if (at == 0)
            note (s, keys[size_t (i)], true);
        if (at == length - gap)
            note (s, keys[size_t (i)], false);
    };
}

void selfOscillate (Synth& s, float hz)
{
    s.setParameter (P_SAW, 0), s.setParameter (P_NOISE, 0.001f), s.setParameter (P_VCF_RES, 1), s.setParameter (P_VCF_FREQ, hz);
    s.setParameter (P_VCF_ENV, 0), s.setParameter (P_VCF_KYBD, 0), s.setParameter (P_VCA_MODE, 1);
}

const std::vector<Scenario> scenarios = {
    { "default-melody", [] (Synth&) {}, melody ({ 36, 48, 55, 60, 67, 72, 79, 84, 96 }, 300, 60), 2800 },
    { "saw-pulse-sub-glide",
      [] (Synth& s)
      {
          s.setParameter (P_PULSE, 0.8f), s.setParameter (P_SUB_OSC, 0.6f), s.setParameter (P_PORTAMENTO_MODE, 2);
          s.setParameter (P_PORTAMENTO, 0.2f), s.setParameter (P_VCF_RES, 0.5f), s.setParameter (P_PWM_SOURCE, 2);
          s.setParameter (P_PULSE_WIDTH, 0.7f);
      },
      melody ({ 40, 52, 47, 59, 64, 45, 71 }, 250, 10), 1800 },
    { "pwm-lfo-noise-res08",
      [] (Synth& s)
      {
          s.setParameter (P_SAW, 0), s.setParameter (P_PULSE, 1), s.setParameter (P_PWM_SOURCE, 0);
          s.setParameter (P_PULSE_WIDTH, 0.8f), s.setParameter (P_SUB_WAVE, 2), s.setParameter (P_SUB_OSC, 0.5f);
          s.setParameter (P_NOISE, 0.1f), s.setParameter (P_VCF_RES, 0.8f), s.setParameter (P_VCF_ENV, 0.8f);
          s.setParameter (P_VCF_MOD, 0.3f), s.setParameter (P_VCO_MOD, 0.05f), s.setParameter (P_LFO_RATE, 6);
      },
      melody ({ 48, 60, 43, 67, 55 }, 350, 50), 1800 },
    { "sweeps-every-block",
      [] (Synth& s) { s.setParameter (P_SUB_OSC, 0.4f), s.setParameter (P_PULSE, 0.4f), s.setParameter (P_VCF_RES, 0.6f); },
      [] (Synth& s, int b)
      {
          if (b == 0)
              note (s, 50, true);
          if (b == 1500)
              note (s, 62, true), note (s, 50, false);
          if (b == 2900)
              note (s, 62, false);
          const float t = float (b) / 3000.0f;
          s.setParameter (P_VCF_FREQ, 100.0f + 8000.0f * t);
          s.setParameter (P_PULSE_WIDTH, 0.5f + 0.5f * std::sin (float (b) * 0.01f));
          s.setParameter (P_TUNE, 30.0f * std::sin (float (b) * 0.003f));
          s.setPitchBend (std::sin (float (b) * 0.004f));
          s.setModWheel (t);
      },
      3200 },
    { "arpeggio",
      [] (Synth& s) { s.setParameter (P_ARPEGGIO, 1), s.setParameter (P_LFO_RATE, 8), s.setParameter (P_VCF_RES, 0.4f); },
      [] (Synth& s, int b)
      {
          if (b == 0)
              note (s, 48, true), note (s, 52, true), note (s, 55, true), note (s, 60, true);
          if (b == 2000)
              note (s, 48, false), note (s, 52, false), note (s, 55, false), note (s, 60, false);
      },
      2400 },
    // The filter alone, singing: a trace of noise to start it, the VCA held open on GATE; the self-oscillation's pitch.
    { "saw-pulse-full",
      [] (Synth& s) { s.setParameter (P_SAW, 1), s.setParameter (P_PULSE, 1), s.setParameter (P_SUB_OSC, 1); },
      melody ({ 36, 48, 60, 72, 84 }, 300, 60), 1500 },
    { "selfosc-440", [] (Synth& s) { selfOscillate (s, 440); }, [] (Synth& s, int b) { if (b == 0) note (s, 60, true); }, 1200 },
    { "selfosc-1500", [] (Synth& s) { selfOscillate (s, 1500); }, [] (Synth& s, int b) { if (b == 0) note (s, 60, true); }, 1200 },
    { "selfosc-5000", [] (Synth& s) { selfOscillate (s, 5000); }, [] (Synth& s, int b) { if (b == 0) note (s, 60, true); }, 1200 },
    { "res095-singing",
      [] (Synth& s) { s.setParameter (P_VCF_RES, 0.95f), s.setParameter (P_VCF_FREQ, 600), s.setParameter (P_VCF_ENV, 0.6f); },
      melody ({ 36, 48, 60, 72 }, 400, 80), 1700 },
    // The VCO at its top: the highest keys with RANGE, TRANSPOSE, TUNE, the bend and VCO MOD all up, every
    // source, the fastest PWM.
    { "top-keys",
      [] (Synth& s)
      {
          s.setParameter (P_PULSE, 1), s.setParameter (P_SAW, 1), s.setParameter (P_SUB_OSC, 1), s.setParameter (P_NOISE, 1);
          s.setParameter (P_PWM_SOURCE, 0), s.setParameter (P_PULSE_WIDTH, 1), s.setParameter (P_LFO_RATE, 30);
          s.setParameter (P_RANGE, 3), s.setParameter (P_TRANSPOSE, 2), s.setParameter (P_TUNE, 100);
          s.setParameter (P_VCO_MOD, 1), s.setParameter (P_BEND_VCO, 1), s.setParameter (P_VCF_FREQ, 20000);
          s.setPitchBend (1);
      },
      melody ({ 96, 103, 108, 115, 120, 124, 127 }, 200, 20), 1400 },
};
} // namespace

int main (int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : ".";
    for (const Scenario& sc : scenarios)
    {
        Synth synth;
        synth.prepare (rate, block);
        sc.patch (synth);
        std::vector<float> out;
        Synth::Sample left[block], right[block];
        for (int b = 0; b < sc.blocks; ++b)
        {
            sc.play (synth, b);
            synth.render (left, right, block);
            for (auto x : left)
#if PICOH_FIXED_POINT
                out.push_back (float (x) / float (1 << 27));
#else
                out.push_back (x);
#endif
        }
        FILE* f = fopen ((dir + "/" + sc.name + ".f32").c_str(), "wb");
        fwrite (out.data(), sizeof (float), out.size(), f);
        fclose (f);
    }
}
