// What the Pico keeps in flash, at the top (of what it maps, below): the sequence in the last 4 sectors (64 saves to a round, so a
// sector is erased once every 16 saves), the patches in the 264 KB below (patches.h). Core 0 writes,
// pausing only itself: core 1 never reads flash once it renders (tools/core1-in-ram.py). Core 0's
// interrupts wait, all but DIN's (din_midi.cpp), which runs from RAM and keeps every byte.
#pragma once
#include <algorithm>
#include "DSP/picohDSP_Sequencer.h"
#include "board.h"
#include "flash_ring.h"
#include "patches.h"
#include "hardware/flash.h"
#include "hardware/irq.h"
#include "hardware/structs/nvic.h"
#include "hardware/sync.h"
#if PICO_RP2350
#include "hardware/structs/qmi.h"
#endif

extern "C" char __flash_binary_end;

namespace flashSaves
{
// Offsets here are as the program sees flash, from XIP_BASE. From a UF2 for both chips, the RP2350's
// bootrom runs this program from further up and maps it there (QMI ATRANS0); writes go to the flash's own.
inline uint32_t mappedFrom()
{
#if PICO_RP2350
    return (qmi_hw->atrans[0] & QMI_ATRANS0_BASE_BITS) * FLASH_SECTOR_SIZE;
#else
    return 0;
#endif
}

inline uint32_t mappedBytes()
{
#if PICO_RP2350
    const uint32_t size = (qmi_hw->atrans[0] & QMI_ATRANS0_SIZE_BITS) >> QMI_ATRANS0_SIZE_LSB;
    return std::min (size * FLASH_SECTOR_SIZE, PICO_FLASH_SIZE_BYTES - mappedFrom());
#else
    return PICO_FLASH_SIZE_BYTES;
#endif
}

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
        const uint32_t lowest = eraseFirst ? offset & ~(FLASH_SECTOR_SIZE - 1) : offset;
        if (! audio::flashFree() || lowest < programEnd || offset % FLASH_PAGE_SIZE
            || offset + FLASH_PAGE_SIZE > mappedBytes())
            return false;
        const uint32_t at = mappedFrom() + offset;
        // Every interrupt of core 0 but DIN's waits. Not with the SDK's irq_set_enabled: it drops what's pending.
        constexpr int words = (NUM_IRQS + 31) / 32;
        io_rw_32* iser = reinterpret_cast<io_rw_32*> (&nvic_hw->iser);
        io_rw_32* icer = reinterpret_cast<io_rw_32*> (&nvic_hw->icer);
        uint32_t enabled[words];
        const uint32_t save = save_and_disable_interrupts();
        for (int i = 0; i < words; ++i)
        {
            enabled[i] = iser[i];
            icer[i] = enabled[i] & ~(i == UART1_IRQ / 32 ? 1u << UART1_IRQ % 32 : 0u);
        }
        restore_interrupts (save);
        if (eraseFirst)
            flash_range_erase (at & ~(FLASH_SECTOR_SIZE - 1), FLASH_SECTOR_SIZE);
        if (page)
            flash_range_program (at, page, FLASH_PAGE_SIZE);
        for (int i = 0; i < words; ++i)
            iser[i] = enabled[i];
        return true;
    }
};
using Ring = flashRing::Ring<PicoFlash>;

constexpr int sequenceSectors = 4;
inline const uint32_t sequenceOffset = mappedBytes() - sequenceSectors * FLASH_SECTOR_SIZE;
inline const uint32_t patchesOffset = sequenceOffset - patches::bytes;

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
