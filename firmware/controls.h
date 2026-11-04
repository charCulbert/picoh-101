// Every input (MIDI, the GUIs) into events for the audio core, and each parameter's value, for a GUI that
// connects later.
#pragma once
#include <cmath>
#include <cstdint>
#include "events.h"
#include "paramKnobs.generated.h"

struct Control
{
    // send queues an event for the audio core; changed tells the GUIs a knob moved.
    void (*send) (const Event&) = nullptr;
    void (*changed) (int id, float value, EventPort) = nullptr;
    void (*wheelMoved) (EventType, float value, EventPort) = nullptr;  // so every GUI's wheels follow
    void (*keyChanged) (int key, bool down, EventPort) = nullptr;      // so every GUI lights the keys
    uint32_t (*nowUs)() = nullptr;

    float values[params::count]; // in the list's units
    uint32_t keysDown[4] = {};   // from any input, one bit each

    bool isDown (int key) const { return keysDown[key / 32] >> (key % 32) & 1; }

    Control()
    {
        for (int id = 0; id < params::count; ++id)
            values[id] = params::table[id].initial;
    }

    // A channel message: status and two data bytes. Any channel.
    void midi (const uint8_t message[3], EventPort port)
    {
        uint8_t type = message[0] & 0xF0, channel = message[0] & 0x0F, a = message[1], b = message[2];
        if (type == 0x90 && b > 0)
            note (a, uint16_t (b * 65535u / 127u), port, channel);
        else if (type == 0x80 || type == 0x90)
            note (a, 0, port, channel);
        else if (type == 0xB0 && (a == 120 || a == 123))
            allNotesOff (port);
        else if (type == 0xB0 && a == 64)
            sustain (b >= 64, port);
        else if (type == 0xB0 && a == 1)
            wheel (EVENT_MOD_WHEEL, b / 127.0f, port);
        else if (type == 0xE0)
        {
            int bend = (b << 7 | a) - 8192; // -8192 to 8191
            wheel (EVENT_PITCH_BEND, bend / (bend < 0 ? 8192.0f : 8191.0f), port);
        }
        else if (type == 0xB0)
        {
            int id = params::fromCc (a);
            if (id >= 0)
                setValue (id, params::toValue (id, b / 127.0f), port);
        }
    }

    // velocity 0-65535; 0 is note off.
    void note (uint8_t key, uint16_t velocity, EventPort port, uint8_t channel = 0)
    {
        Event e = make (velocity ? EVENT_NOTE_ON : EVENT_NOTE_OFF, port);
        e.note.channel = channel;
        e.note.key = key & 0x7F;
        e.note.velocity = velocity;
        stampAndSend (e);
        markKey (e.note.key, velocity != 0, port);
    }

    void sustain (bool down, EventPort port)
    {
        Event e = make (EVENT_SUSTAIN, port);
        e.note.velocity = down ? 65535 : 0;
        stampAndSend (e);
    }

    // EVENT_PITCH_BEND (-1 to 1) or EVENT_MOD_WHEEL (0 to 1).
    void wheel (EventType type, float value, EventPort port)
    {
        Event e = make (type, port);
        e.param.value = value;
        stampAndSend (e);
        if (wheelMoved)
            wheelMoved (type, value, port);
    }

    // EVENT_SEQUENCER: 0 LOAD, 1 PLAY, 2 REST.
    void sequencerButton (int button, EventPort port)
    {
        Event e = make (EVENT_SEQUENCER, port);
        e.param.id = uint16_t (button);
        stampAndSend (e);
    }

    // EVENT_LEGATO or EVENT_KEY_TRANSPOSE.
    void heldButton (EventType type, bool down, EventPort port)
    {
        Event e = make (type, port);
        e.note.velocity = down ? 65535 : 0;
        stampAndSend (e);
    }

    // The sequencer and KEY TRANSPOSE as the audio core last reported them, for a GUI that connects later.
    struct
    {
        int mode = 0, position = 0, length = 0, keyTranspose = 0;
    } sequencer;

    void allNotesOff (EventPort port)
    {
        Event e = make (EVENT_ALL_NOTES_OFF, port);
        stampAndSend (e);
        for (int key = 0; key < 128; ++key)
            markKey (key, false, port);
    }

    // Kept in range; a stepped parameter takes whole values only.
    void setValue (int id, float value, EventPort port)
    {
        if (id < 0 || id >= params::count)
            return;
        const params::Info& p = params::table[id];
        value = value < p.min ? p.min : value > p.max ? p.max : value;
        if (p.stepped)
            value = std::round (value);
        values[id] = value;
        Event e = make (EVENT_PARAM, port);
        e.param.id = uint16_t (id);
        e.param.value = value;
        stampAndSend (e);
        if (changed)
            changed (id, value, port);
    }

private:
    void markKey (int key, bool down, EventPort port)
    {
        if (isDown (key) == down)
            return;
        keysDown[key / 32] ^= 1u << (key % 32);
        if (keyChanged)
            keyChanged (key, down, port);
    }

    static Event make (EventType type, EventPort port)
    {
        Event e {};
        e.type = type;
        e.port = port;
        return e;
    }

    void stampAndSend (Event& e)
    {
        e.timeUs = nowUs ? nowUs() : 0;
        if (send)
            send (e);
    }
};
