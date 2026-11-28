#include "dng.h"

#include "../enhancements.h"
#include "jpeg.h"

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

        // Replaces the entry of e's tag.
        void set(std::vector<IfdEntry> &entries, IfdEntry e)
        {
            for (IfdEntry &old : entries)
                if (old.tag == e.tag)
                    old = std::move(e);
        }

        // Appends bytes padded to an even length.
        void put_bytes(std::vector<uint8_t> &out, const std::vector<uint8_t> &bytes)
        {
            out.insert(out.end(), bytes.begin(), bytes.end());
            if (bytes.size() % 2)
                out.push_back(0);
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

        uint8_t srgb_byte(double linear)
        {
            const double encoded = linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1 / 2.4) - 0.055;
            return static_cast<uint8_t>(std::lround(std::clamp(encoded, 0.0, 1.0) * 255));
        }

        // rgb's pixels averaged over boxes into an image at most side pixels across and down.
        DngThumbnail shrink(const std::vector<uint8_t> &rgb, uint32_t width, uint32_t height, uint32_t side)
        {
            const double scale = std::min(1.0, double(side) / std::max(width, height));
            DngThumbnail out { {}, std::max(1u, uint32_t(std::lround(width * scale))), std::max(1u, uint32_t(std::lround(height * scale))) };
            out.pixels.resize(size_t(out.width) * out.height * 3);
            for (uint32_t y = 0; y < out.height; y++)
                for (uint32_t x = 0; x < out.width; x++)
                {
                    const uint32_t x0 = x * width / out.width, x1 = (x + 1) * width / out.width;
                    const uint32_t y0 = y * height / out.height, y1 = (y + 1) * height / out.height;
                    for (uint32_t k = 0; k < 3; k++)
                    {
                        uint32_t sum = 0;
                        for (uint32_t sy = y0; sy < y1; sy++)
                            for (uint32_t sx = x0; sx < x1; sx++)
                                sum += rgb[(size_t(sy) * width + sx) * 3 + k];
                        const uint32_t count = (x1 - x0) * (y1 - y0);
                        out.pixels[(size_t(y) * out.width + x) * 3 + k] = static_cast<uint8_t>((sum + count / 2) / count);
                    }
                }
            return out;
        }
    }

    float dng_headroom(float largest)
    {
        float headroom = 1;
        while (headroom < largest && headroom < 16)
            headroom *= 2;
        return headroom;
    }

    std::vector<std::pair<float, float>> dng_tone_curve(float headroom)
    {
        std::vector<std::pair<float, float>> curve;
        for (int i = 0; i <= 256; i++)
        {
            const double t = i / 256.0;
            const double x = t * t;
            curve.emplace_back(static_cast<float>(x), static_cast<float>(std::min(1.0, game_tone(headroom * x))));
        }
        return curve;
    }

    namespace
    {
        using Rgb = std::array<double, 3>;
        using Matrix = std::array<Rgb, 3>;

        Rgb times(const Matrix &m, const Rgb &v)
        {
            return { m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2], m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
                m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2] };
        }

        Matrix product(const Matrix &a, const Matrix &b)
        {
            Matrix m {};
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++)
                    m[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
            return m;
        }

        Matrix inverse(const Matrix &m)
        {
            const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
                + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
            Matrix r {};
            r[0] = { (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det, (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det, (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det };
            r[1] = { (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det, (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det, (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det };
            r[2] = { (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det, (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det, (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det };
            return r;
        }

        // Linear Rec. 709 (D65) to ProPhoto linear (D50), through XYZ and Bradford's adaptation, as Adobe's raw processing
        // takes the file's camera space with its ColorMatrix1 for D65.
        Matrix rec709_to_prophoto()
        {
            const Matrix rec709_xyz = { Rgb { 0.4124564, 0.3575761, 0.1804375 }, Rgb { 0.2126729, 0.7151522, 0.0721750 }, Rgb { 0.0193339, 0.1191920, 0.9503041 } };
            const Matrix bradford = { Rgb { 0.8951, 0.2664, -0.1614 }, Rgb { -0.7502, 1.7135, 0.0367 }, Rgb { 0.0389, -0.0685, 1.0296 } };
            const Rgb d65 = times(bradford, { 0.95047, 1.0, 1.08883 }), d50 = times(bradford, { 0.96422, 1.0, 0.82521 });
            const Matrix scale = { Rgb { d50[0] / d65[0], 0, 0 }, Rgb { 0, d50[1] / d65[1], 0 }, Rgb { 0, 0, d50[2] / d65[2] } };
            const Matrix prophoto_xyz = { Rgb { 0.7976749, 0.1351917, 0.0313534 }, Rgb { 0.2880402, 0.7118741, 0.0000857 }, Rgb { 0.0, 0.0, 0.8252100 } };
            return product(inverse(prophoto_xyz), product(inverse(bradford), product(scale, product(bradford, rec709_xyz))));
        }

        // The DNG SDK's RefRGBtoHSV and RefHSVtoRGB: hue 0 to 6, saturation the spread over the largest, value the largest.
        Rgb to_hsv(const Rgb &c)
        {
            const double hi = std::max({ c[0], c[1], c[2] }), lo = std::min({ c[0], c[1], c[2] }), d = hi - lo;
            double h = 0;
            if (d > 0)
                h = c[0] == hi ? (c[1] - c[2]) / d : c[1] == hi ? 2 + (c[2] - c[0]) / d : 4 + (c[0] - c[1]) / d;
            if (h < 0)
                h += 6;
            return { h, hi > 0 ? d / hi : 0, hi };
        }

        Rgb from_hsv(double h, double s, double v)
        {
            h = std::fmod(std::fmod(h, 6) + 6, 6);
            const int i = std::min(static_cast<int>(h), 5);
            const double f = h - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
            switch (i)
            {
            case 0: return { v, t, p };
            case 1: return { q, v, p };
            case 2: return { p, v, t };
            case 3: return { p, q, v };
            case 4: return { t, p, v };
            default: return { v, p, q };
            }
        }

        // The file value the profile's curve turns into y, by halving: the curve rises.
        double curve_input(double y, double headroom)
        {
            double lo = 0, hi = 1;
            for (int i = 0; i < 48; i++)
            {
                const double mid = 0.5 * (lo + hi);
                (std::min(1.0, game_tone(headroom * mid)) < y ? lo : hi) = mid;
            }
            return 0.5 * (lo + hi);
        }

        double srgb_encode(double v)
        {
            return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
        }
    }

    std::vector<float> dng_look_table(float headroom)
    {
        const Matrix to_prophoto = rec709_to_prophoto(), to_rec709 = inverse(to_prophoto);
        // Each node's change, and the value it is for, by its place: value, hue, saturation.
        std::vector<float> table;
        table.reserve(size_t(kDngLookHues) * kDngLookSats * kDngLookVals * 3);
        std::vector<Rgb> row(size_t(kDngLookHues) * kDngLookSats);
        for (uint32_t vi = 0; vi < kDngLookVals; vi++)
        {
            const double encoded = double(vi) / (kDngLookVals - 1);
            const double v = encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
            for (uint32_t hi = 0; hi < kDngLookHues; hi++)
                for (uint32_t si = 0; si < kDngLookSats; si++)
                {
                    const double h = 6.0 * hi / kDngLookHues, s = double(si) / (kDngLookSats - 1);
                    Rgb node = { 0, 1, 1 };
                    if (s > 0 && v > 0)
                    {
                        // What the game shows of this color, as ProPhoto light.
                        const Rgb scene = times(to_rec709, from_hsv(h, s, v));
                        Rgb shown {};
                        for (int k = 0; k < 3; k++)
                            shown[k] = std::min(1.0, game_tone(std::max(scene[k], 0.0) * headroom));
                        Rgb want = times(to_prophoto, shown);
                        for (double &c : want)
                            c = std::clamp(c, 0.0, 1.0);
                        // The color the hue-preserving curve takes there: the largest and smallest through the curve's
                        // inverse, the middle at the same share between them.
                        std::array<int, 3> order = { 0, 1, 2 };
                        std::sort(order.begin(), order.end(), [&want](int a, int b) { return want[a] < want[b]; });
                        const double lo = want[order[0]], mid = want[order[1]], top = want[order[2]];
                        const double lo2 = curve_input(lo, headroom), top2 = curve_input(top, headroom);
                        Rgb before {};
                        before[order[0]] = lo2;
                        before[order[2]] = top2;
                        before[order[1]] = top > lo ? lo2 + (top2 - lo2) * (mid - lo) / (top - lo) : lo2;
                        const Rgb got = to_hsv(before);
                        const double shift = std::fmod(got[0] - h + 9, 6) - 3;
                        // Adobe scales the value as the table encodes it, sRGB.
                        node = { shift * 60, got[1] / s, srgb_encode(got[2]) / encoded };
                    }
                    row[size_t(hi) * kDngLookSats + si] = node;
                }
            // Black has no hue: its nodes take the hue and saturation of the next value's, at its own value.
            if (vi == 0)
                continue;
            if (vi == 1)
                for (const Rgb &n : row)
                    for (const double c : { n[0], n[1], 1.0 })
                        table.push_back(static_cast<float>(c));
            for (const Rgb &n : row)
                for (const double c : n)
                    table.push_back(static_cast<float>(c));
        }
        return table;
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

    DngPreview dng_preview(const uint16_t *samples, uint32_t width, uint32_t height, double scene_per_sample)
    {
        std::vector<uint8_t> level(65536), shown(size_t(width) * height * 3);
        for (uint32_t s = 0; s < 65536; s++)
            level[s] = srgb_byte(std::min(1.0, game_tone(s * scene_per_sample)));
        for (size_t i = 0; i < shown.size(); i++)
            shown[i] = level[samples[i]];
        return { encode_jpeg(shown.data(), width, height, kDngPreviewQuality), shrink(shown, width, height, kDngThumbnail) };
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
        for (const auto &[x, y] : dng_tone_curve(headroom))
            for (const float v : { x, y })
            {
                uint32_t bits;
                std::memcpy(&bits, &v, sizeof bits);
                put32(curve.data, bits);
            }
        IfdEntry look { 50982, kFloat, {} };
        for (const float v : dng_look_table(headroom))
        {
            uint32_t bits;
            std::memcpy(&bits, &v, sizeof bits);
            put32(look.data, bits);
        }
        IfdEntry tile_offsets { 324, kLong, {} }, tile_bytes { 325, kLong, {} };
        for (const std::vector<uint8_t> &t : tiles)
        {
            put32(tile_offsets.data, 0);
            put32(tile_bytes.data, static_cast<uint32_t>(t.size()));
        }

        const DngPreview shown = dng_preview(picture.data(), width, height, headroom / 65535.0);
        const std::vector<uint8_t> &preview = shown.jpeg;
        const DngThumbnail &thumbnail = shown.thumbnail;

        // IFD0 is the thumbnail and holds the camera and the profile, its SubIFDs the raw image and then the preview, as
        // Adobe's DNG Converter lays a file out. Each IFD in tag order, as TIFF requires. The offsets of the thumbnail (273),
        // the SubIFDs (330), the EXIF IFD (34665), the preview (273) and the tiles (324) get their values once the layout is known.
        IfdEntry version { 50706, kByte, { 1, 4, 0, 0 } };
        IfdEntry backward { 50707, kByte, { 1, 4, 0, 0 } };
        std::vector<IfdEntry> entries = {
            longs(254, { 1 }),                  // a reduced-resolution image
            longs(256, { thumbnail.width }),
            longs(257, { thumbnail.height }),
            shorts(258, { 8, 8, 8 }),
            shorts(259, { 1 }),                 // uncompressed
            shorts(262, { 2 }),                 // RGB
            ascii(271, "CameraTools"),
            ascii(272, "Genshin Impact"),
            longs(273, { 0 }),
            shorts(274, { 1 }),                 // rows top to bottom, columns left to right
            shorts(277, { 3 }),
            longs(278, { thumbnail.height }),
            longs(279, { static_cast<uint32_t>(thumbnail.pixels.size()) }),
            shorts(284, { 1 }),                 // the three samples of a pixel side by side
            longs(330, { 0, 0 }),
            longs(34665, { 0 }),
            version,
            backward,
            ascii(50708, "Genshin Impact"),
            // ColorMatrix1, XYZ (D65) to linear Rec. 709 in millionths: the game's primaries, so As Shot white is D65.
            rationals(50721, kSrational, { { 3240454, 1000000 }, { -1537138, 1000000 }, { -498531, 1000000 }, { -969266, 1000000 },
                { 1876011, 1000000 }, { 41556, 1000000 }, { 55643, 1000000 }, { -204026, 1000000 }, { 1057225, 1000000 } }),
            rationals(50728, kRational, { { 1, 1 }, { 1, 1 }, { 1, 1 } }),     // AsShotNeutral
            rationals(50730, kSrational, { { 0, 1 } }),  // BaselineExposure: the profile's curve holds the game's white
            shorts(50778, { 21 }),              // CalibrationIlluminant1: D65
            ascii(50936, "CameraTools Game"),   // ProfileName
            std::move(curve),                   // ProfileToneCurve
            longs(50941, { 0 }),                // ProfileEmbedPolicy: allow copying
            longs(50981, { kDngLookHues, kDngLookSats, kDngLookVals }),   // ProfileLookTableDims
            std::move(look),                    // ProfileLookTableData
            longs(51108, { 1 }),                // ProfileLookTableEncoding: the value divisions sRGB-encoded
        };
        std::vector<IfdEntry> raw_entries = {
            longs(254, { 0 }),
            longs(256, { width }),
            longs(257, { height }),
            shorts(258, { 16, 16, 16 }),
            shorts(259, { 7 }),                 // lossless JPEG
            shorts(262, { 34892 }),             // LinearRaw
            shorts(277, { 3 }),
            shorts(284, { 1 }),
            longs(322, { kDngTile }),
            longs(323, { kDngTile }),
            std::move(tile_offsets),
            std::move(tile_bytes),
            shorts(339, { 1, 1, 1 }),           // unsigned integers
            longs(50717, { 65535 }),            // WhiteLevel
        };
        std::vector<IfdEntry> preview_entries = {
            longs(254, { 1 }),
            longs(256, { width }),
            longs(257, { height }),
            shorts(258, { 8, 8, 8 }),
            shorts(259, { 7 }),                 // JPEG
            shorts(262, { 6 }),                 // YCbCr
            longs(273, { 0 }),
            shorts(277, { 3 }),
            longs(278, { height }),
            longs(279, { static_cast<uint32_t>(preview.size()) }),
            shorts(284, { 1 }),
            shorts(530, { 2, 2 }),              // YCbCrSubSampling: 4:2:0
            longs(50970, { 2 }),                // PreviewColorSpace: sRGB
        };
        const std::vector<IfdEntry> exif_entries = exif_ifd(exif);

        // The header, the four IFDs with their values, the thumbnail, the preview, then the tiles, each padded to an even length.
        const auto padded = [](size_t size) { return static_cast<uint32_t>(size + size % 2); };
        const uint32_t exif_at = 8 + ifd_size(entries), raw_at = exif_at + ifd_size(exif_entries), preview_at = raw_at + ifd_size(raw_entries);
        const uint32_t thumbnail_at = preview_at + ifd_size(preview_entries), preview_jpeg_at = thumbnail_at + padded(thumbnail.pixels.size());
        uint32_t tile_at = preview_jpeg_at + padded(preview.size());
        set(entries, longs(273, { thumbnail_at }));
        set(entries, longs(330, { raw_at, preview_at }));
        set(entries, longs(34665, { exif_at }));
        set(preview_entries, longs(273, { preview_jpeg_at }));
        IfdEntry offsets { 324, kLong, {} };
        for (const std::vector<uint8_t> &t : tiles)
        {
            put32(offsets.data, tile_at);
            tile_at += padded(t.size());
        }
        set(raw_entries, std::move(offsets));

        std::vector<uint8_t> out = { 'I', 'I', 42, 0 };
        out.reserve(tile_at);
        put32(out, 8);
        put_ifd(out, entries);
        put_ifd(out, exif_entries);
        put_ifd(out, raw_entries);
        put_ifd(out, preview_entries);
        put_bytes(out, thumbnail.pixels);
        put_bytes(out, preview);
        for (const std::vector<uint8_t> &t : tiles)
            put_bytes(out, t);
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
        return { settings.bloom_strength, std::exp2(settings.exposure_ev), settings.auto_exposure > 0 };
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
