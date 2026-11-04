#pragma once

#include <algorithm>
#include <type_traits>

namespace picohDSP
{

// A value that moves to a new one in a straight line over some samples.
template <typename SampleType>
struct SmoothedValue
{
    static_assert (std::is_floating_point_v<SampleType>, "SmoothedValue holds a float or a double");

    explicit SmoothedValue (SampleType initial = {}) noexcept
        : value (initial)
        , target (initial)
    {}

    void reset (SampleType value) noexcept;

    // Over this many samples, from where it is now; 0 or 1 jumps on the next sample.
    void setTarget (SampleType target, int frames) noexcept;

    SampleType getNext() noexcept;

    // As that many getNext() calls (to rounding).
    void skip (int frames) noexcept;

    // As getNext() last returned it.
    SampleType getCurrent() const noexcept;

    SampleType getTarget() const noexcept;

private:
    SampleType value {}, target {}, step {}; // step: per sample
    int framesLeft = 0;
};

//=====================================================
//  ╷ ╭┬╮ ╭─╮ ╷   ╭─╴ ╭┬╮ ╭─╴ ╭╮╷ ╶┬╴ ╭─╮ ╶┬╴ ╷ ╭─╮ ╭╮╷
//  │ │││ ├─╯ │   ├─  │││ ├─  │││  │  ├─┤  │  │ │ │ │││
//  ╵ ╵ ╵ ╵   ╰─╴ ╰─╴ ╵ ╵ ╰─╴ ╵╰╯  ╵  ╵ ╵  ╵  ╵ ╰─╯ ╵╰╯
//=====================================================

template <typename SampleType>
void SmoothedValue<SampleType>::reset (SampleType newValue) noexcept
{
    value = target = newValue;
    step = SampleType {};
    framesLeft = 0;
}

template <typename SampleType>
void SmoothedValue<SampleType>::setTarget (SampleType newTarget, int frames) noexcept
{
    target = newTarget;
    framesLeft = std::max (1, frames);
    step = (target - value) / static_cast<SampleType> (framesLeft);
}

// Lands exactly on the target at the end, whatever rounding the steps had.
template <typename SampleType>
SampleType SmoothedValue<SampleType>::getNext() noexcept
{
    if (framesLeft > 0)
        value = --framesLeft == 0 ? target : value + step;

    return value;
}

template <typename SampleType>
void SmoothedValue<SampleType>::skip (int frames) noexcept
{
    if (frames >= framesLeft)
    {
        value = target;
        framesLeft = 0;
        return;
    }

    value += step * static_cast<SampleType> (frames);
    framesLeft -= frames;
}

template <typename SampleType>
SampleType SmoothedValue<SampleType>::getCurrent() const noexcept
{
    return value;
}

template <typename SampleType>
SampleType SmoothedValue<SampleType>::getTarget() const noexcept
{
    return target;
}

} // namespace picohDSP
