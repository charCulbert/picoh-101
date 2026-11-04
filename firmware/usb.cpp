// USB: a MIDI port (notes and CCs, and the GUI lines as SysEx F0 7D 50 48 <line> F7; 7D is MIDI's
// non-commercial ID, 50 48 is "PH") and a serial port (the same lines, and "# " log lines).
#include "board.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "pico/bootrom.h"
#include "pico/time.h"
#include "tusb.h"
#include "trace.h"
#include "hardware/clocks.h"

namespace
{
Guis* guis;

// There while the port is open (DTR up).
Gui serialGui { EVENT_PORT_SERIAL };
bool serialOpen = false, serialNeedsState = false;
char serialLine[128];
size_t serialLength = 0;

// There from its "hello" until 5 s pass without a line (it pings every 2 s; MIDI has no open or close).
Gui sysexGui { EVENT_PORT_SYSEX };
bool sysexOpen = false, sysexNeedsState = false;
uint32_t sysexLastUs = 0;
uint8_t sysex[160];
size_t sysexLength = 0;
bool inSysex = false;
constexpr uint8_t sysexId[] = { 0xF0, 0x7D, 0x50, 0x48 };

void serialWrite (const char* line)
{
    if (! serialOpen)
        return;
    size_t n = strlen (line);
    if (tud_cdc_write_available() < n + 1)
        return;  // a GUI that isn't reading misses this one
    tud_cdc_write (line, uint32_t (n));
    tud_cdc_write_char ('\n');
    tud_cdc_write_flush();
}

void sysexWrite (const char* line)
{
    if (! sysexOpen)
        return;
    uint8_t message[200];
    size_t n = strlen (line);
    if (n + sizeof sysexId + 1 > sizeof message)
        return;
    memcpy (message, sysexId, sizeof sysexId);
    for (size_t i = 0; i < n; ++i)
        message[sizeof sysexId + i] = uint8_t (line[i]) & 0x7F;
    message[sizeof sysexId + n] = 0xF7;
    tud_midi_stream_write (0, message, uint32_t (sizeof sysexId + n + 1));
}

// The trace (trace.h), from the serial port only (tools/trace.py):
//   trace blocks <n>               record the next n blocks
//   trace over <thousandths> [post] record until a block takes longer than that share of its time, then
//                                  `post` blocks more (default 8)
//   trace stop | trace status | trace read
// Replies are "# trace ..." lines, which pages ignore: status <state> <bytes> <blocks> <trigger block>
// <dropped records> <cycles per block> <clock Hz> <core 0 spans>, then for read: data <offset> <hex>
// (96 bytes a line), core0 <startUs> <lengthUs> <kind> <arg> per span, and end.
struct Readout
{
    bool active = false;
    uint32_t offset = 0, span = 0;
} readout;

void traceStatus()
{
    char line[128];
    snprintf (line, sizeof line, "# trace status %u %lu %lu %ld %lu %lu %lu %lu", unsigned (trace::ring.current()),
              (unsigned long) trace::ring.used(), (unsigned long) trace::ring.blocks(),
              trace::ring.trigger() == trace::noBlock ? -1L : long (trace::ring.trigger()),
              (unsigned long) trace::ring.droppedRecords(), (unsigned long) trace::ring.budget(),
              (unsigned long) clock_get_hz (clk_sys), (unsigned long) trace::core0.count());
    serialWrite (line);
}

void traceCommand (const char* command)
{
    char word[16] = "";
    unsigned long value = 0, post = 8;
    sscanf (command, "trace %15s %lu %lu", word, &value, &post);
    if (strcmp (word, "blocks") == 0)
    {
        trace::core0.clear();
        trace::ring.control (trace::blocksMode, value, 0);
    }
    else if (strcmp (word, "over") == 0)
    {
        trace::core0.clear();
        trace::ring.control (trace::overMode, value, post);
    }
    else if (strcmp (word, "stop") == 0)
        trace::ring.control (trace::stopMode, 0, 0);
    else if (strcmp (word, "read") == 0 && trace::ring.current() == trace::frozen)
        readout = { true, 0, 0 };
    traceStatus();
}

void continueReadout()
{
    if (! readout.active)
        return;
    char line[256];
    for (int n = 0; n < 8 && tud_cdc_write_available() >= sizeof line; ++n)
    {
        uint8_t chunk[96];
        uint32_t got = trace::ring.read (readout.offset, chunk, sizeof chunk);
        if (got)
        {
            int at = snprintf (line, sizeof line, "# trace data %lu ", (unsigned long) readout.offset);
            for (uint32_t i = 0; i < got; ++i)
                at += snprintf (line + at, sizeof line - at, "%02x", chunk[i]);
            serialWrite (line);
            readout.offset += got;
        }
        else if (readout.span < trace::core0.count())
        {
            const trace::Core0Log::Span& s = trace::core0.at (readout.span++);
            snprintf (line, sizeof line, "# trace core0 %lu %u %u %u", (unsigned long) s.startUs, unsigned (s.lengthUs),
                      unsigned (s.kind), unsigned (s.arg));
            serialWrite (line);
        }
        else
        {
            serialWrite ("# trace end");
            readout.active = false;
            return;
        }
    }
}

void serialLineIn (char* line)
{
    if (strcmp (line, "hello") == 0)
        serialNeedsState = true;
    else if (strncmp (line, "trace", 5) == 0)
        traceCommand (line);
    else
        guis->receive (serialGui, line);
}

void sysexLineIn (char* line)
{
    sysexLastUs = time_us_32();
    if (strcmp (line, "hello") == 0)
        sysexNeedsState = true;  // a GUI arriving: every value
    else if (strcmp (line, "ping") == 0)
    {
        if (! sysexOpen)
            sysexNeedsState = true;
    }
    else if (sysexOpen)
        guis->receive (sysexGui, line);
    sysexOpen = true;
}

void sysexByte (uint8_t b)
{
    if (b == 0xF0)
    {
        inSysex = true;
        sysexLength = 0;
    }
    if (! inSysex)
        return;
    if (sysexLength < sizeof sysex)
        sysex[sysexLength++] = b;
    if (b != 0xF7)
        return;
    inSysex = false;
    if (sysexLength < sizeof sysexId + 1 || memcmp (sysex, sysexId, sizeof sysexId) != 0)
        return;
    char line[sizeof sysex];
    size_t n = sysexLength - sizeof sysexId - 1;
    memcpy (line, sysex + sizeof sysexId, n);
    line[n] = 0;
    sysexLineIn (line);
}

// A few lines per pass, so the buffers keep up.
void sendStateSlowly (bool& needs, void (*write) (const char*), bool (*room)())
{
    static int next[2] = { 0, 0 };
    int& at = next[write == serialWrite ? 0 : 1];
    if (! needs)
    {
        at = 0;
        return;
    }
    // Every value, the keys that are down, then the sequencer, KEY TRANSPOSE and the patch.
    const int keysEnd = params::count + 128, total = keysEnd + 3;
    const auto& sequencer = guis->control->sequencer;
    char line[40];
    for (int sent = 0; sent < 4 && at < total && room(); ++at)
    {
        if (at < params::count)
            guiLines::valueLine (line, sizeof line, at, guis->control->values[at]);
        else if (at < keysEnd && guis->control->isDown (at - params::count))
            guiLines::keyLine (line, sizeof line, at - params::count, true);
        else if (at == keysEnd)
            guiLines::sequencerLine (line, sizeof line, sequencer.mode, sequencer.position, sequencer.length);
        else if (at == keysEnd + 1)
            guiLines::keyTransposeLine (line, sizeof line, sequencer.keyTranspose);
        else if (at == keysEnd + 2)
            guis->patchLine (line, sizeof line);
        else
            continue;
        write (line);
        ++sent;
    }
    if (at >= total)
        needs = false;
}
} // namespace

namespace usb
{
void start (Guis& p)
{
    guis = &p;
    tusb_init();
}

void poll()
{
    tud_task();

    uint8_t packet[4];
    while (tud_midi_available() && tud_midi_packet_read (packet))
    {
        const uint8_t kind = packet[0] & 0x0F; // code index number
        if (kind >= 0x8 && kind <= 0xE)
            guis->control->midi (packet + 1, EVENT_PORT_USB);
        else if (kind >= 0x4 && kind <= 0x7) // SysEx start or continue (3 bytes), end (1, 2 or 3)
        {
            const int bytes = kind == 0x4 || kind == 0x7 ? 3 : kind == 0x5 ? 1 : 2;
            for (int i = 0; i < bytes; ++i)
                sysexByte (packet[1 + i]);
        }
    }
    if (sysexOpen && time_us_32() - sysexLastUs > 5000000)
    {
        sysexOpen = false;
        guis->release (sysexGui);
    }

    while (tud_cdc_available())
    {
        const int c = tud_cdc_read_char();
        if (c == '\n' || c == '\r')
        {
            if (serialLength)
            {
                serialLine[serialLength] = 0;
                serialLineIn (serialLine);
            }
            serialLength = 0;
        }
        else if (serialLength < sizeof serialLine - 1)
            serialLine[serialLength++] = char (c);
    }

    sendStateSlowly (serialNeedsState, serialWrite, [] { return tud_cdc_write_available() >= 48; });
    continueReadout();
    if (trace::ring.frozeSinceAsked())
        traceStatus();
    sendStateSlowly (sysexNeedsState, sysexWrite, [] { return tud_midi_mounted(); });
}

void sendText (const char* line, const Gui* skip)
{
    if (skip != &serialGui)
        serialWrite (line);
    if (skip != &sysexGui)
        sysexWrite (line);
}

void log (const char* format, ...)
{
    char line[128] = "# ";
    va_list args;
    va_start (args, format);
    vsnprintf (line + 2, sizeof line - 2, format, args);
    va_end (args);
    serialWrite (line);
}
} // namespace usb

// Opening the port at 1200 baud reboots into the bootloader, for the flash target.
extern "C" void tud_cdc_line_state_cb (uint8_t, bool dtr, bool)
{
    if (! dtr && serialOpen)
        guis->release (serialGui);  // it asks for every value with "hello"
    serialOpen = dtr;
}

extern "C" void tud_cdc_line_coding_cb (uint8_t, cdc_line_coding_t const* coding)
{
    if (coding->bit_rate == 1200)
        reset_usb_boot (0, 0);
}
