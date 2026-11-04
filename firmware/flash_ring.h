// A ring of 256-byte pages over whole flash sectors, for saves that survive power off. Each save goes into
// the next blank page. Sectors are erased at boot (eraseAhead), or failing that when the saves reach one
// still written; either way once per round of the ring. The newest page
// whose check matches wins, so a save cut off by power loss leaves the one before. A save equal to the
// newest is skipped. Flash: at (offset) to read, write (offset, page, eraseSectorFirst), erase (offset).
#pragma once
#include <cstdint>
#include <cstring>

namespace flashRing
{
constexpr uint32_t pageBytes = 256, sectorBytes = 4096, pagesPerSector = sectorBytes / pageBytes;

struct Page
{
    uint8_t tag[4];
    uint32_t serial; // up by one each save
    uint16_t count;  // units of data
    uint16_t check;
    uint8_t data[pageBytes - 12];
};
static_assert (sizeof (Page) == pageBytes);

enum class Saved
{
    written,
    unchanged,
    failed
};

template <class Flash>
struct Ring
{
    uint32_t offset; // of its first sector
    int sectors;
    uint8_t tag[4];
    int unit; // bytes in one unit of count
    int next = 0;
    const Page* newest = nullptr;

    int pages() const { return sectors * int (pagesPerSector); }
    uint32_t at (int index) const { return offset + uint32_t (index) * pageBytes; }
    const Page* page (int index) const { return reinterpret_cast<const Page*> (Flash::at (at (index))); }

    uint16_t checkOf (const Page& p) const
    {
        uint32_t sum = p.serial ^ p.count;
        for (size_t i = 0; i < size_t (p.count) * unit; ++i)
            sum = sum * 31 + p.data[i];
        return uint16_t (sum ^ sum >> 16);
    }

    bool valid (const Page& p) const
    {
        return std::memcmp (p.tag, tag, sizeof tag) == 0 && size_t (p.count) * unit <= sizeof p.data
               && p.check == checkOf (p);
    }

    // Finds the newest save (or none) and the page the next goes to.
    const Page* load()
    {
        newest = nullptr;
        int newestIndex = -1;
        for (int i = 0; i < pages(); ++i)
            if (valid (*page (i)) && (! newest || int32_t (page (i)->serial - newest->serial) > 0))
            {
                newest = page (i);
                newestIndex = i;
            }
        next = (newestIndex + 1) % pages();
        return newest;
    }

    Saved write (const void* data, int count)
    {
        if (newest && newest->count == count && std::memcmp (newest->data, data, size_t (count) * unit) == 0)
            return Saved::unchanged;
        static Page p;
        std::memset (&p, 0xFF, sizeof p);
        std::memcpy (p.tag, tag, sizeof tag);
        p.serial = newest ? newest->serial + 1 : 1;
        p.count = uint16_t (count);
        std::memcpy (p.data, data, size_t (count) * unit);
        p.check = checkOf (p);
        // A page that isn't blank mid-sector (a save cut off by power loss) moves the save on to the next sector.
        if (next % pagesPerSector != 0 && ! blank (next))
            next = (next / int (pagesPerSector) + 1) * int (pagesPerSector) % pages();
        const int index = next;
        next = (next + 1) % pages();
        const bool erase = index % pagesPerSector == 0 && ! sectorBlank (index / int (pagesPerSector));
        if (! Flash::write (at (index), reinterpret_cast<const uint8_t*> (&p), erase) || ! valid (*page (index)))
            return Saved::failed;
        newest = page (index);
        return Saved::written;
    }

    // At boot: erases every sector but the newest save's, so a save while playing only writes a page.
    void eraseAhead()
    {
        const int keep = newest ? int ((reinterpret_cast<const uint8_t*> (newest) - Flash::at (offset)) / sectorBytes) : -1;
        for (int s = 0; s < sectors; ++s)
            if (s != keep && ! sectorBlank (s))
                Flash::erase (at (s * int (pagesPerSector)));
    }

    bool sectorBlank (int sector) const
    {
        for (int i = 0; i < int (pagesPerSector); ++i)
            if (! blank (sector * int (pagesPerSector) + i))
                return false;
        return true;
    }

    bool blank (int index) const
    {
        const uint8_t* bytes = Flash::at (at (index));
        for (uint32_t i = 0; i < pageBytes; ++i)
            if (bytes[i] != 0xFF)
                return false;
        return true;
    }
};
} // namespace flashRing
