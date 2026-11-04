// Voice::renderRun in fixed point (included by picohDSP_Voice.h, inside its namespace): the float one's
// passes, with Q30 ADSR, gate and PWM, Q27 sound.
inline void Voice::renderRun (int32_t* __restrict out, int frames) noexcept
{
    const Settings& s = *settings;
    int32_t sound[tickLength], adsr[tickLength], open[tickLength], width[tickLength];
    for (int i = 0; i < frames; ++i)
    {
        adsr[i] = envelope.next();
        open[i] = gate.next();
    }
    const int32_t* widthMod = nullptr;
    if (s.pwmSource == PwmSource::env)
        widthMod = adsr;
    else if (s.pwmSource == PwmSource::lfo)
    {
        std::fill (width, width + frames, lfoWidth);
        widthMod = width;
    }
    vco.render (sound, frames, widthMod);
    PICOH_TRACE_POINT (oscillatorsDone, frames);

    const int32_t* level = s.vcaGate ? open : adsr;
    for (int i = 0; i < frames; ++i)
        out[i] += fixed::mul (filter.process (sound[i]), level[i], 30);
    PICOH_TRACE_POINT (filterDone, frames);
}
