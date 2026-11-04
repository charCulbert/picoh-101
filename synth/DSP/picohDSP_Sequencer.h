#pragma once

#include <cstdint>

namespace picohDSP
{

// Up to 100 equal steps, each a key, a key tied from the step before, or a rest. Saved as these bytes.
struct Sequence
{
    static constexpr int maxSteps = 100;
    static constexpr uint8_t rest = 0xFF, tied = 0x80; // a step: a key 0-127, or'd with tied, or rest

    uint8_t steps[maxSteps] = {};
    uint8_t length = 0;
};

// LOAD, PLAY, REST and LEGATO. Loading stores a step per key, pitch only. Playing steps on the clock's
// rise and closes a detached step's gate on its fall, so the gate is open for the clock's high half.
struct Sequencer
{
    enum class Mode
    {
        stopped,
        loading,
        playing
    };

    struct Event
    {
        enum class Kind
        {
            none,
            note,    // gate opens on key
            tied,    // move to key, the gate stays open
            gateOff, // the end of a detached step, or a rest
        };
        Kind kind = Kind::none;
        int key = 0;
    };

    // LOAD: from stopped, a new sequence from the start; while playing, loading carries on after the step
    // playing, and steps after the new ones stay. Again, or PLAY, ends it.
    void load() noexcept;
    void play() noexcept; // starts or stops; ends loading
    void rest() noexcept;
    void setLegato (bool held) noexcept;
    bool keyDown (int key) noexcept; // stores a step while loading (true), and stops at 100 steps

    Event clockRise() noexcept;
    Event clockFall() noexcept;

    Mode getMode() const noexcept { return mode; }
    int getPosition() const noexcept; // loading: steps stored up to here; playing: the step sounding, from 1
    const Sequence& getSequence() const noexcept { return sequence; }
    void setSequence (const Sequence&) noexcept; // stops
    uint32_t getVersion() const noexcept { return version; } // up by one each time loading ends

private:
    Sequence sequence;
    Mode mode = Mode::stopped;
    int position = 0; // loading: where the next step goes; playing: the next step to play
    bool legato = false, gateOpen = false;
    uint32_t version = 0;

    void store (uint8_t step) noexcept;
    void endLoading() noexcept;
};

//=====================================================
//  ╷ ╭┬╮ ╭─╮ ╷   ╭─╴ ╭┬╮ ╭─╴ ╭╮╷ ╶┬╴ ╭─╮ ╶┬╴ ╷ ╭─╮ ╭╮╷
//  │ │││ ├─╯ │   ├─  │││ ├─  │││  │  ├─┤  │  │ │ │ │││
//  ╵ ╵ ╵ ╵   ╰─╴ ╰─╴ ╵ ╵ ╰─╴ ╵╰╯  ╵  ╵ ╵  ╵  ╵ ╰─╯ ╵╰╯
//=====================================================

inline void Sequencer::load() noexcept
{
    switch (mode)
    {
        case Mode::loading: endLoading(); break;
        case Mode::playing: // position is already the step after the one playing
            mode = Mode::loading;
            gateOpen = false;
            break;
        case Mode::stopped:
            mode = Mode::loading;
            position = 0;
            sequence.length = 0;
            break;
    }
}

inline void Sequencer::play() noexcept
{
    if (mode == Mode::loading)
        endLoading();
    else if (mode == Mode::playing)
    {
        mode = Mode::stopped;
        gateOpen = false;
        return;
    }
    if (sequence.length == 0)
        return;
    mode = Mode::playing;
    position = 0;
    gateOpen = false;
}

inline void Sequencer::rest() noexcept
{
    if (mode == Mode::loading)
        store (Sequence::rest);
}

inline void Sequencer::setLegato (bool held) noexcept
{
    legato = held;
}

inline bool Sequencer::keyDown (int key) noexcept
{
    if (mode != Mode::loading)
        return false;
    const auto k = static_cast<uint8_t> (key & 0x7F);
    store (legato && position > 0 ? static_cast<uint8_t> (k | Sequence::tied) : k);
    return true;
}

inline Sequencer::Event Sequencer::clockRise() noexcept
{
    if (mode != Mode::playing || sequence.length == 0)
        return {};
    const uint8_t step = sequence.steps[position];
    position = (position + 1) % sequence.length;
    if (step == Sequence::rest)
    {
        const bool wasOpen = gateOpen;
        gateOpen = false;
        return wasOpen ? Event { Event::Kind::gateOff, 0 } : Event {};
    }
    const int key = step & 0x7F;
    const bool tie = (step & Sequence::tied) && gateOpen;
    gateOpen = true;
    return { tie ? Event::Kind::tied : Event::Kind::note, key };
}

inline Sequencer::Event Sequencer::clockFall() noexcept
{
    if (mode != Mode::playing || ! gateOpen)
        return {};
    const uint8_t next = sequence.steps[position];
    if (next != Sequence::rest && (next & Sequence::tied))
        return {};
    gateOpen = false;
    return { Event::Kind::gateOff, 0 };
}

inline int Sequencer::getPosition() const noexcept
{
    if (mode != Mode::playing || sequence.length == 0)
        return position;
    return (position + sequence.length - 1) % sequence.length + 1;
}

inline void Sequencer::setSequence (const Sequence& newSequence) noexcept
{
    sequence = newSequence;
    if (sequence.length > Sequence::maxSteps)
        sequence.length = Sequence::maxSteps;
    mode = Mode::stopped;
    position = 0;
    gateOpen = false;
}

inline void Sequencer::store (uint8_t step) noexcept
{
    sequence.steps[position++] = step;
    if (position > sequence.length)
        sequence.length = static_cast<uint8_t> (position);
    if (position == Sequence::maxSteps)
        endLoading();
}

inline void Sequencer::endLoading() noexcept
{
    mode = Mode::stopped;
    position = 0;
    ++version;
}

} // namespace picohDSP
