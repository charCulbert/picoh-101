// Synth as the Pico drives it: notes by key, and Volume applied to the output with a short glide.
#pragma once
#include <cstdint>
#include "Synth.h"

struct SynthHost
{
    // Allocates: once, before audio starts.
    void prepare (float sampleRate, int maxFrames) { synth.prepare (sampleRate, maxFrames); }

    void noteOn (uint8_t key) { synth.noteOn (note (key)); }
    void noteOff (uint8_t key) { synth.noteOff (note (key)); }
    void sustainPedal (bool down) { synth.setSustainPedal (down); }
    void allNotesOff() { synth.releaseAll(); }
    void pitchBend (float value) { synth.setPitchBend (value); }
    void modWheel (float value) { synth.setModWheel (value); }
    void setParameter (uint32_t id, float value) { synth.setParameter (id, value); }
    void sequencerButton (int button)
    {
        if (button == 0)
            synth.sequencerLoad();
        else if (button == 1)
            synth.sequencerPlay();
        else if (button == 2)
            synth.sequencerRest();
    }
    void legato (bool down) { synth.setLegato (down); }
    void keyTransposeHeld (bool down) { synth.setKeyTransposeHeld (down); }

    Synth::SequencerState sequencerState() const { return synth.getSequencerState(); }
    const picohDSP::Sequence& sequence() const { return synth.getSequence(); }
    void setSequence (const picohDSP::Sequence& s) { synth.setSequence (s); }
    uint32_t sequenceVersion() const { return synth.getSequenceVersion(); }

#if PICOH_FIXED_POINT
    // left and right in Q27; the volume, gain and rate in Q30.
    void render (int32_t* left, int32_t* right, int frames)
    {
        synth.render (left, right, frames);
        const int32_t target = picohDSP::fixed::toQ30 (synth.getVolume());
        constexpr int32_t rate = int32_t (0.004 * (1 << 30));
        for (int i = 0; i < frames; ++i)
        {
            gain += picohDSP::fixed::mul (target - gain, rate, 30);
            left[i] = picohDSP::fixed::mul (left[i], gain, 30);
            right[i] = picohDSP::fixed::mul (right[i], gain, 30);
        }
    }
#else
    void render (float* left, float* right, int frames)
    {
        synth.render (left, right, frames);
        // ~5 ms, so a step doesn't click.
        const float target = synth.getVolume();
        for (int i = 0; i < frames; ++i)
        {
            gain += 0.004f * (target - gain);
            left[i] *= gain;
            right[i] *= gain;
        }
    }
#endif

private:
    static picohDSP::Note note (uint8_t key)
    {
        picohDSP::Note n;
        n.key = key;
        return n;
    }

    Synth synth;
#if PICOH_FIXED_POINT
    int32_t gain = 0;
#else
    float gain = 0.0f;
#endif
};
