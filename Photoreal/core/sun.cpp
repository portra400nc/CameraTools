#include "sun.h"

#include <cmath>
#include <cstring>

namespace photoreal
{
    namespace
    {
        // Each buffer's first row in SunCbuffers, in rows of 16 bytes.
        constexpr uint32_t kB0 = 0, kB1 = 768 / 16, kB2 = kB1 + 192 / 16, kB3 = kB2 + 352 / 16;
        static_assert(kB3 * 16 + 912 == kSunCbufferTotal);

        Row row(const SunCbuffers &bytes, uint32_t index)
        {
            Row r;
            std::memcpy(r.data(), bytes.data() + index * 16, sizeof r);
            return r;
        }

        Vec3 xyz(const Row &r) { return { r[0], r[1], r[2] }; }
        float dot(const Vec3 &a, const Vec3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
        float length(const Vec3 &a) { return std::sqrt(dot(a, a)); }
        Vec3 minus(const Vec3 &a, const Vec3 &b) { return { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }

        bool finite(const Row &r) { return std::isfinite(r[0]) && std::isfinite(r[1]) && std::isfinite(r[2]) && std::isfinite(r[3]); }

        bool rows_finite(const SunCbuffers &bytes, uint32_t first, uint32_t count)
        {
            for (uint32_t i = first; i < first + count; i++)
                if (!finite(row(bytes, i)))
                    return false;
            return true;
        }

        bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

        // The depth gradient of a cascade's matrix: toward the sun, 1 / depth range long.
        Vec3 depth_gradient(const std::array<Row, 4> &m) { return { m[0][2], m[1][2], m[2][2] }; }

        SunParse reject(SunCheck check) { return { check, {} }; }
    }

    const std::array<const char *, static_cast<size_t>(SunCheck::count)> kSunCheckNames = {
        "ok", "unread", "signature", "atlas-changed", "not-finite", "atlas", "tiles", "radii-rows", "radii-order", "depth-range", "basis",
        "camera",
    };

    SunParse parse_sun_shadows(const SunCbuffers &bytes)
    {
        if (!finite(row(bytes, kB0)) || !finite(row(bytes, kB0 + 10)) || !rows_finite(bytes, kB0 + 12, kMaxSunCascades)
            || !finite(row(bytes, kB1 + 5)) || !rows_finite(bytes, kB2 + 10, 4) || !rows_finite(bytes, kB2 + 18, 3)
            || !rows_finite(bytes, kB3, 10) || !finite(row(bytes, kB3 + 18)))
            return reject(SunCheck::not_finite);
        SunShadowConstants c;

        const Row texel = row(bytes, kB0);
        if (texel[2] < 1 || texel[2] > 16384 || texel[3] < 1 || texel[3] > 16384 || texel[2] != std::floor(texel[2])
            || texel[3] != std::floor(texel[3]) || !near(texel[0] * texel[2], 1, 1e-4f) || !near(texel[1] * texel[3], 1, 1e-4f))
            return reject(SunCheck::atlas);
        c.atlas = { static_cast<uint32_t>(texel[2]), static_cast<uint32_t>(texel[3]) };

        // Cascade 0's tile center is half a tile in from the top-left corner, so it gives the grid.
        const Row first_center = row(bytes, kB3 + 18);
        if (!(first_center[0] > 0.05f && first_center[0] <= 0.5f && first_center[1] > 0.05f && first_center[1] <= 0.5f))
            return reject(SunCheck::tiles);
        c.columns = static_cast<uint32_t>(std::lround(0.5f / first_center[0]));
        c.rows = static_cast<uint32_t>(std::lround(0.5f / first_center[1]));
        if (c.atlas.width % c.columns != 0 || c.atlas.height % c.rows != 0 || c.atlas.width / c.columns != c.atlas.height / c.rows)
            return reject(SunCheck::atlas);
        // The used cascades are those up to the first whose tile center is off the grid.
        for (uint32_t i = 0; i < kMaxSunCascades && i < c.columns * c.rows; i++)
        {
            const Row center = row(bytes, kB3 + 18 + 4 * i);
            if (!std::isfinite(center[0]) || !near(center[0], (i % c.columns + 0.5f) / c.columns, 1e-3f)
                || !near(center[1], (i / c.columns + 0.5f) / c.rows, 1e-3f))
                break;
            c.cascades = i + 1;
        }
        if (c.cascades == 0)
            return reject(SunCheck::tiles);

        for (uint32_t i = 0; i < c.cascades; i++)
            if (!rows_finite(bytes, kB3 + 15 + 4 * i, 4) || !finite(row(bytes, kB3 + 49 + i)))
                return reject(SunCheck::not_finite);
        c.shadow_distance = row(bytes, kB0 + 10)[0];
        if (!(c.shadow_distance > 0))
            return reject(SunCheck::not_finite);

        for (uint32_t i = 0; i < kMaxSunCascades; i++)
        {
            const float radius2 = row(bytes, kB3 + i)[3];
            if (!near(row(bytes, kB3 + 8 + i / 4)[i % 4], radius2, 1e-6f * std::fabs(radius2)))
                return reject(SunCheck::radii_rows);
        }
        for (uint32_t i = 0; i < c.cascades; i++)
        {
            SunCascade &cascade = c.cascade[i];
            const Row sphere = row(bytes, kB3 + i);
            cascade.center = xyz(sphere);
            cascade.radius = std::sqrt(std::fmax(sphere[3], 0.0f));
            if (!(cascade.radius > (i == 0 ? 0 : c.cascade[i - 1].radius)))
                return reject(SunCheck::radii_order);
            for (uint32_t k = 0; k < 4; k++)
                cascade.matrix[k] = row(bytes, kB3 + 15 + 4 * i + k);
            cascade.origin = xyz(row(bytes, kB3 + 49 + i));
            const float gradient = length(depth_gradient(cascade.matrix));
            if (!(gradient > 0))
                return reject(SunCheck::depth_range);
            cascade.depth_range = 1 / gradient;
            if (!near(row(bytes, kB0 + 12 + i)[0], cascade.depth_range, 0.01f * cascade.depth_range))
                return reject(SunCheck::depth_range);
        }

        for (uint32_t i = 0; i < 3; i++)
            c.basis[i] = xyz(row(bytes, kB2 + 18 + i));
        for (uint32_t i = 0; i < 3; i++)
            if (!near(length(c.basis[i]), 1, 1e-3f) || !near(dot(c.basis[i], c.basis[(i + 1) % 3]), 0, 1e-3f))
                return reject(SunCheck::basis);
        for (uint32_t i = 0; i < 4; i++)
            c.inverse_projection[i] = row(bytes, kB2 + 10 + i);

        // Cascade 0's sphere reaches back to the near plane, a few tenths of a meter in front of the camera.
        c.camera = xyz(row(bytes, kB1 + 5));
        if (!(length(minus(c.camera, c.cascade[0].center)) <= c.cascade[0].radius + 1))
            return reject(SunCheck::camera);

        const Vec3 gradient = depth_gradient(c.cascade[0].matrix);
        const float scale = c.cascade[0].depth_range;
        c.toward_sun = { gradient[0] * scale, gradient[1] * scale, gradient[2] * scale };
        return { SunCheck::ok, c };
    }

    SunCheck sun_check(const SunParse &last, bool signature_matched, Size atlas)
    {
        if (!signature_matched)
            return SunCheck::signature;
        if (last.check != SunCheck::ok)
            return last.check;
        return last.constants.atlas == atlas ? SunCheck::ok : SunCheck::atlas_changed;
    }
}
