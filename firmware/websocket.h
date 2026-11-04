// The parts of WebSocket (RFC 6455) a small server needs: the accept key, reading masked frames, framing
// replies. No networking, so the desktop checks test it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ws
{
inline void sha1 (const uint8_t* data, size_t length, uint8_t out[20])
{
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    auto rotl = [] (uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
    uint64_t bits = uint64_t (length) * 8;
    size_t total = ((length + 8) / 64 + 1) * 64; // message, 0x80, zeros, 64-bit length
    for (size_t block = 0; block < total; block += 64)
    {
        uint32_t w[80];
        for (int i = 0; i < 64; ++i)
        {
            size_t at = block + size_t (i);
            uint8_t byte = at < length       ? data[at]
                           : at == length    ? 0x80
                           : at >= total - 8 ? uint8_t (bits >> (8 * (total - 1 - at)))
                                             : 0;
            if (i % 4 == 0)
                w[i / 4] = 0;
            w[i / 4] |= uint32_t (byte) << (24 - 8 * (i % 4));
        }
        for (int i = 16; i < 80; ++i)
            w[i] = rotl (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i)
        {
            uint32_t f, k;
            if (i < 20)
                f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40)
                f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60)
                f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else
                f = b ^ c ^ d, k = 0xCA62C1D6;
            uint32_t t = rotl (a, 5) + f + e + k + w[i];
            e = d, d = c, c = rotl (b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    for (int i = 0; i < 20; ++i)
        out[i] = uint8_t (h[i / 4] >> (24 - 8 * (i % 4)));
}

// Base64 of n bytes into out, which needs 4 * ((n + 2) / 3) + 1 chars.
inline void base64 (const uint8_t* in, size_t n, char* out)
{
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < n; i += 3)
    {
        uint32_t v = uint32_t (in[i]) << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        *out++ = abc[v >> 18 & 63];
        *out++ = abc[v >> 12 & 63];
        *out++ = i + 1 < n ? abc[v >> 6 & 63] : '=';
        *out++ = i + 2 < n ? abc[v & 63] : '=';
    }
    *out = 0;
}

// Sec-WebSocket-Accept for the browser's Sec-WebSocket-Key. out needs 29 chars.
inline void acceptKey (const char* key, size_t keyLength, char out[29])
{
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t joined[64 + sizeof guid];
    if (keyLength > 64)
        keyLength = 64;
    memcpy (joined, key, keyLength);
    memcpy (joined + keyLength, guid, sizeof guid - 1);
    uint8_t digest[20];
    sha1 (joined, keyLength + sizeof guid - 1, digest);
    base64 (digest, 20, out);
}

enum Opcode : uint8_t
{
    text = 1,
    binary = 2,
    close = 8,
    ping = 9,
    pong = 10
};

struct Frame
{
    Opcode opcode;
    bool final;
    uint8_t* payload; // unmasked in place
    size_t length;
    size_t size;      // header + payload: how many bytes of the buffer it used
};

// One frame from the start of buf; false if it doesn't hold a whole one yet. Unmasks in place.
inline bool readFrame (uint8_t* buf, size_t available, Frame& frame)
{
    if (available < 2)
        return false;
    size_t length = buf[1] & 0x7F, header = 2;
    if (length == 126)
    {
        if (available < 4)
            return false;
        length = size_t (buf[2]) << 8 | buf[3];
        header = 4;
    }
    else if (length == 127)
        return false; // no message here comes near 64 KB
    bool masked = buf[1] & 0x80;
    uint8_t* mask = buf + header;
    if (masked)
        header += 4;
    if (available < header + length)
        return false;
    frame.opcode = Opcode (buf[0] & 0x0F);
    frame.final = buf[0] & 0x80;
    frame.payload = buf + header;
    frame.length = length;
    frame.size = header + length;
    if (masked)
        for (size_t i = 0; i < length; ++i)
            frame.payload[i] ^= mask[i % 4];
    return true;
}

// The header of an unmasked frame (out needs 4 bytes); returns its length.
inline size_t frameHeader (Opcode opcode, size_t length, uint8_t out[4])
{
    out[0] = 0x80 | opcode;
    if (length < 126)
    {
        out[1] = uint8_t (length);
        return 2;
    }
    out[1] = 126;
    out[2] = uint8_t (length >> 8);
    out[3] = uint8_t (length);
    return 4;
}
} // namespace ws
