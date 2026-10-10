#include "fixture.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace photoreal::fixture
{
    namespace
    {
        const std::map<std::string, Format> kFormats = {
            { "R32G8X24_TYPELESS", Format::r32g8x24_typeless },
            { "D32_FLOAT_S8X24_UINT", Format::d32_float_s8x24_uint },
            { "R10G10B10A2_UNORM", Format::r10g10b10a2_unorm },
            { "R11G11B10_FLOAT", Format::r11g11b10_float },
            { "R8G8B8A8_TYPELESS", Format::r8g8b8a8_typeless },
            { "R8G8B8A8_UNORM", Format::r8g8b8a8_unorm },
            { "R8G8B8A8_UNORM_SRGB", Format::r8g8b8a8_unorm_srgb },
            { "R8G8_UNORM", Format::r8g8_unorm },
            { "R16_TYPELESS", Format::r16_typeless },
            { "D16_UNORM", Format::d16_unorm },
            { "R16_UNORM", Format::r16_unorm },
            { "R8_TYPELESS", Format::r8_typeless },
            { "R8_UNORM", Format::r8_unorm },
        };

        std::vector<std::string> split(const std::string &text, char separator)
        {
            std::vector<std::string> parts;
            std::stringstream stream(text);
            for (std::string part; std::getline(stream, part, separator);)
                parts.push_back(part);
            return parts;
        }

        Texture texture(const std::string &text)
        {
            const std::vector<std::string> parts = split(text, ':');
            const std::vector<std::string> size = split(parts.at(2), 'x');
            const auto format = kFormats.find(parts.at(1));
            return { static_cast<ResourceId>(std::stoull(parts.at(0))), format != kFormats.end() ? format->second : Format::unknown,
                { static_cast<uint32_t>(std::stoul(size.at(0))), static_cast<uint32_t>(std::stoul(size.at(1))) } };
        }

        std::vector<Texture> textures(const std::string &text)
        {
            std::vector<Texture> list;
            if (text != "-")
                for (const std::string &part : split(text, ','))
                    list.push_back(texture(part));
            return list;
        }

        template <class F>
        void each_texture(Row &row, F f)
        {
            for (uint8_t i = 0; i < row.targets.count; i++)
                f(row.targets.color[i]);
            if (row.targets.depth)
                f(*row.targets.depth);
            f(row.cleared);
            for (Texture &t : row.draw.inputs)
                f(t);
        }
    }

    std::vector<Row> load(const std::string &path)
    {
        std::ifstream file(path);
        if (!file)
            throw std::runtime_error("cannot open " + path);
        std::vector<Row> rows;
        std::string line;
        std::getline(file, line);
        while (std::getline(file, line))
        {
            const std::vector<std::string> cols = split(line, '\t');
            Row row;
            row.seq = static_cast<uint32_t>(std::stoul(cols.at(0)));
            const std::string &kind = cols.at(1);
            if (kind == "bind")
            {
                row.kind = Row::Kind::bind;
                const std::vector<Texture> color = textures(cols.at(2));
                row.targets.count = static_cast<uint8_t>(std::min<size_t>(color.size(), kMaxTargets));
                std::copy_n(color.begin(), row.targets.count, row.targets.color.begin());
                if (cols.at(3) != "-")
                    row.targets.depth = texture(cols.at(3));
            }
            else if (kind == "clear")
            {
                row.kind = Row::Kind::clear;
                row.cleared = texture(cols.at(2));
                const std::vector<std::string> color = split(cols.at(5), ',');
                for (size_t i = 0; i < 4 && i < color.size(); i++)
                    row.color[i] = std::stof(color[i]);
            }
            else
            {
                row.kind = Row::Kind::draw;
                row.draw.inputs = textures(cols.at(4));
            }
            rows.push_back(std::move(row));
        }
        return rows;
    }

    FrameEvent event(Row &row)
    {
        switch (row.kind)
        {
        case Row::Kind::bind:
            return BindTargets { row.targets };
        case Row::Kind::clear:
            return ClearTarget { row.cleared, row.color };
        case Row::Kind::draw:
            break;
        }
        return Draw { row.draw };
    }

    void rescale(std::vector<Row> &rows, Size from, Size to, Size to_quarter)
    {
        const Size from_quarter { from.width / 4, from.height / 4 };
        for (Row &row : rows)
            each_texture(row, [&](Texture &t) {
                if (t.size == from)
                    t.size = to;
                else if (t.size == from_quarter)
                    t.size = to_quarter;
            });
    }

    void drop(std::vector<Row> &rows, uint32_t first, uint32_t last)
    {
        rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const Row &r) { return r.seq >= first && r.seq <= last; }), rows.end());
    }

    std::vector<Row> slice(const std::vector<Row> &rows, uint32_t first, uint32_t last)
    {
        std::vector<Row> out;
        std::copy_if(rows.begin(), rows.end(), std::back_inserter(out), [&](const Row &r) { return r.seq >= first && r.seq <= last; });
        return out;
    }

    void remap(std::vector<Row> &rows, const std::vector<std::pair<uint64_t, uint64_t>> &ids)
    {
        for (Row &row : rows)
            each_texture(row, [&](Texture &t) {
                for (const auto &[from, to] : ids)
                    if (static_cast<uint64_t>(t.id) == from)
                    {
                        t.id = static_cast<ResourceId>(to);
                        break;
                    }
            });
    }

    void insert_before(std::vector<Row> &rows, uint32_t seq, const std::vector<Row> &extra)
    {
        rows.insert(std::find_if(rows.begin(), rows.end(), [&](const Row &r) { return r.seq == seq; }), extra.begin(), extra.end());
    }

    Row &find(std::vector<Row> &rows, uint32_t seq)
    {
        const auto it = std::find_if(rows.begin(), rows.end(), [&](const Row &r) { return r.seq == seq; });
        if (it == rows.end())
            throw std::runtime_error("no row with seq " + std::to_string(seq));
        return *it;
    }

    std::vector<Moment> replay(std::vector<Row> &rows, FrameTracker &tracker)
    {
        std::vector<Moment> moments;
        tracker.begin_frame();
        for (Row &row : rows)
            if (const std::optional<Step> step = tracker.feed(event(row)))
                moments.push_back({ row.seq, *step });
        return moments;
    }
}
