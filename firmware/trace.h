#pragma once
// Cycle-counter stamps from the audio core, kept in RAM and read over USB serial by tools/trace.py (a
// Perfetto file), with the flash cache's counters and core 0's work on the same timeline.
//
// Records are 8 bytes: cycles u32 (core 1's DWT cycle counter), point u8, a u8, b u16. A point of 0x80 or
// more marks a wide record, 16 bytes, with x u32 and y u32 after those. Per block:
//   BLOCK_START (wide)  a frames, b events waiting, x blockUs (time_us_32), y block number
//   CAL                 a second cycle read right after BLOCK_START: the cost of one stamp
//   APPLY (wide)        a event type, b key or parameter id, x the event's timeUs, y the sample it applies at
//   RENDER_BEGIN/END    a piece, b first sample / the sample it stops before
//   OUT_END             the block is converted into the I2S buffer
//   BLOCK_END (wide)    a pieces, b events applied, x flash cache hits, y flash cache accesses (this block;
//                       both cores' accesses: the cache is shared)
//
// TRACE_DSP adds the synth's points (picohDSP_TraceHook.h, via trace_dsp.h): 8-byte records, b the arg.
//
// One writer, the audio core, so nothing locks. The control core changes the state only through control(),
// which stops the writer (state idle) before it touches anything else and starts it last. It reads the ring
// only while frozen.
//
// Modes: capture N blocks from the start of the ring, freezing after N blocks or when the next block might
// not fit. Or watch: record round the ring until a block takes more than the threshold, then record `post`
// more blocks and freeze. The ring wraps only at a block start, and `tail` is kept on the oldest whole
// block, so a frozen ring reads out as whole blocks in order.
#include <atomic>
#include <cstdint>
#include "DSP/picohDSP_TraceHook.h"
#include "hardware/structs/xip_ctrl.h"

namespace trace
{
enum Point : uint8_t
{
    CAL = 0x01,
    RENDER_BEGIN = 0x02,
    RENDER_END = 0x03,
    OUT_END = 0x04,
    BLOCK_START = 0x80,
    APPLY = 0x81,
    BLOCK_END = 0x82,
};
// The synth's points, from 0x10 in picohDSP_TraceHook.h's order (tools/trace.py reads the same list).
namespace dsp
{
enum Point : uint8_t
{
    beforeFirst = 0x0F,
    PICOH_TRACE_POINTS (PICOH_TRACE_ENUM) afterLast
};
static_assert (afterLast <= 0x80, "synth points must stay below BLOCK_START");
} // namespace dsp

enum State : uint8_t
{
    idle,
    capturing,
    watching,
    triggered,
    frozen
};
enum Mode : uint32_t
{
    stopMode = 0,
    blocksMode = 1,
    overMode = 2
};

// A block's records are at most blockMaxBytes. With the synth's stamps a render piece writes about 3
// records, 2 more per voice and 3 more per voice that renders.
constexpr uint32_t ringBytes = 32768, blockMaxBytes = 1024;
constexpr uint32_t noBlock = 0xFFFFFFFFu;

// Core 1's cycle counter (Armv8-M DWT). Each core has its own.
inline uint32_t cycleCounter()
{
    return *reinterpret_cast<volatile uint32_t*> (0xE0001004u);
}

class Ring
{
public:
    constexpr explicit Ring (uint8_t* storage)
        : bytes (storage)
    {}

    // The audio core, once at start: turns its cycle counter on. cyclesPerBlock: a block's time in cycles.
    void startCycleCounter (uint32_t cyclesPerBlock)
    {
        *reinterpret_cast<volatile uint32_t*> (0xE000EDFCu) |= 1u << 24; // DEMCR.TRCENA
        *reinterpret_cast<volatile uint32_t*> (0xE0001000u) |= 1u;       // DWT_CTRL.CYCCNTENA
        blockBudget = cyclesPerBlock;
    }

    // The control core: arm or stop. value: blocks to capture, or the threshold in thousandths of a
    // block's time.
    void control (uint32_t mode, uint32_t value, uint32_t post)
    {
        State was = state.load (std::memory_order_relaxed);
        state.store (idle, std::memory_order_release);
        limit = 0;
        if (mode == stopMode)
        {
            if (was != idle)
                state.store (frozen, std::memory_order_release);
            return;
        }
        if ((mode != blocksMode && mode != overMode) || ! value)
            return;
        pos = end = tail = 0;
        lapped = false;
        blockCount = dropped = 0;
        triggerBlock = noBlock;
        target = value;
        threshold = value > 1000 ? 0xFFFFFFFFu : uint32_t (uint64_t (value) * blockBudget / 1000);
        postBlocks = post;
        announced = false;
        state.store (mode == blocksMode ? capturing : watching, std::memory_order_release);
    }

    // The audio core, once per block: whether to stamp this block.
    bool active() const
    {
        State s = state.load (std::memory_order_acquire);
        return s == capturing || s == watching || s == triggered;
    }

    // The audio core, first stamp of an active block. False if the capture just froze.
    bool blockStart (uint32_t blockUs, uint32_t waiting, uint32_t frames)
    {
        uint32_t cycles = cycleCounter();
        if (pos + blockMaxBytes > ringBytes)
        {
            if (state.load (std::memory_order_relaxed) == capturing)
            {
                freeze();
                return false;
            }
            end = pos;
            pos = tail = 0;
            lapped = true;
        }
        // Keep the oldest whole block clear of the one about to be written.
        while (lapped && (tail < pos + blockMaxBytes || point (tail) != BLOCK_START))
        {
            tail += point (tail) >= 0x80 ? 16 : 8;
            if (tail >= end)
                lapped = false;
        }
        blockCycles = cycles;
        limit = pos + blockMaxBytes;
        // The flash cache's counters, from zero for this block.
        xip_ctrl_hw->ctr_hit = 0;
        xip_ctrl_hw->ctr_acc = 0;
        wide (cycles, BLOCK_START, uint8_t (frames), uint16_t (waiting), blockUs, blockCount);
        put (cycleCounter(), CAL, 0, 0);
        return true;
    }

    void stamp (Point p, uint8_t a, uint16_t b) { put (cycleCounter(), p, a, b); }
    void stamp (dsp::Point p, uint16_t b) { put (cycleCounter(), Point (p), 0, b); }
    void stampWide (Point p, uint8_t a, uint16_t b, uint32_t x, uint32_t y) { wide (cycleCounter(), p, a, b, x, y); }

    // The audio core, last stamp of an active block; tests the trigger.
    void blockEnd (uint8_t pieces, uint16_t applied)
    {
        uint32_t cycles = cycleCounter();
        wide (cycles, BLOCK_END, pieces, applied, xip_ctrl_hw->ctr_hit, xip_ctrl_hw->ctr_acc);
        State s = state.load (std::memory_order_relaxed);
        uint32_t number = blockCount++;
        if (s == capturing && blockCount >= target)
            freeze();
        else if (s == watching && cycles - blockCycles > threshold)
        {
            triggerBlock = number;
            if (postBlocks)
                state.store (triggered, std::memory_order_release);
            else
                freeze();
        }
        else if (s == triggered && number - triggerBlock >= postBlocks)
            freeze();
    }

    // The control core: true once after the audio core froze a capture.
    bool frozeSinceAsked()
    {
        if (announced || state.load (std::memory_order_acquire) != frozen)
            return false;
        announced = true;
        return true;
    }

    State current() const { return state.load (std::memory_order_acquire); }
    uint32_t used() const { return (lapped ? end - tail : 0) + pos; }
    uint32_t blocks() const { return blockCount; }
    uint32_t trigger() const { return triggerBlock; }
    uint32_t droppedRecords() const { return dropped; }
    uint32_t budget() const { return blockBudget; }

    // The control core, frozen only: copies up to `count` bytes from `offset` in the capture, oldest first.
    uint32_t read (uint32_t offset, uint8_t* out, uint32_t count) const
    {
        if (current() != frozen)
            return 0;
        uint32_t total = used();
        if (offset >= total)
            return 0;
        if (count > total - offset)
            count = total - offset;
        uint32_t older = lapped ? end - tail : 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            uint32_t at = offset + i;
            out[i] = bytes[at < older ? tail + at : at - older];
        }
        return count;
    }

private:
    uint8_t point (uint32_t at) const { return bytes[at + 4]; }

    void put (uint32_t cycles, Point p, uint8_t a, uint16_t b)
    {
        if (pos + 8 > limit)
        {
            ++dropped;
            return;
        }
        uint32_t* to = reinterpret_cast<uint32_t*> (bytes + pos);
        to[0] = cycles;
        to[1] = uint32_t (p) | uint32_t (a) << 8 | uint32_t (b) << 16;
        pos += 8;
    }

    void wide (uint32_t cycles, Point p, uint8_t a, uint16_t b, uint32_t x, uint32_t y)
    {
        if (pos + 16 > limit)
        {
            ++dropped;
            return;
        }
        uint32_t* to = reinterpret_cast<uint32_t*> (bytes + pos);
        to[0] = cycles;
        to[1] = uint32_t (p) | uint32_t (a) << 8 | uint32_t (b) << 16;
        to[2] = x;
        to[3] = y;
        pos += 16;
    }

    void freeze()
    {
        limit = 0;
        state.store (frozen, std::memory_order_release);
    }

    uint8_t* const bytes;
    std::atomic<State> state { idle };
    // Written by the audio core while active, by control() while idle.
    uint32_t pos = 0, limit = 0, end = 0, tail = 0, blockCount = 0, dropped = 0, triggerBlock = noBlock;
    uint32_t blockCycles = 0, target = 0, threshold = 0, postBlocks = 0, blockBudget = 200000;
    bool lapped = false;
    bool announced = true; // the control core only
};

// The control core's work, for the same timeline: what it did (Core0Kind) and from when to when, in
// microseconds (the timer both cores read). Spans shorter than minUs aren't kept. Written while the ring
// above is recording, frozen with it.
enum Core0Kind : uint8_t
{
    usbPoll = 1,
    dinPoll = 2,
    webPoll = 3,
    pageLine = 4,
    loadReport = 5
};

class Core0Log
{
public:
    static constexpr uint32_t entries = 4096, minUs = 5;
    struct Span
    {
        uint32_t startUs;
        uint16_t lengthUs;
        uint8_t kind, arg;
    };

    // The control core: one span, if the ring is recording.
    void span (Core0Kind kind, uint32_t startUs, uint32_t endUs, uint8_t arg, const Ring& ring)
    {
        if (! ring.active() || endUs - startUs < minUs)
            return;
        Span& s = spans[next++ % entries];
        s.startUs = startUs;
        s.lengthUs = uint16_t (endUs - startUs > 0xFFFF ? 0xFFFF : endUs - startUs);
        s.kind = kind;
        s.arg = arg;
    }

    void clear() { next = 0; }
    uint32_t count() const { return next < entries ? next : entries; }
    // Oldest first.
    const Span& at (uint32_t i) const { return spans[(next < entries ? i : next + i) % entries]; }

private:
    Span spans[entries];
    uint32_t next = 0;
};

// The ring, core 0's log, and the ring the synth's stamps go to (set per recorded block).
extern Ring ring;
extern Core0Log core0;
extern Ring* dspRing;
} // namespace trace
