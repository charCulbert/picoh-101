// Core 1: renders the synth a block at a time into one of two buffers while DMA plays the other to I2S.
#include "board.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "i2s.pio.h"
#include "trace.h"

// Trace stamps in the scheduler: each event applied, each piece rendered.
namespace
{
bool tracingBlock = false;
}
#define RENDER_TRACE_APPLY(event, sample)                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (tracingBlock)                                                                                              \
            trace::ring.stampWide (trace::APPLY, (event).type,                                                         \
                                   (event).type == EVENT_PARAM ? uint16_t ((event).param.id)                           \
                                                               : uint16_t ((event).note.key),                          \
                                   (event).timeUs, uint32_t (sample));                                                 \
    } while (0)
#define RENDER_TRACE_PIECE(piece, from, until, begin)                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        if (tracingBlock)                                                                                              \
            trace::ring.stamp ((begin) ? trace::RENDER_BEGIN : trace::RENDER_END, uint8_t (piece),                     \
                               uint16_t ((begin) ? (from) : (until)));                                                 \
    } while (0)
#include "event_scheduler.h"
#include "flash_saves.h"
#include "synth_host.h"

alignas (8) static uint8_t traceBytes[trace::ringBytes];
trace::Ring trace::ring { traceBytes };
trace::Core0Log trace::core0;
trace::Ring* trace::dspRing = nullptr;

namespace
{
uint32_t dmaBuffers[2][audio::blockFrames]; // one stereo frame per word: left in the top half
float left[audio::blockFrames], right[audio::blockFrames];
int dmaChannel;
volatile int fillIndex;   // the buffer the render loop fills next
volatile bool filled;     // whether it has finished filling it
queue_t* events;
SynthHost synth;

std::atomic<uint32_t> busiestUs { 0 }, totalUs { 0 }, blockCount { 0 }, underruns { 0 };

// The sequencer's state for core 0, packed; and a newly loaded sequence, handed over once to be saved.
std::atomic<uint32_t> sequencerPacked { 0 };
picohDSP::Sequence toSave;
std::atomic<bool> savePending { false };
uint32_t savedVersion = 0;
std::atomic<bool> rendering { false }; // core 1 is in renderLoop

void publishSequencer()
{
    const Synth::SequencerState s = synth.sequencerState();
    sequencerPacked.store (uint32_t (s.mode) | uint32_t (s.position) << 2 | uint32_t (s.length) << 9
                               | uint32_t (s.keyTranspose + 128) << 16,
                           std::memory_order_relaxed);
    if (synth.sequenceVersion() != savedVersion && ! savePending.load (std::memory_order_acquire))
    {
        savedVersion = synth.sequenceVersion();
        toSave = synth.sequence();
        savePending.store (true, std::memory_order_release);
    }
}

// A buffer finished playing: start the one just filled, hand the other back.
void __isr dmaFinished()
{
    dma_hw->ints1 = 1u << dmaChannel;
    if (! filled)
        underruns.fetch_add (1, std::memory_order_relaxed);
    dma_channel_set_read_addr (dmaChannel, dmaBuffers[fillIndex], true);
    fillIndex ^= 1;
    filled = false;
}

void startI2s()
{
    PIO pio;
    uint sm, offset;
    // A PIO block that reaches all three pins.
    const unsigned lowest = std::min (audio::dataPin, audio::bclkPin);
    const unsigned highest = std::max (audio::dataPin, audio::lrclkPin);
    hard_assert (pio_claim_free_sm_and_add_program_for_gpio_range (&audio_i2s_program, &pio, &sm, &offset, lowest,
                                                                   highest - lowest + 1, true));
    pio_gpio_init (pio, audio::dataPin);
    pio_gpio_init (pio, audio::bclkPin);
    pio_gpio_init (pio, audio::lrclkPin);
    audio_i2s_program_init (pio, sm, offset, audio::dataPin, audio::bclkPin);
    // 2 PIO cycles a bit, 32 bits a stereo frame.
    pio_sm_set_clkdiv (pio, sm, float (clock_get_hz (clk_sys)) / float (audio::sampleRate * 64));

    dmaChannel = dma_claim_unused_channel (true);
    dma_channel_config config = dma_channel_get_default_config (dmaChannel);
    channel_config_set_transfer_data_size (&config, DMA_SIZE_32);
    channel_config_set_read_increment (&config, true);
    channel_config_set_write_increment (&config, false);
    channel_config_set_dreq (&config, pio_get_dreq (pio, sm, true));
    dma_channel_configure (dmaChannel, &config, &pio->txf[sm], dmaBuffers[0], audio::blockFrames, false);

    // On this core, so the interrupt never waits on core 0.
    dma_channel_set_irq1_enabled (dmaChannel, true);
    irq_set_exclusive_handler (DMA_IRQ_1, dmaFinished);
    irq_set_enabled (DMA_IRQ_1, true);

    pio_sm_set_enabled (pio, sm, true);
    fillIndex = 1;
    filled = true; // both buffers start silent
    dma_channel_start (dmaChannel);
}

inline uint32_t toFrame (float l, float r)
{
    auto toInt = [] (float x)
    {
        x = x < -1.0f ? -1.0f : x > 1.0f ? 1.0f : x; // clip rather than wrap
        return uint32_t (uint16_t (int16_t (lrintf (x * 32767.0f))));
    };
    return toInt (l) << 16 | toInt (r);
}

[[noreturn]] void renderLoop (EventScheduler<SynthHost>& scheduler);

void run()
{
    // Flush denormals to zero on this core, so decaying filters and envelopes stay fast.
    uint32_t fpscr;
    asm volatile ("vmrs %0, fpscr" : "=r"(fpscr));
    asm volatile ("vmsr fpscr, %0" : : "r"(fpscr | (1u << 24)));

    synth.prepare (float (audio::sampleRate), audio::blockFrames);
    if (picohDSP::Sequence stored; flashSaves::readSequence (stored))
        synth.setSequence (stored);
    trace::ring.startCycleCounter (
        uint32_t (uint64_t (clock_get_hz (clk_sys)) * audio::blockFrames / audio::sampleRate));
    // High while this core works on a block, for a scope.
    gpio_init (audio::busyPin);
    gpio_set_dir (audio::busyPin, GPIO_OUT);
    EventScheduler<SynthHost> scheduler (synth, audio::sampleRate, audio::blockFrames * 1000000u / audio::sampleRate);
    startI2s();
    rendering.store (true, std::memory_order_release);
    renderLoop (scheduler);
}

// Everything from here on runs from RAM and reads nothing in flash (tools/core1-in-ram.py), so flash can
// be written while it plays.
[[noreturn]] __attribute__ ((noinline)) void renderLoop (EventScheduler<SynthHost>& scheduler)
{
    auto pop = [] (Event& e) { return queue_try_remove (events, &e); };
    while (true)
    {
        while (filled)
            tight_loop_contents();
        uint32_t startUs = time_us_32();
        gpio_put (audio::busyPin, true);
        uint32_t* out = dmaBuffers[fillIndex];
        tracingBlock =
            trace::ring.active() && trace::ring.blockStart (startUs, queue_get_level (events), audio::blockFrames);
        trace::dspRing = tracingBlock ? &trace::ring : nullptr;

        scheduler.render (pop, startUs, left, right, audio::blockFrames);
        trace::dspRing = nullptr;
        for (int i = 0; i < audio::blockFrames; ++i)
            out[i] = toFrame (left[i], right[i]);
        if (tracingBlock)
        {
            trace::ring.stamp (trace::OUT_END, 0, 0);
            trace::ring.blockEnd (scheduler.pieces, scheduler.applied);
        }
        filled = true;
        gpio_put (audio::busyPin, false);
        publishSequencer();

        uint32_t tookUs = time_us_32() - startUs;
        totalUs.fetch_add (tookUs, std::memory_order_relaxed);
        blockCount.fetch_add (1, std::memory_order_relaxed);
        if (tookUs > busiestUs.load (std::memory_order_relaxed))
            busiestUs.store (tookUs, std::memory_order_relaxed);
    }
}
} // namespace

namespace audio
{
void start (queue_t* eventQueue)
{
    events = eventQueue;
    multicore_launch_core1 (run);
    for (uint32_t t0 = time_us_32(); ! rendering.load (std::memory_order_acquire) && time_us_32() - t0 < 1000000;)
        tight_loop_contents();
}

bool flashFree()
{
    return rendering.load (std::memory_order_acquire);
}

Sequencer sequencer()
{
    const uint32_t p = sequencerPacked.load (std::memory_order_relaxed);
    return { int (p & 3), int (p >> 2 & 0x7F), int (p >> 9 & 0x7F), int (p >> 16 & 0xFF) - 128 };
}

bool takeSequence (picohDSP::Sequence& sequence)
{
    if (! savePending.load (std::memory_order_acquire))
        return false;
    sequence = toSave;
    savePending.store (false, std::memory_order_release);
    return true;
}

Load takeLoad()
{
    constexpr float blockUs = blockFrames * 1e6f / sampleRate;
    uint32_t blocks = blockCount.exchange (0, std::memory_order_relaxed);
    uint32_t total = totalUs.exchange (0, std::memory_order_relaxed);
    uint32_t busiest = busiestUs.exchange (0, std::memory_order_relaxed);
    Load load;
    load.average = blocks ? 100.0f * float (total) / float (blocks) / blockUs : 0.0f;
    load.busiest = 100.0f * float (busiest) / blockUs;
    load.underruns = underruns.exchange (0, std::memory_order_relaxed);
    return load;
}
} // namespace audio
