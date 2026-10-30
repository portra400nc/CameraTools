#include "dng.h"

#include "../enhancements.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace enhancements
{
    namespace
    {
        enum Type : uint16_t { kByte = 1, kAscii = 2, kShort = 3, kLong = 4, kRational = 5, kUndefined = 7, kSrational = 10, kFloat = 11 };

        // One IFD entry and its values' bytes, little-endian.
        struct IfdEntry
        {
            uint16_t tag;
            Type type;
            std::vector<uint8_t> data;
        };

        uint32_t type_bytes(Type type)
        {
            switch (type)
            {
            case kByte: case kAscii: case kUndefined: return 1;
            case kShort: return 2;
            case kLong: case kFloat: return 4;
            case kRational: case kSrational: return 8;
            }
            return 1;
        }

        void put16(std::vector<uint8_t> &out, uint32_t v)
        {
            out.push_back(static_cast<uint8_t>(v));
            out.push_back(static_cast<uint8_t>(v >> 8));
        }

        void put32(std::vector<uint8_t> &out, uint32_t v)
        {
            for (int shift = 0; shift < 32; shift += 8)
                out.push_back(static_cast<uint8_t>(v >> shift));
        }

        IfdEntry shorts(uint16_t tag, std::initializer_list<uint32_t> values)
        {
            IfdEntry e { tag, kShort, {} };
            for (uint32_t v : values)
                put16(e.data, v);
            return e;
        }

        IfdEntry longs(uint16_t tag, std::initializer_list<uint32_t> values)
        {
            IfdEntry e { tag, kLong, {} };
            for (uint32_t v : values)
                put32(e.data, v);
            return e;
        }

        IfdEntry ascii(uint16_t tag, const char *text)
        {
            IfdEntry e { tag, kAscii, {} };
            e.data.assign(text, text + std::strlen(text) + 1);
            return e;
        }

        // Numerator and denominator pairs; a signed numerator is stored as its two's complement.
        IfdEntry rationals(uint16_t tag, Type type, std::initializer_list<std::pair<int32_t, uint32_t>> values)
        {
            IfdEntry e { tag, type, {} };
            for (const auto &[numerator, denominator] : values)
            {
                put32(e.data, static_cast<uint32_t>(numerator));
                put32(e.data, denominator);
            }
            return e;
        }

        // An IFD's bytes from 2 for its count through 4 for its next IFD (none), with its values longer than 4 bytes.
        uint32_t ifd_size(const std::vector<IfdEntry> &entries)
        {
            uint32_t size = 2 + 12 * static_cast<uint32_t>(entries.size()) + 4;
            for (const IfdEntry &e : entries)
                if (e.data.size() > 4)
                    size += static_cast<uint32_t>(e.data.size() + e.data.size() % 2);
            return size;
        }

        // Appends an IFD where out ends, its values longer than 4 bytes right after it in entry order, each padded to an
        // even length.
        void put_ifd(std::vector<uint8_t> &out, const std::vector<IfdEntry> &entries)
        {
            uint32_t extra = static_cast<uint32_t>(out.size()) + 2 + 12 * static_cast<uint32_t>(entries.size()) + 4;
            put16(out, static_cast<uint32_t>(entries.size()));
            for (const IfdEntry &e : entries)
            {
                put16(out, e.tag);
                put16(out, e.type);
                put32(out, static_cast<uint32_t>(e.data.size() / type_bytes(e.type)));
                if (e.data.size() <= 4)
                {
                    out.insert(out.end(), e.data.begin(), e.data.end());
                    out.insert(out.end(), 4 - e.data.size(), 0);
                }
                else
                {
                    put32(out, extra);
                    extra += static_cast<uint32_t>(e.data.size() + e.data.size() % 2);
                }
            }
            put32(out, 0);
            for (const IfdEntry &e : entries)
                if (e.data.size() > 4)
                {
                    out.insert(out.end(), e.data.begin(), e.data.end());
                    if (e.data.size() % 2)
                        out.push_back(0);
                }
        }

        // ExposureTime as a camera writes it, FNumber in hundredths, ISOSpeedRatings, ExifVersion 0231, DateTimeOriginal,
        // SubjectDistance in centimeters, FocalLength in tenths and FocalLengthIn35mmFilm; those of 0, or no date, left out.
        std::vector<IfdEntry> exif_ifd(const DngExif &exif)
        {
            std::vector<IfdEntry> e;
            const auto in = [](float value, uint32_t parts) { return std::pair<int32_t, uint32_t>(static_cast<int32_t>(std::lround(double(value) * parts)), parts); };
            if (exif.exposure_time_s > 0 && std::isfinite(exif.exposure_time_s))
            {
                const auto whole = [](double x) { return static_cast<int32_t>(std::min(std::llround(x), 0x7FFFFFFFLL)); };
                const double t = exif.exposure_time_s;
                e.push_back(rationals(33434, kRational, { t < 1 ? std::pair<int32_t, uint32_t>(1, whole(1 / t)) : std::pair<int32_t, uint32_t>(whole(t), 1) }));
            }
            if (exif.f_number > 0)
                e.push_back(rationals(33437, kRational, { in(exif.f_number, 100) }));
            if (exif.iso > 0)
                e.push_back(shorts(34855, { std::min<uint32_t>(exif.iso, 65535) }));
            e.push_back(IfdEntry { 36864, kUndefined, { '0', '2', '3', '1' } });
            if (!exif.date_time.empty())
                e.push_back(ascii(36867, exif.date_time.c_str()));
            if (exif.subject_distance_m > 0)
                e.push_back(rationals(37382, kRational, { in(exif.subject_distance_m, 100) }));
            if (exif.focal_length_mm > 0)
            {
                e.push_back(rationals(37386, kRational, { in(exif.focal_length_mm, 10) }));
                e.push_back(shorts(41989, { static_cast<uint32_t>(std::lround(exif.focal_length_mm)) }));
            }
            return e;
        }

        // A table of the 17 difference categories after T.81 Annex K.2: code lengths of at most 16 bits from their counts,
        // with K.2's reserved symbol taken out so no code is all ones; BITS, HUFFVAL in code order, and each category's code.
        struct HuffmanTable
        {
            std::array<uint8_t, 16> bits {};
            std::vector<uint8_t> values;
            std::array<uint32_t, 17> code {};
            std::array<uint8_t, 17> length {};
        };

        HuffmanTable huffman_table(const std::array<uint64_t, 17> &counts)
        {
            constexpr int n = 18;
            std::array<uint64_t, n> freq {};
            std::copy(counts.begin(), counts.end(), freq.begin());
            freq[17] = 1;
            std::array<int, n> size {}, others;
            others.fill(-1);
            std::array<bool, n> live {};
            for (int i = 0; i < n; i++)
                live[i] = freq[i] > 0;
            for (;;)
            {
                // The least frequent two, the higher symbol first among equals.
                int v1 = -1, v2 = -1;
                for (int i = 0; i < n; i++)
                    if (live[i] && (v1 < 0 || freq[i] <= freq[v1]))
                        v1 = i;
                for (int i = 0; i < n; i++)
                    if (live[i] && i != v1 && (v2 < 0 || freq[i] <= freq[v2]))
                        v2 = i;
                if (v2 < 0)
                    break;
                freq[v1] += freq[v2];
                live[v2] = false;
                for (size[v1]++; others[v1] != -1; size[v1]++)
                    v1 = others[v1];
                others[v1] = v2;
                for (size[v2]++; others[v2] != -1; size[v2]++)
                    v2 = others[v2];
            }
            std::array<int, 33> bits {};
            for (const int s : size)
                if (s > 0)
                    bits[s]++;
            for (int i = 32; i > 16; i--)
                while (bits[i] > 0)
                {
                    int j = i - 2;
                    while (bits[j] == 0)
                        j--;
                    bits[i] -= 2;
                    bits[i - 1] += 1;
                    bits[j + 1] += 2;
                    bits[j] -= 1;
                }
            int longest = 16;
            while (bits[longest] == 0)
                longest--;
            bits[longest]--;

            HuffmanTable table;
            for (int i = 0; i < 16; i++)
                table.bits[i] = static_cast<uint8_t>(bits[i + 1]);
            for (int s = 1; s <= 32; s++)
                for (int v = 0; v < n - 1; v++)
                    if (size[v] == s)
                        table.values.push_back(static_cast<uint8_t>(v));
            uint32_t code = 0;
            size_t k = 0;
            for (int length = 1; length <= 16; length++, code <<= 1)
                for (int i = 0; i < bits[length]; i++, code++, k++)
                {
                    table.code[table.values[k]] = code;
                    table.length[table.values[k]] = static_cast<uint8_t>(length);
                }
            return table;
        }

        // Exactly what the curve's Python prototype computed in doubles: one operation a statement, so no compiler fuses a
        // multiply and an add and moves a point by a rounding.
        double game_tone(double x)
        {
            const double a = 1.36 * x;
            const double b = a + 0.047;
            const double numerator = x * b;
            const double c = 0.93 * x;
            const double d = c + 0.56;
            const double e = x * d;
            const double denominator = e + 0.14;
            return numerator / denominator;
        }

        uint16_t quantize(float value, double headroom)
        {
            if (!(value > 0))
                return 0;
            const double share = std::min(double(value) / headroom, 1.0);
            const double scaled = share * 65535.0;
            return static_cast<uint16_t>(scaled + 0.5);
        }
    }

    float dng_headroom(float largest)
    {
        float headroom = 1;
        while (headroom < largest && headroom < 16)
            headroom *= 2;
        return headroom;
    }

    std::vector<std::pair<float, float>> dng_tone_curve()
    {
        std::vector<std::pair<float, float>> curve;
        const double top = game_tone(1.0);
        for (int i = 0; i <= 256; i++)
        {
            const double t = i / 256.0;
            const double x = t * t;
            curve.emplace_back(static_cast<float>(x), static_cast<float>(std::min(1.0, game_tone(x) / top)));
        }
        return curve;
    }

    std::vector<uint8_t> dng_lossless_jpeg(const uint16_t *samples, uint32_t width, uint32_t height)
    {
        // Each sample's difference from its predictor, the one to its left (above in the first column, 2^15 at the start),
        // modulo 2^16, as a category of 0 to 16 bits and that many extra bits (none for 16, whose difference is -32768).
        const size_t count = size_t(width) * height * 3;
        std::vector<uint8_t> category(count);
        std::vector<uint16_t> extra(count);
        std::array<uint64_t, 17> counts {};
        for (uint32_t y = 0; y < height; y++)
            for (uint32_t x = 0; x < width; x++)
                for (uint32_t k = 0; k < 3; k++)
                {
                    const size_t at = (size_t(y) * width + x) * 3 + k;
                    const int predicted = x > 0 ? samples[at - 3] : y > 0 ? samples[at - size_t(width) * 3] : 32768;
                    int difference = (int(samples[at]) - predicted) & 0xFFFF;
                    if (difference >= 32768)
                        difference -= 65536;
                    uint8_t bits = 16;
                    uint32_t more = 0;
                    if (difference != -32768)
                    {
                        const uint32_t magnitude = static_cast<uint32_t>(std::abs(difference));
                        bits = 0;
                        while ((magnitude >> bits) != 0)
                            bits++;
                        more = static_cast<uint32_t>(difference >= 0 ? difference : difference + (1 << bits) - 1) & ((1u << bits) - 1);
                    }
                    category[at] = bits;
                    extra[at] = static_cast<uint16_t>(more);
                    counts[bits]++;
                }
        const HuffmanTable table = huffman_table(counts);

        std::vector<uint8_t> out = { 0xFF, 0xD8 };
        const uint8_t frame[] = { 0xFF, 0xC3, 0, 17, 16, uint8_t(height >> 8), uint8_t(height), uint8_t(width >> 8), uint8_t(width), 3,
            1, 0x11, 0, 2, 0x11, 0, 3, 0x11, 0 };
        out.insert(out.end(), std::begin(frame), std::end(frame));
        const uint32_t dht = 2 + 1 + 16 + static_cast<uint32_t>(table.values.size());
        out.insert(out.end(), { 0xFF, 0xC4, uint8_t(dht >> 8), uint8_t(dht), 0 });
        out.insert(out.end(), table.bits.begin(), table.bits.end());
        out.insert(out.end(), table.values.begin(), table.values.end());
        // Three components on table 0, predictor 1, no point transform.
        out.insert(out.end(), { 0xFF, 0xDA, 0, 12, 3, 1, 0x00, 2, 0x00, 3, 0x00, 1, 0, 0 });

        uint64_t pending = 0;
        int pending_bits = 0;
        const auto put = [&](uint32_t value, int bits)
        {
            pending = (pending << bits) | value;
            pending_bits += bits;
            while (pending_bits >= 8)
            {
                pending_bits -= 8;
                const uint8_t byte = static_cast<uint8_t>(pending >> pending_bits);
                out.push_back(byte);
                if (byte == 0xFF)
                    out.push_back(0);
            }
            pending &= (uint64_t(1) << pending_bits) - 1;
        };
        for (size_t i = 0; i < count; i++)
        {
            const uint8_t bits = category[i];
            put(table.code[bits], table.length[bits]);
            if (bits > 0 && bits < 16)
                put(extra[i], bits);
        }
        if (pending_bits > 0)
            put((1u << (8 - pending_bits)) - 1, 8 - pending_bits);
        out.insert(out.end(), { 0xFF, 0xD9 });
        return out;
    }

    std::vector<uint8_t> encode_dng(const DngImage &image, const DngExif &exif)
    {
        const uint32_t width = image.width, height = image.height, channels = image.channels;
        float largest = 0;
        for (size_t i = 0, n = size_t(width) * height; i < n; i++)
            for (uint32_t k = 0; k < 3; k++)
                if (const float v = image.pixels[i * channels + k]; std::isfinite(v))
                    largest = std::max(largest, v);
        const float headroom = dng_headroom(largest);
        const uint32_t stops = static_cast<uint32_t>(std::log2(headroom));

        // The samples top row first, then each tile, its pixels past the picture's edge repeating the last row and column.
        std::vector<uint16_t> picture(size_t(width) * height * 3);
        for (uint32_t row = 0; row < height; row++)
        {
            const float *from = image.pixels + size_t(image.rows_up ? height - 1 - row : row) * width * channels;
            for (uint32_t x = 0; x < width; x++)
                for (uint32_t k = 0; k < 3; k++)
                    picture[(size_t(row) * width + x) * 3 + k] = quantize(from[size_t(x) * channels + k], headroom);
        }
        const uint32_t across = (width + kDngTile - 1) / kDngTile, down = (height + kDngTile - 1) / kDngTile;
        std::vector<std::vector<uint8_t>> tiles;
        std::vector<uint16_t> tile(size_t(kDngTile) * kDngTile * 3);
        for (uint32_t ty = 0; ty < down; ty++)
            for (uint32_t tx = 0; tx < across; tx++)
            {
                for (uint32_t y = 0; y < kDngTile; y++)
                    for (uint32_t x = 0; x < kDngTile; x++)
                    {
                        const uint32_t sy = std::min(ty * kDngTile + y, height - 1), sx = std::min(tx * kDngTile + x, width - 1);
                        std::memcpy(&tile[(size_t(y) * kDngTile + x) * 3], &picture[(size_t(sy) * width + sx) * 3], 3 * sizeof(uint16_t));
                    }
                tiles.push_back(dng_lossless_jpeg(tile.data(), kDngTile, kDngTile));
            }

        IfdEntry curve { 50940, kFloat, {} };
        for (const auto &[x, y] : dng_tone_curve())
            for (const float v : { x, y })
            {
                uint32_t bits;
                std::memcpy(&bits, &v, sizeof bits);
                put32(curve.data, bits);
            }
        IfdEntry tile_offsets { 324, kLong, {} }, tile_bytes { 325, kLong, {} };
        for (const std::vector<uint8_t> &t : tiles)
        {
            put32(tile_offsets.data, 0);
            put32(tile_bytes.data, static_cast<uint32_t>(t.size()));
        }
        IfdEntry version { 50706, kByte, { 1, 4, 0, 0 } };
        IfdEntry backward { 50707, kByte, { 1, 4, 0, 0 } };
        // In tag order, as TIFF requires. TileOffsets (324) and ExifIFD (34665) get their values once the layout is known.
        std::vector<IfdEntry> entries = {
            longs(254, { 0 }),
            longs(256, { width }),
            longs(257, { height }),
            shorts(258, { 16, 16, 16 }),
            shorts(259, { 7 }),                 // lossless JPEG
            shorts(262, { 34892 }),             // LinearRaw
            ascii(271, "CameraTools"),
            ascii(272, "Genshin Impact"),
            shorts(274, { 1 }),                 // rows top to bottom, columns left to right
            shorts(277, { 3 }),
            shorts(284, { 1 }),                 // the three samples of a pixel side by side
            longs(322, { kDngTile }),
            longs(323, { kDngTile }),
            std::move(tile_offsets),
            std::move(tile_bytes),
            shorts(339, { 1, 1, 1 }),           // unsigned integers
            longs(34665, { 0 }),
            version,
            backward,
            ascii(50708, "Genshin Impact"),
            longs(50717, { 65535 }),            // WhiteLevel
            // ColorMatrix1, XYZ (D65) to linear Rec. 709 in millionths: the game's primaries, so As Shot white is D65.
            rationals(50721, kSrational, { { 3240454, 1000000 }, { -1537138, 1000000 }, { -498531, 1000000 }, { -969266, 1000000 },
                { 1876011, 1000000 }, { 41556, 1000000 }, { 55643, 1000000 }, { -204026, 1000000 }, { 1057225, 1000000 } }),
            rationals(50728, kRational, { { 1, 1 }, { 1, 1 }, { 1, 1 } }),     // AsShotNeutral
            rationals(50730, kSrational, { { static_cast<int32_t>(stops * 1000), 1000 } }),  // BaselineExposure
            shorts(50778, { 21 }),              // CalibrationIlluminant1: D65
            ascii(50936, "CameraTools Game"),   // ProfileName
            std::move(curve),                   // ProfileToneCurve
            longs(50941, { 0 }),                // ProfileEmbedPolicy: allow copying
        };
        const std::vector<IfdEntry> exif_entries = exif_ifd(exif);

        // The header, the IFD and its values, the EXIF IFD and its values, then the tiles, each padded to an even length.
        const uint32_t exif_at = 8 + ifd_size(entries);
        uint32_t tile_at = exif_at + ifd_size(exif_entries);
        for (IfdEntry &e : entries)
            if (e.tag == 34665)
                e = longs(34665, { exif_at });
            else if (e.tag == 324)
            {
                e.data.clear();
                for (const std::vector<uint8_t> &t : tiles)
                {
                    put32(e.data, tile_at);
                    tile_at += static_cast<uint32_t>(t.size() + t.size() % 2);
                }
            }

        std::vector<uint8_t> out = { 'I', 'I', 42, 0 };
        out.reserve(tile_at);
        put32(out, 8);
        put_ifd(out, entries);
        put_ifd(out, exif_entries);
        for (const std::vector<uint8_t> &t : tiles)
        {
            out.insert(out.end(), t.begin(), t.end());
            if (t.size() % 2)
                out.push_back(0);
        }
        return out;
    }

    bool write_dng(const std::filesystem::path &path, const DngImage &image, const DngExif &exif)
    {
        const std::vector<uint8_t> bytes = encode_dng(image, exif);
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(file.flush());
    }

    DngGrade dng_grade(const TonemapSettings &settings, bool ours)
    {
        if (!ours)
            return {};
        return { settings.bloom_strength, std::exp2(settings.exposure_ev) };
    }

    static_assert(uint32_t(DngState::failed) == ENHANCEMENTS_DNG_FAILED && uint32_t(DngFailure::dropped) == ENHANCEMENTS_DNG_FAILED_DROPPED,
        "the states and failures count as enhancements.h's");

    bool dng_develop(DngJob &job, uint32_t id, uint64_t presents)
    {
        if (id == 0 || job.state == DngState::developing || job.state == DngState::writing)
            return false;
        job = { id, DngState::developing, DngFailure::none, presents };
        return true;
    }

    bool dng_developed(DngJob &job, uint32_t id, DngFailure failure, uint64_t presents)
    {
        if (job.id != id || job.state != DngState::developing)
            return false;
        job = { id, failure == DngFailure::none ? DngState::ready : DngState::failed, failure, presents };
        return job.state == DngState::ready;
    }

    bool dng_write(DngJob &job, uint32_t id)
    {
        if (job.id != id || job.state != DngState::ready)
            return false;
        job.state = DngState::writing;
        return true;
    }

    void dng_written(DngJob &job, uint32_t id, bool ok)
    {
        if (job.id != id || job.state != DngState::writing)
            return;
        job.state = ok ? DngState::written : DngState::failed;
        job.failure = ok ? DngFailure::none : DngFailure::write;
    }

    bool dng_expire(DngJob &job, uint64_t presents)
    {
        const uint64_t waited = presents - job.since;
        if (job.state == DngState::developing && waited > kDngDevelopWait)
            job.failure = DngFailure::no_tonemap;
        else if (job.state == DngState::ready && waited > kDngHold)
            job.failure = DngFailure::dropped;
        else
            return false;
        job.state = DngState::failed;
        job.since = presents;
        return true;
    }
}
