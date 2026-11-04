// Core 0: USB (MIDI, and the GUI over serial or SysEx), DIN MIDI and Wi-Fi in; events out to the audio
// core through one queue.
#include <algorithm>
#include <cstdio>
#include "board.h"
#include "DSP/picohDSP_Voice.h"
#include "params.h"
#include "flash_saves.h"
#include "pico/stdlib.h"
#if PICO_RP2040
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#endif
#include "trace.h"

namespace
{
queue_t events;
Control control;
Guis guis;
bool haveWeb = false;
uint32_t droppedEvents = 0;

uint32_t longestPassUs = 0; // core 0's loop, this second: how late a USB or Wi-Fi input can be stamped

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

const char* said (flashRing::Saved saved)
{
    return saved == flashRing::Saved::written     ? "saved"
           : saved == flashRing::Saved::unchanged ? "unchanged"
                                                  : "NOT saved";
}

void patchChanged()
{
    char line[24];
    guis.patchLine (line, sizeof line);
    toGuis (line, nullptr);
}

// The slot loaded or stored last, for power on: written later, from the control loop, once a load's
// values have all gone out (a flash write holds the control loop up).
int slotToRemember = -1;

// A loaded patch's values go to the audio core loadGroup at a time, a tick apart, each group sharing a
// stamp: a load is never one burst in one block.
constexpr int loadGroup = 4;
constexpr uint32_t tickUs = picohDSP::Voice::tickLength * 1000000u / audio::sampleRate;
float loading[params::count];
int loadingFrom = params::count; // the next id to send
uint32_t nextGroupUs = 0;

// The next group when its time comes. VOLUME, ARPEGGIO and HOLD aren't in a patch: they stay as they are.
void sendLoading()
{
    const uint32_t now = time_us_32();
    if (loadingFrom >= params::count || int32_t (now - nextGroupUs) < 0)
        return;
    nextGroupUs = now + tickUs;
    // The group goes out first, then the GUIs hear of it: a line to each takes long enough that the
    // group's last value could miss the block its stamp is due in.
    int sent[params::count];
    int count = 0;
    const auto changed = control.changed;
    control.changed = nullptr;
    control.at (now,
                [&]
                {
                    for (; loadingFrom < params::count && count < loadGroup; ++loadingFrom)
                        if (patches::kept (loadingFrom) && loading[loadingFrom] != control.values[loadingFrom])
                        {
                            control.setValue (loadingFrom, loading[loadingFrom], EVENT_PORT_USB);
                            sent[count++] = loadingFrom;
                        }
                });
    control.changed = changed;
    const Gui* sender = guis.sender;
    guis.sender = nullptr; // the GUI whose key loaded it shows it too
    for (int i = 0; i < count; ++i)
        changed (sent[i], control.values[sent[i]], EVENT_PORT_USB);
    guis.sender = sender;
}

bool loadPatch (int slot)
{
    float values[params::count];
    std::memcpy (values, control.values, sizeof values);
    if (! flashSaves::patchMemory.load (slot, values))
    {
        usb::log ("patch %d empty", slot + 1);
        return false;
    }
    std::memcpy (loading, values, sizeof values);
    loadingFrom = 0;
    nextGroupUs = time_us_32();
    sendLoading();
    slotToRemember = slot;
    usb::log ("patch %d loaded", slot + 1);
    return true;
}

bool storePatch (int slot)
{
    // A load still going out goes on in its groups; the patch stored is where it ends.
    float values[params::count];
    std::memcpy (values, control.values, sizeof values);
    for (int id = loadingFrom; id < params::count; ++id)
        values[id] = loading[id];
    const uint32_t t0 = time_us_32();
    const auto saved = flashSaves::patchMemory.store (slot, values);
    usb::log ("patch %d %s in %lu us", slot + 1, said (saved), (unsigned long) (time_us_32() - t0));
    if (saved == flashRing::Saved::failed)
        return false;
    slotToRemember = slot;
    return true;
}

void rememberSlot()
{
    if (slotToRemember < 0 || loadingFrom < params::count)
        return;
    const auto saved = flashSaves::patchMemory.remember (slotToRemember);
    if (saved == flashRing::Saved::failed)
        usb::log ("last patch NOT saved");
    slotToRemember = -1;
}

// The sequence, and the patch loaded or stored last; the playing controls at their defaults.
void loadSaved()
{
    flashSaves::sequences.load();
    flashSaves::sequences.eraseAhead();
    flashSaves::patchMemory.eraseAhead();
    const int slot = flashSaves::patchMemory.lastSlot();
    if (slot >= 0 && loadPatch (slot))
        control.patchSlot = slot;
    slotToRemember = -1;
}

// The other GUIs follow; the one that moved it already shows it.
void knobMoved (int id, float value, EventPort)
{
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
    char line[80];
    snprintf (line, sizeof line, "load %.1f %.1f %lu %lu %lu %lu", double (load.average), double (load.busiest),
              (unsigned long) load.underruns, (unsigned long) longestPassUs, (unsigned long) load.late,
              (unsigned long) load.latestUs);
    longestPassUs = 0;
    toGuis (line, nullptr);
    guiLines::keysLine (line, sizeof line, control.keysDown);
    toGuis (line, nullptr);
    if (droppedEvents)
        usb::log ("%lu events dropped (queue full)", (unsigned long) droppedEvents);
    droppedEvents = 0;
}
} // namespace

// std::terminate's default handler prints the exception's name, which links in a C++ demangler.
namespace __gnu_cxx
{
void __verbose_terminate_handler()
{
    panic ("terminate");
}
} // namespace __gnu_cxx

int main()
{
#if PICO_RP2040
    // The fixed-point voice needs 250 MHz, at 1.2 V: set before anything takes its clock from clk_sys.
    vreg_set_voltage (VREG_VOLTAGE_1_20);
    sleep_ms (10);
    set_sys_clock_khz (250000, true);
#endif
    queue_init (&events, sizeof (Event), eventQueueCapacity);
    control.send = send;
    control.changed = knobMoved;
    control.wheelMoved = wheelMoved;
    control.keyChanged = keyChanged;
    control.nowUs = nowUs;
    control.loadPatch = loadPatch;
    control.storePatch = storePatch;
    control.patchChanged = patchChanged;

    guis.control = &control;
    guis.broadcast = toGuis;
    usb::start (guis);
    audio::start (&events);
    loadSaved();
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
        sendLoading();
        reportLoad();
        followSequencer();
        rememberSlot();
        const uint32_t t4 = time_us_32();
        trace::core0.span (trace::loadReport, t3, t4, 0, trace::ring);
        longestPassUs = std::max (longestPassUs, t4 - t0);
    }
}
