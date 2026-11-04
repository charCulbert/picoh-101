// Renders one block, applying each event at its own sample one block after it arrived, so every event is
// late by the same fixed amount.
#pragma once
#include <cstdint>
#include "events.h"

// Trace stamps, defined by synth_core.cpp.
#ifndef RENDER_TRACE_APPLY
#define RENDER_TRACE_APPLY(event, sample) ((void) 0)
#define RENDER_TRACE_PIECE(piece, from, until, begin) ((void) 0)
#endif

template <typename Synth>
struct EventScheduler
{
    Synth& synth;
    uint32_t sampleRate;
    uint32_t delayUs; // how long after arriving an event sounds: one block

    EventScheduler (Synth& synth, uint32_t sampleRate, uint32_t delayUs)
        : synth (synth)
        , sampleRate (sampleRate)
        , delayUs (delayUs)
    {}

    // pop(Event&) is false when no event waits. startUs is the time of the block's first sample.
    uint8_t pieces = 0;
    uint16_t applied = 0;

    template <typename Pop>
    void render (Pop&& pop, uint32_t startUs, float* left, float* right, int frames)
    {
        pieces = 0;
        applied = 0;
        for (int done = 0, until; done < frames; done = until)
        {
            until = frames;
            while (haveDue || (haveDue = pop (due)))
            {
                int sample = sampleOf (due, startUs);
                if (sample > done)
                {
                    until = sample < frames ? sample : frames;
                    break;
                }
                RENDER_TRACE_APPLY (due, done);
                apply (due);
                ++applied;
                haveDue = false;
            }
            RENDER_TRACE_PIECE (pieces, done, until, true);
            synth.render (left + done, right + done, until - done);
            RENDER_TRACE_PIECE (pieces, done, until, false);
            ++pieces;
        }
    }

    int sampleOf (const Event& event, uint32_t startUs) const
    {
        int32_t aheadUs = int32_t (event.timeUs + delayUs - startUs);
        if (aheadUs <= 0)
            return 0;
        return int ((int64_t (aheadUs) * sampleRate + 500000) / 1000000);
    }

    void apply (const Event& e)
    {
        switch (e.type)
        {
            case EVENT_NOTE_ON: synth.noteOn (e.note.key); break;
            case EVENT_NOTE_OFF: synth.noteOff (e.note.key); break;
            case EVENT_PARAM: synth.setParameter (e.param.id, e.param.value); break;
            case EVENT_SUSTAIN: synth.sustainPedal (e.note.velocity != 0); break;
            case EVENT_ALL_NOTES_OFF: synth.allNotesOff(); break;
            case EVENT_PITCH_BEND: synth.pitchBend (e.param.value); break;
            case EVENT_MOD_WHEEL: synth.modWheel (e.param.value); break;
            case EVENT_SEQUENCER: synth.sequencerButton (e.param.id); break;
            case EVENT_LEGATO: synth.legato (e.note.velocity != 0); break;
            case EVENT_KEY_TRANSPOSE: synth.keyTransposeHeld (e.note.velocity != 0); break;
        }
    }

private:
    Event due {};
    bool haveDue = false;
};
