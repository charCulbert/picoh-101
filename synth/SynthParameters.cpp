// Synth::setParameter, apart from Synth.cpp so it stays in flash on the Pico (Synth.cpp runs from RAM).
#include "Synth.h"
#include <algorithm>
#include "params.h"

void Synth::setLevel (picohDSP::Vco::Source source, float level) noexcept
{
    voice.getVco().setLevel (source, level);
}

void Synth::setDefaults() noexcept
{
    for (const Param& p : paramTable)
        setParameter (p.id, (float) p.defaultValue);
}

void Synth::setParameter (uint32_t id, float value) noexcept
{
    using picohDSP::Voice;
    switch (id)
    {
        case P_TUNE:
            tuneCents = value;
            retune();
            break;
        case P_RANGE:
            rangeIndex = (int) value;
            retune();
            break;
        case P_TRANSPOSE:
            transposeIndex = (int) value;
            retune();
            break;
        case P_LFO_RATE: voice.getLfo().setRate (value); break;
        case P_LFO_WAVE:
        {
            using W = picohDSP::LFO::Waveform;
            static constexpr W waves[] = { W::triangle, W::square, W::sampleHold, W::noise };
            voice.getLfo().setWaveform (waves[(int) value & 3]);
            break;
        }
        case P_VCO_MOD: voiceSettings.lfoToPitchCents = value * vcoModCents; break;
        // Bottom is a square; up narrows the pulse to 5%. The pulse shape spans 2% to 98%.
        case P_PULSE_WIDTH: voice.getVco().setPulseWidth (value); break;
        case P_PWM_SOURCE:
        {
            using S = Voice::PwmSource;
            static constexpr S sources[] = { S::lfo, S::manual, S::env }; // the switch: LFO, MAN, ENV
            voiceSettings.pwmSource = sources[std::clamp ((int) value, 0, 2)];
            break;
        }
        case P_PULSE: setLevel (picohDSP::Vco::Source::pulse, value); break;
        case P_SAW: setLevel (picohDSP::Vco::Source::saw, value); break;
        case P_SUB_OSC: setLevel (picohDSP::Vco::Source::sub, value); break;
        case P_NOISE: setLevel (picohDSP::Vco::Source::noise, value); break;
        case P_SUB_WAVE:
        {
            const auto wave = static_cast<picohDSP::Vco::SubWave> (std::clamp ((int) value, 0, 2));
            voice.getVco().setSubWave (wave);
            break;
        }
        case P_VCF_FREQ:
        {
            const float note = picohDSP::Filter::cutoffToNote (value);
            voice.getFilter().setCutoffNote (note);
            break;
        }
        case P_VCF_RES: voice.getFilter().setResonance (value); break;
        case P_VCF_ENV: voiceSettings.envAmount = std::clamp (value, -1.0f, 1.0f); break;
        case P_VCF_MOD: voiceSettings.lfoToCutoffSemitones = value * vcfModSemitones; break;
        case P_VCF_KYBD: voiceSettings.keyTrack = std::clamp (value, -1.0f, 1.0f); break;
        case P_ENV_TRIGGER:
            keys.setTrigger (static_cast<picohDSP::MonoKeys::Trigger> (std::clamp ((int) value, 0, 2)));
            break;
        case P_PORTAMENTO: voice.getGlide().setTime (value); break;
        case P_PORTAMENTO_MODE: portamento = static_cast<Portamento> (std::clamp ((int) value, 0, 2)); break;
        case P_BEND_VCO:
            bendToVco = value;
            updateBender();
            break;
        case P_BEND_VCF:
            bendToVcf = value;
            updateBender();
            break;
        case P_LFO_MOD:
            lfoMod = value;
            updateBender();
            break;
        case P_VCA_MODE: voiceSettings.vcaGate = value >= 1; break;
        case P_ATTACK: envelopeSettings.setAttack (value); break;
        case P_DECAY: envelopeSettings.setDecay (value); break;
        case P_SUSTAIN: envelopeSettings.setSustain (value); break;
        case P_RELEASE: envelopeSettings.setRelease (value); break;
        case P_VOLUME: volume = value; break;
        case P_ARPEGGIO:
        {
            const bool was = clockRuns();
            arpeggio.setMode (static_cast<picohDSP::Arpeggio::Mode> (std::clamp ((int) value, 0, 3)));
            clockChanged (was);
            break;
        }
        case P_HOLD:
            holdOn = value >= 1;
            updateHold();
            break;
        default: break; // a control the DSP doesn't have yet
    }

    if (id == P_ATTACK || id == P_DECAY || id == P_SUSTAIN || id == P_RELEASE)
        voice.getEnvelope().settingsChanged();
}

void Synth::updateBender() noexcept
{
    voiceSettings.bendCents = bend * bendToVco * bendSemitones * 100.0f;
    voiceSettings.bendCutoffSemitones = bend * bendToVcf * bendCutoffSemitones;
    voiceSettings.wheelToPitchCents = wheel * lfoMod * vcoModCents;
    voiceSettings.wheelToCutoffSemitones = wheel * lfoMod * vcfModSemitones;
}

// RANGE is 16', 8', 4', 2' (8' is concert pitch); TRANSPOSE is an octave down, none, or up; KEY TRANSPOSE
// is semitones.
void Synth::retune() noexcept
{
    const float cents = tuneCents + 1200.0f * float (rangeIndex - 1) + 1200.0f * float (transposeIndex - 1)
                        + 100.0f * float (keyTranspose);
    voice.getVco().setTune (cents);
}
