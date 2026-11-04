// Turns a MIDI byte stream (DIN) into channel messages. Handles running
// status, skips system exclusive, and lets real-time bytes through without
// breaking the message they land in.
#pragma once
#include <cstdint>

struct MidiParser
{
    // Returns true when message holds a complete channel message: status, then
    // one or two data bytes (the third is 0 for one-byte messages).
    bool feed (uint8_t byte, uint8_t message[3])
    {
        if (byte >= 0xF8)
            return false; // real time: clock, start, stop...
        if (byte >= 0xF0)
        {
            status = 0; // system common and sysex cancel running status
            inSysex = byte == 0xF0;
            return false;
        }
        if (byte & 0x80)
        {
            status = byte;
            inSysex = false;
            count = 0;
            return false;
        }
        if (inSysex || status == 0)
            return false;

        data[count++] = byte;
        uint8_t needed = (status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0 ? 1 : 2;
        if (count < needed)
            return false;
        message[0] = status;
        message[1] = data[0];
        message[2] = needed == 2 ? data[1] : 0;
        count = 0;
        return true;
    }

private:
    uint8_t status = 0, data[2] {}, count = 0;
    bool inSysex = false;
};
