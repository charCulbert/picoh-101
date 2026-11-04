// Notes and parameters in, audio out.
#pragma once
#include <cstdint>

#include "DSP/picohDSP_Arpeggio.h"
#include "DSP/picohDSP_MonoKeys.h"
#include "DSP/picohDSP_Sequencer.h"
#include "DSP/picohDSP_Voice.h"
#include <functional>
#include <vector>

struct Synth
{
    // Allocates. maxFrames is the most render() is asked for at once.
    void prepare (double sampleRate, int maxFrames);

    void noteOn (const picohDSP::Note&) noexcept;
    void noteOff (const picohDSP::Note&) noexcept;
    void setSustainPedal (bool down) noexcept;

    void releaseAll() noexcept; // every note released normally
    void stopAll() noexcept;    // silence now

    // By id (params/parameters.json), in the list's units.
    void setParameter (uint32_t id, float value) noexcept;
    void setDefaults() noexcept;
    // The bender: sideways -1 to 1 (pitch bend), pushed 0 to 1 (the mod wheel).
    void setPitchBend (float) noexcept;
    void setModWheel (float) noexcept;
    // The buttons that aren't parameters (ARPEGGIO and HOLD are). REST and LEGATO share KEY TRANSPOSE's
    // and HOLD's buttons; the GUI picks which while loading.
    void sequencerLoad() noexcept;
    void sequencerPlay() noexcept;
    void sequencerRest() noexcept;
    void setLegato (bool held) noexcept;
    void setKeyTransposeHeld (bool held) noexcept; // while held, a key sets the transpose and doesn't play

    struct SequencerState
    {
        picohDSP::Sequencer::Mode mode;
        int position, length;
        int keyTranspose; // semitones from the lowest C
    };
    SequencerState getSequencerState() const noexcept;
    const picohDSP::Sequence& getSequence() const noexcept { return sequencer.getSequence(); }
    void setSequence (const picohDSP::Sequence&) noexcept;
    uint32_t getSequenceVersion() const noexcept { return sequencer.getVersion(); } // changed: save it

    // Each target applies Volume to its own output, gliding its own way.
    float getVolume() const noexcept { return volume; }

    // The same in both channels.
    using Sample = picohDSP::Voice::Sample; // float, or Q27 in the fixed-point build
    void render (Sample* left, Sample* right, int frames) noexcept;

    // Each note that has finished sounding, during noteOn, stopAll or render.
    std::function<void (const picohDSP::Note&)> onNoteEnded;

private:
    float volume = 1.0f;
    // TUNE, RANGE, TRANSPOSE and KEY TRANSPOSE together set the VCO's pitch, so KYBD doesn't follow them.
    float tuneCents = 0.0f;
    int rangeIndex = 1, transposeIndex = 1;
    static constexpr int lowestC = 48; // KEY TRANSPOSE's 0
    int keyTranspose = 0;
    bool keyTransposeHeld = false;
    void retune() noexcept;
    // BENDER VCO, VCF and LFO MOD at full, with the lever all the way.
    static constexpr float bendSemitones = 12.0f, bendCutoffSemitones = 24.0f;
    // VCO MOD and VCF MOD at full: about an octave, and 4 octaves.
    static constexpr float vcoModCents = 1200.0f, vcfModSemitones = 48.0f;
    float bend = 0.0f, wheel = 0.0f, bendToVco = 0.0f, bendToVcf = 0.0f, lfoMod = 0.0f;
    void updateBender() noexcept; // into voiceSettings
    enum class Portamento // the switch's order
    {
        autoLegato,
        off,
        on
    };
    Portamento portamento = Portamento::off;
    void setLevel (picohDSP::Vco::Source, float level) noexcept; // a mixer fader
    // A voice peaks at Pulse + Saw (2), and resonance can add more: this doesn't prevent clipping.
    static constexpr float outputLevel = 0.25f;

    picohDSP::MonoKeys keys;
    picohDSP::Voice voice;
    std::vector<Sample> mono;
    picohDSP::Voice::Settings voiceSettings;
    picohDSP::Envelope::Settings envelopeSettings;
    picohDSP::CoefficientTable filterTable;
    picohDSP::Voice::CorrectionTable correctionTable;

    picohDSP::Arpeggio arpeggio;
    picohDSP::Sequencer sequencer;
    bool clockHigh = false; // the LFO's square at the last clock edge
    bool clockRuns() const noexcept;      // an arpeggio or a sequence is playing
    void clockChanged (bool wasRunning) noexcept;
    void clockEdge (bool rising) noexcept; // the arpeggio's or sequence's step
    bool sequenceGate = false;             // a sequence step's gate is open
    bool holdOn = false, pedalDown = false; // HOLD, and the sustain pedal
    void updateHold() noexcept;

    bool lfoHigh = false; // the ADSR's gate as the LFO's square last left it
    void apply (const picohDSP::MonoKeys::Action&) noexcept;
    void followLfo (bool lfoGates) noexcept;
    void noteEnded (const picohDSP::Note&) noexcept;
};
