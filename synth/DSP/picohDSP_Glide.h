#pragma once

#include <algorithm>
#include <cmath>

namespace picohDSP
{

// Portamento: the key moves from where it is to a new one over a set time, on a curve like an RC lag's
// that lands exactly at the end.
struct Glide
{
    static constexpr float shape = 1.0f;         // 0 a straight line, 1 the RC-like curve
    static constexpr float timePerOctave = 0.0f; // 0: the same time for any interval, 1: the time per octave

    void prepare (double sampleRate) noexcept;
    void setTime (float seconds) noexcept;

    void jump (float key) noexcept;   // there at once
    bool moveTo (float key) noexcept; // from where it is; false if it jumped (no key before, or no time)

    float next (int frames) noexcept; // the key after this many samples

private:
    float key = 60.0f, from = 60.0f, to = 60.0f;
    float position = 1.0f, step = 1.0f; // 0 to 1 through the glide, and per sample
    float seconds = 0.05f, sampleRate = 0.0f;
    bool hasKey = false;
};

//=====================================================
//  ╷ ╭┬╮ ╭─╮ ╷   ╭─╴ ╭┬╮ ╭─╴ ╭╮╷ ╶┬╴ ╭─╮ ╶┬╴ ╷ ╭─╮ ╭╮╷
//  │ │││ ├─╯ │   ├─  │││ ├─  │││  │  ├─┤  │  │ │ │ │││
//  ╵ ╵ ╵ ╵   ╰─╴ ╰─╴ ╵ ╵ ╰─╴ ╵╰╯  ╵  ╵ ╵  ╵  ╵ ╰─╯ ╵╰╯
//=====================================================

inline void Glide::prepare (double newSampleRate) noexcept
{
    sampleRate = static_cast<float> (newSampleRate);
    hasKey = false;
    position = step = 1.0f;
}

inline void Glide::setTime (float newSeconds) noexcept
{
    seconds = std::max (0.0f, newSeconds);
}

inline void Glide::jump (float newKey) noexcept
{
    key = from = to = newKey;
    position = step = 1.0f;
    hasKey = true;
}

inline bool Glide::moveTo (float newKey) noexcept
{
    if (! hasKey || seconds <= 0.0f)
    {
        jump (newKey);
        return false;
    }

    from = key;
    to = newKey;
    const float octaves = std::abs (to - from) / 12.0f;
    const float length = seconds * ((1.0f - timePerOctave) + timePerOctave * octaves) * sampleRate;
    position = 0.0f;
    step = 1.0f / std::max (1.0f, length);
    return true;
}

// The curve: a straight line bent toward an RC lag's 1 - e^(-kt), scaled to land on 1 at t = 1.
inline float Glide::next (int frames) noexcept
{
    if (position >= 1.0f)
        return key;

    position = std::min (1.0f, position + step * static_cast<float> (frames));
    constexpr float strength = 1.0f + shape * 5.0f;
    const float rc = (1.0f - std::exp (-strength * position)) / (1.0f - std::exp (-strength));
    key = from + (to - from) * (position + (rc - position) * shape);
    return key;
}

} // namespace picohDSP
