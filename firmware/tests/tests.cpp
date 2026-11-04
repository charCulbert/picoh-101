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
    static inline uint8_t bytes[8 * flashRing::sectorBytes];
    static inline int erases = 0, programs = 0;
    static const uint8_t* at (uint32_t offset) { return bytes + offset; }
    static bool write (uint32_t offset, const uint8_t* page, bool eraseFirst)
    {
        if (eraseFirst)
        {
            std::memset (bytes + (offset & ~(flashRing::sectorBytes - 1)), 0xFF, flashRing::sectorBytes);
            ++erases;
        }
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
    flashRing_();
    std::printf (failures ? "%d check(s) failed\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
