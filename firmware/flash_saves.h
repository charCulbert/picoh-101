// What the Pico keeps in flash, at the top: the sequence in the last 4 sectors (64 saves to a round, so a
// sector is erased once every 16 saves), the patches in the 264 KB below (patches.h). Core 0 writes,
// pausing only itself: core 1 never reads flash once it renders (tools/core1-in-ram.py).
#pragma once
#include "DSP/picohDSP_Sequencer.h"
#include "board.h"
#include "flash_ring.h"
#include "patches.h"
#include "hardware/flash.h"
#include "pico/flash.h"

extern "C" char __flash_binary_end;

namespace flashSaves
{
struct PicoFlash
{
    static const uint8_t* at (uint32_t offset) { return reinterpret_cast<const uint8_t*> (XIP_BASE + offset); }

    static bool write (uint32_t offset, const uint8_t* page, bool eraseFirst)
    {
        return run (offset, page, eraseFirst);
    }

    static bool erase (uint32_t offset) { return run (offset, nullptr, true); }

    // Erases the sector first if asked, then programs the page (if any). Never into the program, nor while
    // core 1 might be running from flash.
    static bool run (uint32_t offset, const uint8_t* page, bool eraseFirst)
    {
        static_assert (flashRing::pageBytes == FLASH_PAGE_SIZE && flashRing::sectorBytes == FLASH_SECTOR_SIZE);
        const uint32_t programEnd = uint32_t (reinterpret_cast<uintptr_t> (&__flash_binary_end) - XIP_BASE);
        if (! audio::flashFree() || offset < programEnd || offset % FLASH_PAGE_SIZE
            || offset + FLASH_PAGE_SIZE > PICO_FLASH_SIZE_BYTES)
            return false;
        static struct
        {
            uint32_t offset;
            const uint8_t* page;
            bool eraseFirst;
        } job;
        job = { offset, page, eraseFirst };
        auto program = [] (void*)
        {
            if (job.eraseFirst)
                flash_range_erase (job.offset & ~(FLASH_SECTOR_SIZE - 1), FLASH_SECTOR_SIZE);
            if (job.page)
                flash_range_program (job.offset, job.page, FLASH_PAGE_SIZE);
        };
        return flash_safe_execute (program, nullptr, 100) == PICO_OK;
    }
};
using Ring = flashRing::Ring<PicoFlash>;

constexpr int sequenceSectors = 4;
constexpr uint32_t sequenceOffset = PICO_FLASH_SIZE_BYTES - sequenceSectors * FLASH_SECTOR_SIZE;
constexpr uint32_t patchesOffset = sequenceOffset - patches::bytes;

inline Ring sequences { sequenceOffset, sequenceSectors, { 'S', 'Q', '0', '2' }, 1 };
inline const patches::Memory<PicoFlash> patchMemory { patchesOffset };

// The sequence saved last.
inline bool readSequence (picohDSP::Sequence& sequence)
{
    Ring ring = sequences;
    const flashRing::Page* p = ring.load();
    if (! p || p->count != sizeof sequence)
        return false;
    std::memcpy (&sequence, p->data, sizeof sequence);
    return sequence.length <= picohDSP::Sequence::maxSteps;
}
} // namespace flashSaves
