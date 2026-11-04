// How the parameters' values read as text, for hosts. web/live.js has the same rules. The named values
// (switches) are in paramChoices.generated.h.
#pragma once
#include <ctype.h>
#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "paramChoices.generated.h"

// A percent's value with up to two decimals: "12.5%", "50%". A bipolar one, whose
// range goes below 0, shows its sign: "+25%".
inline void ParamPercentToText (double value, bool bipolar, char* display, uint32_t size)
{
    const double shown = round (value * 10000) / 100 + 0.0; // no "-0"
    char number[32];
    snprintf (number, sizeof (number), "%.2f", shown);
    char* last = number + strlen (number) - 1;
    while (*last == '0')
        *last-- = 0;
    if (*last == '.' || *last == ',')
        *last = 0;
    snprintf (display, size, bipolar && shown > 0 ? "+%s%%" : "%s%%", number);
}

inline bool ParamValueToText (ParamUnit unit, double value, bool bipolar, char* display, uint32_t size)
{
    int count = 0;
    if (const char* const* names = ParamChoices (unit, &count))
    {
        snprintf (display, size, "%s", names[(int) fmin (fmax (value, 0.0), count - 1.0)]); // NaN: the first
        return true;
    }
    switch (unit)
    {
        case UNIT_SECONDS:
            snprintf (display, size, value < 1 ? "%.0f ms" : "%.2f s", value < 1 ? value * 1000 : value);
            return true;
        case UNIT_PERCENT: ParamPercentToText (value, bipolar, display, size); return true;
        case UNIT_NUMBER: snprintf (display, size, "%.2f", value); return true;
        case UNIT_CENTS: snprintf (display, size, "%+.0f ct", value); return true;
        case UNIT_HERTZ: // "0.50 Hz" (an LFO), "440 Hz", "1.20 kHz"
            snprintf (display, size,
                      value < 10     ? "%.2f Hz"
                      : value < 1000 ? "%.0f Hz"
                                     : "%.2f kHz",
                      value < 1000 ? value : value / 1000);
            return true;
        default: return false;
    }
}

// What a typed number counts in, as the value times this: a bare number, in
// the smaller unit shown ("300" is 300 ms, "75" is 75%).
inline double ParamTypedScale (ParamUnit unit)
{
    return unit == UNIT_SECONDS ? 1000 : unit == UNIT_PERCENT ? 100 : 1;
}

// A number at the start of text, with '.' or ',' as its decimal point
// whatever locale the host set (strtod alone reads only the locale's).
inline double ParamReadNumber (const char* text, char** end)
{
    char copy[64];
    snprintf (copy, sizeof (copy), "%s", text);
    for (char* c = copy; *c; c++)
        if (*c == '.' || *c == ',')
            *c = *localeconv()->decimal_point;
    char* stop = nullptr;
    const double number = strtod (copy, &stop);
    *end = (char*) text + (stop - copy);
    return number;
}

// Which of names the text is: the whole name, whatever the case, or its start
// from two letters on ("tri", "GA"). Nothing else, so "Not a value" is none.
inline bool ParamReadChoice (const char* const* names, int count, const char* display, double* value)
{
    const size_t length = strlen (display);
    for (int i = 0; length >= 1 && i < count; i++) // a whole name, even "L" or "8'"
        if (strlen (names[i]) == length)
        {
            size_t same = 0;
            while (same < length && tolower ((unsigned char) display[same]) == tolower ((unsigned char) names[i][same]))
                same++;
            if (same == length)
                return *value = i, true;
        }
    for (int i = 0; length >= 2 && i < count; i++)
    {
        size_t same = 0;
        while (same < length && names[i][same] &&
               tolower ((unsigned char) display[same]) == tolower ((unsigned char) names[i][same]))
            same++;
        if (same == length)
            return *value = i, true;
    }
    return false;
}

inline bool ParamTextToValue (ParamUnit unit, const char* display, double* value)
{
    int count = 0;
    if (const char* const* names = ParamChoices (unit, &count))
    {
        if (ParamReadChoice (names, count, display, value))
            return true;
        char* end = nullptr; // or the value's number: "2"
        const double number = ParamReadNumber (display, &end);
        if (end == display || *end)
            return false;
        *value = fmin (fmax (trunc (number), 0.0), count - 1.0);
        return true;
    }
    char* end = nullptr;
    const double number = ParamReadNumber (display, &end); // "300 ms" reads 300
    const bool read = end != display;
    while (*end == ' ')
        end++;
    if (read && ! *end)
    { // a bare number, as typed
        *value = number / ParamTypedScale (unit);
        return true;
    }
    switch (unit)
    { // else the text value_to_text wrote, as a host may read it
        case UNIT_SECONDS: // "300 ms", "1.20 s"
            *value = strcmp (end, "ms") == 0 ? number / 1000 : number;
            return read && (strcmp (end, "ms") == 0 || strcmp (end, "s") == 0);
        case UNIT_PERCENT: // "85%"
            *value = number / 100;
            return read && strcmp (end, "%") == 0;
        case UNIT_CENTS: // "+700 ct"
            *value = number;
            return read && strcmp (end, "ct") == 0;
        case UNIT_HERTZ: // "1.20 kHz", "440 Hz"
            *value = strcmp (end, "kHz") == 0 ? number * 1000 : number;
            return read && (strcmp (end, "kHz") == 0 || strcmp (end, "Hz") == 0);
        default: return false;
    }
}
