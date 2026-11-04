#pragma once

#include <cstdint>

namespace picohDSP
{

struct Note
{
    int16_t channel = 0;
    int16_t key = 0;
    int32_t hostId = -1; // -1: the host gave none

    bool matches (const Note& other) const noexcept;
};

// The keys held, and what the one voice does about them. The lowest key sounds on GATE and LFO, the last
// pressed on GATE+TRIG. GATE+TRIG restarts the ADSR on every new key; GATE and LFO only when no key was
// held. Releasing the sounding key while others are held moves to the one that now wins, without a new
// attack. After the last key goes up on GATE or LFO, the gate stays open for graceSeconds.
struct MonoKeys
{
    enum class Trigger // ENV TRIGGER's order
    {
        gateTrig,
        gate,
        lfo
    };

    struct Action
    {
        enum class Kind
        {
            none,
            start,   // gate opens on play
            restart, // play, and the ADSR's attack again
            move,    // play, the ADSR carries on
            release  // gate closes
        };
        Kind kind = Kind::none;
        Note play;
        bool hasEnded = false;
        Note ended; // a note that stops sounding now, for the host
    };

    static constexpr float graceSeconds = 0.024f;
    static constexpr int capacity = 16; // a 17th key replaces the oldest

    void prepare (double sampleRate) noexcept;
    void setTrigger (Trigger) noexcept;
    Trigger getTrigger() const noexcept { return trigger; }

    Action keyDown (const Note&) noexcept;
    Action keyUp (const Note&) noexcept;
    Action setSustainPedal (bool down) noexcept;
    template <typename OnEnded>
    Action releaseAll (OnEnded&&) noexcept; // onEnded for each key up that wasn't sounding
    Action voiceEnded() noexcept;           // the voice has fallen silent

    Action advance (int frames) noexcept;   // the release, when the grace runs out
    int samplesToGraceEnd() const noexcept; // -1 when no grace is running

    bool isGateOpen() const noexcept { return gateOpen; }
    const Note& sounding() const noexcept { return soundingNote; }

private:
    struct Held
    {
        Note note;
        uint32_t order = 0; // 0: empty
    };
    Held held[capacity];
    Trigger trigger = Trigger::gateTrig;
    bool gateOpen = false, pedalDown = false;
    bool lingering = false; // the sounding note's key is up, and it hasn't ended
    Note soundingNote;
    int graceLeft = -1, graceSamples = 0;
    uint32_t nextOrder = 1;

    Held* find (const Note&) noexcept;
    const Held* winner() const noexcept;
    Action close() noexcept;
};

//=====================================================
//  ╷ ╭┬╮ ╭─╮ ╷   ╭─╴ ╭┬╮ ╭─╴ ╭╮╷ ╶┬╴ ╭─╮ ╶┬╴ ╷ ╭─╮ ╭╮╷
//  │ │││ ├─╯ │   ├─  │││ ├─  │││  │  ├─┤  │  │ │ │ │││
//  ╵ ╵ ╵ ╵   ╰─╴ ╰─╴ ╵ ╵ ╰─╴ ╵╰╯  ╵  ╵ ╵  ╵  ╵ ╰─╯ ╵╰╯
//=====================================================

inline bool Note::matches (const Note& other) const noexcept
{
    if (hostId != -1 && other.hostId != -1)
        return hostId == other.hostId;

    return channel == other.channel && key == other.key;
}

inline void MonoKeys::prepare (double sampleRate) noexcept
{
    graceSamples = static_cast<int> (graceSeconds * sampleRate);
    for (auto& h : held)
        h = {};
    gateOpen = pedalDown = false;
    lingering = false;
    graceLeft = -1;
    nextOrder = 1;
}

inline void MonoKeys::setTrigger (Trigger newTrigger) noexcept
{
    trigger = newTrigger;
}

inline MonoKeys::Action MonoKeys::keyDown (const Note& note) noexcept
{
    Action action;
    Held* slot = find (note);
    if (! slot)
        for (auto& h : held)
            if (! slot || h.order < slot->order)
                slot = &h; // an empty one, or else the oldest
    const Note& old = slot->note;
    if (slot->order != 0 && (old.hostId != note.hostId || ! old.matches (note)))
    {
        action.hasEnded = true;
        action.ended = old;
    }
    else if (lingering)
    {
        action.hasEnded = true;
        action.ended = soundingNote;
    }
    lingering = false;
    *slot = { note, nextOrder++ };
    graceLeft = -1;

    const Note& next = winner()->note;
    if (! gateOpen)
        action.kind = Action::Kind::start;
    else if (trigger == Trigger::gateTrig)
        action.kind = Action::Kind::restart;
    else if (next.hostId != soundingNote.hostId || ! next.matches (soundingNote))
        action.kind = Action::Kind::move;
    else
        return action;

    gateOpen = true;
    action.play = soundingNote = next;
    return action;
}

inline MonoKeys::Action MonoKeys::keyUp (const Note& note) noexcept
{
    Action action;
    Held* slot = find (note);
    if (! slot)
        return action;

    action.ended = slot->note;
    *slot = {};
    const bool wasSounding = gateOpen && action.ended.hostId == soundingNote.hostId && action.ended.matches (soundingNote);
    if (! wasSounding)
    {
        action.hasEnded = true;
        return action;
    }
    if (const Held* next = winner())
    {
        action.hasEnded = true;
        action.kind = Action::Kind::move;
        action.play = soundingNote = next->note;
        return action;
    }

    lingering = true;
    if (pedalDown)
        return {};
    if (trigger != Trigger::gateTrig && graceSamples > 0)
    {
        graceLeft = graceSamples;
        return {};
    }
    return close();
}

inline MonoKeys::Action MonoKeys::setSustainPedal (bool down) noexcept
{
    pedalDown = down;
    if (down)
        graceLeft = -1;
    else if (gateOpen && ! winner())
        return close();
    return {};
}

template <typename OnEnded>
MonoKeys::Action MonoKeys::releaseAll (OnEnded&& onEnded) noexcept
{
    for (auto& h : held)
    {
        if (h.order == 0)
            continue;
        if (gateOpen && h.note.hostId == soundingNote.hostId && h.note.matches (soundingNote))
            lingering = true;
        else
            onEnded (h.note);
        h = {};
    }
    pedalDown = false;
    return gateOpen ? close() : Action {};
}

inline MonoKeys::Action MonoKeys::voiceEnded() noexcept
{
    Action action;
    if (! gateOpen && lingering)
    {
        action.hasEnded = true;
        action.ended = soundingNote;
        lingering = false;
    }
    return action;
}

inline MonoKeys::Action MonoKeys::advance (int frames) noexcept
{
    if (graceLeft < 0)
        return {};
    graceLeft -= frames;
    if (graceLeft > 0)
        return {};
    return close();
}

inline int MonoKeys::samplesToGraceEnd() const noexcept
{
    return graceLeft;
}

inline MonoKeys::Held* MonoKeys::find (const Note& note) noexcept
{
    for (auto& h : held)
        if (h.order != 0 && h.note.matches (note))
            return &h;
    return nullptr;
}

// The lowest on GATE and LFO, the last pressed on GATE+TRIG.
inline const MonoKeys::Held* MonoKeys::winner() const noexcept
{
    const Held* best = nullptr;
    for (const auto& h : held)
    {
        if (h.order == 0)
            continue;
        if (! best || (trigger == Trigger::gateTrig ? h.order > best->order : h.note.key < best->note.key))
            best = &h;
    }
    return best;
}

inline MonoKeys::Action MonoKeys::close() noexcept
{
    gateOpen = false;
    graceLeft = -1;
    Action action;
    action.kind = Action::Kind::release;
    return action;
}

} // namespace picohDSP
