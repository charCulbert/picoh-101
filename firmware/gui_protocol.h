// The GUIs connected to the Pico, over any transport (wifi.cpp, usb.cpp), speaking ../protocol/gui_lines.h.
// No hardware here, so the desktop checks test it.
#pragma once
#include "controls.h"
#include "gui_lines.h"

struct Gui
{
    EventPort port;
    uint32_t held[4] = {}; // keys this GUI holds down, one bit each
};

struct Guis
{
    Control* control = nullptr;
    // Sends a line to every GUI but skip, the one that made the change and already shows it (nullptr: all).
    void (*broadcast) (const char* line, const Gui* skip) = nullptr;
    // The GUI whose line is being handled: it already shows its own change.
    const Gui* sender = nullptr;

    static bool fromGui (EventPort port)
    {
        return port == EVENT_PORT_WEB || port == EVENT_PORT_SERIAL || port == EVENT_PORT_SYSEX;
    }

    void receive (Gui& from, const char* text)
    {
        using Kind = guiLines::Line::Kind;
        const guiLines::Line line = guiLines::parse (text);
        sender = &from;
        switch (line.kind)
        {
            case Kind::value: control->setValue (line.id, line.value, from.port); break;
            case Kind::note:
                if (line.value > 0)
                    from.held[line.id / 32] |= 1u << (line.id % 32);
                else
                    from.held[line.id / 32] &= ~(1u << (line.id % 32));
                control->note (uint8_t (line.id), uint16_t (line.value * 65535.0f), from.port);
                break;
            case Kind::allNotesOff: release (from); break; // this GUI's notes only
            case Kind::bend: control->wheel (EVENT_PITCH_BEND, line.value, from.port); break;
            case Kind::mod: control->wheel (EVENT_MOD_WHEEL, line.value, from.port); break;
            case Kind::sequencer: control->sequencerButton (line.id, from.port); break;
            case Kind::legato: control->heldButton (EVENT_LEGATO, line.value > 0, from.port); break;
            case Kind::keyTranspose: control->heldButton (EVENT_KEY_TRANSPOSE, line.value > 0, from.port); break;
            default: break; // gestures are the plugin's; hello and ping are the transport's
        }
        sender = nullptr;
    }

    // A GUI that goes away lets go of its notes.
    void release (Gui& gui)
    {
        for (int key = 0; key < 128; ++key)
            if (gui.held[key / 32] >> (key % 32) & 1)
                control->note (uint8_t (key), 0, gui.port);
        for (uint32_t& word : gui.held)
            word = 0;
    }

    template <typename Send>
    void sendState (Send send) const
    {
        char line[40];
        for (int id = 0; id < params::count; ++id)
        {
            guiLines::valueLine (line, sizeof line, id, control->values[id]);
            send (line);
        }
        for (int key = 0; key < 128; ++key)
            if (control->isDown (key))
            {
                guiLines::keyLine (line, sizeof line, key, true);
                send (line);
            }
        const auto& s = control->sequencer;
        guiLines::sequencerLine (line, sizeof line, s.mode, s.position, s.length);
        send (line);
        guiLines::keyTransposeLine (line, sizeof line, s.keyTranspose);
        send (line);
    }
};
