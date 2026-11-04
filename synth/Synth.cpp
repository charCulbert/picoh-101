#include "Synth.h"
#include <algorithm>

void Synth::prepare (double sampleRate, int maxFrames)
{
    mono.assign (static_cast<size_t> (std::max (1, maxFrames)), Sample {});
    keys.prepare (sampleRate);
    voice.prepare (sampleRate, 0);
    filterTable.build (sampleRate);
    correctionTable.build (sampleRate);
    envelopeSettings.prepare (sampleRate);
    voice.setSettings (voiceSettings);
    voice.getFilter().setTable (filterTable);
    voice.getVco().setCorrectionTable (correctionTable);
    voice.getEnvelope().setSettings (envelopeSettings);
    setDefaults();
}

void Synth::noteOn (const picohDSP::Note& note) noexcept
{
    if (keyTransposeHeld)
    {
        keyTranspose = note.key - lowestC;
        retune();
        noteEnded (note);
        return;
    }
    sequencer.keyDown (note.key);
    const bool was = clockRuns();
    arpeggio.keyDown (note.key);
    apply (keys.keyDown (note));
    clockChanged (was);
}

void Synth::noteOff (const picohDSP::Note& note) noexcept
{
    const bool was = clockRuns();
    arpeggio.keyUp (note.key);
    apply (keys.keyUp (note));
    clockChanged (was);
}

void Synth::setSustainPedal (bool down) noexcept
{
    pedalDown = down;
    updateHold();
}

void Synth::updateHold() noexcept
{
    const bool was = clockRuns();
    arpeggio.setHold (holdOn);
    apply (keys.setSustainPedal (holdOn || pedalDown));
    clockChanged (was);
}

void Synth::releaseAll() noexcept
{
    const bool was = clockRuns();
    arpeggio.clear();
    apply (keys.releaseAll ([this] (const picohDSP::Note& ended) { noteEnded (ended); }));
    clockChanged (was);
}

void Synth::sequencerLoad() noexcept
{
    const bool was = clockRuns();
    sequencer.load();
    clockChanged (was);
}

void Synth::sequencerPlay() noexcept
{
    const bool was = clockRuns();
    sequencer.play();
    clockChanged (was);
}

void Synth::sequencerRest() noexcept
{
    sequencer.rest();
}

void Synth::setLegato (bool held) noexcept
{
    sequencer.setLegato (held);
}

void Synth::setKeyTransposeHeld (bool held) noexcept
{
    keyTransposeHeld = held;
}

Synth::SequencerState Synth::getSequencerState() const noexcept
{
    return { sequencer.getMode(), sequencer.getPosition(), sequencer.getSequence().length, keyTranspose };
}

void Synth::setSequence (const picohDSP::Sequence& sequence) noexcept
{
    const bool was = clockRuns();
    sequencer.setSequence (sequence);
    clockChanged (was);
}

void Synth::stopAll() noexcept
{
    releaseAll();
    voice.stop();
    apply (keys.voiceEnded());
}

void Synth::setPitchBend (float value) noexcept
{
    bend = std::clamp (value, -1.0f, 1.0f);
    updateBender();
}

void Synth::setModWheel (float value) noexcept
{
    wheel = std::clamp (value, 0.0f, 1.0f);
    updateBender();
}

void Synth::render (Sample* left, Sample* right, int frames) noexcept
{
    PICOH_TRACE_POINT (synthBegin, frames);

    // In pieces no longer than the buffer prepare() made, split where the grace ends and at the LFO's edges.
    for (int done = 0; done < frames;)
    {
        int count = std::min (frames - done, static_cast<int> (mono.size()));
        if (const int grace = keys.samplesToGraceEnd(); grace > 0)
            count = std::min (count, grace);
        const bool lfoGates = keys.getTrigger() == picohDSP::MonoKeys::Trigger::lfo && (keys.isGateOpen() || sequenceGate);
        const bool clocked = clockRuns();
        if (lfoGates || clocked)
            count = std::min (count, voice.getLfo().samplesToEdge());

        std::fill (mono.begin(), mono.begin() + count, Sample {});
        PICOH_TRACE_POINT (monoCleared, count);
        PICOH_TRACE_POINT (voiceBegin, 0);
        if (voice.isActive())
        {
            voice.render (mono.data(), count);
            if (! voice.isActive())
                apply (keys.voiceEnded());
        }
        else
        {
            voice.skip (count);
            PICOH_TRACE_POINT (voiceSkipped, 0);
        }
        PICOH_TRACE_POINT (voiceEnd, 0);
        apply (keys.advance (count));
        if (clocked && voice.getLfo().isHigh() != clockHigh)
        {
            clockHigh = ! clockHigh;
            clockEdge (clockHigh);
        }
        if (keys.isGateOpen() || sequenceGate)
            followLfo (lfoGates);

        for (int i = 0; i < count; ++i)
            left[done + i] = right[done + i] = picohDSP::Voice::scale (mono[static_cast<size_t> (i)], outputLevel);
        PICOH_TRACE_POINT (outputCopied, count);

        done += count;
    }
}

void Synth::apply (const picohDSP::MonoKeys::Action& action) noexcept
{
    using Kind = picohDSP::MonoKeys::Action::Kind;
    const bool glides = portamento == Portamento::on || (portamento == Portamento::autoLegato && action.kind != Kind::start);
    if (action.hasEnded)
        noteEnded (action.ended);
    if (arpeggio.isRunning() && sequencer.getMode() != picohDSP::Sequencer::Mode::playing && action.kind != Kind::release)
        return; // the arpeggio plays the keys, on the clock
    switch (action.kind)
    {
        case Kind::start:
            if (keys.getTrigger() == picohDSP::MonoKeys::Trigger::lfo)
                voice.getLfo().restart();
            lfoHigh = true;
            sequenceGate = false;
            voice.start (action.play.key, glides);
            break;
        case Kind::restart: voice.start (action.play.key, glides); break;
        case Kind::move: voice.setKey (action.play.key, glides); break;
        case Kind::release:
            if (! sequenceGate)
                voice.release();
            break;
        case Kind::none: break;
    }
}

bool Synth::clockRuns() const noexcept
{
    return sequencer.getMode() == picohDSP::Sequencer::Mode::playing || arpeggio.isRunning();
}

// The clock starts with a step at once; an arpeggio that stops leaves the held key sounding.
void Synth::clockChanged (bool wasRunning) noexcept
{
    const bool running = clockRuns();
    if (running && ! wasRunning)
    {
        voice.getLfo().restart();
        lfoHigh = clockHigh = true;
        arpeggio.restart();
        clockEdge (true);
    }
    else if (! running && wasRunning)
    {
        if (sequenceGate)
        {
            sequenceGate = false;
            if (! keys.isGateOpen())
                voice.release();
        }
        if (keys.isGateOpen())
            voice.setKey (keys.sounding().key, false);
    }
}

// A rise steps the sequence or the arpeggio; a fall ends a detached step of the sequence. Keys held over a
// playing sequence keep the voice.
void Synth::clockEdge (bool rising) noexcept
{
    using Trigger = picohDSP::MonoKeys::Trigger;
    const bool retrigger = keys.getTrigger() == Trigger::gateTrig;
    if (sequencer.getMode() == picohDSP::Sequencer::Mode::playing)
    {
        const auto event = rising ? sequencer.clockRise() : sequencer.clockFall();
        if (keys.isGateOpen())
            return;
        using Kind = picohDSP::Sequencer::Event::Kind;
        switch (event.kind)
        {
            case Kind::note:
                sequenceGate = true;
                lfoHigh = true;
                voice.start (event.key, portamento == Portamento::on);
                break;
            case Kind::tied:
                if (retrigger)
                    voice.start (event.key, portamento != Portamento::off);
                else
                    voice.setKey (event.key, portamento != Portamento::off);
                break;
            case Kind::gateOff:
                sequenceGate = false;
                voice.release();
                break;
            case Kind::none: break;
        }
        return;
    }
    if (! rising || ! arpeggio.isRunning())
        return;
    const int key = arpeggio.step();
    const bool glides = portamento == Portamento::on;
    if (retrigger)
        voice.start (key, glides);
    else
        voice.setKey (key, glides);
}

// On LFO the square opens and closes the ADSR's gate; off LFO, a gate it left closed opens again.
void Synth::followLfo (bool lfoGates) noexcept
{
    const bool high = ! lfoGates || voice.getLfo().isHigh();
    if (high == lfoHigh)
        return;
    lfoHigh = high;
    if (high)
        voice.attack();
    else
        voice.release();
}

void Synth::noteEnded (const picohDSP::Note& note) noexcept
{
    if (onNoteEnded)
        onNoteEnded (note);
}
