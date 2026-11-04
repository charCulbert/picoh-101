// The 32 patches, one per key of the panel's keyboard. Each is a ring of its own over 2 sectors, so a store
// cut off by power loss leaves the slot's version before; the slot loaded or stored last is in a ring below
// them. A patch is every parameter's value but the playing ones.
#pragma once
#include "flash_ring.h"
#include "paramKnobs.generated.h"
#include "params.h"

namespace patches
{
constexpr int slots = 32, slotSectors = 2, lastSectors = 2;
constexpr uint32_t bytes = uint32_t (slots * slotSectors + lastSectors) * flashRing::sectorBytes;
static_assert (params::count * sizeof (float) <= sizeof (flashRing::Page::data));

inline bool kept (int id)
{
    return id != P_VOLUME && id != P_TRANSPOSE && id != P_ARPEGGIO && id != P_HOLD;
}

template <class Flash>
struct Memory
{
    using Ring = flashRing::Ring<Flash>;
    uint32_t offset; // of the lowest sector

    Ring slot (int i) const
    {
        return { offset + uint32_t (lastSectors + i * slotSectors) * flashRing::sectorBytes, slotSectors,
                 { 'P', 'A', '0', '1' }, sizeof (float) };
    }
    Ring last() const { return { offset, lastSectors, { 'P', 'L', '0', '1' }, 1 }; }

    // The kept values into values (params::count of them); false for an empty slot. A parameter newer than
    // the patch takes its default.
    bool load (int i, float values[]) const
    {
        Ring ring = slot (i);
        const flashRing::Page* p = ring.load();
        if (! p)
            return false;
        float saved[params::count];
        const int count = p->count < params::count ? p->count : params::count;
        std::memcpy (saved, p->data, size_t (count) * sizeof (float));
        for (int id = 0; id < params::count; ++id)
            if (kept (id))
                values[id] = id < count ? saved[id] : params::table[id].initial;
        return true;
    }

    flashRing::Saved store (int i, const float values[]) const
    {
        float patch[params::count];
        for (int id = 0; id < params::count; ++id)
            patch[id] = kept (id) ? values[id] : params::table[id].initial;
        Ring ring = slot (i);
        ring.load();
        return ring.write (patch, params::count);
    }

    // -1 for none.
    int lastSlot() const
    {
        Ring ring = last();
        const flashRing::Page* p = ring.load();
        return p && p->count == 1 && p->data[0] < slots ? p->data[0] : -1;
    }

    flashRing::Saved remember (int i) const
    {
        Ring ring = last();
        ring.load();
        const uint8_t slot = uint8_t (i);
        return ring.write (&slot, 1);
    }
};
} // namespace patches
