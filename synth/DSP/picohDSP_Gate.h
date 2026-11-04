#pragma once

#include <algorithm>

namespace picohDSP
{

// The VCA on GATE: open or closed, with straight ramps just long enough not to click.
struct Gate
{
    static constexpr float openSeconds = 0.001f, closeSeconds = 0.003f;

    void prepare (double sampleRate) noexcept;

    void start() noexcept { step = openStep; }
    void release() noexcept { step = -closeStep; }
    void stop() noexcept;

    bool isActive() const noexcept { return level > 0.0f || step > 0.0f; }

    float next() noexcept;

private:
    float level = 0.0f;
    float step = 0.0f; // added each sample: up while opening, down while closing
    float openStep = 1.0f, closeStep = 1.0f, sampleRate = 0.0f;
};

//=====================================================
//  ╷ ╭┬╮ ╭─╮ ╷   ╭─╴ ╭┬╮ ╭─╴ ╭╮╷ ╶┬╴ ╭─╮ ╶┬╴ ╷ ╭─╮ ╭╮╷
//  │ │││ ├─╯ │   ├─  │││ ├─  │││  │  ├─┤  │  │ │ │ │││
//  ╵ ╵ ╵ ╵   ╰─╴ ╰─╴ ╵ ╵ ╰─╴ ╵╰╯  ╵  ╵ ╵  ╵  ╵ ╰─╯ ╵╰╯
//=====================================================

inline void Gate::prepare (double newSampleRate) noexcept
{
    sampleRate = static_cast<float> (newSampleRate);
    openStep = 1.0f / (openSeconds * sampleRate);
    closeStep = 1.0f / (closeSeconds * sampleRate);
    stop();
}

inline void Gate::stop() noexcept
{
    level = 0.0f;
    step = 0.0f;
}

inline float Gate::next() noexcept
{
    level = std::clamp (level + step, 0.0f, 1.0f);
    if (level <= 0.0f)
        step = 0.0f;
    return level;
}

} // namespace picohDSP
