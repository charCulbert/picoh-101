// The parameters: their CLAP ids, and each one's range and unit. From params/parameters.json, through
// params/generate.py.
#pragma once
#include "paramText.h"
#include <array>
#include <iterator>
#include <stdint.h>

// Each parameter, in the order the host lists them. key is what the state saves the value under: never
// change one once released. name can change freely. A stepped parameter takes whole values only.
struct Param
{
    uint32_t id;
    const char* key;
    const char* name;
    double min, max, defaultValue;
    bool stepped;
    ParamUnit unit;
};

#include "params.generated.h"
static_assert (std::size (paramTable) == P_COUNT, "a row for each id");

inline constexpr auto paramRows = []
{
    std::array<uint32_t, P_COUNT> rows {};
    for (uint32_t row = 0; row < P_COUNT; row++)
        rows[paramTable[row].id] = row;
    return rows;
}();
static_assert (
    []
    {
        for (uint32_t id = 0; id < P_COUNT; id++)
            if (paramTable[paramRows[id]].id != id)
                return false;
        return true;
    }(),
    "each id in exactly one row");

inline const Param& paramById (uint32_t id)
{
    return paramTable[paramRows[id]];
}
