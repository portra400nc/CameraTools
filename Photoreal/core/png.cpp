#include "png.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace photoreal
{
    namespace
    {
        uint32_t crc32(const uint8_t *data, size_t size, uint32_t crc)
        {
            static const std::array<uint32_t, 256> table = [] {
                std::array<uint32_t, 256> t {};
                for (uint32_t n = 0; n < 256; n++)
                {
                    uint32_t c = n;
                    for (int k = 0; k < 8; k++)
                        c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    t[n] = c;
                }
                return t;
            }();
            crc = ~crc;
            for (size_t i = 0; i < size; i++)
                crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
            return ~crc;
        }

        void put32(std::string &out, uint32_t value)
        {
            for (int shift = 24; shift >= 0; shift -= 8)
                out += static_cast<char>(value >> shift & 0xFF);
        }

        void chunk(std::string &out, const char type[5], const std::string &data)
        {
            put32(out, static_cast<uint32_t>(data.size()));
            const size_t start = out.size();
            out.append(type, 4);
            out += data;
            put32(out, crc32(reinterpret_cast<const uint8_t *>(out.data() + start), out.size() - start, 0));
        }
    }

    std::string encode_png(const uint8_t *rgb, uint32_t width, uint32_t height)
    {
        std::string header;
        put32(header, width);
        put32(header, height);
        header += std::string("\x08\x02\x00\x00\x00", 5);  // 8 bits, RGB, deflate, no filter method, no interlace

        // Each scanline is a filter byte (0, none) and the row.
        const size_t row = size_t(width) * 3;
        std::string raw;
        raw.reserve((row + 1) * height);
        for (uint32_t y = 0; y < height; y++)
        {
            raw += '\0';
            raw.append(reinterpret_cast<const char *>(rgb + y * row), row);
        }

        std::string zlib = "\x78\x01";
        constexpr size_t kStoredMax = 65535;
        uint32_t a = 1, b = 0;
        for (size_t at = 0; at < raw.size(); at += kStoredMax)
        {
            const size_t length = std::min(kStoredMax, raw.size() - at);
            zlib += static_cast<char>(at + length == raw.size() ? 1 : 0);  // BFINAL, BTYPE 00 (stored)
            zlib += static_cast<char>(length & 0xFF);
            zlib += static_cast<char>(length >> 8);
            zlib += static_cast<char>(~length & 0xFF);
            zlib += static_cast<char>(~length >> 8 & 0xFF);
            zlib.append(raw, at, length);
            // Adler-32, reduced once per block: 65535 bytes cannot overflow b before the modulo.
            for (size_t i = at; i < at + length; i++)
            {
                a += static_cast<uint8_t>(raw[i]);
                b += a;
                if (a >= 65521)
                    a -= 65521;
            }
            b %= 65521;
        }
        put32(zlib, b << 16 | a);

        std::string png = "\x89PNG\r\n\x1a\n";
        chunk(png, "IHDR", header);
        chunk(png, "IDAT", zlib);
        chunk(png, "IEND", {});
        return png;
    }
}
