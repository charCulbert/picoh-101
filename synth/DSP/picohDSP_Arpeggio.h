#pragma once

namespace picohDSP
{

// The arpeggio: the chord's keys in pitch order, one per clock step. It runs with two keys or more; one
// key plays normally. With hold, keys stay in the chord after they go up, and a key pressed once every
// key is up starts a new chord.
struct Arpeggio
{
    enum class Mode // P_ARPEGGIO's order
    {
        off,
        down,
        upDown, // C E G E C E G E: the ends once each
        up
    };

    static constexpr int capacity = 16;

    void setMode (Mode) noexcept;
    void setHold (bool) noexcept;

    void keyDown (int key) noexcept;
    void keyUp (int key) noexcept;
    void clear() noexcept;

    bool isRunning() const noexcept;
    void restart() noexcept; // the next step is the first: the lowest key, or the highest on DOWN
    int step() noexcept;     // the key for this clock step; a changed chord carries on from where it was

private:
    struct Key
    {
        int key;
        bool down;
    };
    Key chord[capacity] = {}; // in pitch order
    int count = 0, position = 0, direction = 1;
    Mode mode = Mode::off;
    bool hold = false;

    int find (int key) const noexcept;
    void remove (int index) noexcept;
    void removeUp() noexcept;
};

//=====================================================
//  ╷ ╭┬╮ ╭─╮ ╷   ╭─╴ ╭┬╮ ╭─╴ ╭╮╷ ╶┬╴ ╭─╮ ╶┬╴ ╷ ╭─╮ ╭╮╷
//  │ │││ ├─╯ │   ├─  │││ ├─  │││  │  ├─┤  │  │ │ │ │││
//  ╵ ╵ ╵ ╵   ╰─╴ ╰─╴ ╵ ╵ ╰─╴ ╵╰╯  ╵  ╵ ╵  ╵  ╵ ╰─╯ ╵╰╯
//=====================================================

inline void Arpeggio::setMode (Mode newMode) noexcept
{
    if (newMode != mode)
    {
        mode = newMode;
        restart();
    }
}

inline void Arpeggio::setHold (bool newHold) noexcept
{
    hold = newHold;
    if (! hold)
        removeUp();
}

inline void Arpeggio::keyDown (int key) noexcept
{
    bool anyDown = false;
    for (int i = 0; i < count; ++i)
        anyDown = anyDown || chord[i].down;
    if (! anyDown)
        count = 0;

    if (const int i = find (key); i >= 0)
    {
        chord[i].down = true;
        return;
    }
    if (count == capacity)
        return;
    int i = count++;
    for (; i > 0 && chord[i - 1].key > key; --i)
        chord[i] = chord[i - 1];
    chord[i] = { key, true };
    if (count > 1 && i <= position)
        ++position; // still on the key it was on
}

inline void Arpeggio::keyUp (int key) noexcept
{
    if (const int i = find (key); i >= 0)
    {
        chord[i].down = false;
        if (! hold)
            remove (i);
    }
}

inline void Arpeggio::clear() noexcept
{
    count = 0;
    restart();
}

inline bool Arpeggio::isRunning() const noexcept
{
    return mode != Mode::off && count >= 2;
}

inline void Arpeggio::restart() noexcept
{
    direction = mode == Mode::down ? -1 : 1;
    position = mode == Mode::down ? count - 1 : 0;
}

inline int Arpeggio::step() noexcept
{
    if (count == 0)
        return 0;
    position = position < 0 ? 0 : position >= count ? count - 1 : position;
    const int key = chord[position].key;
    if (mode == Mode::upDown)
    {
        position += direction;
        if (position >= count || position < 0)
        {
            direction = -direction;
            position += 2 * direction;
        }
    }
    else
        position = (position + direction + count) % count;
    return key;
}

inline int Arpeggio::find (int key) const noexcept
{
    for (int i = 0; i < count; ++i)
        if (chord[i].key == key)
            return i;
    return -1;
}

inline void Arpeggio::remove (int index) noexcept
{
    for (int i = index; i < count - 1; ++i)
        chord[i] = chord[i + 1];
    --count;
    if (index < position)
        --position;
}

inline void Arpeggio::removeUp() noexcept
{
    for (int i = count - 1; i >= 0; --i)
        if (! chord[i].down)
            remove (i);
}

} // namespace picohDSP
