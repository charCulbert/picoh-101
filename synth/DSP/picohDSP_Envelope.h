#pragma once

#include <algorithm>
#include <cmath>

namespace picohDSP
{

// An analog-style ADSR, 0 to 1: each stage charges toward a point just past its target (by overshoot) and
// stops there. A full stage takes its set time; one that starts part way arrives sooner, as in hardware.
// The times and sustain live in Settings, so a knob works out its exp and log once, not per sample.
struct Envelope
{
    // How far past each target the stages aim, in multiples of the full 0-1 range: about 0.04 is
    // curved, 10 nearly straight.
    static constexpr float overshoot = 0.5f;

    struct Settings
    {
        void prepare (double sampleRate) noexcept;

        // Then settingsChanged() on each envelope.
        void setAttack (float seconds) noexcept;
        void setDecay (float seconds) noexcept;
        void setSustain (float level) noexcept; // 0 to 1
        void setRelease (float seconds) noexcept;

        float getAttack() const noexcept { return attackSeconds; }

    private:
        friend struct Envelope;

        float sampleRate = 0.0f;
        float attackSeconds = 0.005f, decaySeconds = 0.1f, sustainLevel = 1.0f, releaseSeconds = 0.2f;

        // Each stage's fraction of the way to its aim, per sample.
        float attackCoefficient = 1.0f, decayCoefficient = 1.0f, releaseCoefficient = 1.0f;

        float coefficientFor (float seconds) const noexcept;
    };

    void prepare (double sampleRate) noexcept;

    // Must be set before start().
    void setSettings (const Settings&) noexcept;

    // Takes up changed settings in the running stage.
    void settingsChanged() noexcept;

    float getAttack() const noexcept { return settings->getAttack(); }

    void start() noexcept; // the attack, from the current level
    void release() noexcept;
    void stop() noexcept;

    bool isActive() const noexcept;

    float next() noexcept;

private:
    enum class Stage
    {
        idle,
        attack,
        decay,
        sustain,
        release
    };

    Stage stage = Stage::idle;
    float level = 0.0f;
    float sampleRate = 0.0f; // stays idle until prepare()
    const Settings* settings = nullptr;

    float aim = 0.0f;         // where the running stage is heading
    float coefficient = 0.0f; // the fraction of the way to aim covered per sample

    void enter (Stage) noexcept;
    void refresh() noexcept;
};

//=====================================================
//  ╷ ╭┬╮ ╭─╮ ╷   ╭─╴ ╭┬╮ ╭─╴ ╭╮╷ ╶┬╴ ╭─╮ ╶┬╴ ╷ ╭─╮ ╭╮╷
//  │ │││ ├─╯ │   ├─  │││ ├─  │││  │  ├─┤  │  │ │ │ │││
//  ╵ ╵ ╵ ╵   ╰─╴ ╰─╴ ╵ ╵ ╰─╴ ╵╰╯  ╵  ╵ ╵  ╵  ╵ ╰─╯ ╵╰╯
//=====================================================

inline void Envelope::Settings::prepare (double newSampleRate) noexcept
{
    sampleRate = static_cast<float> (newSampleRate);
    attackCoefficient = coefficientFor (attackSeconds);
    decayCoefficient = coefficientFor (decaySeconds);
    releaseCoefficient = coefficientFor (releaseSeconds);
}

inline void Envelope::Settings::setAttack (float seconds) noexcept
{
    attackSeconds = std::max (0.0f, seconds);
    attackCoefficient = coefficientFor (attackSeconds);
}

inline void Envelope::Settings::setDecay (float seconds) noexcept
{
    decaySeconds = std::max (0.0f, seconds);
    decayCoefficient = coefficientFor (decaySeconds);
}

inline void Envelope::Settings::setSustain (float level) noexcept
{
    sustainLevel = std::clamp (level, 0.0f, 1.0f);
}

inline void Envelope::Settings::setRelease (float seconds) noexcept
{
    releaseSeconds = std::max (0.0f, seconds);
    releaseCoefficient = coefficientFor (releaseSeconds);
}

// Charging from 0 toward 1 + overshoot reaches 1 after ln ((1 + overshoot) / overshoot) time constants.
inline float Envelope::Settings::coefficientFor (float seconds) const noexcept
{
    if (seconds <= 0.0f || sampleRate <= 0.0f)
        return 1.0f;

    return 1.0f - std::exp (-std::log ((1.0f + overshoot) / overshoot) / (seconds * sampleRate));
}

inline void Envelope::prepare (double newSampleRate) noexcept
{
    sampleRate = static_cast<float> (newSampleRate);
    stop();
}

inline void Envelope::setSettings (const Settings& newSettings) noexcept
{
    settings = &newSettings;
}

inline void Envelope::settingsChanged() noexcept
{
    // Decay only heads down: a sustain raised above the level glides up in the sustain stage.
    if (stage == Stage::decay && level < settings->sustainLevel)
        stage = Stage::sustain;

    refresh();
}

inline void Envelope::start() noexcept
{
    if (sampleRate <= 0.0f)
        return;

    enter (Stage::attack);
}

inline void Envelope::release() noexcept
{
    if (stage != Stage::idle)
        enter (Stage::release);
}

inline void Envelope::stop() noexcept
{
    stage = Stage::idle;
    level = 0.0f;
}

inline bool Envelope::isActive() const noexcept
{
    return stage != Stage::idle;
}

inline float Envelope::next() noexcept
{
    switch (stage)
    {
        case Stage::idle: break;

        case Stage::attack:
            level += (aim - level) * coefficient;

            if (level >= 1.0f)
            {
                level = 1.0f;
                enter (Stage::decay);
            }
            break;

        case Stage::decay:
            level += (aim - level) * coefficient;

            if (level <= settings->sustainLevel)
            {
                level = settings->sustainLevel;
                stage = Stage::sustain;
            }
            break;

        case Stage::sustain: level += (settings->sustainLevel - level) * coefficient; break;

        case Stage::release:
            level += (aim - level) * coefficient;

            if (level <= 0.0f)
                stop();
            break;

    }

    return level;
}

inline void Envelope::enter (Stage newStage) noexcept
{
    stage = newStage;
    refresh();
}

// Idle (as before prepare()) reads nothing, so settings can change before setSettings().
inline void Envelope::refresh() noexcept
{
    switch (stage)
    {
        case Stage::attack:
            aim = 1.0f + overshoot;
            coefficient = settings->attackCoefficient;
            break;

        case Stage::decay:
            aim = settings->sustainLevel - overshoot * (1.0f - settings->sustainLevel);
            coefficient = settings->decayCoefficient;
            break;

        case Stage::sustain: coefficient = settings->decayCoefficient; break;

        case Stage::release:
            aim = -overshoot;
            coefficient = settings->releaseCoefficient;
            break;

        case Stage::idle: break;
    }
}

} // namespace picohDSP
