// The sequence in the last sector of flash, so it survives power off: a tag, then Sequence's 101 bytes.
#pragma once
#include <cstring>
#include "DSP/picohDSP_Sequencer.h"
#include "hardware/flash.h"
#include "pico/flash.h"

namespace sequenceFlash
{
constexpr uint32_t offset = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE;
constexpr uint8_t tag[4] = { 'S', 'Q', '0', '1' };

inline bool read (picohDSP::Sequence& sequence)
{
    const auto* stored = reinterpret_cast<const uint8_t*> (XIP_BASE + offset);
    if (std::memcmp (stored, tag, sizeof tag) != 0)
        return false;
    std::memcpy (&sequence, stored + sizeof tag, sizeof sequence);
    return sequence.length <= picohDSP::Sequence::maxSteps;
}

// Core 0. Pauses core 1 while flash is written, a few tens of milliseconds.
inline bool write (const picohDSP::Sequence& sequence)
{
    static uint8_t page[FLASH_PAGE_SIZE];
    static_assert (sizeof tag + sizeof (picohDSP::Sequence) <= FLASH_PAGE_SIZE);
    std::memset (page, 0xFF, sizeof page);
    std::memcpy (page, tag, sizeof tag);
    std::memcpy (page + sizeof tag, &sequence, sizeof sequence);
    auto program = [] (void*)
    {
        flash_range_erase (offset, FLASH_SECTOR_SIZE);
        flash_range_program (offset, page, FLASH_PAGE_SIZE);
    };
    return flash_safe_execute (program, nullptr, 100) == PICO_OK;
}
} // namespace sequenceFlash
