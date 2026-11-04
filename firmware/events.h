// Every note and parameter change from any input is one Event, stamped when it arrived. Modelled on
// CLAP's events: the control core is the host, the audio core the plugin.
#pragma once
#include <cstdint>

enum EventType : uint8_t
{
    EVENT_NOTE_ON = 1,
    EVENT_NOTE_OFF = 2,
    EVENT_PARAM = 3,
    EVENT_SUSTAIN = 4,       // note.velocity: 0 up, anything else down
    EVENT_ALL_NOTES_OFF = 5,
    EVENT_PITCH_BEND = 6,    // param.value: -1 (down) to 1 (up), 0 centre
    EVENT_MOD_WHEEL = 7,     // param.value: 0 to 1
    EVENT_SEQUENCER = 8,     // param.id: 0 LOAD, 1 PLAY, 2 REST
    EVENT_LEGATO = 9,        // note.velocity: 0 up, anything else down
    EVENT_KEY_TRANSPOSE = 10, // the same
};

// Where an event came from, so the same key from two inputs can't collide.
enum EventPort : uint8_t
{
    EVENT_PORT_USB = 0,
    EVENT_PORT_DIN = 1,
    EVENT_PORT_WEB = 2,  // the GUI over Wi-Fi
    EVENT_PORT_SERIAL = 3,  // the GUI over the USB serial port
    EVENT_PORT_SYSEX = 4,  // the GUI over SysEx on the USB MIDI port
};

constexpr int eventQueueCapacity = 128; // core 0 to core 1

struct Event
{
    uint32_t timeUs; // microsecond clock both cores share, when it arrived. Wraps every
                     // 71.6 minutes: compare times by subtracting as uint32_t.
    uint8_t type;    // EventType
    uint8_t port;    // EventPort
    union
    {
        struct
        {
            uint8_t channel;   // 0-15
            uint8_t key;       // MIDI note number 0-127
            uint16_t velocity; // 0-65535, 65535 = fastest
        } note;
        struct
        {
            uint16_t id;  // from params/parameters.json
            float value; // in the parameter's own units (Hz, cents, %)
        } param;
    };
};
