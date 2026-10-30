// The DNG writer: a small image's file parsed back as a TIFF reader would, its tags, EXIF and pixels against literal values,
// its tiles decoded by a lossless JPEG decoder of the test's own, the headroom each brightness gets, and a 320x200 image for
// run.sh to hand to macOS's own raw decoder. Then the tone map's
// weights a DNG takes, and a DNG's way from EnhancementsDevelopDng to its file.
#include "../core/dng.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace enhancements;

namespace
{
    int failures = 0;

    void check(const char *name, bool ok)
    {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        failures += ok ? 0 : 1;
    }

    uint32_t u16(const std::vector<uint8_t> &b, size_t at) { return b[at] | b[at + 1] << 8; }
    uint32_t u32(const std::vector<uint8_t> &b, size_t at) { return u16(b, at) | u16(b, at + 2) << 16; }

    // A tag's type, count and the bytes of its values, wherever they are stored.
    struct Tag
    {
        uint32_t type = 0, count = 0;
        std::vector<uint8_t> data;
    };

    struct Tiff
    {
        bool little_endian = false;
        std::vector<uint16_t> order;
        std::map<uint16_t, Tag> tags;
        uint32_t next_ifd = 1;
    };

    // The IFD at ifd, or the file's first.
    Tiff parse(const std::vector<uint8_t> &b, uint32_t ifd = 0)
    {
        Tiff t;
        t.little_endian = b.size() > 8 && b[0] == 'I' && b[1] == 'I' && u16(b, 2) == 42;
        if (!t.little_endian)
            return t;
        if (ifd == 0)
            ifd = u32(b, 4);
        const uint32_t count = u16(b, ifd);
        const uint32_t sizes[] = { 0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8 };  // by TIFF type, UNDEFINED (7) 1
        for (uint32_t i = 0; i < count; i++)
        {
            const size_t at = ifd + 2 + 12 * size_t(i);
            Tag tag { u16(b, at + 2), u32(b, at + 4), {} };
            const size_t bytes = size_t(tag.count) * sizes[tag.type];
            const size_t from = bytes <= 4 ? at + 8 : u32(b, at + 8);
            tag.data.assign(b.begin() + from, b.begin() + from + bytes);
            t.order.push_back(static_cast<uint16_t>(u16(b, at)));
            t.tags[t.order.back()] = tag;
        }
        t.next_ifd = u32(b, ifd + 2 + 12 * size_t(count));
        return t;
    }

    uint32_t value(const Tiff &t, uint16_t tag, size_t index = 0)
    {
        const Tag &v = t.tags.at(tag);
        return v.type == 3 ? u16(v.data, index * 2) : u32(v.data, index * 4);
    }

    std::string text(const Tiff &t, uint16_t tag)
    {
        const Tag &v = t.tags.at(tag);
        return std::string(v.data.begin(), v.data.end());
    }

    int32_t numerator(const Tiff &t, uint16_t tag, size_t index = 0) { return static_cast<int32_t>(u32(t.tags.at(tag).data, index * 8)); }
    uint32_t denominator(const Tiff &t, uint16_t tag, size_t index = 0) { return u32(t.tags.at(tag).data, index * 8 + 4); }

    float float_at(const Tiff &t, uint16_t tag, size_t index)
    {
        const uint32_t bits = u32(t.tags.at(tag).data, index * 4);
        float v;
        std::memcpy(&v, &bits, sizeof v);
        return v;
    }

    // A lossless JPEG stream back to its samples, as T.81's decoder reads it: SOF3, DHT and SOS, then each sample's category
    // code, its extra bits and the predictor 1 sum, modulo 2^16. Empty when a marker is not where this writer puts it.
    std::vector<uint16_t> decode_lossless_jpeg(const uint8_t *b, size_t size, uint32_t &width, uint32_t &height)
    {
        size_t at = 2;
        std::vector<std::pair<uint32_t, uint32_t>> codes;     // (length, code) of each value in HUFFVAL order
        std::vector<uint8_t> values;
        while (at + 4 <= size && b[at] == 0xFF && b[at + 1] != 0xDA)
        {
            const uint32_t length = uint32_t(b[at + 2]) << 8 | b[at + 3];
            if (b[at + 1] == 0xC3)
            {
                height = uint32_t(b[at + 5]) << 8 | b[at + 6];
                width = uint32_t(b[at + 7]) << 8 | b[at + 8];
            }
            if (b[at + 1] == 0xC4)
            {
                uint32_t code = 0, total = 0;
                for (uint32_t len = 1; len <= 16; len++, code <<= 1)
                    for (uint32_t i = 0; i < b[at + 4 + len]; i++, code++, total++)
                        codes.emplace_back(len, code);
                values.assign(b + at + 21, b + at + 21 + total);
            }
            at += 2 + length;
        }
        if (at + 4 > size || values.empty())
            return {};
        at += 2 + (uint32_t(b[at + 2]) << 8 | b[at + 3]);
        std::vector<uint8_t> bits;
        for (; at + 1 < size && !(b[at] == 0xFF && b[at + 1] == 0xD9); at++)
        {
            for (int k = 7; k >= 0; k--)
                bits.push_back((b[at] >> k) & 1);
            if (b[at] == 0xFF)
                at++;
        }
        std::vector<uint16_t> out(size_t(width) * height * 3);
        size_t pos = 0;
        const auto read = [&](uint32_t n)
        {
            uint32_t v = 0;
            for (uint32_t i = 0; i < n && pos < bits.size(); i++)
                v = v << 1 | bits[pos++];
            return v;
        };
        for (size_t i = 0; i < out.size(); i++)
        {
            uint32_t code = 0, len = 0;
            int category = -1;
            while (category < 0 && len < 16)
            {
                code = code << 1 | read(1);
                len++;
                for (size_t c = 0; c < codes.size(); c++)
                    if (codes[c] == std::pair(len, code))
                        category = values[c];
            }
            if (category < 0)
                return {};
            int difference = 32768;
            if (category < 16)
            {
                const int v = category == 0 ? 0 : int(read(category));
                difference = category == 0 ? 0 : v >= 1 << (category - 1) ? v : v - (1 << category) + 1;
            }
            const size_t x = (i / 3) % width;
            const int predicted = x > 0 ? out[i - 3] : i >= size_t(width) * 3 ? out[i - size_t(width) * 3] : 32768;
            out[i] = static_cast<uint16_t>((predicted + difference) & 0xFFFF);
        }
        return out;
    }

    // The file's samples, red, green and blue of each pixel, top row first, from its tiles.
    std::vector<uint32_t> samples(const std::vector<uint8_t> &b, const Tiff &t)
    {
        const uint32_t width = value(t, 256), height = value(t, 257), tile = value(t, 322), across = (width + tile - 1) / tile;
        std::vector<uint32_t> out(size_t(width) * height * 3);
        for (size_t i = 0; i < t.tags.at(324).count; i++)
        {
            uint32_t w = 0, h = 0;
            const std::vector<uint16_t> pixels = decode_lossless_jpeg(b.data() + value(t, 324, i), value(t, 325, i), w, h);
            if (pixels.empty() || w != tile || h != tile)
                return {};
            const uint32_t x0 = uint32_t(i % across) * tile, y0 = uint32_t(i / across) * tile;
            for (uint32_t y = y0; y < std::min(y0 + tile, height); y++)
                for (uint32_t x = x0; x < std::min(x0 + tile, width); x++)
                    for (uint32_t k = 0; k < 3; k++)
                        out[(size_t(y) * width + x) * 3 + k] = pixels[(size_t(y - y0) * tile + (x - x0)) * 3 + k];
        }
        return out;
    }

    // The baseline exposure an image of one bright pixel gets.
    double exposure_of(float largest)
    {
        const float pixels[] = { largest, 0.1f, 0.0f, 0.2f, 0.3f, 0.4f };
        const Tiff t = parse(encode_dng({ pixels, 2, 1 }));
        return double(numerator(t, 50730)) / denominator(t, 50730);
    }
}

int main(int argc, char **argv)
{
    check("the headroom is the smallest power of two at or above the brightest channel, from 1 to 16",
        dng_headroom(0) == 1 && dng_headroom(0.7f) == 1 && dng_headroom(1) == 1 && dng_headroom(1.01f) == 2 && dng_headroom(3.2f) == 4
        && dng_headroom(4) == 4 && dng_headroom(9.75f) == 16 && dng_headroom(40) == 16 && dng_headroom(NAN) == 1);
    check("BaselineExposure is log2 of the headroom in thousandths: 0 for a brightest channel of 0.7, 2 for 3.2, 4 for 40",
        exposure_of(0.7f) == 0 && exposure_of(3.2f) == 2 && exposure_of(40) == 4);

    {
        // Two rows of two RGBA pixels, stored as the game stores them, rows running up the screen. The top row seen is
        // the second stored. Its brightest finite channel, 4, makes the headroom 4, and the alpha of 9 counts for nothing.
        const float stored[] = {
            0.5f, 1.0f, 2.0f, 9.0f,     3.2f, 0.25f, 0.0f, 9.0f,       // bottom row
            1.0f, 2.0f, 4.0f, 9.0f,     -1.0f, NAN, INFINITY, 9.0f,    // top row
        };
        const std::vector<uint8_t> file = encode_dng({ stored, 2, 2, 4, true });
        const Tiff t = parse(file);
        check("the file is a little-endian TIFF whose IFD is the only one", t.little_endian && t.next_ifd == 0);
        const std::vector<uint16_t> expected_order = { 254, 256, 257, 258, 259, 262, 271, 272, 274, 277, 284, 322, 323, 324, 325, 339, 34665,
            50706, 50707, 50708, 50717, 50721, 50728, 50730, 50778, 50936, 50940, 50941 };
        check("its 28 tags come in ascending order", t.order == expected_order);
        check("it is 2x2 LinearRaw, lossless JPEG, three 16-bit unsigned samples side by side, in one tile of 256x256",
            value(t, 254) == 0 && value(t, 256) == 2 && value(t, 257) == 2 && value(t, 258, 0) == 16 && value(t, 258, 1) == 16 && value(t, 258, 2) == 16
            && value(t, 259) == 7 && value(t, 262) == 34892 && value(t, 274) == 1 && value(t, 277) == 3 && value(t, 284) == 1 && value(t, 322) == 256
            && value(t, 323) == 256 && t.tags.at(324).count == 1 && t.tags.at(325).count == 1 && value(t, 339, 0) == 1 && value(t, 339, 1) == 1
            && value(t, 339, 2) == 1 && value(t, 50717) == 65535);
        check("its tile ends the file, and is a lossless JPEG stream from SOI to EOI", value(t, 324) + value(t, 325) == file.size()
            && file[value(t, 324)] == 0xFF && file[value(t, 324) + 1] == 0xD8 && file[value(t, 324) + 2] == 0xFF && file[value(t, 324) + 3] == 0xC3
            && file[file.size() - 2] == 0xFF && file[file.size() - 1] == 0xD9);
        const Tiff exif = parse(file, value(t, 34665));
        check("with no camera given, its EXIF IFD holds only ExifVersion 0231", exif.order == std::vector<uint16_t> { 36864 }
            && exif.tags.at(36864).type == 7 && text(exif, 36864) == "0231" && exif.next_ifd == 0);
        check("it names CameraTools as the make and Genshin Impact as the model and the unique camera model",
            text(t, 271) == std::string("CameraTools", 12) && text(t, 272) == std::string("Genshin Impact", 15) && text(t, 50708) == std::string("Genshin Impact", 15));
        check("it is DNG 1.4.0.0, readable as 1.4.0.0", t.tags.at(50706).data == std::vector<uint8_t> { 1, 4, 0, 0 }
            && t.tags.at(50707).data == std::vector<uint8_t> { 1, 4, 0, 0 } && t.tags.at(50706).type == 1);
        const int32_t matrix[] = { 3240454, -1537138, -498531, -969266, 1876011, 41556, 55643, -204026, 1057225 };
        bool matrix_ok = t.tags.at(50721).type == 10 && t.tags.at(50721).count == 9;
        for (size_t i = 0; i < 9; i++)
            matrix_ok &= numerator(t, 50721, i) == matrix[i] && denominator(t, 50721, i) == 1000000;
        check("ColorMatrix1 is XYZ (D65) to linear Rec. 709 in millionths, as shot under D65 with a neutral of 1, 1, 1", matrix_ok && value(t, 50778) == 21
            && numerator(t, 50728, 0) == 1 && denominator(t, 50728, 0) == 1 && numerator(t, 50728, 2) == 1 && denominator(t, 50728, 2) == 1);
        check("BaselineExposure is 2000/1000 for a headroom of 4", t.tags.at(50730).type == 10 && numerator(t, 50730) == 2000 && denominator(t, 50730) == 1000);
        check("the profile is CameraTools Game, free to copy", text(t, 50936) == std::string("CameraTools Game", 17) && value(t, 50941) == 0);
        const Tag &curve = t.tags.at(50940);
        check("its tone curve is 257 pairs of floats from (0, 0) to (1, 1), at x = (i / 256)^2, the game's curve over its value at 1",
            curve.type == 11 && curve.count == 514 && float_at(t, 50940, 0) == 0 && float_at(t, 50940, 1) == 0 && float_at(t, 50940, 512) == 1
            && float_at(t, 50940, 513) == 1 && float_at(t, 50940, 2 * 128) == 0.25f
            && std::fabs(float_at(t, 50940, 2 * 128 + 1) - 0.25f * (1.36f * 0.25f + 0.047f) / (0.25f * (0.93f * 0.25f + 0.56f) + 0.14f) / (1.407f / 1.63f)) < 1e-6f);
        // v / 4 * 65535, rounded: 1 -> 16384 (16383.75), 2 -> 32768 (32767.5), 4 -> 65535; -1, NaN and 0 -> 0; infinity -> 65535.
        const std::vector<uint32_t> expected = {
            16384, 32768, 65535,    0, 0, 65535,            // top row as seen: the second stored
            8192, 16384, 32768,     52428, 4096, 0,         // 0.5, 1, 2; 3.2 (52428.0), 0.25 (4095.94), 0
        };
        check("the samples are each channel over the headroom times 65535, rounded, top row first, with alpha left out and nothing not finite or "
              "below 0 above 0", samples(file, t) == expected);
    }
    {
        const float one[] = { 0.5f, 0.5f, 0.5f };
        const std::vector<uint8_t> file = encode_dng({ one, 1, 1 }, { 85.0f, 1.4f, 2.35f, "2026:10:29 11:30:00" });
        const Tiff exif = parse(file, value(parse(file), 34665));
        check("the EXIF IFD holds FNumber, ExifVersion, DateTimeOriginal, SubjectDistance, FocalLength and FocalLengthIn35mmFilm in tag order",
            exif.order == std::vector<uint16_t> { 33437, 36864, 36867, 37382, 37386, 41989 });
        check("f/1.4 in hundredths, 2.35 m in centimeters, 85 mm in tenths and as 85 in 35 mm film, and the date as given",
            numerator(exif, 33437) == 140 && denominator(exif, 33437) == 100 && numerator(exif, 37382) == 235 && denominator(exif, 37382) == 100
            && numerator(exif, 37386) == 850 && denominator(exif, 37386) == 10 && value(exif, 41989) == 85
            && text(exif, 36867) == std::string("2026:10:29 11:30:00", 20));
        const std::vector<uint8_t> lens = encode_dng({ one, 1, 1 }, { 24.4f, 0, 0, "" });
        const Tiff lens_only = parse(lens, value(parse(lens), 34665));
        check("a shot without depth of field or a date gives its focal length alone, 24.4 mm as 244/10 and 24",
            lens_only.order == std::vector<uint16_t> { 36864, 37386, 41989 } && numerator(lens_only, 37386) == 244 && value(lens_only, 41989) == 24);
        check("a shot with no shutter time and no ISO has neither ExposureTime nor ISOSpeedRatings",
            !exif.tags.count(33434) && !exif.tags.count(34855) && !lens_only.tags.count(33434) && !lens_only.tags.count(34855));
        DngExif eighth { 35, 0, 0, "" };
        eighth.exposure_time_s = 0.125f;
        eighth.iso = 400;
        const std::vector<uint8_t> short_file = encode_dng({ one, 1, 1 }, eighth);
        const Tiff short_exif = parse(short_file, value(parse(short_file), 34665));
        check("a 1/8 s exposure at ISO 400 writes ExposureTime 1/8 first and ISOSpeedRatings 400 as a SHORT, in tag order",
            short_exif.order == std::vector<uint16_t> { 33434, 34855, 36864, 37386, 41989 } && short_exif.tags.at(33434).type == 5
            && numerator(short_exif, 33434) == 1 && denominator(short_exif, 33434) == 8 && short_exif.tags.at(34855).type == 3
            && value(short_exif, 34855) == 400);
        DngExif four { 85.0f, 1.4f, 2.35f, "2026:10:29 11:30:00" };
        four.exposure_time_s = 4;
        four.iso = 100;
        const std::vector<uint8_t> long_file = encode_dng({ one, 1, 1 }, four);
        const Tiff long_exif = parse(long_file, value(parse(long_file), 34665));
        check("a 4 s exposure writes ExposureTime 4/1, before FNumber, with ISO 100 after it",
            long_exif.order == std::vector<uint16_t> { 33434, 33437, 34855, 36864, 36867, 37382, 37386, 41989 }
            && numerator(long_exif, 33434) == 4 && denominator(long_exif, 33434) == 1 && value(long_exif, 34855) == 100);
    }
    {
        // Every category, the largest jumps both ways included: 0 after the start's 32768 is a difference of -32768, the one
        // category with no extra bits, and 65535 after 0 wraps to -1.
        std::vector<uint16_t> tile(size_t(kDngTile) * kDngTile * 3);
        uint32_t seed = 12345;
        for (size_t i = 0; i < tile.size(); i++)
        {
            seed = seed * 1103515245u + 12345u;
            tile[i] = static_cast<uint16_t>(i < 3 ? 0 : i % 7 == 0 ? 65535 : i % 5 == 0 ? (seed >> 8) : (i * 37) & 0xFFFF);
        }
        uint32_t w = 0, h = 0;
        const std::vector<uint8_t> stream = dng_lossless_jpeg(tile.data(), kDngTile, kDngTile);
        check("a tile of every kind of jump decodes back exactly, as 256x256", decode_lossless_jpeg(stream.data(), stream.size(), w, h) == tile
            && w == 256 && h == 256);
        const std::vector<uint16_t> flat(size_t(kDngTile) * kDngTile * 3, 1000);
        const std::vector<uint8_t> small = dng_lossless_jpeg(flat.data(), kDngTile, kDngTile);
        check("a flat tile, one category after its first samples, decodes back at about a bit a sample, under 25 KB",
            decode_lossless_jpeg(small.data(), small.size(), w, h) == flat && small.size() < 25 * 1024);
    }
    {
        // 300x260: four tiles, the right and bottom ones mostly padding.
        const uint32_t width = 300, height = 260;
        std::vector<float> rgb(size_t(width) * height * 3);
        for (size_t i = 0; i < rgb.size(); i++)
            rgb[i] = float((i * 7919) % 1000) / 1000.0f;
        const std::vector<uint8_t> file = encode_dng({ rgb.data(), width, height });
        const Tiff t = parse(file);
        const std::vector<uint32_t> got = samples(file, t);
        bool same = got.size() == rgb.size();
        for (size_t i = 0; same && i < rgb.size(); i++)
            same = got[i] == uint32_t(double(rgb[i]) * 65535.0 + 0.5);
        check("a 300x260 picture takes 2x2 tiles, and every pixel comes back from them", t.tags.at(324).count == 4 && same);
    }
    {
        const float bright[] = { 40.0f, 16.0f, 8.0f };
        const Tiff t = parse(encode_dng({ bright, 1, 1 }));
        check("a channel of 40 clips at the largest headroom, 16, which a channel of 16 fills and one of 8 halves",
            samples(encode_dng({ bright, 1, 1 }), t) == std::vector<uint32_t> { 65535, 65535, 32768 } && numerator(t, 50730) == 4000);
    }
    {
        const float dark[] = { 0.0f, 0.0f, 0.0f };
        check("a black image gets a headroom of 1", numerator(parse(encode_dng({ dark, 1, 1 })), 50730) == 0);
    }
    {
        TonemapSettings ours;
        ours.bloom_strength = 0.5f;
        ours.exposure_ev = -2;
        const DngGrade game = dng_grade(ours, false), drawn = dng_grade(ours, true);
        check("the DNG takes the tone map's bloom strength and exposure while our tonemap pass draws, and 1 and 1 while the game's does",
            game.bloom_strength == 1 && game.exposure_scale == 1 && drawn.bloom_strength == 0.5f && drawn.exposure_scale == 0.25f);
    }
    {
        DngJob job;
        check("id 0 is never accepted, since the status's 0 means none", !dng_develop(job, 0, 10) && job.state == DngState::none);
        const bool first = dng_develop(job, 7, 10);
        check("a DNG develops from the present it was asked at, and a second waits for it", first && job.id == 7 && job.state == DngState::developing
            && job.since == 10 && !dng_develop(job, 8, 11) && job.id == 7);
        check("nothing is written before it is ready", !dng_write(job, 7) && job.state == DngState::developing);
        check("an image read back for another id changes nothing", !dng_developed(job, 6, DngFailure::none, 12) && job.state == DngState::developing);
        const bool held = dng_developed(job, 7, DngFailure::none, 13);
        check("its own image makes it ready from that present, to be held", held && job.state == DngState::ready && job.since == 13);
        check("a write for another id is refused, and its own goes to the worker once", !dng_write(job, 8) && dng_write(job, 7)
            && job.state == DngState::writing && !dng_write(job, 7));
        check("no new DNG starts while one is written", !dng_develop(job, 8, 14) && job.id == 7);
        dng_written(job, 8, false);
        dng_written(job, 7, true);
        check("the worker's result for its id ends it written", job.state == DngState::written && job.failure == DngFailure::none);
        dng_develop(job, 8, 20);
        dng_developed(job, 8, DngFailure::none, 21);
        dng_write(job, 8);
        dng_written(job, 8, false);
        check("a file that could not be written fails it", job.state == DngState::failed && job.failure == DngFailure::write);

        DngJob late;
        dng_develop(late, 3, 100);
        check("a DNG may develop for 300 presents", !dng_expire(late, 400) && late.state == DngState::developing);
        check("and fails with no tone map draw after that", dng_expire(late, 401) && late.state == DngState::failed
            && late.failure == DngFailure::no_tonemap && !dng_expire(late, 5000));
        dng_develop(late, 4, 500);
        check("a failed read back fails it, with no image to hold", !dng_developed(late, 4, DngFailure::readback, 502) && late.state == DngState::failed
            && late.failure == DngFailure::readback);
        dng_develop(late, 5, 600);
        dng_developed(late, 5, DngFailure::none, 601);
        check("a ready image is held for 1800 presents and then dropped", !dng_expire(late, 2401) && dng_expire(late, 2402)
            && late.state == DngState::failed && late.failure == DngFailure::dropped && !dng_write(late, 5));
        dng_develop(late, 6, 3000);
        dng_developed(late, 6, DngFailure::none, 3001);
        check("a new DNG may start while one is ready, which drops it", dng_develop(late, 9, 3002) && late.id == 9 && late.state == DngState::developing
            && !dng_write(late, 6));
    }
    if (argc > 1)
    {
        // A 320x200 ramp, red across, green down and blue on the diagonal, up to 2, for macOS's raw decoder.
        const uint32_t width = 320, height = 200;
        std::vector<float> rgb(size_t(width) * height * 3);
        for (uint32_t y = 0; y < height; y++)
            for (uint32_t x = 0; x < width; x++)
            {
                float *p = &rgb[(size_t(y) * width + x) * 3];
                p[0] = 2.0f * x / width;
                p[1] = 2.0f * y / height;
                p[2] = (p[0] + p[1]) * 0.5f;
            }
        check("the 320x200 ramp is written", write_dng(std::string(argv[1]) + "/ramp.dng", { rgb.data(), width, height }));
    }
    std::printf("%s: %d failed\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
