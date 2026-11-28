// Gives a DNG CameraTools wrote before its files carried a preview the thumbnail and preview a file carries now, laid out as
// core/dng.cpp lays one out. Every tag of the file is kept, its profile among them, so Lightroom shows it as before, and the
// preview shows the light as that profile does. It refuses a file whose tone curve is not the game's, and leaves a file
// that has a preview alone.
//
//     dng_add_preview <in.dng> <out.dng>
//
// Exits 0 when it wrote out, 3 when in has a preview already, 1 otherwise. in and out may be the same file.
#include "../core/dng.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace enhancements;

namespace
{
    struct Tag
    {
        uint16_t type;
        uint32_t count;
        std::vector<uint8_t> data;
        bool operator==(const Tag &o) const { return type == o.type && count == o.count && data == o.data; }
    };
    using Ifd = std::map<uint16_t, Tag>;

    uint32_t u16(const std::vector<uint8_t> &b, size_t at) { return b.at(at) | b.at(at + 1) << 8; }
    uint32_t u32(const std::vector<uint8_t> &b, size_t at) { return u16(b, at) | u16(b, at + 2) << 16; }

    Ifd parse(const std::vector<uint8_t> &b, size_t at)
    {
        static const uint32_t sizes[] = { 0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8 };
        Ifd ifd;
        for (uint32_t i = 0, n = u16(b, at); i < n; i++)
        {
            const size_t e = at + 2 + 12 * size_t(i);
            const uint16_t type = static_cast<uint16_t>(u16(b, e + 2));
            const uint32_t count = u32(b, e + 4);
            const size_t bytes = size_t(count) * sizes[type < 13 ? type : 1], from = bytes <= 4 ? e + 8 : u32(b, e + 8);
            if (from + bytes > b.size())
                throw std::runtime_error("a tag's values run past the file's end");
            ifd[static_cast<uint16_t>(u16(b, e))] = { type, count, std::vector<uint8_t>(b.begin() + from, b.begin() + from + bytes) };
        }
        return ifd;
    }

    uint32_t value(const Ifd &ifd, uint16_t tag, size_t index = 0)
    {
        const Tag &t = ifd.at(tag);
        return t.type == 3 ? u16(t.data, index * 2) : u32(t.data, index * 4);
    }

    // A lossless JPEG tile as dng_lossless_jpeg writes it, back to its samples: one Huffman table, predictor 1.
    std::vector<uint16_t> decode_tile(const uint8_t *b, size_t size, uint32_t &width, uint32_t &height)
    {
        std::vector<uint8_t> length(1 << 16), symbol(1 << 16);
        size_t at = 2;
        while (at + 4 <= size && b[at] == 0xFF && b[at + 1] != 0xDA)
        {
            if (b[at + 1] == 0xC3)
            {
                height = uint32_t(b[at + 5]) << 8 | b[at + 6];
                width = uint32_t(b[at + 7]) << 8 | b[at + 8];
            }
            if (b[at + 1] == 0xC4)
            {
                uint32_t code = 0, k = 0;
                for (uint32_t bits = 1; bits <= 16; bits++, code <<= 1)
                    for (uint32_t i = 0; i < b[at + 4 + bits]; i++, code++, k++)
                        for (uint32_t j = 0; j < 1u << (16 - bits); j++)
                        {
                            length[(code << (16 - bits)) + j] = static_cast<uint8_t>(bits);
                            symbol[(code << (16 - bits)) + j] = b[at + 21 + k];
                        }
            }
            at += 2 + (size_t(b[at + 2]) << 8 | b[at + 3]);
        }
        if (at + 4 > size)
            return {};
        at += 2 + (size_t(b[at + 2]) << 8 | b[at + 3]);
        std::vector<uint8_t> data;
        for (; at + 1 < size && !(b[at] == 0xFF && b[at + 1] == 0xD9); at++)
        {
            data.push_back(b[at]);
            if (b[at] == 0xFF)
                at++;
        }
        data.resize(data.size() + 8, 0);
        size_t pos = 0;
        const auto peek = [&](uint32_t n)
        {
            uint64_t v = 0;
            for (size_t i = 0; i < 8; i++)
                v = v << 8 | data[(pos >> 3) + i];
            return uint32_t((v << (pos & 7)) >> (64 - n));
        };
        std::vector<uint16_t> out(size_t(width) * height * 3);
        for (size_t i = 0; i < out.size(); i++)
        {
            if ((pos >> 3) + 8 > data.size())
                return {};
            const uint32_t key = peek(16);
            if (length[key] == 0)
                return {};
            const int category = symbol[key];
            pos += length[key];
            int difference = 32768;
            if (category < 16)
            {
                const int v = category == 0 ? 0 : int(peek(uint32_t(category)));
                pos += size_t(category);
                difference = category == 0 ? 0 : v >= 1 << (category - 1) ? v : v - (1 << category) + 1;
            }
            const size_t x = (i / 3) % width;
            const int predicted = x > 0 ? out[i - 3] : i >= size_t(width) * 3 ? out[i - size_t(width) * 3] : 32768;
            out[i] = static_cast<uint16_t>((predicted + difference) & 0xFFFF);
        }
        return out;
    }

    std::vector<std::vector<uint8_t>> tiles_of(const std::vector<uint8_t> &b, const Ifd &raw)
    {
        std::vector<std::vector<uint8_t>> tiles;
        for (uint32_t i = 0; i < raw.at(324).count; i++)
        {
            const size_t at = value(raw, 324, i), size = value(raw, 325, i);
            if (at + size > b.size())
                throw std::runtime_error("has a tile that runs past the file's end");
            tiles.emplace_back(b.begin() + at, b.begin() + at + size);
        }
        return tiles;
    }

    std::vector<uint8_t> read(const char *path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            throw std::runtime_error("cannot be read");
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
    }

    // The tags of the raw image's IFD, which IFD0 held before the file carried a preview.
    const std::set<uint16_t> kRawTags = { 254, 256, 257, 258, 259, 262, 277, 284, 322, 323, 324, 325, 339, 50717 };

    Tag longs(std::initializer_list<uint32_t> values)
    {
        Tag t { 4, uint32_t(values.size()), {} };
        for (uint32_t v : values)
            for (int shift = 0; shift < 32; shift += 8)
                t.data.push_back(static_cast<uint8_t>(v >> shift));
        return t;
    }

    Tag shorts(std::initializer_list<uint32_t> values)
    {
        Tag t { 3, uint32_t(values.size()), {} };
        for (uint32_t v : values)
            t.data.insert(t.data.end(), { static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8) });
        return t;
    }

    uint32_t padded(size_t size) { return static_cast<uint32_t>(size + size % 2); }

    uint32_t ifd_size(const Ifd &ifd)
    {
        uint32_t size = 2 + 12 * uint32_t(ifd.size()) + 4;
        for (const auto &[tag, t] : ifd)
            if (t.data.size() > 4)
                size += padded(t.data.size());
        return size;
    }

    // As core/dng.cpp's put_ifd: the entries in tag order, then their values longer than 4 bytes in entry order.
    void put_ifd(std::vector<uint8_t> &out, const Ifd &ifd)
    {
        const auto put = [&](uint32_t v, int bytes) { for (int i = 0; i < bytes; i++) out.push_back(static_cast<uint8_t>(v >> (8 * i))); };
        uint32_t extra = uint32_t(out.size()) + 2 + 12 * uint32_t(ifd.size()) + 4;
        put(uint32_t(ifd.size()), 2);
        for (const auto &[tag, t] : ifd)
        {
            put(tag, 2);
            put(t.type, 2);
            put(t.count, 4);
            if (t.data.size() <= 4)
            {
                out.insert(out.end(), t.data.begin(), t.data.end());
                out.insert(out.end(), 4 - t.data.size(), 0);
            }
            else
            {
                put(extra, 4);
                extra += padded(t.data.size());
            }
        }
        put(0, 4);
        for (const auto &[tag, t] : ifd)
            if (t.data.size() > 4)
            {
                out.insert(out.end(), t.data.begin(), t.data.end());
                if (t.data.size() % 2)
                    out.push_back(0);
            }
    }

    void put_bytes(std::vector<uint8_t> &out, const std::vector<uint8_t> &bytes)
    {
        out.insert(out.end(), bytes.begin(), bytes.end());
        if (bytes.size() % 2)
            out.push_back(0);
    }

    double game_tone(double x)
    {
        return x * (1.36 * x + 0.047) / (x * (0.93 * x + 0.56) + 0.14);
    }

    // The light the game's curve takes of a sample's 1. The file's linear 1 is its headroom, a power of two. Files since
    // 2026-11-27 say so with the game's curve of the headroom times the value and no BaselineExposure; those from
    // 2026-11-16 with the curve of the game's white times the value and the stops from that white to the headroom as their
    // BaselineExposure. Both are the curve's scale times 2^BaselineExposure.
    double scene_per_sample(const Ifd &first)
    {
        const Tag &curve = first.at(50940);
        std::vector<float> points(curve.count);
        std::memcpy(points.data(), curve.data.data(), points.size() * sizeof(float));
        if (points.size() < 260)
            throw std::runtime_error("has a tone curve of too few points");
        double lo = 0, hi = 64;
        for (int i = 0; i < 100; i++)
        {
            const double mid = (lo + hi) / 2;
            (game_tone(mid * points[128]) < points[129] ? lo : hi) = mid;
        }
        const double scale = (lo + hi) / 2;
        for (size_t i = 0; i + 1 < points.size(); i += 2)
            if (std::fabs(std::min(1.0, game_tone(scale * points[i])) - points[i + 1]) > 1e-5)
                throw std::runtime_error("has a tone curve that is not the game's");
        double stops = std::log2(scale);
        if (first.count(50730))
            stops += double(int32_t(value(first, 50730, 0))) / int32_t(value(first, 50730, 1));
        const double headroom = std::exp2(std::round(stops));
        if (std::fabs(stops - std::log2(headroom)) > 0.01 || headroom < 1 || headroom > 16)
            throw std::runtime_error("has no headroom that is a power of two from 1 to 16");
        return headroom / 65535.0;
    }

    int run(const char *in_path, const char *out_path)
    {
        const std::vector<uint8_t> old = read(in_path);
        if (old.size() < 8 || old[0] != 'I' || old[1] != 'I' || u16(old, 2) != 42)
            throw std::runtime_error("is not a little-endian TIFF");
        const Ifd first = parse(old, u32(old, 4));
        if (first.count(330))
            return 3;
        if (!first.count(271) || std::strcmp(reinterpret_cast<const char *>(first.at(271).data.data()), "CameraTools") != 0
            || value(first, 262) != 34892 || value(first, 259) != 7 || value(first, 254) != 0 || !first.count(50940))
            throw std::runtime_error("is not a raw image CameraTools wrote");

        const uint32_t width = value(first, 256), height = value(first, 257), tile = value(first, 322);
        const uint32_t across = (width + tile - 1) / tile;
        std::vector<uint16_t> samples(size_t(width) * height * 3);
        const std::vector<std::vector<uint8_t>> tiles = tiles_of(old, first);
        for (size_t i = 0; i < tiles.size(); i++)
        {
            uint32_t w = 0, h = 0;
            const std::vector<uint16_t> decoded = decode_tile(tiles[i].data(), tiles[i].size(), w, h);
            if (decoded.empty() || w != tile || h != tile)
                throw std::runtime_error("has a tile that does not decode");
            const uint32_t x0 = uint32_t(i % across) * tile, y0 = uint32_t(i / across) * tile;
            for (uint32_t y = y0; y < std::min(y0 + tile, height); y++)
                for (uint32_t x = x0; x < std::min(x0 + tile, width); x++)
                    std::memcpy(&samples[(size_t(y) * width + x) * 3], &decoded[(size_t(y - y0) * tile + (x - x0)) * 3], 3 * sizeof(uint16_t));
        }
        const DngPreview shown = dng_preview(samples.data(), width, height, scene_per_sample(first));

        Ifd ifd0, raw, preview;
        for (const auto &[tag, t] : first)
            (kRawTags.count(tag) ? raw : ifd0)[tag] = t;
        const DngThumbnail &thumb = shown.thumbnail;
        ifd0[254] = longs({ 1 });
        ifd0[256] = longs({ thumb.width });
        ifd0[257] = longs({ thumb.height });
        ifd0[258] = shorts({ 8, 8, 8 });
        ifd0[259] = shorts({ 1 });
        ifd0[262] = shorts({ 2 });
        ifd0[273] = longs({ 0 });
        ifd0[277] = shorts({ 3 });
        ifd0[278] = longs({ thumb.height });
        ifd0[279] = longs({ uint32_t(thumb.pixels.size()) });
        ifd0[284] = shorts({ 1 });
        ifd0[330] = longs({ 0, 0 });
        preview = { { 254, longs({ 1 }) }, { 256, longs({ width }) }, { 257, longs({ height }) }, { 258, shorts({ 8, 8, 8 }) },
            { 259, shorts({ 7 }) }, { 262, shorts({ 6 }) }, { 273, longs({ 0 }) }, { 277, shorts({ 3 }) }, { 278, longs({ height }) },
            { 279, longs({ uint32_t(shown.jpeg.size()) }) }, { 284, shorts({ 1 }) }, { 530, shorts({ 2, 2 }) }, { 50970, longs({ 2 }) } };
        const Ifd exif = parse(old, value(first, 34665));

        const uint32_t exif_at = 8 + ifd_size(ifd0), raw_at = exif_at + ifd_size(exif), preview_at = raw_at + ifd_size(raw);
        const uint32_t thumbnail_at = preview_at + ifd_size(preview), jpeg_at = thumbnail_at + padded(thumb.pixels.size());
        uint32_t tile_at = jpeg_at + padded(shown.jpeg.size());
        ifd0[273] = longs({ thumbnail_at });
        ifd0[330] = longs({ raw_at, preview_at });
        ifd0[34665] = longs({ exif_at });
        preview[273] = longs({ jpeg_at });
        raw[324].data.clear();
        for (const std::vector<uint8_t> &t : tiles)
        {
            for (int shift = 0; shift < 32; shift += 8)
                raw[324].data.push_back(static_cast<uint8_t>(tile_at >> shift));
            tile_at += padded(t.size());
        }

        std::vector<uint8_t> written = { 'I', 'I', 42, 0, 8, 0, 0, 0 };
        written.reserve(tile_at);
        put_ifd(written, ifd0);
        put_ifd(written, exif);
        put_ifd(written, raw);
        put_ifd(written, preview);
        put_bytes(written, thumb.pixels);
        put_bytes(written, shown.jpeg);
        for (const std::vector<uint8_t> &t : tiles)
            put_bytes(written, t);

        const std::string part = std::string(out_path) + ".part";
        {
            std::ofstream out(part, std::ios::binary);
            out.write(reinterpret_cast<const char *>(written.data()), static_cast<std::streamsize>(written.size()));
            if (!out.flush())
                throw std::runtime_error("could not be written");
        }
        if (std::rename(part.c_str(), out_path) != 0)
            throw std::runtime_error("could not be renamed into place");
        std::printf("%s: %zu -> %zu bytes\n", out_path, old.size(), written.size());
        return 0;
    }
}

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: %s <in.dng> <out.dng>\n", argv[0]);
        return 2;
    }
    try
    {
        const int status = run(argv[1], argv[2]);
        if (status == 3)
            std::printf("%s: has a preview already\n", argv[1]);
        return status;
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "%s %s\n", argv[1], e.what());
        return 1;
    }
}
