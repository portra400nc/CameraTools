#include "jpeg.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace enhancements
{
    namespace
    {
        constexpr uint8_t kZigzag[64] = { 0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,
            7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55,
            62, 63 };

        // T.81 Annex K's tables, in row order.
        constexpr uint8_t kLumaQuant[64] = { 16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14,
            17, 22, 29, 51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72,
            92, 95, 98, 112, 100, 103, 99 };
        constexpr uint8_t kChromaQuant[64] = { 17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47,
            66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
            99, 99, 99, 99, 99, 99 };

        struct HuffmanSpec
        {
            uint8_t bits[16];
            std::vector<uint8_t> values;
        };

        const HuffmanSpec kLumaDc { { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 }, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 } };
        const HuffmanSpec kChromaDc { { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 }, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 } };
        const HuffmanSpec kLumaAc { { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d },
            { 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91,
                0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a,
                0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53,
                0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
                0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
                0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9,
                0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2,
                0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa } };
        const HuffmanSpec kChromaAc { { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 },
            { 0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14,
                0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17,
                0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a,
                0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
                0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
                0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
                0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2,
                0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa } };

        struct HuffmanCodes
        {
            std::array<uint16_t, 256> code {};
            std::array<uint8_t, 256> length {};
        };

        HuffmanCodes codes_of(const HuffmanSpec &spec)
        {
            HuffmanCodes c;
            uint32_t code = 0;
            size_t k = 0;
            for (int length = 1; length <= 16; length++, code <<= 1)
                for (int i = 0; i < spec.bits[length - 1]; i++, code++, k++)
                {
                    c.code[spec.values[k]] = static_cast<uint16_t>(code);
                    c.length[spec.values[k]] = static_cast<uint8_t>(length);
                }
            return c;
        }

        // libjpeg's jpeg_quality_scaling and jpeg_add_quant_table with force_baseline, in zigzag order.
        std::array<uint8_t, 64> scaled(const uint8_t *table, int quality)
        {
            quality = std::clamp(quality, 1, 100);
            const int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
            std::array<uint8_t, 64> out {};
            for (int k = 0; k < 64; k++)
                out[k] = static_cast<uint8_t>(std::clamp((table[kZigzag[k]] * scale + 50) / 100, 1, 255));
            return out;
        }

        class BitWriter
        {
        public:
            explicit BitWriter(std::vector<uint8_t> &out) : out_(out) {}

            void put(uint32_t value, int bits)
            {
                pending_ = (pending_ << bits) | (value & ((1u << bits) - 1));
                pending_bits_ += bits;
                while (pending_bits_ >= 8)
                {
                    const uint8_t byte = static_cast<uint8_t>(pending_ >> (pending_bits_ - 8));
                    out_.push_back(byte);
                    if (byte == 0xFF)
                        out_.push_back(0);
                    pending_bits_ -= 8;
                }
                pending_ &= (1u << pending_bits_) - 1;
            }

            void flush()
            {
                if (pending_bits_ > 0)
                    put((1u << (8 - pending_bits_)) - 1, 8 - pending_bits_);
            }

        private:
            std::vector<uint8_t> &out_;
            uint32_t pending_ = 0;
            int pending_bits_ = 0;
        };

        // The magnitude category of v and its extra bits, as T.81's F.1.2.1 codes a DC difference or an AC coefficient.
        void categorize(int v, int &category, uint32_t &extra)
        {
            const int magnitude = std::abs(v);
            category = 0;
            while ((magnitude >> category) != 0)
                category++;
            extra = static_cast<uint32_t>(v >= 0 ? v : v + (1 << category) - 1);
        }

        struct Component
        {
            const std::array<uint8_t, 64> *quant;
            const HuffmanCodes *dc, *ac;
            int previous_dc = 0;
        };

        // One 8x8 block of samples centred on 0, row order: its DCT, quantized in zigzag order, then coded.
        void put_block(BitWriter &w, const float *samples, Component &c, const std::array<std::array<float, 8>, 8> &basis)
        {
            float rows[64], coefficients[64];
            for (int y = 0; y < 8; y++)
                for (int u = 0; u < 8; u++)
                {
                    float s = 0;
                    for (int x = 0; x < 8; x++)
                        s += basis[u][x] * samples[y * 8 + x];
                    rows[y * 8 + u] = s;
                }
            for (int v = 0; v < 8; v++)
                for (int u = 0; u < 8; u++)
                {
                    float s = 0;
                    for (int y = 0; y < 8; y++)
                        s += basis[v][y] * rows[y * 8 + u];
                    coefficients[v * 8 + u] = s;
                }
            int quantized[64];
            for (int k = 0; k < 64; k++)
                quantized[k] = std::clamp(static_cast<int>(std::lround(coefficients[kZigzag[k]] / (*c.quant)[k])), -1023, 1023);

            int category;
            uint32_t extra;
            const int difference = quantized[0] - c.previous_dc;
            c.previous_dc = quantized[0];
            categorize(difference, category, extra);
            w.put(c.dc->code[category], c.dc->length[category]);
            if (category > 0)
                w.put(extra, category);
            int run = 0;
            for (int k = 1; k < 64; k++)
            {
                if (quantized[k] == 0)
                {
                    run++;
                    continue;
                }
                for (; run >= 16; run -= 16)
                    w.put(c.ac->code[0xF0], c.ac->length[0xF0]);
                categorize(quantized[k], category, extra);
                const int symbol = run << 4 | category;
                w.put(c.ac->code[symbol], c.ac->length[symbol]);
                w.put(extra, category);
                run = 0;
            }
            if (run > 0)
                w.put(c.ac->code[0], c.ac->length[0]);
        }

        void put_segment(std::vector<uint8_t> &out, uint8_t marker, const std::vector<uint8_t> &body)
        {
            const size_t length = body.size() + 2;
            out.insert(out.end(), { 0xFF, marker, uint8_t(length >> 8), uint8_t(length) });
            out.insert(out.end(), body.begin(), body.end());
        }

        void put_table(std::vector<uint8_t> &body, uint8_t id, const HuffmanSpec &spec)
        {
            body.push_back(id);
            body.insert(body.end(), spec.bits, spec.bits + 16);
            body.insert(body.end(), spec.values.begin(), spec.values.end());
        }
    }

    std::vector<uint8_t> encode_jpeg(const uint8_t *rgb, uint32_t width, uint32_t height, int quality)
    {
        const std::array<uint8_t, 64> luma_quant = scaled(kLumaQuant, quality), chroma_quant = scaled(kChromaQuant, quality);
        const HuffmanCodes luma_dc = codes_of(kLumaDc), luma_ac = codes_of(kLumaAc), chroma_dc = codes_of(kChromaDc), chroma_ac = codes_of(kChromaAc);

        std::vector<uint8_t> out = { 0xFF, 0xD8 };
        put_segment(out, 0xE0, { 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0 });
        std::vector<uint8_t> dqt = { 0 };
        dqt.insert(dqt.end(), luma_quant.begin(), luma_quant.end());
        dqt.push_back(1);
        dqt.insert(dqt.end(), chroma_quant.begin(), chroma_quant.end());
        put_segment(out, 0xDB, dqt);
        put_segment(out, 0xC0, { 8, uint8_t(height >> 8), uint8_t(height), uint8_t(width >> 8), uint8_t(width), 3, 1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1 });
        std::vector<uint8_t> dht;
        put_table(dht, 0x00, kLumaDc);
        put_table(dht, 0x10, kLumaAc);
        put_table(dht, 0x01, kChromaDc);
        put_table(dht, 0x11, kChromaAc);
        put_segment(out, 0xC4, dht);
        put_segment(out, 0xDA, { 3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0 });

        // basis[u][x] = C(u) / 2 cos((2x + 1) u pi / 16), so a block's two passes are T.81's FDCT.
        std::array<std::array<float, 8>, 8> basis {};
        for (int u = 0; u < 8; u++)
            for (int x = 0; x < 8; x++)
                basis[u][x] = static_cast<float>((u == 0 ? std::sqrt(0.5) : 1.0) / 2 * std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16));

        Component y_c { &luma_quant, &luma_dc, &luma_ac }, cb_c { &chroma_quant, &chroma_dc, &chroma_ac }, cr_c { &chroma_quant, &chroma_dc, &chroma_ac };
        BitWriter w(out);
        float luma[4][64], cb[64], cr[64];
        for (uint32_t my = 0; my < height; my += 16)
            for (uint32_t mx = 0; mx < width; mx += 16)
            {
                // A 16x16 MCU, pixels past the picture's edge repeating its last row and column; chroma the mean of each 2x2.
                std::fill(cb, cb + 64, 0.0f);
                std::fill(cr, cr + 64, 0.0f);
                for (uint32_t y = 0; y < 16; y++)
                    for (uint32_t x = 0; x < 16; x++)
                    {
                        const uint8_t *p = rgb + (size_t(std::min(my + y, height - 1)) * width + std::min(mx + x, width - 1)) * 3;
                        const float r = p[0], g = p[1], b = p[2];
                        luma[(y / 8) * 2 + x / 8][(y % 8) * 8 + x % 8] = 0.299f * r + 0.587f * g + 0.114f * b - 128;
                        cb[(y / 2) * 8 + x / 2] += 0.25f * (-0.168736f * r - 0.331264f * g + 0.5f * b);
                        cr[(y / 2) * 8 + x / 2] += 0.25f * (0.5f * r - 0.418688f * g - 0.081312f * b);
                    }
                for (const float *block : luma)
                    put_block(w, block, y_c, basis);
                put_block(w, cb, cb_c, basis);
                put_block(w, cr, cr_c, basis);
            }
        w.flush();
        out.insert(out.end(), { 0xFF, 0xD9 });
        return out;
    }
}
