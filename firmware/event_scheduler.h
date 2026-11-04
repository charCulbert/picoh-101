// Renders one block, applying each event at its own sample one block after it arrived. An event that
// reaches the queue only after a later one was applied lands at the next sample it can and counts as late.
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

    // The last block's: render pieces, events applied, events applied more than lateUs after their time and
    // the latest of them.
    static constexpr int32_t lateUs = 20;
    uint8_t pieces = 0;
    uint16_t applied = 0, late = 0;
    uint32_t latestUs = 0;

    // pop(Event&) is false when no event waits. startUs is the time of the block's first sample. The
    // events taken are applied in time order, the same time in the order they came: an input polled later
    // can queue an older event behind a newer one.
    template <typename Pop, typename Sample>
    void render (Pop&& pop, uint32_t startUs, Sample* left, Sample* right, int frames)
    {
        pieces = 0;
        applied = late = 0;
        latestUs = 0;
        while (waiting < capacity && pop (pending[waiting]))
        {
            const Event e = pending[waiting];
            int i = waiting++;
            for (; i > 0 && int32_t (e.timeUs - pending[i - 1].timeUs) < 0; --i)
                pending[i] = pending[i - 1];
            pending[i] = e;
        }
        int next = 0;
        for (int done = 0, until; done < frames; done = until)
        {
            until = frames;
            for (; next < waiting; ++next)
            {
                const Event& due = pending[next];
                int sample = sampleOf (due, startUs);
                if (sample > done)
                {
                    until = sample < frames ? sample : frames;
                    break;
                }
                const uint32_t appliedUs = startUs + uint32_t (done) * 1000000u / sampleRate;
                if (const int32_t lateBy = int32_t (appliedUs - due.timeUs - delayUs); lateBy > lateUs)
                {
                    ++late;
                    latestUs = lateBy > int32_t (latestUs) ? uint32_t (lateBy) : latestUs;
                }
                RENDER_TRACE_APPLY (due, done);
                apply (due);
                ++applied;
            }
            RENDER_TRACE_PIECE (pieces, done, until, true);
            synth.render (left + done, right + done, until - done);
            RENDER_TRACE_PIECE (pieces, done, until, false);
            ++pieces;
        }
        for (int i = next; i < waiting; ++i)
            pending[i - next] = pending[i];
        waiting -= next;
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
    // Events taken from the queue and not yet applied: a full queue on top of as many kept for later.
    static constexpr int capacity = 2 * eventQueueCapacity;
    Event pending[capacity];
    int waiting = 0;
};
