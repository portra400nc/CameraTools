#include "trace_scene.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace photoreal
{
    namespace
    {
        // Coordinates past this many millimeters from the reference do not fit the key's int32 grid.
        constexpr double kKeyRange = 2e9;

        // How a draw's copy of a triangle ranks against another's: the main pass's over an equal-depth draw's over the
        // depth prepass's.
        uint8_t rank(const SceneDrawRecord &record)
        {
            const char *kind = draw_kind(record);
            return std::strcmp(kind, "main") == 0 ? 2 : std::strcmp(kind, "equal") == 0 ? 1 : 0;
        }

        uint64_t hash(const TriangleKey &key)
        {
            uint64_t h = 0x9E3779B97F4A7C15ull;
            for (int32_t v : key.q)
            {
                h ^= static_cast<uint32_t>(v);
                h *= 0xBF58476D1CE4E5B9ull;
                h ^= h >> 31;
            }
            return h ^ (h >> 29);
        }

        double distance2(const TraceTriangle &t)
        {
            double sum = 0;
            for (int k = 0; k < 3; k++)
            {
                const double c = (double(t.corner[0][k]) + t.corner[1][k] + t.corner[2][k]) / 3;
                sum += c * c;
            }
            return sum;
        }

        template <class T>
        void put(std::string &out, const T &value)
        {
            out.append(reinterpret_cast<const char *>(&value), sizeof value);
        }
    }

    const std::array<const char *, static_cast<size_t>(TraceClass::count)> kTraceClassNames = { "world", "vegetation", "foliage", "other", "character" };

    std::optional<TraceClass> trace_class(const SceneDrawRecord &record)
    {
        switch (draw_class(record))
        {
        case DrawClass::s128: case DrawClass::s144: return TraceClass::world;
        case DrawClass::s136: return TraceClass::vegetation;
        case DrawClass::s137: return TraceClass::foliage;
        case DrawClass::s133: case DrawClass::s165: return TraceClass::character;
        case DrawClass::s129: return std::nullopt;
        case DrawClass::pre: case DrawClass::other: case DrawClass::count: break;
        }
        return TraceClass::other;
    }

    bool leaves_depth(const SceneDrawRecord &record)
    {
        constexpr uint32_t kComparisonEqual = 3;
        return record.depth_write || record.depth_func == kComparisonEqual;
    }

    std::optional<TriangleKey> triangle_key(const std::array<Vec3d, 3> &corners)
    {
        std::array<std::array<int32_t, 3>, 3> q;
        for (int c = 0; c < 3; c++)
            for (int k = 0; k < 3; k++)
            {
                const double mm = std::floor(corners[c][k] * 1000.0 + 0.5);
                if (!(std::fabs(mm) < kKeyRange))
                    return std::nullopt;
                q[c][k] = static_cast<int32_t>(mm);
            }
        std::sort(q.begin(), q.end());
        if (q[0] == q[1] || q[1] == q[2])
            return std::nullopt;
        TriangleKey key;
        for (int c = 0; c < 3; c++)
            for (int k = 0; k < 3; k++)
                key.q[c * 3 + k] = q[c][k];
        return key;
    }

    void SceneBuilder::grow()
    {
        slots_.assign(std::max<size_t>(1024, slots_.size() * 2), 0);
        const size_t mask = slots_.size() - 1;
        for (uint32_t i = 0; i < keys_.size(); i++)
        {
            size_t at = hash(keys_[i]) & mask;
            while (slots_[at] != 0)
                at = (at + 1) & mask;
            slots_[at] = i + 1;
        }
    }

    void SceneBuilder::add_draw(const SceneDrawRecord &draw, const std::vector<std::array<Vec3d, 3>> &triangles, const AlphaDraw *alpha,
        const float *attributes)
    {
        counts_.in += triangles.size();
        if (!leaves_depth(draw))
        {
            counts_.dropped_no_depth += triangles.size();
            return;
        }
        const std::optional<TraceClass> cls = trace_class(draw);
        if (!cls)
        {
            counts_.dropped_class += triangles.size();
            return;
        }
        const uint8_t r = rank(draw);
        const uint32_t info = triangle_info(*cls);
        // The draw's alpha test, kept once for all its triangles.
        uint32_t alpha_draw = 0;
        const uint32_t floats = alpha && alpha->program ? alpha->program->attribute_floats() : 0;
        if (alpha && alpha->program && (attributes || floats == 0))
        {
            alpha_draws_.push_back(*alpha);
            alpha_draw = static_cast<uint32_t>(alpha_draws_.size());
        }
        for (size_t n = 0; n < triangles.size(); n++)
        {
            const std::array<Vec3d, 3> &corners = triangles[n];
            const uint64_t at = attributes_.size();
            const auto keep_attributes = [&] {
                if (alpha_draw != 0)
                    attributes_.insert(attributes_.end(), attributes + n * 3 * floats, attributes + (n + 1) * 3 * floats);
            };
            const std::optional<TriangleKey> key = triangle_key(corners);
            if (!key)
            {
                counts_.dropped_degenerate++;
                continue;
            }
            // Kept at most half full, so a probe ends at an empty slot soon.
            if ((keys_.size() + 1) * 2 > slots_.size())
                grow();
            const size_t mask = slots_.size() - 1;
            size_t slot = hash(*key) & mask;
            while (slots_[slot] != 0 && !(keys_[slots_[slot] - 1] == *key))
                slot = (slot + 1) & mask;
            TraceTriangle t;
            for (int c = 0; c < 3; c++)
                for (int k = 0; k < 3; k++)
                    t.corner[c][k] = static_cast<float>(corners[c][k]);
            t.info = info;
            if (slots_[slot] == 0)
            {
                slots_[slot] = static_cast<uint32_t>(keys_.size() + 1);
                keys_.push_back(*key);
                triangles_.push_back(t);
                ranks_.push_back(r);
                alpha_of_.push_back(alpha_draw);
                attributes_at_.push_back(at);
                keep_attributes();
                counts_.unique++;
                counts_.pre_only += r == 0;
                continue;
            }
            // The faces arrive in capture order, so of two copies of one kind the kept one came first.
            const uint32_t index = slots_[slot] - 1;
            if (r <= ranks_[index])
                continue;
            counts_.pre_only -= ranks_[index] == 0;
            counts_.replaced++;
            ranks_[index] = r;
            if (r == 1 && alpha_draw == 0 && alpha_of_[index] != 0)
            {
                // An equal-depth draw draws only where the prepass's alpha test kept depth, so its copy keeps that test,
                // and the corners in the order the test's floats follow.
                t.corner = triangles_[index].corner;
                triangles_[index] = t;
                counts_.inherited++;
                continue;
            }
            triangles_[index] = t;
            alpha_of_[index] = alpha_draw;
            attributes_at_[index] = at;
            keep_attributes();
        }
    }

    std::vector<TraceTriangle> SceneBuilder::take(uint64_t max_triangles, std::vector<uint32_t> *alpha)
    {
        std::vector<TraceTriangle> out = std::move(triangles_);
        triangles_ = {};
        keys_ = {};
        ranks_ = {};
        slots_ = {};
        // Each kept triangle's index among out, nearest first past max_triangles, ties by arrival, so the kept set does not
        // depend on the sort's order.
        std::vector<uint32_t> kept_index;
        if (out.size() <= max_triangles)
        {
            kept_index.resize(out.size());
            for (uint32_t i = 0; i < out.size(); i++)
                kept_index[i] = i;
        }
        else
        {
            std::vector<std::pair<double, uint32_t>> order(out.size());
            for (uint32_t i = 0; i < out.size(); i++)
                order[i] = { distance2(out[i]), i };
            std::nth_element(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(max_triangles), order.end());
            order.resize(max_triangles);
            std::sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
            for (const auto &o : order)
                kept_index.push_back(o.second);
            counts_.past_max = out.size() - kept_index.size();
        }
        std::vector<TraceTriangle> kept;
        kept.reserve(kept_index.size());
        AlphaBuffer buffer;
        std::vector<uint32_t> draw_offsets(alpha_draws_.size(), kNoRecord);
        for (const uint32_t i : kept_index)
        {
            kept.push_back(out[i]);
            if (!alpha || alpha_of_[i] == 0)
                continue;
            // A draw's block and its program go into the buffer at its first kept triangle.
            const AlphaDraw &d = alpha_draws_[alpha_of_[i] - 1];
            uint32_t &draw = draw_offsets[alpha_of_[i] - 1];
            if (draw == kNoRecord)
                draw = buffer.add_draw(buffer.add_program(d.program), d.tile, d.address, d.rows);
            const uint32_t record = buffer.add_record(draw, attributes_.data() + attributes_at_[i], d.program->attribute_floats());
            if (record == kNoRecord)
            {
                counts_.alpha_past_max++;
                continue;
            }
            kept.back().info = triangle_info(info_class(kept.back().info), record);
            counts_.alpha++;
        }
        if (alpha)
            *alpha = counts_.alpha == 0 ? std::vector<uint32_t> {} : buffer.words(atlas_);
        alpha_draws_ = {};
        alpha_of_ = {};
        attributes_at_ = {};
        attributes_ = {};
        atlas_ = {};
        return kept;
    }

    void add_face(SceneBuilder &builder, const SceneCamera &camera, const Vec3d &origin, const std::vector<SceneDrawRecord> &records,
        const std::vector<float> &clip, const std::vector<float> &attributes, const std::vector<uint32_t> &constants)
    {
        std::vector<std::array<Vec3d, 3>> triangles;
        for (const SceneDrawRecord &record : records)
        {
            if (!streamed(record) || record.written == 0)
                continue;
            // The draw's alpha test, when its program's inputs, attributes and rows all came back.
            std::optional<AlphaDraw> alpha;
            const AlphaProgram *program = record.alpha == DrawAlpha::program && record.analysis ? record.analysis->program.get() : nullptr;
            const uint64_t floats = program ? program->attribute_floats() : 0;
            if (program && record.stride == kCornerBytes + 4 * floats && record.first_attribute + record.written * 3 * floats <= attributes.size()
                && record.alpha_constants + uint64_t(record.alpha_rows) * 4 <= constants.size())
            {
                alpha = AlphaDraw { record.analysis->program, record.alpha_tile, record.alpha_address,
                    std::vector<uint32_t>(constants.begin() + static_cast<std::ptrdiff_t>(record.alpha_constants),
                        constants.begin() + static_cast<std::ptrdiff_t>(record.alpha_constants + uint64_t(record.alpha_rows) * 4)) };
            }
            triangles.clear();
            const size_t first = size_t(record.first_triangle) * 12;
            for (uint64_t t = 0; t < record.written && first + (t + 1) * 12 <= clip.size(); t++)
            {
                std::array<Vec3d, 3> &corners = triangles.emplace_back();
                for (int k = 0; k < 3; k++)
                {
                    const float *c = &clip[first + t * 12 + k * 4];
                    // A corner at infinity becomes NaN, which triangle_key refuses.
                    const std::optional<Vec3d> world = clip_to_world(camera, { c[0], c[1], c[2], c[3] });
                    for (int i = 0; i < 3; i++)
                        corners[k][i] = world ? (*world)[i] - origin[i] : NAN;
                }
            }
            builder.add_draw(record, triangles, alpha ? &*alpha : nullptr, alpha ? attributes.data() + record.first_attribute : nullptr);
        }
    }

    std::vector<TraceTriangle> self_test_scene(const Vec3d &forward)
    {
        const Vec3d f = forward, r = { forward[2], 0, -forward[0] };   // right of forward, with +Y up
        const auto at = [&](double ahead, double side, double up) {
            return Vec3f { float(f[0] * ahead + r[0] * side), float(up), float(f[2] * ahead + r[2] * side) };
        };
        std::vector<TraceTriangle> out(3);
        out[0].corner = { Vec3f { -12, 4, -12 }, Vec3f { 12, 4, -12 }, Vec3f { 0, 4, 14 } };
        out[1].corner = { at(5, -2, -3), at(5, 2, -3), at(5, 0, 1) };
        out[2].corner = { at(1, -1, -1), at(1, 1, -1), at(3, 0, -1) };
        return out;
    }

    std::string trace_scene_bytes(const std::vector<TraceTriangle> &triangles, const Vec3d &reference, const std::vector<uint32_t> &alpha)
    {
        std::string out = "PRTS";
        out.reserve(4 + 4 + 24 + triangles.size() * 40 + (alpha.empty() ? 0 : 4 + alpha.size() * 4));
        put(out, static_cast<uint32_t>(triangles.size()));
        for (double v : reference)
            put(out, v);
        for (const TraceTriangle &t : triangles)
        {
            for (const Vec3f &c : t.corner)
                for (float v : c)
                    put(out, v);
            put(out, t.info);
        }
        if (!alpha.empty())
        {
            put(out, static_cast<uint32_t>(alpha.size()));
            out.append(reinterpret_cast<const char *>(alpha.data()), alpha.size() * 4);
        }
        return out;
    }

    std::optional<TraceSceneFile> parse_trace_scene(const std::string &bytes)
    {
        uint32_t count = 0;
        if (bytes.size() < 32 || bytes.compare(0, 4, "PRTS") != 0)
            return std::nullopt;
        std::memcpy(&count, bytes.data() + 4, sizeof count);
        const size_t triangles_end = 32 + size_t(count) * 40;
        uint32_t words = 0;
        if (bytes.size() > triangles_end && (bytes.size() < triangles_end + 4
            || (std::memcpy(&words, bytes.data() + triangles_end, 4), bytes.size() != triangles_end + 4 + size_t(words) * 4) || words == 0))
            return std::nullopt;
        if (bytes.size() < triangles_end)
            return std::nullopt;
        TraceSceneFile file;
        std::memcpy(file.reference.data(), bytes.data() + 8, 24);
        file.triangles.resize(count);
        for (uint32_t i = 0; i < count; i++)
        {
            const char *at = bytes.data() + 32 + size_t(i) * 40;
            std::memcpy(file.triangles[i].corner.data(), at, 36);
            std::memcpy(&file.triangles[i].info, at + 36, 4);
        }
        file.alpha.resize(words);
        if (words != 0)
            std::memcpy(file.alpha.data(), bytes.data() + triangles_end + 4, size_t(words) * 4);
        return file;
    }
}
