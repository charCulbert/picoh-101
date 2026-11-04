// Desktop checks for the parts of the firmware that don't touch hardware.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include "controls.h"
#include "midi_parser.h"
#include "event_scheduler.h"
#include "synth_host.h"
#include "params.h"
#include "gui_protocol.h"
#include "websocket.h"
#include "flash_ring.h"
#include "patches.h"

static int failures = 0;

static int idOf (const char* key)
{
    for (const Param& p : paramTable)
        if (std::string (p.key) == key)
            return int (p.id);
    return -1;
}
#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (! (x))                                                                                                     \
        {                                                                                                              \
            std::printf ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                                                  \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

static void midiParser()
{
    MidiParser p;
    uint8_t m[3];
    std::vector<std::vector<uint8_t>> got;
    // note on, running-status note on, a clock byte inside a message, sysex, program change
    const uint8_t bytes[] = { 0x90, 60, 100, 62, 0xF8, 90, 0xF0, 1, 2, 3, 0xF7, 0xC0, 5, 0xB1, 74, 0x40 };
    for (uint8_t b : bytes)
        if (p.feed (b, m))
            got.push_back ({ m[0], m[1], m[2] });
    CHECK (got.size() == 4);
    CHECK ((got[0] == std::vector<uint8_t> { 0x90, 60, 100 }));
    CHECK ((got[1] == std::vector<uint8_t> { 0x90, 62, 90 }));
    CHECK ((got[2] == std::vector<uint8_t> { 0xC0, 5, 0 }));
    CHECK ((got[3] == std::vector<uint8_t> { 0xB1, 74, 0x40 }));
    // data after sysex with no new status is dropped
    MidiParser q;
    const uint8_t after[] = { 0xF0, 1, 0xF7, 60, 100 };
    int count = 0;
    for (uint8_t b : after)
        count += q.feed (b, m);
    CHECK (count == 0);
}

// A synth that records what happened at which sample.
struct Recorder
{
    std::vector<std::string> log;
    int sample = 0;
    void noteOn (uint8_t k) { log.push_back ("on " + std::to_string (k) + " @" + std::to_string (sample)); }
    void noteOff (uint8_t k) { log.push_back ("off " + std::to_string (k) + " @" + std::to_string (sample)); }
    void setParameter (uint32_t id, float)
    {
        log.push_back ("p " + std::to_string (id) + " @" + std::to_string (sample));
    }
    void allNotesOff() { log.push_back ("all @" + std::to_string (sample)); }
    void pitchBend (float v)
    {
        log.push_back ("bend " + std::to_string (int (v * 100)) + " @" + std::to_string (sample));
    }
    void modWheel (float v)
    {
        log.push_back ("mod " + std::to_string (int (v * 100)) + " @" + std::to_string (sample));
    }
    void sustainPedal (bool down)
    {
        log.push_back (std::string (down ? "down" : "up") + " @" + std::to_string (sample));
    }
    void sequencerButton (int b) { log.push_back ("seq " + std::to_string (b) + " @" + std::to_string (sample)); }
    void legato (bool down) { log.push_back (std::string ("legato ") + (down ? "1" : "0")); }
    void keyTransposeHeld (bool down) { log.push_back (std::string ("kt ") + (down ? "1" : "0")); }
    void render (float* l, float* r, int n)
    {
        for (int i = 0; i < n; ++i)
            l[i] = r[i] = 0;
        sample += n;
    }
};

static void scheduler()
{
    Recorder synth;
    EventScheduler<Recorder> r { synth, 48000, 1000 }; // commands sound 1 ms after they arrive
    std::deque<Event> queue;
    auto pop = [&] (Event& e)
    {
        if (queue.empty())
            return false;
        e = queue.front();
        queue.pop_front();
        return true;
    };
    auto event = [] (EventType type, uint32_t timeUs)
    {
        Event e {};
        e.type = type;
        e.timeUs = timeUs;
        e.note.key = 60;
        return e;
    };
    float l[64], rr[64];

    queue = { event (EVENT_NOTE_ON, 0), event (EVENT_NOTE_OFF, 500) }; // due at 1000 and 1500 us
    r.render (pop, 0, l, rr, 64);    // block 0: 0 to 1333 us, the note on lands at sample 48
    r.render (pop, 1333, l, rr, 64); // block 1: the note off at 1500 us, sample 64 + 8
    CHECK (synth.log.size() == 2);
    CHECK (synth.log[0] == "on 60 @48");
    CHECK (synth.log[1] == "off 60 @72");

    // an event that arrived long ago applies at the block's first sample
    synth.log.clear();
    Event p = event (EVENT_PARAM, 0);
    p.param.id = 3;
    queue = { p };
    r.render (pop, 100000, l, rr, 64);
    CHECK (synth.log.size() == 1 && synth.log[0] == "p 3 @128");
    CHECK (r.late == 1 && r.latestUs == 100000 - 1000);

    // queued out of order (USB polled before DIN's older bytes): applied in time order, on time; the
    // same time in the order they came
    synth.log.clear();
    Event a = event (EVENT_NOTE_ON, 200600), b = event (EVENT_NOTE_ON, 200100), c = event (EVENT_NOTE_OFF, 200100);
    a.note.key = 61;
    queue = { a, b, c };
    r.render (pop, 200000, l, rr, 64); // b and c at 201100 us: sample 53
    r.render (pop, 201333, l, rr, 64); // a at 201600 us: sample 64 + 13
    CHECK (synth.log.size() == 3);
    CHECK (synth.log[0] == "on 60 @245" && synth.log[1] == "off 60 @245" && synth.log[2] == "on 61 @269");
    CHECK (r.late == 0);

    // a full queue kept for the next block, then another full queue: a note off first and its note on,
    // stamped earlier, last. All of them are applied in one block, in time order
    synth.log.clear();
    const uint32_t s = 1000000;
    queue.clear();
    for (int i = 0; i < eventQueueCapacity; ++i)
        queue.push_back (event (EVENT_NOTE_ON, s + 400)); // due at s + 1400: sample 3 of the next block
    r.render (pop, s, l, rr, 64);
    CHECK (synth.log.empty() && queue.empty());
    Event older = event (EVENT_NOTE_ON, s + 340), newer = event (EVENT_NOTE_OFF, s + 350);
    older.note.key = newer.note.key = 70;
    queue.push_back (newer);
    Event filler = event (EVENT_PARAM, s + 360);
    filler.param.id = 4;
    for (int i = 0; i < eventQueueCapacity - 2; ++i)
        queue.push_back (filler);
    queue.push_back (older);
    const int at = synth.sample;
    r.render (pop, s + 1333, l, rr, 64);
    CHECK (queue.empty() && r.applied == 2 * eventQueueCapacity && r.late == 0);
    CHECK (synth.log.size() == size_t (2 * eventQueueCapacity));
    CHECK (synth.log[0] == "on 70 @" + std::to_string (at) && synth.log[1] == "off 70 @" + std::to_string (at + 1));
    CHECK (synth.log[2] == "p 4 @" + std::to_string (at + 1));
    CHECK (synth.log[eventQueueCapacity - 1] == "p 4 @" + std::to_string (at + 1));
    CHECK (synth.log[eventQueueCapacity] == "on 60 @" + std::to_string (at + 3));
    CHECK (synth.log.back() == "on 60 @" + std::to_string (at + 3));
}

static std::vector<Event> sent;
static std::vector<int> changedIds;

static void control()
{
    Control c;
    c.send = [] (const Event& x) { sent.push_back (x); };
    c.changed = [] (int id, float, EventPort) { changedIds.push_back (id); };
    const int cutoff = idOf ("vcf freq");
    CHECK (cutoff >= 0);
    uint8_t cutoffCc[3] = { 0xB0, uint8_t (params::table[cutoff].cc), 127 };
    c.midi (cutoffCc, EVENT_PORT_USB);
    CHECK (sent.size() == 1 && sent[0].type == EVENT_PARAM && sent[0].param.id == cutoff);
    CHECK (std::fabs (sent[0].param.value - params::table[cutoff].max) < 0.5f);
    CHECK (changedIds.size() == 1 && changedIds[0] == cutoff);
    uint8_t zeroVelocity[3] = { 0x92, 64, 0 };
    c.midi (zeroVelocity, EVENT_PORT_USB);
    CHECK (sent.back().type == EVENT_NOTE_OFF && sent.back().note.key == 64 && sent.back().note.channel == 2);
    uint8_t loud[3] = { 0x90, 64, 127 };
    c.midi (loud, EVENT_PORT_DIN);
    CHECK (sent.back().type == EVENT_NOTE_ON && sent.back().note.velocity == 65535 &&
           sent.back().port == EVENT_PORT_DIN);
    // Keys down, from any input, told to the GUIs once each way.
    static std::vector<std::pair<int, bool>> keys;
    c.keyChanged = [] (int key, bool down, EventPort) { keys.push_back ({ key, down }); };
    c.note (60, 40000, EVENT_PORT_WEB);
    c.note (60, 40000, EVENT_PORT_DIN); // already down: no change to tell
    CHECK (c.isDown (60) && keys.size() == 1 && keys[0] == std::make_pair (60, true));
    c.allNotesOff (EVENT_PORT_USB);
    CHECK (! c.isDown (60) && ! c.isDown (64)); // 64 was down from the notes above
    CHECK (std::count (keys.begin(), keys.end(), std::make_pair (60, false)) == 1);
    c.keyChanged = nullptr;
    uint8_t unmapped[3] = { 0xB0, 2, 10 }; // breath: left free
    size_t before = sent.size();
    c.midi (unmapped, EVENT_PORT_USB);
    CHECK (sent.size() == before);
    // stepped values come out whole
    c.setValue (idOf ("range"), 1.4f, EVENT_PORT_WEB);
    CHECK (sent.back().param.value == 1.0f);
    c.setValue (idOf ("vcf freq"), 1.0e9f, EVENT_PORT_WEB); // kept in range
    CHECK (sent.back().param.value == params::table[idOf ("vcf freq")].max);
    uint8_t bendUp[3] = { 0xE0, 0x7F, 0x7F }, bendDown[3] = { 0xE5, 0, 0 }, centre[3] = { 0xE0, 0, 0x40 },
            modFull[3] = { 0xB0, 1, 127 };
    c.midi (bendUp, EVENT_PORT_DIN);
    CHECK (sent.back().type == EVENT_PITCH_BEND && sent.back().param.value == 1.0f);
    c.midi (bendDown, EVENT_PORT_DIN);
    CHECK (sent.back().param.value == -1.0f);
    c.midi (centre, EVENT_PORT_DIN);
    CHECK (sent.back().param.value == 0.0f);
    c.midi (modFull, EVENT_PORT_USB);
    CHECK (sent.back().type == EVENT_MOD_WHEEL && sent.back().param.value == 1.0f);
    uint8_t pedal[3] = { 0xB0, 64, 127 };
    c.midi (pedal, EVENT_PORT_USB);
    CHECK (sent.back().type == EVENT_SUSTAIN && sent.back().note.velocity != 0);
}

static void params_()
{
    for (int id = 0; id < params::count; ++id)
    {
        const auto& p = params::table[id];
        CHECK (std::fabs (params::toValue (id, params::toPosition (id, p.initial)) - p.initial) <
               1e-3f * (p.max - p.min) + 0.01f);
    }
    // the knob's centre gives mid (VCF freq: 1 kHz)
    const int cutoff = idOf ("vcf freq");
    CHECK (std::fabs (params::toValue (cutoff, 0.5f) - params::table[cutoff].mid) < 0.5f);
    // every CC maps to one parameter
    for (int id = 0; id < params::count; ++id)
        CHECK (params::table[id].cc < 0 || params::fromCc (uint8_t (params::table[id].cc)) == id);
}

static void webSocket()
{
    char accept[29];
    const char* key = "dGhlIHNhbXBsZSBub25jZQ=="; // RFC 6455's example
    ws::acceptKey (key, std::strlen (key), accept);
    CHECK (std::string (accept) == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

    uint8_t digest[20];
    ws::sha1 (reinterpret_cast<const uint8_t*> ("abc"), 3, digest);
    char hex[41];
    for (int i = 0; i < 20; ++i)
        std::snprintf (hex + 2 * i, 3, "%02x", digest[i]);
    CHECK (std::string (hex) == "a9993e364706816aba3e25717850c26c9cd0d89d");

    // RFC 6455's masked "Hello"
    uint8_t frame[] = { 0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58 };
    ws::Frame f;
    CHECK (! ws::readFrame (frame, sizeof frame - 1, f));
    CHECK (ws::readFrame (frame, sizeof frame, f));
    CHECK (f.opcode == ws::text && f.final && f.length == 5 && std::memcmp (f.payload, "Hello", 5) == 0);
    CHECK (f.size == sizeof frame);
}

static void synth()
{
    SynthHost s;
    s.prepare (48000, 64);
    float l[256], r[256];
    s.render (l, r, 256);
    float silent = 0;
    for (float x : l)
        silent = std::fmax (silent, std::fabs (x));
    CHECK (silent == 0);
    s.pitchBend (1.0f);
    s.modWheel (0.5f);
    s.noteOn (60);
    float peak = 0;
    for (int b = 0; b < 40; ++b)
    {
        s.render (l, r, 256);
        for (int i = 0; i < 256; ++i)
        {
            peak = std::fmax (peak, std::fabs (l[i]));
            CHECK (std::isfinite (l[i]) && l[i] == r[i]);
        }
    }
    CHECK (peak > 0.01f && peak < 2.0f);
}

// The page's lines, as every transport hands them to Guis: values, notes held by one page and let go
// when it leaves, wheels, and which page the others shouldn't echo a change back to.
static std::vector<std::string> changes;
static const Gui* changedExcept = nullptr;
static void pages_()
{
    static Guis guis;
    static Control c;
    sent.clear();
    c.send = [] (const Event& x) { sent.push_back (x); };
    c.changed = [] (int id, float value, EventPort)
    {
        char line[40];
        guiLines::valueLine (line, sizeof line, id, value);
        changes.push_back (line);
        changedExcept = guis.sender;
    };
    c.nowUs = [] { return 0u; };
    guis.control = &c;
    Gui serial { EVENT_PORT_SERIAL }, web { EVENT_PORT_WEB };

    const int cutoff = idOf ("vcf freq");
    char line[40];
    std::snprintf (line, sizeof line, "v %d 440", cutoff);
    guis.receive (serial, line);
    CHECK (sent.size() == 1 && sent[0].type == EVENT_PARAM && sent[0].port == EVENT_PORT_SERIAL);
    CHECK (sent[0].param.value == 440.0f);
    CHECK (changes.size() == 1 && changedExcept == &serial); // the others hear it, not the sender
    CHECK (guis.sender == nullptr);

    guis.receive (serial, "n 60 0.5");
    guis.receive (web, "n 62 1");
    CHECK (sent.back().type == EVENT_NOTE_ON && sent.back().port == EVENT_PORT_WEB);
    size_t before = sent.size();
    guis.release (serial); // unplugged: its note goes, the web page's stays
    CHECK (sent.size() == before + 1 && sent.back().type == EVENT_NOTE_OFF && sent.back().note.key == 60);
    guis.release (serial);
    CHECK (sent.size() == before + 1);

    guis.receive (web, "b -2");
    CHECK (sent.back().type == EVENT_PITCH_BEND && sent.back().param.value == -1.0f);
    before = sent.size();
    guis.receive (web, "bogus");
    std::snprintf (line, sizeof line, "v %d 1", params::count);
    guis.receive (web, line);
    CHECK (sent.size() == before);

    int lines = 0;
    guis.sendState ([&] (const char* text) { lines += text[0] == 'v'; });
    CHECK (lines == params::count);
    CHECK (Guis::fromGui (EVENT_PORT_SYSEX) && ! Guis::fromGui (EVENT_PORT_USB));
}

// The line parser and writers shared by the Pico and the plugin.
static void guiLines_()
{
    using Kind = guiLines::Line::Kind;
    auto v = guiLines::parse ("v 12 800");
    CHECK (v.kind == Kind::value && v.id == 12 && v.value == 800.0f);
    auto g = guiLines::parse ("g 3 1");
    CHECK (g.kind == Kind::gesture && g.id == 3 && g.value == 1.0f);
    auto n = guiLines::parse ("n 200 2");
    CHECK (n.kind == Kind::note && n.id == (200 & 0x7F) && n.value == 1.0f);
    CHECK (guiLines::parse ("b -3").value == -1.0f && guiLines::parse ("m 0.5").kind == Kind::mod);
    CHECK (guiLines::parse ("hello").kind == Kind::hello && guiLines::parse ("ping").kind == Kind::ping);
    CHECK (guiLines::parse ("a").kind == Kind::allNotesOff);
    CHECK (guiLines::parse ("s play").kind == Kind::sequencer && guiLines::parse ("s play").id == 1);
    CHECK (guiLines::parse ("s stop").kind == Kind::unknown);
    CHECK (guiLines::parse ("l 1").kind == Kind::legato && guiLines::parse ("t 0").kind == Kind::keyTranspose);
    CHECK (guiLines::parse ("t 0").value == 0.0f && guiLines::parse ("t 2").kind == Kind::unknown);
    CHECK (guiLines::parse ("p load").kind == Kind::patch && guiLines::parse ("p load").id == 0);
    CHECK (guiLines::parse ("p store").kind == Kind::patch && guiLines::parse ("p store").id == 1);
    for (const char* bad : { "p", "p ", "p off", "p loads", "p store 3" })
        CHECK (guiLines::parse (bad).kind == Kind::unknown);
    for (const char* bad : { "", "v", "v x 1", "v -1 2", "v 2147483648 1", "v 70000 1", "v 12 nan", "n 60 nan", "b inf",
                             "m nan", "bogus", "load 1 2 3", "# log" })
        CHECK (guiLines::parse (bad).kind == Kind::unknown);
    char line[40];
    guiLines::keyLine (line, sizeof line, 60, true);
    CHECK (std::string (line) == "k 60 1");
    const uint32_t down[4] = { 0, 1u << 28, 0, 0x80000000u }; // keys 60 and 127
    guiLines::keysLine (line, sizeof line, down);
    CHECK (std::string (line) == "keys 00000000100000000000000080000000");
    guiLines::valueLine (line, sizeof line, 12, 1234.5f);
    CHECK (std::string (line) == "v 12 1234.5");
    guiLines::patchLine (line, sizeof line, 2, 41);
    CHECK (std::string (line) == "p store 41");
    guiLines::patchLine (line, sizeof line, 0, 0);
    CHECK (std::string (line) == "p off 0");

    CHECK (guiLines::parse ("page").kind == Kind::page && guiLines::parse ("page").id == -1);
    CHECK (guiLines::parse ("page 2097151").kind == Kind::page && guiLines::parse ("page 2097151").id == 2097151);
    for (const char* bad : { "page ", "page x", "page -1", "page 2097152", "page 1 2", "pages" })
        CHECK (guiLines::parse (bad).kind == Kind::unknown);
    guiLines::pageLine (line, sizeof line, 0x3b54f5b04a477504ull, 183174);
    CHECK (std::string (line) == "page 3b54f5b04a477504 183174");

    // Every byte value, at an offset using all three bytes; app/src/page.rs decodes the same message.
    uint8_t data[256], message[guiLines::pageChunkSize (256)];
    for (int i = 0; i < 256; ++i)
        data[i] = uint8_t (255 - i);
    const size_t length = guiLines::pageChunk (message, 0x1ABCDE, data, 9);
    const std::vector<uint8_t> nine (message, message + length);
    CHECK ((nine == std::vector<uint8_t> { 0xF0, 0x7D, 0x50, 0x50, 0x6A, 0x79, 0x5E, 0x7F, 0x7F, 0x7E, 0x7D, 0x7C,
                                           0x7B, 0x7A, 0x79, 0x03, 0x78, 0x77, 0xF7 }));
    CHECK (guiLines::pageChunk (message, 0, data, 256) == guiLines::pageChunkSize (256));
    bool sevenBits = message[0] == 0xF0 && message[guiLines::pageChunkSize (256) - 1] == 0xF7;
    for (size_t i = 1; i + 1 < guiLines::pageChunkSize (256); ++i)
        sevenBits = sevenBits && message[i] < 0x80;
    CHECK (sevenBits);
    // Decoded back.
    uint8_t back[256];
    size_t got = 0;
    for (size_t at = 7; at + 1 < guiLines::pageChunkSize (256); at += 8)
    {
        const uint8_t high = message[at];
        for (size_t j = 0; j < 7 && at + 1 + j + 1 < guiLines::pageChunkSize (256); ++j)
            back[got++] = uint8_t (message[at + 1 + j] | (high >> j & 1) << 7);
    }
    CHECK (got == 256 && std::memcmp (back, data, 256) == 0);
}

// LOAD and STORE: armed by their button (again: disarmed), the next key from 41 to 72, from any input,
// picks the slot and doesn't play; keys outside leave it armed.
static std::vector<std::pair<char, int>> patchCalls;
static int patchTold = 0;
static void patchButtons_()
{
    static Guis guis;
    static Control c;
    sent.clear();
    c.send = [] (const Event& x) { sent.push_back (x); };
    c.loadPatch = [] (int slot)
    {
        patchCalls.push_back ({ 'l', slot });
        return slot != 7; // empty
    };
    c.storePatch = [] (int slot)
    {
        patchCalls.push_back ({ 's', slot });
        return true;
    };
    c.patchChanged = [] { ++patchTold; };
    guis.control = &c;
    Gui web { EVENT_PORT_WEB };

    guis.receive (web, "p store");
    CHECK (c.armed == Control::armedStore && patchTold == 1);
    c.note (40, 30000, EVENT_PORT_DIN); // below the keyboard: plays, still armed
    CHECK (sent.size() == 1 && sent[0].type == EVENT_NOTE_ON && c.armed == Control::armedStore);
    c.note (40, 0, EVENT_PORT_DIN);
    c.note (45, 30000, EVENT_PORT_DIN);
    CHECK (patchCalls.size() == 1 && patchCalls[0] == std::make_pair ('s', 4));
    CHECK (sent.size() == 2 && c.armed == Control::unarmed && c.patchSlot == 4 && ! c.isDown (45));

    guis.receive (web, "p load");
    guis.receive (web, "p load"); // cancelled
    CHECK (c.armed == Control::unarmed && patchTold == 4);
    c.note (50, 30000, EVENT_PORT_USB);
    CHECK (patchCalls.size() == 1 && sent.back().type == EVENT_NOTE_ON);

    guis.receive (web, "p store");
    guis.receive (web, "p load"); // the other button takes over
    CHECK (c.armed == Control::armedLoad);
    const size_t before = sent.size();
    guis.receive (web, "n 48 1"); // slot 7, empty: nothing loaded, disarmed
    CHECK (patchCalls.back() == std::make_pair ('l', 7) && c.patchSlot == 4 && c.armed == Control::unarmed);
    CHECK (sent.size() == before && ! c.isDown (48));

    guis.receive (web, "p load");
    c.note (72, 30000, EVENT_PORT_DIN);
    CHECK (patchCalls.back() == std::make_pair ('l', 31) && c.patchSlot == 31);
    CHECK (sent.size() == before && ! c.isDown (72));
    c.note (72, 30000, EVENT_PORT_DIN); // disarmed: just a key
    CHECK (patchCalls.size() == 3);

    guis.receive (web, "p store");
    std::string state;
    guis.sendState ([&] (const char* text) { if (text[0] == 'p') state = text; });
    CHECK (state == "p store 72");
}

// The chord in pitch order; U&D turns at the ends without repeating them; HOLD keeps the chord until a
// key goes down with none held.
static void arpeggio_()
{
    using picohDSP::Arpeggio;
    Arpeggio a;
    a.setMode (Arpeggio::Mode::upDown);
    a.keyDown (67);
    CHECK (! a.isRunning());
    a.keyDown (60);
    a.keyDown (64);
    CHECK (a.isRunning());
    a.restart();
    int steps[8];
    for (int& k : steps)
        k = a.step();
    const int upDown[8] = { 60, 64, 67, 64, 60, 64, 67, 64 };
    CHECK (std::equal (steps, steps + 8, upDown));

    a.setMode (Arpeggio::Mode::down);
    CHECK (a.step() == 67 && a.step() == 64 && a.step() == 60 && a.step() == 67);

    a.setHold (true);
    a.keyUp (60);
    a.keyUp (64);
    a.keyUp (67);
    CHECK (a.isRunning()); // held
    a.keyDown (72);        // none down: a new chord
    CHECK (! a.isRunning());
    a.keyDown (48);
    a.setMode (Arpeggio::Mode::up);
    CHECK (a.step() == 48 && a.step() == 72);
    a.keyUp (48);
    a.setHold (false); // the key that's up goes
    CHECK (! a.isRunning());
}

// Loading: keys, a rest, a tie; playing: a gate per step, closed on the fall, open across a tie.
static void sequencer_()
{
    using picohDSP::Sequencer;
    using Kind = Sequencer::Event::Kind;
    Sequencer q;
    q.load();
    CHECK (q.getMode() == Sequencer::Mode::loading);
    q.keyDown (60);
    q.rest();
    q.keyDown (62);
    q.setLegato (true);
    q.keyDown (62); // tied: one long D
    q.setLegato (false);
    q.load();
    CHECK (q.getMode() == Sequencer::Mode::stopped && q.getSequence().length == 4 && q.getVersion() == 1);

    q.play();
    auto e = q.clockRise();
    CHECK (e.kind == Kind::note && e.key == 60 && q.getPosition() == 1);
    CHECK (q.clockFall().kind == Kind::gateOff);
    CHECK (q.clockRise().kind == Kind::none); // the rest: already closed
    CHECK (q.clockFall().kind == Kind::none);
    e = q.clockRise();
    CHECK (e.kind == Kind::note && e.key == 62);
    CHECK (q.clockFall().kind == Kind::none); // the next step is tied
    e = q.clockRise();
    CHECK (e.kind == Kind::tied && e.key == 62);
    CHECK (q.clockFall().kind == Kind::gateOff);
    CHECK (q.clockRise().key == 60); // round again

    // LOAD while playing carries on after the step playing; the old step after it stays.
    q.load();
    q.keyDown (65);
    q.load();
    CHECK (q.getSequence().length == 4 && q.getSequence().steps[1] == 65 && q.getSequence().steps[2] == 62);

    // LOAD when stopped starts over.
    q.load();
    CHECK (q.getSequence().length == 0);
    for (int i = 0; i < 120; ++i)
        q.keyDown (60);
    CHECK (q.getMode() == Sequencer::Mode::stopped && q.getSequence().length == 100);
}

// Flash as NOR: erase sets a sector to 0xFF, programming only clears bits.
struct FakeFlash
{
    static inline uint8_t bytes[80 * flashRing::sectorBytes];
    static inline int erases = 0, programs = 0;
    static inline bool cutAfterErase = false; // power lost between the erase and the page
    static const uint8_t* at (uint32_t offset) { return bytes + offset; }
    static bool write (uint32_t offset, const uint8_t* page, bool eraseFirst)
    {
        if (eraseFirst)
        {
            std::memset (bytes + (offset & ~(flashRing::sectorBytes - 1)), 0xFF, flashRing::sectorBytes);
            ++erases;
        }
        if (cutAfterErase)
            return false;
        for (uint32_t i = 0; i < flashRing::pageBytes; ++i)
            bytes[offset + i] &= page[i];
        ++programs;
        return true;
    }
    static bool erase (uint32_t offset)
    {
        std::memset (bytes + offset, 0xFF, flashRing::sectorBytes);
        ++erases;
        return true;
    }
};

static void flashRing_()
{
    using Ring = flashRing::Ring<FakeFlash>;
    std::memset (FakeFlash::bytes, 0x5A, sizeof FakeFlash::bytes); // never erased
    const uint32_t offset = 2 * flashRing::sectorBytes;
    auto ring = [&] { return Ring { offset, 4, { 'T', 'S', '0', '1' }, 1 }; };
    Ring r = ring();
    CHECK (r.load() == nullptr);
    uint8_t data[101];
    auto fill = [&] (int n) { for (int i = 0; i < 101; ++i) data[i] = uint8_t (n * 7 + i); };

    // Round trip, and an unchanged save writes nothing.
    fill (0);
    CHECK (r.write (data, 101) == flashRing::Saved::written);
    CHECK (FakeFlash::erases == 1 && FakeFlash::programs == 1);
    CHECK (r.write (data, 101) == flashRing::Saved::unchanged);
    CHECK (FakeFlash::programs == 1);
    Ring again = ring();
    CHECK (again.load() && std::memcmp (again.newest->data, data, 101) == 0);

    // Many saves, round the ring several times, reloading as after a reboot: always the last one back,
    // one erase each 16 saves, nothing written outside the ring.
    FakeFlash::erases = FakeFlash::programs = 0;
    for (int n = 1; n <= 1000; ++n)
    {
        fill (n);
        CHECK (r.write (data, 101) == flashRing::Saved::written);
        if (n % 97 == 0)
        {
            r = ring();
            CHECK (r.load() && std::memcmp (r.newest->data, data, 101) == 0);
        }
    }
    CHECK (FakeFlash::programs == 1000);
    CHECK (FakeFlash::erases == 1000 / 16 + 1 || FakeFlash::erases == 1000 / 16);
    CHECK (FakeFlash::bytes[offset - 1] == 0x5A && FakeFlash::bytes[offset + 4 * flashRing::sectorBytes] == 0x5A);
    std::printf ("flash ring: 1000 saves, %d erases\n", FakeFlash::erases);

    // A save cut off part way leaves the one before.
    fill (1000);
    const int cut = r.next;
    fill (1001);
    r.write (data, 101);
    std::memset (FakeFlash::bytes + offset + cut * flashRing::pageBytes + 40, 0xFF, 100);
    r = ring();
    fill (1000);
    CHECK (r.load() && std::memcmp (r.newest->data, data, 101) == 0);

    // A page left dirty mid-sector (a save cut off by power loss) is skipped: the save moves on to the next sector.
    r = ring();
    r.load();
    const int dirty = r.next;
    if (dirty % flashRing::pagesPerSector != 0)
    {
        FakeFlash::bytes[offset + dirty * flashRing::pageBytes] = 0;
        fill (2000);
        CHECK (r.write (data, 101) == flashRing::Saved::written);
        CHECK (r.next == (dirty / 16 + 1) * 16 % 64 + 1);
        r = ring();
        CHECK (r.load() && std::memcmp (r.newest->data, data, 101) == 0);
    }

    // After eraseAhead, the saves until the ring comes back to the newest's sector erase nothing, and the
    // newest survives it.
    r = ring();
    r.load();
    fill (3000);
    r.write (data, 101);
    r.load();
    r.eraseAhead();
    CHECK (r.load() && std::memcmp (r.newest->data, data, 101) == 0);
    const int before = FakeFlash::erases, free = 48 + (16 - r.next % 16) % 16;
    for (int i = 0; i < free; ++i)
    {
        fill (4000 + i);
        CHECK (r.write (data, 101) == flashRing::Saved::written);
    }
    CHECK (FakeFlash::erases == before);
}

// The patches: round trip of the kept values only, each slot on its own, a store cut off by power loss
// leaving the version before, many stores to one slot round its ring, and the slot last used.
static void patchMemory_()
{
    std::memset (FakeFlash::bytes, 0x5A, sizeof FakeFlash::bytes); // never erased
    const uint32_t offset = 4 * flashRing::sectorBytes;
    const patches::Memory<FakeFlash> m { offset };
    float panel[params::count], loaded[params::count];
    for (int id = 0; id < params::count; ++id)
        panel[id] = params::table[id].initial;
    CHECK (m.lastSlot() == -1);
    CHECK (! m.load (3, loaded));
    const int cutoff = idOf ("vcf freq"), volume = idOf ("volume"), hold = idOf ("hold");
    CHECK (! patches::kept (volume) && ! patches::kept (hold) && ! patches::kept (idOf ("arpeggio"))
           && patches::kept (idOf ("transpose")) && patches::kept (cutoff));

    panel[cutoff] = 555.0f;
    panel[volume] = 0.1f;
    CHECK (m.store (3, panel) == flashRing::Saved::written);
    CHECK (m.store (3, panel) == flashRing::Saved::unchanged);
    panel[cutoff] = 666.0f;
    CHECK (m.store (4, panel) == flashRing::Saved::written);
    std::memcpy (loaded, panel, sizeof loaded);
    loaded[cutoff] = 0.0f;
    loaded[volume] = 0.9f;
    CHECK (m.load (3, loaded) && loaded[cutoff] == 555.0f && loaded[volume] == 0.9f);
    CHECK (m.load (4, loaded) && loaded[cutoff] == 666.0f);
    CHECK (! m.load (5, loaded));
    CHECK (m.remember (3) == flashRing::Saved::written && m.lastSlot() == 3);
    CHECK (m.remember (3) == flashRing::Saved::unchanged);
    CHECK (m.remember (31) == flashRing::Saved::written && m.lastSlot() == 31);

    // Cut off mid-page, and between the erase at a wrap and its page: the version before stays.
    auto storeCut = [&] (float value, bool afterErase)
    {
        panel[cutoff] = value;
        auto ring = m.slot (3);
        ring.load();
        const uint32_t page = ring.at (ring.next);
        FakeFlash::cutAfterErase = afterErase;
        m.store (3, panel);
        FakeFlash::cutAfterErase = false;
        if (! afterErase)
            std::memset (FakeFlash::bytes + page + 40, 0xFF, 100);
    };
    int cuts = 0;
    for (int n = 0; n < 70; ++n)
    {
        panel[cutoff] = 1000.0f + float (n);
        CHECK (m.store (3, panel) == flashRing::Saved::written);
        CHECK (m.load (3, loaded) && loaded[cutoff] == 1000.0f + float (n));
        const int next = [&] { auto r = m.slot (3); r.load(); return r.next; }();
        const bool atWrap = next % flashRing::pagesPerSector == 0;
        if (atWrap || n % 23 == 0)
        {
            storeCut (2.0f, atWrap);
            CHECK (m.load (3, loaded) && loaded[cutoff] == 1000.0f + float (n));
            cuts += atWrap ? 100 : 1;
        }
    }
    CHECK (cuts / 100 >= 1 && cuts % 100 >= 1); // both kinds of cut happened
    CHECK (m.load (3, loaded) && loaded[cutoff] == 1069.0f);
    CHECK (m.load (4, loaded) && loaded[cutoff] == 666.0f && m.lastSlot() == 31);
    CHECK (FakeFlash::bytes[offset - 1] == 0x5A && FakeFlash::bytes[offset + patches::bytes] == 0x5A);

    // After eraseAhead, the patches survive and the next 16 stores to a slot erase nothing.
    m.eraseAhead();
    CHECK (m.load (3, loaded) && loaded[cutoff] == 1069.0f && m.lastSlot() == 31);
    const int erases = FakeFlash::erases;
    for (int n = 0; n < 16; ++n)
    {
        panel[cutoff] = 2000.0f + float (n);
        CHECK (m.store (3, panel) == flashRing::Saved::written);
    }
    CHECK (FakeFlash::erases == erases);
}

int main()
{
    guiLines_();
    arpeggio_();
    sequencer_();
    midiParser();
    scheduler();
    control();
    params_();
    webSocket();
    synth();
    pages_();
    patchButtons_();
    flashRing_();
    patchMemory_();
    std::printf (failures ? "%d check(s) failed\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
