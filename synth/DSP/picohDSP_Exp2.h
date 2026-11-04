#pragma once
#include <cstdint>
#include <cstring>

namespace picohDSP
{
// 2^x for |x| under 126, within 2 float steps of exact (under 0.0002 cents as a pitch). std::exp2 on the Picos
// goes through a double exp, done in software; this stays in float: the nearest whole power from the exponent
// bits, a polynomial for the rest.
inline float exp2 (float x) noexcept
{
    const int whole = static_cast<int> (x < 0.0f ? x - 0.5f : x + 0.5f);
    const float f = x - static_cast<float> (whole); // -0.5 to 0.5
    float p = 1.535336188e-4f;
    p = p * f + 1.339887440e-3f;
    p = p * f + 9.618437358e-3f;
    p = p * f + 5.550332471e-2f;
    p = p * f + 2.402264791e-1f;
    p = p * f + 6.931472029e-1f;
    p = p * f + 1.0f;
    const uint32_t bits = static_cast<uint32_t> (whole + 127) << 23;
    float power;
    std::memcpy (&power, &bits, sizeof power);
    return p * power;
}
} // namespace picohDSP
