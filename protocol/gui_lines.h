// The lines a GUI and the synth speak, one per message, over any transport: the Pico's Wi-Fi, USB serial
// and SysEx, and the plugin's webview. No dependencies, so the Pico, the plugin and the desktop checks
// all build it.
//
// GUI to synth:
//   hello                   answered with every value (v), and the keys sounding (k)
//   v <id> <value>          set a parameter, in its list's units
//   g <id> <1|0>            a drag on a parameter begins or ends
//   n <key> <velocity 0-1>  note on; velocity 0 is note off
//   a                       let go of every note this GUI holds
//   b <-1 to 1>             pitch wheel
//   m <0 to 1>              mod wheel
//   s <load|play|rest>      the sequencer's buttons (REST: KEY TRANSPOSE's button while loading)
//   l <1|0>                 LEGATO (HOLD's button while loading) down or up
//   t <1|0>                 KEY TRANSPOSE down or up
//   p <load|store>          (Pico) the patch buttons: arms one, or disarms it if armed. The next key from
//                           41 to 72 (F to C), from any input, loads or stores that key's patch
//   ping                    (USB MIDI) still here; no answer. A SysEx GUI silent for 5 s is gone.
//   page                    (Pico, USB MIDI) answered with page <hash> <size>: the page's files, for a GUI
//                           that carries none (the app). Over Wi-Fi it's GET /page, with If-None-Match.
//   page <offset>           (Pico, USB MIDI) the page from offset to its end, in pageChunk messages. Any other
//                           line stops it.
//
// Synth to GUI:
//   v <id> <value>          a parameter's value
//   k <key> <1|0>           a key sounding or not, from any source
//   keys <32 hex digits>    (Pico) every key down, once a second, so a lost k line can't leave one lit:
//                           four 8-digit words, key 32w+n is bit n of word w
//   b <-1 to 1>, m <0 to 1> the wheels
//   s <stopped|loading|playing> <position> <length>   the sequencer, when any of them changes
//   t <semitones>           KEY TRANSPOSE (0: off, its light out)
//   p <off|load|store> <key>   (Pico) the patch button armed, if any, and the key of the patch last
//                           loaded or stored (0: none), when either changes
//   load <avg%> <busiest%> <underruns> <loop µs> <late> <latest µs>   (Pico) once a second: the audio
//                           core's load, core 0's longest loop pass, events applied late and how late the latest was
//   page <hash> <size>      (Pico) the page: 16 hex digits of its FNV-1a 64 hash, and its size in bytes
//   # <text>                (USB serial) a log line for people; GUIs ignore it
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace guiLines
{
struct Line
{
    enum class Kind
    {
        unknown,
        hello,
        value,
        gesture,
        note,
        allNotesOff,
        bend,
        mod,
        sequencer, // id: 0 load, 1 play, 2 rest
        legato,    // value: 1 down, 0 up
        keyTranspose,
        patch, // id: 0 load, 1 store
        ping,
        page // id: the offset to send from, or -1 for the hash and size
    };
    Kind kind = Kind::unknown;
    int id = -1;        // value, gesture: the parameter; note: the key, 0-127; sequencer: the button
    float value = 0.0f; // value: in the list's units; gesture: 1 begins, 0 ends; note: velocity; bend, mod
};

inline float clamp (float x, float lo, float hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}

// On the bits, through a volatile: with -ffast-math (the firmware's) a compiler may take a float for
// finite and drop a check on it.
inline bool finite (float x)
{
    uint32_t bits;
    std::memcpy (&bits, &x, sizeof bits);
    const volatile uint32_t seen = bits;
    return (seen & 0x7F800000u) != 0x7F800000u;
}

// A parameter id is checked only for being 0 to 65535: the caller knows how many there are. Values that
// aren't finite (nan, inf) make the line unknown.
inline Line parse (const char* text)
{
    Line line;
    if (std::strcmp (text, "hello") == 0)
        line.kind = Line::Kind::hello;
    else if (std::strcmp (text, "ping") == 0)
        line.kind = Line::Kind::ping;
    else if (std::strcmp (text, "a") == 0)
        line.kind = Line::Kind::allNotesOff;
    else if (std::strncmp (text, "page", 4) == 0 && (text[4] == 0 || text[4] == ' '))
    {
        char* rest = nullptr;
        const long offset = text[4] ? std::strtol (text + 5, &rest, 10) : -1;
        if (text[4] && (rest == text + 5 || *rest || offset < 0 || offset > 0x1FFFFF))
            return line;
        line.kind = Line::Kind::page;
        line.id = int (offset);
    }
    else if (text[0] && text[1] == ' ')
    {
        char* rest = nullptr;
        switch (text[0])
        {
            case 'v':
            case 'g':
            case 'n':
            {
                const long n = std::strtol (text + 2, &rest, 10);
                if (rest == text + 2 || n < 0 || n > 0xFFFF)
                    return line;
                line.id = int (n);
                line.value = std::strtof (rest, nullptr);
                if (! finite (line.value))
                    return Line {};
                line.kind = text[0] == 'v'   ? Line::Kind::value
                            : text[0] == 'g' ? Line::Kind::gesture
                                             : Line::Kind::note;
                if (line.kind == Line::Kind::note)
                {
                    line.id &= 0x7F;
                    line.value = clamp (line.value, 0.0f, 1.0f);
                }
                break;
            }
            case 'b':
            case 'm':
            {
                const float value = std::strtof (text + 2, nullptr);
                if (! finite (value))
                    return line;
                line.kind = text[0] == 'b' ? Line::Kind::bend : Line::Kind::mod;
                line.value = clamp (value, text[0] == 'b' ? -1.0f : 0.0f, 1.0f);
                break;
            }
            case 's':
            {
                static constexpr const char* buttons[] = { "load", "play", "rest" };
                for (int i = 0; i < 3; ++i)
                    if (std::strcmp (text + 2, buttons[i]) == 0)
                    {
                        line.kind = Line::Kind::sequencer;
                        line.id = i;
                    }
                break;
            }
            case 'p':
                if (std::strcmp (text + 2, "load") == 0 || std::strcmp (text + 2, "store") == 0)
                {
                    line.kind = Line::Kind::patch;
                    line.id = text[2] == 's';
                }
                break;
            case 'l':
            case 't':
                if ((text[2] == '0' || text[2] == '1') && text[3] == 0)
                {
                    line.kind = text[0] == 'l' ? Line::Kind::legato : Line::Kind::keyTranspose;
                    line.value = text[2] == '1' ? 1.0f : 0.0f;
                }
                break;
        }
    }
    return line;
}

inline void pageLine (char* out, size_t size, uint64_t hash, uint32_t bytes)
{
    std::snprintf (out, size, "page %08lx%08lx %lu", (unsigned long) (hash >> 32), (unsigned long) (hash & 0xFFFFFFFFu),
                   (unsigned long) bytes);
}

// The page's bytes over USB MIDI, where a SysEx byte has 7 bits: F0 7D 50 50 <offset: 3 bytes, high first>
// <data> F7, the data in groups of up to 7 bytes, each group led by a byte holding their top bits (bit i:
// byte i's). out needs pageChunkSize (n) bytes; returns that.
constexpr uint8_t pageChunkId[] = { 0xF0, 0x7D, 0x50, 0x50 };
constexpr size_t pageChunkSize (size_t n) { return sizeof pageChunkId + 3 + n + (n + 6) / 7 + 1; }

inline size_t pageChunk (uint8_t* out, uint32_t offset, const uint8_t* data, size_t n)
{
    uint8_t* at = out;
    for (uint8_t b : pageChunkId)
        *at++ = b;
    *at++ = uint8_t (offset >> 14 & 0x7F);
    *at++ = uint8_t (offset >> 7 & 0x7F);
    *at++ = uint8_t (offset & 0x7F);
    for (size_t i = 0; i < n; i += 7)
    {
        uint8_t& high = *at++;
        high = 0;
        for (size_t j = 0; j < 7 && i + j < n; ++j)
        {
            high |= uint8_t (data[i + j] >> 7 << j);
            *at++ = data[i + j] & 0x7F;
        }
    }
    *at++ = 0xF7;
    return size_t (at - out);
}

inline void valueLine (char* out, size_t size, int id, float value)
{
    std::snprintf (out, size, "v %d %.7g", id, double (value));
}

inline void keyLine (char* out, size_t size, int key, bool down)
{
    std::snprintf (out, size, "k %d %d", key, down ? 1 : 0);
}

inline void keysLine (char* out, size_t size, const uint32_t down[4])
{
    std::snprintf (out, size, "keys %08lx%08lx%08lx%08lx", (unsigned long) down[0], (unsigned long) down[1],
                   (unsigned long) down[2], (unsigned long) down[3]);
}

// mode: 0 stopped, 1 loading, 2 playing.
inline void sequencerLine (char* out, size_t size, int mode, int position, int length)
{
    static constexpr const char* modes[] = { "stopped", "loading", "playing" };
    std::snprintf (out, size, "s %s %d %d", modes[mode < 0 || mode > 2 ? 0 : mode], position, length);
}

inline void keyTransposeLine (char* out, size_t size, int semitones)
{
    std::snprintf (out, size, "t %d", semitones);
}

// armed: 0 off, 1 load, 2 store.
inline void patchLine (char* out, size_t size, int armed, int key)
{
    static constexpr const char* states[] = { "off", "load", "store" };
    std::snprintf (out, size, "p %s %d", states[armed < 0 || armed > 2 ? 0 : armed], key);
}

// kind: 'b' (pitch) or 'm' (mod).
inline void wheelLine (char* out, size_t size, char kind, float value)
{
    std::snprintf (out, size, "%c %.4f", kind, double (value));
}
} // namespace guiLines
