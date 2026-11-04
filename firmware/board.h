// Pins, audio settings, and the parts each core runs.
#pragma once
#include <cstdint>
#include "DSP/picohDSP_Sequencer.h"
#include "controls.h"
#include "gui_protocol.h"
#include "pico/multicore.h"
#include "pico/util/queue.h"

namespace audio
{
constexpr uint32_t sampleRate = 48000;
#if PICO_RP2040
constexpr int blockFrames = 64; // 1.33 ms: at 32 the busiest block passes 92%
#else
constexpr int blockFrames = 32; // 0.67 ms
#endif
// BCLK and LRCLK are the PIO program's side-set pins, which must be consecutive; DIN can be any pin.
constexpr unsigned bclkPin = 19;
constexpr unsigned lrclkPin = 20;
constexpr unsigned dataPin = 21;
static_assert (lrclkPin == bclkPin + 1, "LRCLK must be the pin after BCLK");
constexpr unsigned busyPin = 15; // high while the audio core works on a block (optional, for a scope)

// Starts the synth and I2S on core 1. Events come in through the queue.
void start (queue_t* events);

struct Load
{
    float average, busiest; // percent of a block's time spent rendering it
    uint32_t underruns;     // blocks that weren't ready when their turn came
    uint32_t late, latestUs; // events applied after their sample, and the latest by how long
};
// Since the last call.
Load takeLoad();

struct Sequencer
{
    int mode, position, length, keyTranspose; // mode: 0 stopped, 1 loading, 2 playing
};
Sequencer sequencer();
bool takeSequence (picohDSP::Sequence&); // a sequence just loaded, to save
bool flashFree(); // core 1 is rendering, from RAM only: flash can be written
} // namespace audio

namespace usb
{
void start (Guis&);
void poll();  // MIDI, and the GUI lines over serial or SysEx
void sendText (const char* line, const Gui* skip);  // to the GUIs connected over USB
void log (const char* format, ...);            // "# ..." on the serial port, for people
} // namespace usb

namespace din
{
constexpr unsigned rxPin = 9; // UART1 RX, from the optocoupler
void start();
void poll (Control&);
} // namespace din

namespace web
{
// Joins the network named at build time (WIFI_SSID), or makes its own
// (picoh-101) if none was. Returns false if the radio didn't start.
bool start (Guis&);
void poll();
void sendText (const char* line, const Gui* skip);  // to the GUIs connected over Wi-Fi
} // namespace web
