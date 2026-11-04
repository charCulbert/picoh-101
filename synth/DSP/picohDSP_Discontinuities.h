#pragma once

#include <array>
#include <cassert>

namespace picohDSP
{

// A jump in a wave: where in the sample it falls, and by how much.
struct Discontinuity
{
    float time;   // 0 to 1, from the last sample to this one
    float height;
};

// The jumps one oscillator crossed in its last sample.
struct Discontinuities
{
    static constexpr int capacity = 4; // a pulse makes at most 3 a sample: fall, wrap, fall

    void addStep (float time, float height) noexcept;
    void clear() noexcept { count = 0; }

    const Discontinuity* begin() const noexcept { return held.data(); }
    const Discontinuity* end() const noexcept { return held.data() + count; }

private:
    std::array<Discontinuity, capacity> held {};
    int count = 0;
};

inline void Discontinuities::addStep (float time, float height) noexcept
{
    assert (count < capacity);
    if (count < capacity)
        held[static_cast<size_t> (count++)] = { time, height };
}

} // namespace picohDSP
