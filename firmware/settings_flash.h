// The panel's settings in the 16 sectors below the sequence's. Each save is one 256-byte page, written
// into the next blank page; a sector is erased only when the saves come round to it again, so each is
// erased once every 256 saves (100,000 erases a sector: about 25 million saves). The newest page whose
// check matches wins, so a save cut off by power loss leaves the one before.
#pragma once
#include <cstdint>
#include <cstring>
#include "hardware/flash.h"
#include "pico/flash.h"
#include "sequence_flash.h"

namespace settingsFlash
{
constexpr int sectors = 16, pagesPerSector = FLASH_SECTOR_SIZE / FLASH_PAGE_SIZE, pages = sectors * pagesPerSector;
constexpr uint32_t offset = sequenceFlash::offset - sectors * FLASH_SECTOR_SIZE;
constexpr int maxValues = (FLASH_PAGE_SIZE - 12) / 4;

struct Record
{
    uint8_t tag[4];
    uint32_t serial; // up by one each save
    uint16_t count;  // values[0, count): by parameter id
    uint16_t check;
    float values[maxValues];
};
static_assert (sizeof (Record) == FLASH_PAGE_SIZE);
constexpr uint8_t tag[4] = { 'S', 'T', '0', '1' };

inline uint16_t checkOf (const Record& r)
{
    uint32_t sum = r.serial ^ r.count;
    const auto* bytes = reinterpret_cast<const uint8_t*> (r.values);
    for (size_t i = 0; i < r.count * sizeof (float); ++i)
        sum = sum * 31 + bytes[i];
    return uint16_t (sum ^ sum >> 16);
}

inline const Record* page (int index)
{
    return reinterpret_cast<const Record*> (XIP_BASE + offset + uint32_t (index) * FLASH_PAGE_SIZE);
}

inline bool valid (const Record& r)
{
    return std::memcmp (r.tag, tag, sizeof tag) == 0 && r.count <= maxValues && r.check == checkOf (r);
}

// The newest save, or null. next: the page the next save goes to.
inline const Record* newest (int& next)
{
    const Record* best = nullptr;
    int bestIndex = -1;
    for (int i = 0; i < pages; ++i)
        if (valid (*page (i)) && (! best || int32_t (page (i)->serial - best->serial) > 0))
        {
            best = page (i);
            bestIndex = i;
        }
    next = (bestIndex + 1) % pages;
    return best;
}

// Core 0. The page goes into the next slot; on a sector's first page, that sector is erased first.
inline bool write (const float* values, int count, uint32_t serial, int& next)
{
    static Record record;
    std::memset (&record, 0xFF, sizeof record);
    std::memcpy (record.tag, tag, sizeof tag);
    record.serial = serial;
    record.count = uint16_t (count < maxValues ? count : maxValues);
    std::memcpy (record.values, values, record.count * sizeof (float));
    record.check = checkOf (record);
    static int slot;
    slot = next;
    auto program = [] (void*)
    {
        const uint32_t at = offset + uint32_t (slot) * FLASH_PAGE_SIZE;
        if (slot % pagesPerSector == 0)
            flash_range_erase (at, FLASH_SECTOR_SIZE);
        flash_range_program (at, reinterpret_cast<const uint8_t*> (&record), FLASH_PAGE_SIZE);
    };
    const bool ok = flash_safe_execute (program, nullptr, 100) == PICO_OK;
    next = (next + 1) % pages;
    return ok;
}
} // namespace settingsFlash
