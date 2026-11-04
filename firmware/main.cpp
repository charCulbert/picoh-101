// Core 0: USB (MIDI, and the GUI over serial or SysEx), DIN MIDI and Wi-Fi in; events out to the audio
// core through one queue.
#include <cstdio>
#include "board.h"
#include "params.h"
#include "flash_saves.h"
#include "pico/stdlib.h"
#include "trace.h"

namespace
{
queue_t events;
Control control;
Guis guis;
bool haveWeb = false;
uint32_t droppedEvents = 0;

void send (const Event& e)
{
    if (! queue_try_add (&events, &e))
        ++droppedEvents;
}

// To every connected GUI but skip.
void toGuis (const char* line, const Gui* skip)
{
    usb::sendText (line, skip);
    if (haveWeb)
        web::sendText (line, skip);
}

// The panel's settings, saved 2 s after the last change. ARPEGGIO and HOLD are playing, not settings: on
// the SH-101 they're off at power on.
constexpr uint32_t settleUs = 2000000;
bool settingsChanged = false;
uint32_t lastChangeUs = 0;

bool isSetting (int id)
{
    return id != P_ARPEGGIO && id != P_HOLD;
}

void loadSettings()
{
    const flashRing::Page* saved = flashSaves::settings.load();
    flashSaves::sequences.load();
    flashSaves::settings.eraseAhead();
    flashSaves::sequences.eraseAhead();
    if (! saved)
        return;
    float values[flashSaves::maxSettings];
    std::memcpy (values, saved->data, saved->count * sizeof (float));
    for (int id = 0; id < saved->count && id < params::count; ++id)
        if (isSetting (id))
            control.setValue (id, values[id], EVENT_PORT_USB);
    settingsChanged = false;
}

const char* said (flashRing::Saved saved)
{
    return saved == flashRing::Saved::written     ? "saved"
           : saved == flashRing::Saved::unchanged ? "unchanged"
                                                  : "NOT saved";
}

void saveSettings()
{
    if (! settingsChanged || time_us_32() - lastChangeUs < settleUs)
        return;
    settingsChanged = false;
    const uint32_t t0 = time_us_32();
    float values[flashSaves::maxSettings];
    const int count = params::count < flashSaves::maxSettings ? params::count : flashSaves::maxSettings;
    for (int id = 0; id < count; ++id)
        values[id] = isSetting (id) ? control.values[id] : 0.0f;
    const auto saved = flashSaves::settings.write (values, count);
    usb::log ("settings %s in %lu us", said (saved), (unsigned long) (time_us_32() - t0));
}

// The other GUIs follow; the one that moved it already shows it.
void knobMoved (int id, float value, EventPort)
{
    if (isSetting (id))
    {
        settingsChanged = true;
        lastChangeUs = time_us_32();
    }
    char line[40];
    guiLines::valueLine (line, sizeof line, id, value);
    toGuis (line, guis.sender);
}

void wheelMoved (EventType type, float value, EventPort)
{
    char line[24];
    guiLines::wheelLine (line, sizeof line, type == EVENT_PITCH_BEND ? 'b' : 'm', value);
    toGuis (line, guis.sender);
}

void keyChanged (int key, bool down, EventPort)
{
    char line[16];
    guiLines::keyLine (line, sizeof line, key, down);
    toGuis (line, guis.sender);
}

// The sequencer's lights and step to the GUIs when they change; a sequence just loaded into flash.
void followSequencer()
{
    const audio::Sequencer now = audio::sequencer();
    auto& shown = control.sequencer;
    char line[40];
    if (now.mode != shown.mode || now.position != shown.position || now.length != shown.length)
    {
        shown.mode = now.mode;
        shown.position = now.position;
        shown.length = now.length;
        guiLines::sequencerLine (line, sizeof line, now.mode, now.position, now.length);
        toGuis (line, nullptr);
    }
    if (now.keyTranspose != shown.keyTranspose)
    {
        shown.keyTranspose = now.keyTranspose;
        guiLines::keyTransposeLine (line, sizeof line, now.keyTranspose);
        toGuis (line, nullptr);
    }
    if (picohDSP::Sequence sequence; audio::takeSequence (sequence))
    {
        const uint32_t t0 = time_us_32();
        const auto saved = flashSaves::sequences.write (&sequence, sizeof sequence);
        usb::log ("sequence of %d steps %s in %lu us", sequence.length, said (saved),
                  (unsigned long) (time_us_32() - t0));
    }
}

uint32_t nowUs()
{
    return time_us_32();
}

void reportLoad()
{
    static uint32_t nextUs = 0;
    uint32_t now = time_us_32();
    if (int32_t (now - nextUs) < 0)
        return;
    nextUs = now + 1000000;
    audio::Load load = audio::takeLoad();
    char line[64];
    snprintf (line, sizeof line, "load %.1f %.1f %lu", double (load.average), double (load.busiest),
              (unsigned long) load.underruns);
    toGuis (line, nullptr);
    guiLines::keysLine (line, sizeof line, control.keysDown);
    toGuis (line, nullptr);
    if (droppedEvents)
        usb::log ("%lu events dropped (queue full)", (unsigned long) droppedEvents);
}
} // namespace

int main()
{
    queue_init (&events, sizeof (Event), 128);
    control.send = send;
    control.changed = knobMoved;
    control.wheelMoved = wheelMoved;
    control.keyChanged = keyChanged;
    control.nowUs = nowUs;

    guis.control = &control;
    guis.broadcast = toGuis;
    usb::start (guis);
    audio::start (&events);
    loadSettings();
    din::start();
#if HAVE_WEB
    haveWeb = web::start (guis);
#endif

    while (true)
    {
        // Each job's time goes into the trace while it records.
        uint32_t t0 = time_us_32();
        usb::poll();
        uint32_t t1 = time_us_32();
        trace::core0.span (trace::usbPoll, t0, t1, 0, trace::ring);
        din::poll (control);
        uint32_t t2 = time_us_32();
        trace::core0.span (trace::dinPoll, t1, t2, 0, trace::ring);
#if HAVE_WEB
        if (haveWeb)
            web::poll();
#endif
        uint32_t t3 = time_us_32();
        trace::core0.span (trace::webPoll, t2, t3, 0, trace::ring);
        reportLoad();
        followSequencer();
        saveSettings();
        trace::core0.span (trace::loadReport, t3, time_us_32(), 0, trace::ring);
    }
}
