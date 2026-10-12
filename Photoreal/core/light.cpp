#include "light.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>

// No fused multiply-adds, and below only sums, products, quotients, square roots and powers of two, which round the
// same everywhere, so a bake on the Mac (arm64) gives the Deck's bits (x86-64 without FMA), as for the BVH.
#pragma STDC FP_CONTRACT OFF

namespace photoreal
{
    namespace
    {
        // The fifth root by Newton's steps from 1, which fall to it from above for a in (0, 1]: pow would round as its
        // library pleases.
        double fifth_root(double a)
        {
            double y = 1;
            for (int i = 0; i < 64; i++)
            {
                const double y2 = y * y;
                y = (4 * y + a / (y2 * y2)) / 5;
            }
            return y;
        }

        struct SrgbTable
        {
            float linear[256];
            SrgbTable()
            {
                for (int i = 0; i < 256; i++)
                {
                    const double c = i / 255.0;
                    // ((c + 0.055) / 1.055)^2.4 as x^2 times the fifth root of x^2.
                    const double x = (c + 0.055) / 1.055, x2 = x * x;
                    linear[i] = static_cast<float>(c <= 0.04045 ? c / 12.92 : x2 * fifth_root(x2));
                }
            }
        };

        // A float of R11G11B10_FLOAT: 5 bits of exponent above mantissa_bits of mantissa, no sign.
        float small_float(uint32_t bits, int mantissa_bits)
        {
            const uint32_t exponent = bits >> mantissa_bits & 31u, mantissa = bits & ((1u << mantissa_bits) - 1);
            const float scale = static_cast<float>(1u << mantissa_bits);
            if (exponent == 0)
                return std::ldexp(mantissa / scale, -14);
            // Infinity reads as the largest value, and NaN as nothing.
            if (exponent == 31)
                return mantissa == 0 ? std::ldexp(2 - 1 / scale, 15) : 0.0f;
            return std::ldexp(1 + mantissa / scale, static_cast<int>(exponent) - 15);
        }

        uint32_t small_bits(float value, int mantissa_bits)
        {
            const float scale = static_cast<float>(1u << mantissa_bits);
            if (!(value > 0))
                return 0;
            if (value >= std::ldexp(2 - 1 / scale, 15))
                return 30u << mantissa_bits | ((1u << mantissa_bits) - 1);
            if (value < std::ldexp(1.0f, -14))
                return static_cast<uint32_t>(std::ldexp(value, 14) * scale);
            int exponent = 0;
            const float fraction = std::frexp(value, &exponent);     // value = fraction * 2^exponent, fraction in [0.5, 1)
            return static_cast<uint32_t>(exponent - 1 + 15) << mantissa_bits | static_cast<uint32_t>((fraction * 2 - 1) * scale);
        }

        Mat4d multiply(const Mat4d &a, const Mat4d &b)
        {
            Mat4d out {};
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                    for (int k = 0; k < 4; k++)
                        out[r * 4 + c] += a[r * 4 + k] * b[k * 4 + c];
            return out;
        }

        double luminance(const Vec3f &c) { return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]; }

        bool finite(const Vec3 &v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

        bool is_cover_stencil(uint8_t s) { return s == 129 || s == 136 || s == 137; }

        // Runs work(first, last) over [0, count) on tasks threads, in ranges of their own.
        template <class Work>
        void in_parallel(size_t count, uint32_t tasks, Work work)
        {
            tasks = static_cast<uint32_t>(std::max<size_t>(1, std::min<size_t>(tasks, count / 4096 + 1)));
            std::vector<std::thread> threads;
            for (uint32_t t = 1; t < tasks; t++)
                threads.emplace_back(work, count * t / tasks, count * (t + 1) / tasks);
            work(0, count / tasks);
            for (std::thread &thread : threads)
                thread.join();
        }

        template <class T>
        void put(std::string &out, const T &value)
        {
            out.append(reinterpret_cast<const char *>(&value), sizeof value);
        }

        void put(std::string &out, const Vec3f &v)
        {
            for (float f : v)
                put(out, f);
        }

        struct Reader
        {
            const std::string &bytes;
            size_t at = 0;
            bool ok = true;

            template <class T>
            void get(T &value)
            {
                if (at + sizeof value > bytes.size())
                {
                    ok = false;
                    return;
                }
                std::memcpy(&value, bytes.data() + at, sizeof value);
                at += sizeof value;
            }
            void get(Vec3f &v)
            {
                for (float &f : v)
                    get(f);
            }
        };
    }

    const std::array<const char *, static_cast<size_t>(AlbedoSource::count)> kAlbedoSourceNames = { "direct", "covered", "material", "class" };
    const std::array<const char *, static_cast<size_t>(LightSource::count)> kLightSourceNames = { "game", "pushed" };

    std::optional<FaceImages> face_images(uint32_t face, const SceneCamera &camera, Size size)
    {
        const std::optional<Mat4d> world_to_clip = invert(camera.clip_to_world);
        if (!world_to_clip)
            return std::nullopt;
        FaceImages images;
        images.face = face;
        images.size = size;
        images.world_to_clip = *world_to_clip;
        images.clip_to_world = camera.clip_to_world;
        return images;
    }

    void halve_face(FaceImages &f)
    {
        const Size half = { std::max(f.size.width / 2, 1u), std::max(f.size.height / 2, 1u) };
        const size_t pixels = size_t(half.width) * half.height, full = size_t(f.size.width) * f.size.height;
        const bool has_depth = f.depth.size() == full, has_albedo = f.albedo.size() == full * 4, has_stencil = f.stencil.size() == full,
            has_hdr = f.hdr.size() == full;
        std::vector<float> depth(has_depth ? pixels : 0);
        std::vector<uint8_t> albedo(has_albedo ? pixels * 4 : 0), stencil(has_stencil ? pixels : 0);
        std::vector<uint32_t> hdr(has_hdr ? pixels : 0);
        for (uint32_t y = 0; y < half.height; y++)
            for (uint32_t x = 0; x < half.width; x++)
            {
                // The four pixels, clamped at an odd size's edge, and the nearest of them: the largest reversed depth.
                size_t from[4];
                for (uint32_t k = 0; k < 4; k++)
                    from[k] = size_t(std::min(y * 2 + k / 2, f.size.height - 1)) * f.size.width + std::min(x * 2 + k % 2, f.size.width - 1);
                size_t nearest = from[0];
                if (has_depth)
                    for (size_t at : from)
                        if (f.depth[at] > f.depth[nearest])
                            nearest = at;
                const size_t to = size_t(y) * half.width + x;
                if (has_depth)
                    depth[to] = f.depth[nearest];
                if (has_stencil)
                    stencil[to] = f.stencil[nearest];
                if (has_albedo)
                    for (int c = 0; c < 4; c++)
                        albedo[to * 4 + c] = static_cast<uint8_t>((f.albedo[from[0] * 4 + c] + f.albedo[from[1] * 4 + c] + f.albedo[from[2] * 4 + c]
                            + f.albedo[from[3] * 4 + c] + 2) / 4);
                if (has_hdr)
                {
                    Vec3f sum {};
                    for (size_t at : from)
                    {
                        const Vec3f c = hdr_rgb(f.hdr[at]);
                        for (int k = 0; k < 3; k++)
                            sum[k] += c[k];
                    }
                    hdr[to] = hdr_stored({ sum[0] * 0.25f, sum[1] * 0.25f, sum[2] * 0.25f });
                }
            }
        f.size = half;
        f.depth = std::move(depth);
        f.albedo = std::move(albedo);
        f.stencil = std::move(stencil);
        f.hdr = std::move(hdr);
    }

    Vec3f hdr_rgb(uint32_t stored)
    {
        return { small_float(stored & 0x7FFu, 6), small_float(stored >> 11 & 0x7FFu, 6), small_float(stored >> 22, 5) };
    }

    uint32_t hdr_stored(const Vec3f &rgb)
    {
        return small_bits(rgb[0], 6) | small_bits(rgb[1], 6) << 11 | small_bits(rgb[2], 5) << 22;
    }

    float srgb_linear(uint8_t stored)
    {
        static const SrgbTable table;
        return table.linear[stored];
    }

    uint32_t pack_light(const Vec3f &albedo, AlbedoSource source)
    {
        uint32_t word = static_cast<uint32_t>(source) << 30;
        for (int k = 0; k < 3; k++)
        {
            const float clamped = std::fmin(std::fmax(albedo[k], 0.0f), 1.0f);
            word |= static_cast<uint32_t>(std::sqrt(clamped) * 1023.0f + 0.5f) << (k * 10);
        }
        return word;
    }

    Vec3f light_albedo(uint32_t light)
    {
        Vec3f albedo;
        for (int k = 0; k < 3; k++)
        {
            const float root = static_cast<float>(light >> (k * 10) & 1023u) * (1.0f / 1023.0f);
            albedo[k] = root * root;
        }
        return albedo;
    }

    AlbedoCounts bake_albedo(std::vector<TraceTriangle> &triangles, const std::vector<uint32_t> &materials, const std::vector<FaceImages> &faces,
        const Vec3d &reference, uint32_t tasks)
    {
        // Each face with an albedo, with its camera taking the scene's frame to clip space.
        struct Projector
        {
            const FaceImages *face;
            Mat4d scene_to_clip;
            bool stencil;
        };
        std::vector<Projector> projectors;
        for (const FaceImages &f : faces)
        {
            const size_t pixels = size_t(f.size.width) * f.size.height;
            if (pixels == 0 || f.albedo.size() != pixels * 4 || f.depth.size() != pixels)
                continue;
            const Mat4d to_world = { 1, 0, 0, reference[0], 0, 1, 0, reference[1], 0, 0, 1, reference[2], 0, 0, 0, 1 };
            projectors.push_back({ &f, multiply(f.world_to_clip, to_world), f.stencil.size() == pixels });
        }

        // 1 a triangle with direct samples, 2 with covered ones alone, 0 with neither yet.
        std::vector<uint8_t> seen(triangles.size(), 0);
        in_parallel(triangles.size(), tasks, [&](size_t first, size_t last) {
            static const double kPoints[4][3] = { { 1 / 3.0, 1 / 3.0, 1 / 3.0 }, { 2 / 3.0, 1 / 6.0, 1 / 6.0 }, { 1 / 6.0, 2 / 3.0, 1 / 6.0 },
                { 1 / 6.0, 1 / 6.0, 2 / 3.0 } };
            for (size_t i = first; i < last; i++)
            {
                TraceTriangle &t = triangles[i];
                double direct[3] = {}, covered[3] = {};
                uint32_t direct_count = 0, covered_count = 0;
                for (const auto &weights : kPoints)
                {
                    double p[3];
                    for (int k = 0; k < 3; k++)
                        p[k] = weights[0] * t.corner[0][k] + weights[1] * t.corner[1][k] + weights[2] * t.corner[2][k];
                    for (const Projector &projector : projectors)
                    {
                        const FaceImages &f = *projector.face;
                        const Mat4d &m = projector.scene_to_clip;
                        const double w = m[12] * p[0] + m[13] * p[1] + m[14] * p[2] + m[15];
                        if (!(w > 1e-3))
                            continue;
                        const double column = ((m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3]) / w * 0.5 + 0.5) * f.size.width;
                        const double row = (0.5 - 0.5 * (m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7]) / w) * f.size.height;
                        if (!(column >= 0 && column < f.size.width && row >= 0 && row < f.size.height))
                            continue;
                        const uint32_t px = static_cast<uint32_t>(column), py = static_cast<uint32_t>(row);
                        const size_t at = size_t(py) * f.size.width + px;
                        const float stored = f.depth[at];
                        if (!(stored > 0))
                            continue;
                        // The pixel's surface along the face's forward axis, from its center, as core/scene.h's depth
                        // check places it: clip w is 1 over the bottom row of clip-to-world at (ndc x, ndc y, depth, 1).
                        const double ndc_x = (px + 0.5) / f.size.width * 2 - 1, ndc_y = 1 - (py + 0.5) / f.size.height * 2;
                        const Mat4d &c = f.clip_to_world;
                        const double shown = 1 / (c[12] * ndc_x + c[13] * ndc_y + c[14] * stored + c[15]);
                        const double tolerance = std::max(0.05, 0.01 * w);
                        const bool is_direct = std::fabs(shown - w) <= tolerance;
                        if (!is_direct && !(shown < w && w - shown <= kCoverMeters && projector.stencil && is_cover_stencil(f.stencil[at])))
                            continue;
                        double *sum = is_direct ? direct : covered;
                        for (int k = 0; k < 3; k++)
                            sum[k] += srgb_linear(f.albedo[at * 4 + k]);
                        (is_direct ? direct_count : covered_count)++;
                    }
                }
                if (direct_count == 0 && covered_count == 0)
                    continue;
                const double *sum = direct_count != 0 ? direct : covered;
                const double count = direct_count != 0 ? direct_count : covered_count;
                seen[i] = direct_count != 0 ? 1 : 2;
                t.light = pack_light({ float(sum[0] / count), float(sum[1] / count), float(sum[2] / count) },
                    direct_count != 0 ? AlbedoSource::direct : AlbedoSource::covered);
            }
        });

        // The seen triangles' albedo by material, by class and over the scene, weighted by area, in the triangles' order.
        struct Mean
        {
            double sum[3] = {}, area = 0;
            void add(const Vec3f &albedo, double a)
            {
                for (int k = 0; k < 3; k++)
                    sum[k] += albedo[k] * a;
                area += a;
            }
            Vec3f value() const { return { float(sum[0] / area), float(sum[1] / area), float(sum[2] / area) }; }
        };
        const bool by_material = materials.size() == triangles.size();
        uint32_t material_count = 1;
        if (by_material)
            for (uint32_t m : materials)
                material_count = std::max(material_count, m + 1);
        std::vector<Mean> material_means(material_count);
        std::array<Mean, static_cast<size_t>(TraceClass::count)> class_means;
        Mean scene_mean;
        std::vector<double> areas(triangles.size());
        for (size_t i = 0; i < triangles.size(); i++)
        {
            const TraceTriangle &t = triangles[i];
            double e1[3], e2[3];
            for (int k = 0; k < 3; k++)
            {
                e1[k] = double(t.corner[1][k]) - t.corner[0][k];
                e2[k] = double(t.corner[2][k]) - t.corner[0][k];
            }
            const double cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2], cz = e1[0] * e2[1] - e1[1] * e2[0];
            areas[i] = 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
            if (seen[i] == 0)
                continue;
            const Vec3f albedo = light_albedo(t.light);
            material_means[by_material ? materials[i] : 0].add(albedo, areas[i]);
            const size_t cls = static_cast<size_t>(info_class(t.info));
            if (cls < class_means.size())
                class_means[cls].add(albedo, areas[i]);
            scene_mean.add(albedo, areas[i]);
        }
        AlbedoCounts counts;
        for (size_t i = 0; i < triangles.size(); i++)
        {
            TraceTriangle &t = triangles[i];
            if (seen[i] == 0)
            {
                const Mean &material = material_means[by_material ? materials[i] : 0];
                const size_t cls = static_cast<size_t>(info_class(t.info));
                if (material.area > 0)
                    t.light = pack_light(material.value(), AlbedoSource::material);
                else if (cls < class_means.size() && class_means[cls].area > 0)
                    t.light = pack_light(class_means[cls].value(), AlbedoSource::class_mean);
                else
                    t.light = pack_light(scene_mean.area > 0 ? scene_mean.value() : Vec3f { kNoAlbedo, kNoAlbedo, kNoAlbedo }, AlbedoSource::class_mean);
            }
            const size_t source = static_cast<size_t>(light_source(t.light));
            counts.triangles[source]++;
            counts.area[source] += areas[i];
        }
        return counts;
    }

    AlbedoCounts count_albedo(const std::vector<TraceTriangle> &triangles)
    {
        AlbedoCounts counts;
        for (const TraceTriangle &t : triangles)
        {
            double e1[3], e2[3];
            for (int k = 0; k < 3; k++)
            {
                e1[k] = double(t.corner[1][k]) - t.corner[0][k];
                e2[k] = double(t.corner[2][k]) - t.corner[0][k];
            }
            const double cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2], cz = e1[0] * e2[1] - e1[1] * e2[0];
            const size_t source = static_cast<size_t>(light_source(t.light));
            counts.triangles[source]++;
            counts.area[source] += 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
        }
        return counts;
    }

    Vec3f sky_direction(float u, float v)
    {
        // The map's point in -1..1, the octahedron's x and z where |x| + |z| <= 1, and its lower half folded out of the
        // corners, as sky_texel_of folds it in.
        float x = u * 2 - 1, z = v * 2 - 1;
        float y = 1 - std::fabs(x) - std::fabs(z);
        if (y < 0)
        {
            const float fx = (1 - std::fabs(z)) * (x >= 0 ? 1.0f : -1.0f), fz = (1 - std::fabs(x)) * (z >= 0 ? 1.0f : -1.0f);
            x = fx;
            z = fz;
        }
        const float length = std::sqrt(x * x + y * y + z * z);
        return { x / length, y / length, z / length };
    }

    Vec3f sky_irradiance(const SkyMap &sky, const Vec3f &n)
    {
        const float basis[9] = { 1, n[1], n[2], n[0], n[0] * n[1], n[1] * n[2], 3 * n[2] * n[2] - 1, n[0] * n[2], n[0] * n[0] - n[1] * n[1] };
        Vec3f out {};
        for (int i = 0; i < 9; i++)
            for (int k = 0; k < 3; k++)
                out[k] += sky.harmonics[i][k] * basis[i];
        return out;
    }

    namespace
    {
        // The cosine-weighted mean of the map over the hemisphere of a normal, as second-order spherical harmonics: each
        // texel's light times its solid angle, du dv over the cube of the octahedron point's length, onto the nine
        // polynomials, then each band's share of a clamped cosine over pi: 1, 2/3 and 1/4.
        void set_harmonics(SkyMap &sky)
        {
            double sums[9][3] = {}, sphere = 0;
            for (uint32_t y = 0; y < kSkyMapSide; y++)
                for (uint32_t x = 0; x < kSkyMapSide; x++)
                {
                    const double px = (x + 0.5) / kSkyMapSide * 2 - 1, pz = (y + 0.5) / kSkyMapSide * 2 - 1;
                    // The unfolded octahedron point has |x| + |y| + |z| = 1 in either half.
                    double ox = px, oz = pz, oy = 1 - std::fabs(px) - std::fabs(pz);
                    if (oy < 0)
                    {
                        ox = (1 - std::fabs(pz)) * (px >= 0 ? 1.0 : -1.0);
                        oz = (1 - std::fabs(px)) * (pz >= 0 ? 1.0 : -1.0);
                    }
                    const double length2 = ox * ox + oy * oy + oz * oz, length = std::sqrt(length2);
                    const double weight = 1 / (length2 * length);
                    const double n[3] = { ox / length, oy / length, oz / length };
                    // Real spherical harmonics, up to their constants, which the bands' factors below take.
                    const double basis[9] = { 1, n[1], n[2], n[0], n[0] * n[1], n[1] * n[2], 3 * n[2] * n[2] - 1, n[0] * n[2], n[0] * n[0] - n[1] * n[1] };
                    const float *texel = &sky.texels[(size_t(y) * kSkyMapSide + x) * 4];
                    for (int i = 0; i < 9; i++)
                        for (int k = 0; k < 3; k++)
                            sums[i][k] += texel[k] * basis[i] * weight;
                    sphere += weight;
                }
            // Each polynomial's squared constant times its band's factor: 1 / 4 pi; 3 / 4 pi times 2/3; and for the second
            // band 15 / 4 pi, 15 / 4 pi, 5 / 16 pi, 15 / 4 pi and 15 / 16 pi, times 1/4. The sum of the weights stands for
            // the sphere's 4 pi, which the map's texels cover to a part in a thousand.
            static const double kFactor[9] = { 1, 3 * (2 / 3.0), 3 * (2 / 3.0), 3 * (2 / 3.0), 15 * 0.25, 15 * 0.25, 1.25 * 0.25, 15 * 0.25, 3.75 * 0.25 };
            for (int i = 0; i < 9; i++)
                for (int k = 0; k < 3; k++)
                    sky.harmonics[i][k] = static_cast<float>(sums[i][k] / sphere * kFactor[i]);
        }

        // Fills the texels without weight from those with, by a pull-push over the map's mip pyramid: each level's texel
        // is the mean of its four that have any, and a texel with none takes its parent's.
        void fill_holes(std::vector<float> &texels)
        {
            std::vector<std::vector<float>> levels;
            levels.push_back(texels);
            for (uint32_t side = kSkyMapSide / 2; side >= 1; side /= 2)
            {
                const std::vector<float> &fine = levels.back();
                std::vector<float> coarse(size_t(side) * side * 4, 0.0f);
                for (uint32_t y = 0; y < side; y++)
                    for (uint32_t x = 0; x < side; x++)
                    {
                        float sum[3] = {}, weight = 0;
                        for (uint32_t k = 0; k < 4; k++)
                        {
                            const float *t = &fine[(size_t(y * 2 + k / 2) * side * 2 + x * 2 + k % 2) * 4];
                            for (int c = 0; c < 3; c++)
                                sum[c] += t[c] * t[3];
                            weight += t[3];
                        }
                        float *out = &coarse[(size_t(y) * side + x) * 4];
                        if (weight > 0)
                        {
                            for (int c = 0; c < 3; c++)
                                out[c] = sum[c] / weight;
                            out[3] = 1;
                        }
                    }
                levels.push_back(std::move(coarse));
            }
            for (size_t level = levels.size() - 1; level-- > 0;)
            {
                const uint32_t side = kSkyMapSide >> level;
                const std::vector<float> &coarse = levels[level + 1];
                std::vector<float> &fine = levels[level];
                for (uint32_t y = 0; y < side; y++)
                    for (uint32_t x = 0; x < side; x++)
                    {
                        float *t = &fine[(size_t(y) * side + x) * 4];
                        if (t[3] > 0)
                            continue;
                        const float *parent = &coarse[(size_t(y / 2) * (side / 2) + x / 2) * 4];
                        for (int c = 0; c < 3; c++)
                            t[c] = parent[c];
                        // Filled for the levels below, which then take it too.
                        t[3] = parent[3] > 0 ? 0.5f : 0.0f;
                    }
            }
            for (size_t i = 0; i < texels.size(); i += 4)
                for (int c = 0; c < 3; c++)
                    if (!(texels[i + 3] > 0))
                        texels[i + c] = levels[0][i + c];
        }
    }

    SkyMap build_sky_map(const std::vector<FaceImages> &faces, const Vec3f &fallback)
    {
        SkyMap sky;
        sky.texels.assign(size_t(kSkyMapSide) * kSkyMapSide * 4, 0.0f);
        // The faces with an HDR copy, and which of them is each cube face.
        std::vector<const FaceImages *> with_hdr;
        std::array<const FaceImages *, 6> cube {};
        for (const FaceImages &f : faces)
        {
            const size_t pixels = size_t(f.size.width) * f.size.height;
            if (pixels == 0 || f.hdr.size() != pixels || f.depth.size() != pixels)
                continue;
            with_hdr.push_back(&f);
            if (f.face < cube.size())
                cube[f.face] = &f;
        }
        // The face's sky at a direction: 1 with its color, 0 where the face shows geometry there, -1 when it does not
        // hold the direction. A direction is a point at infinity, so the camera's place drops out.
        const auto look = [](const FaceImages &f, const Vec3f &d, Vec3f &color) {
            const Mat4d &m = f.world_to_clip;
            const double w = m[12] * d[0] + m[13] * d[1] + m[14] * d[2];
            if (!(w > 1e-9))
                return -1;
            const double column = ((m[0] * d[0] + m[1] * d[1] + m[2] * d[2]) / w * 0.5 + 0.5) * f.size.width;
            const double row = (0.5 - 0.5 * (m[4] * d[0] + m[5] * d[1] + m[6] * d[2]) / w) * f.size.height;
            if (!(column >= 0 && column < f.size.width && row >= 0 && row < f.size.height))
                return -1;
            const size_t at = size_t(static_cast<uint32_t>(row)) * f.size.width + static_cast<uint32_t>(column);
            if (f.depth[at] != 0)
                return 0;
            color = hdr_rgb(f.hdr[at]);
            return 1;
        };
        std::vector<float> luminances;
        for (uint32_t y = 0; y < kSkyMapSide; y++)
            for (uint32_t x = 0; x < kSkyMapSide; x++)
            {
                Vec3f sum {};
                uint32_t samples = 0;
                for (uint32_t k = 0; k < 16; k++)
                {
                    const Vec3f d = sky_direction((x + (k % 4 + 0.5f) / 4) / kSkyMapSide, (y + (k / 4 + 0.5f) / 4) / kSkyMapSide);
                    // +X, -X, +Y, -Y, +Z, -Z, as PHOTOREAL_FACE_* numbers them.
                    const float ax = std::fabs(d[0]), ay = std::fabs(d[1]), az = std::fabs(d[2]);
                    const uint32_t major = ax >= ay && ax >= az ? (d[0] >= 0 ? 0 : 1) : ay >= az ? (d[1] >= 0 ? 2 : 3) : (d[2] >= 0 ? 4 : 5);
                    Vec3f color {};
                    int found = cube[major] ? look(*cube[major], d, color) : -1;
                    for (size_t f = 0; found < 0 && f < with_hdr.size(); f++)
                        if (with_hdr[f] != cube[major])
                            found = look(*with_hdr[f], d, color);
                    if (found != 1)
                        continue;
                    for (int c = 0; c < 3; c++)
                        sum[c] += color[c];
                    samples++;
                }
                if (samples < 4)
                    continue;
                float *texel = &sky.texels[(size_t(y) * kSkyMapSide + x) * 4];
                for (int c = 0; c < 3; c++)
                    texel[c] = sum[c] / samples;
                texel[3] = 1;
                luminances.push_back(static_cast<float>(luminance({ texel[0], texel[1], texel[2] })));
                sky.real++;
            }
        if (with_hdr.empty() || sky.real * 20 < kSkyMapSide * kSkyMapSide)
        {
            sky.fallback = true;
            sky.real = 0;
            for (size_t i = 0; i < sky.texels.size(); i += 4)
            {
                for (int c = 0; c < 3; c++)
                    sky.texels[i + c] = fallback[c];
                sky.texels[i + 3] = 0;
            }
        }
        else
        {
            std::nth_element(luminances.begin(), luminances.begin() + static_cast<std::ptrdiff_t>(luminances.size() / 2), luminances.end());
            const float cap = 8 * luminances[luminances.size() / 2];
            for (size_t i = 0; i < sky.texels.size(); i += 4)
            {
                const float l = static_cast<float>(luminance({ sky.texels[i], sky.texels[i + 1], sky.texels[i + 2] }));
                if (sky.texels[i + 3] > 0 && l > cap)
                    for (int c = 0; c < 3; c++)
                        sky.texels[i + c] *= cap / l;
            }
            fill_holes(sky.texels);
        }
        set_harmonics(sky);
        return sky;
    }

    AmbientFit fit_ambient(Size size, const std::vector<uint8_t> &irradiance, const std::vector<uint8_t> &normals, const std::vector<uint8_t> &stencil,
        const std::vector<float> &depth, const Vec3f &pushed)
    {
        AmbientFit fit;
        fit.rows[0] = pushed;
        const size_t pixels = size_t(size.width) * size.height;
        if (pixels == 0 || irradiance.size() != pixels * 4 || normals.size() != pixels * 4 || stencil.size() != pixels || depth.size() != pixels)
            return fit;
        // The normal equations of (1, n.x, n.y, n.z) against each channel, and the channels' sums and squares.
        double ata[4][4] = {}, atb[4][3] = {}, sum[3] = {}, squares[3] = {};
        for (size_t i = 0; i < pixels; i++)
        {
            if ((stencil[i] & 0x84) != 0x80 || !(depth[i] > 0))
                continue;
            uint32_t n_stored, i_stored;
            std::memcpy(&n_stored, &normals[i * 4], 4);
            std::memcpy(&i_stored, &irradiance[i * 4], 4);
            double n[3];
            for (int k = 0; k < 3; k++)
                n[k] = (n_stored >> (k * 10) & 1023u) / 1023.0 * 2 - 1;
            const double length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (!(length > 1e-6))
                continue;
            const double basis[4] = { 1, n[0] / length, n[1] / length, n[2] / length };
            const Vec3f value = hdr_rgb(i_stored);
            for (int r = 0; r < 4; r++)
            {
                for (int c = 0; c < 4; c++)
                    ata[r][c] += basis[r] * basis[c];
                for (int k = 0; k < 3; k++)
                    atb[r][k] += basis[r] * value[k];
            }
            for (int k = 0; k < 3; k++)
            {
                sum[k] += value[k];
                squares[k] += double(value[k]) * value[k];
            }
            fit.pixels++;
        }
        if (fit.pixels == 0)
            return fit;
        fit.kind = AmbientFit::Kind::mean;
        fit.rows[0] = { float(sum[0] / fit.pixels), float(sum[1] / fit.pixels), float(sum[2] / fit.pixels) };
        if (fit.pixels < 10000)
            return fit;
        // Gaussian elimination with the largest pivot of each column, the three channels at once.
        double m[4][7];
        for (int r = 0; r < 4; r++)
        {
            for (int c = 0; c < 4; c++)
                m[r][c] = ata[r][c];
            for (int k = 0; k < 3; k++)
                m[r][4 + k] = atb[r][k];
        }
        for (int column = 0; column < 4; column++)
        {
            int pivot = column;
            for (int r = column + 1; r < 4; r++)
                if (std::fabs(m[r][column]) > std::fabs(m[pivot][column]))
                    pivot = r;
            if (!(std::fabs(m[pivot][column]) > 1e-9 * fit.pixels))
                return fit;
            for (int c = 0; c < 7; c++)
                std::swap(m[column][c], m[pivot][c]);
            for (int r = 0; r < 4; r++)
            {
                if (r == column)
                    continue;
                const double factor = m[r][column] / m[column][column];
                for (int c = column; c < 7; c++)
                    m[r][c] -= factor * m[column][c];
            }
        }
        double rows[4][3];
        for (int r = 0; r < 4; r++)
            for (int k = 0; k < 3; k++)
                rows[r][k] = m[r][4 + k] / m[r][r];
        // The residual's squares are b.b - x.(At b) at the least-squares solution.
        bool explains = true;
        for (int k = 0; k < 3; k++)
        {
            double explained = 0;
            for (int r = 0; r < 4; r++)
                explained += rows[r][k] * atb[r][k];
            const double total = squares[k] - sum[k] * sum[k] / fit.pixels;
            fit.r2[k] = total > 0 ? static_cast<float>(1 - (squares[k] - explained) / total) : 1.0f;
            explains &= fit.r2[k] >= 0.9f;
        }
        if (!explains)
            return fit;
        fit.kind = AmbientFit::Kind::fit;
        for (int k = 0; k < 3; k++)
        {
            // Never below zero for a unit normal: the slope no longer than the constant.
            const double slope = std::sqrt(rows[1][k] * rows[1][k] + rows[2][k] * rows[2][k] + rows[3][k] * rows[3][k]);
            const double constant = std::max(rows[0][k], 0.0), scale = slope > constant ? constant / slope : 1.0;
            fit.rows[0][k] = static_cast<float>(constant);
            for (int r = 1; r < 4; r++)
                fit.rows[r][k] = static_cast<float>(rows[r][k] * scale);
        }
        return fit;
    }

    SunLight sun_light(const std::optional<CombineConstants> &combine, const std::optional<Vec3> &cascade, const Vec3 &pushed_color,
        const Vec3 &pushed_direction)
    {
        SunLight sun;
        sun.color = pushed_color;
        sun.shade = pushed_direction;
        if (!combine)
        {
            sun.why = "unread";
            return sun;
        }
        if (combine->bytes != kCombineCbufferBytes)
        {
            sun.why = "sizes";
            return sun;
        }
        const Vec3 color = { combine->b0[0], combine->b0[1], combine->b0[2] };
        if (finite(color) && color[0] >= 0 && color[1] >= 0 && color[2] >= 0 && color[0] < 64 && color[1] < 64 && color[2] < 64)
        {
            sun.color = color;
            sun.color_source = LightSource::game;
        }
        else
            sun.why = "color";
        const Vec3 d = { combine->b3[0], combine->b3[1], combine->b3[2] };
        const double length2 = double(d[0]) * d[0] + double(d[1]) * d[1] + double(d[2]) * d[2];
        if (!finite(d) || !(std::fabs(length2 - 1) <= 2e-3) || !(combine->b3[3] < 0.5f))
        {
            sun.why += sun.why.empty() ? "direction" : ", direction";
            return sun;
        }
        if (cascade)
        {
            // Within 2 degrees of cascade 0's azimuth: the cosine between the two directions' horizontal parts, with no
            // verdict where either points straight up.
            const double a = std::sqrt(double(d[0]) * d[0] + double(d[2]) * d[2]);
            const double b = std::sqrt(double((*cascade)[0]) * (*cascade)[0] + double((*cascade)[2]) * (*cascade)[2]);
            if (a > 1e-3 && b > 1e-3 && (double(d[0]) * (*cascade)[0] + double(d[2]) * (*cascade)[2]) / (a * b) < 0.99939082701909576)
            {
                sun.why += sun.why.empty() ? "azimuth" : ", azimuth";
                return sun;
            }
        }
        sun.shade = d;
        sun.shade_source = LightSource::game;
        return sun;
    }

    SceneLight build_light(std::vector<TraceTriangle> &triangles, const std::vector<uint32_t> &materials, const LightInputs &inputs, const Vec3d &reference,
        uint32_t tasks)
    {
        SceneLight light;
        light.albedo = bake_albedo(triangles, materials, inputs.faces, reference, tasks);
        light.sky = build_sky_map(inputs.faces, inputs.pushed_sky);
        light.ambient = fit_ambient(inputs.first_size, inputs.first_irradiance, inputs.first_normals, inputs.first_stencil, inputs.first_depth, inputs.pushed_sky);
        light.sun = sun_light(inputs.combine, inputs.cascade, inputs.pushed_sun_color, inputs.pushed_sun_direction);
        if (inputs.cascade)
            light.cascade = *inputs.cascade;
        return light;
    }

    uint32_t SceneLight::flags() const
    {
        return (albedo.triangles[static_cast<size_t>(AlbedoSource::direct)] + albedo.triangles[static_cast<size_t>(AlbedoSource::covered)] != 0 ? kLightAlbedo : 0)
            | (sky.fallback ? 0 : kLightSky) | (sun.color_source == LightSource::game && sun.shade_source == LightSource::game ? kLightSun : 0)
            | (ambient.kind == AmbientFit::Kind::fit ? kLightAmbient : 0);
    }

    std::string light_bytes(const std::vector<TraceTriangle> &triangles, const std::vector<uint32_t> &materials, const SceneLight &light,
        const std::vector<FaceImages> &faces)
    {
        std::string out = "PRTL";
        out.reserve(8 + triangles.size() * 8 + light.sky.texels.size() * 4 + 256);
        put(out, static_cast<uint32_t>(triangles.size()));
        for (size_t i = 0; i < triangles.size(); i++)
        {
            put(out, triangles[i].light);
            put(out, i < materials.size() ? materials[i] : 0u);
        }
        put(out, kSkyMapSide);
        out.append(reinterpret_cast<const char *>(light.sky.texels.data()), light.sky.texels.size() * sizeof(float));
        put(out, light.sky.real);
        put(out, static_cast<uint32_t>(light.sky.fallback));
        for (const Vec3f &h : light.sky.harmonics)
            put(out, h);
        for (const Vec3f &row : light.ambient.rows)
            put(out, row);
        put(out, light.ambient.r2);
        put(out, light.ambient.pixels);
        put(out, static_cast<uint32_t>(light.ambient.kind));
        put(out, light.sun.color);
        put(out, light.sun.shade);
        put(out, light.cascade);
        put(out, static_cast<uint32_t>(light.sun.color_source));
        put(out, static_cast<uint32_t>(light.sun.shade_source));
        put(out, static_cast<uint32_t>(faces.size()));
        for (const FaceImages &f : faces)
        {
            put(out, f.face);
            put(out, f.size.width);
            put(out, f.size.height);
            for (double v : f.clip_to_world)
                put(out, v);
        }
        return out;
    }

    std::optional<LightFile> parse_light(const std::string &bytes)
    {
        if (bytes.size() < 8 || bytes.compare(0, 4, "PRTL") != 0)
            return std::nullopt;
        Reader in { bytes, 4 };
        uint32_t count = 0, side = 0, fallback = 0, kind = 0, color_source = 0, shade_source = 0;
        in.get(count);
        if (bytes.size() < 8 + size_t(count) * 8)
            return std::nullopt;
        LightFile file;
        file.words.resize(count);
        file.materials.resize(count);
        for (uint32_t i = 0; i < count; i++)
        {
            in.get(file.words[i]);
            in.get(file.materials[i]);
        }
        in.get(side);
        if (!in.ok || side != kSkyMapSide || in.at + size_t(side) * side * 16 > bytes.size())
            return std::nullopt;
        SceneLight &light = file.light;
        light.sky.texels.resize(size_t(side) * side * 4);
        std::memcpy(light.sky.texels.data(), bytes.data() + in.at, light.sky.texels.size() * sizeof(float));
        in.at += light.sky.texels.size() * sizeof(float);
        in.get(light.sky.real);
        in.get(fallback);
        for (Vec3f &h : light.sky.harmonics)
            in.get(h);
        for (Vec3f &row : light.ambient.rows)
            in.get(row);
        in.get(light.ambient.r2);
        in.get(light.ambient.pixels);
        in.get(kind);
        in.get(light.sun.color);
        in.get(light.sun.shade);
        in.get(light.cascade);
        in.get(color_source);
        in.get(shade_source);
        uint32_t faces = 0;
        in.get(faces);
        for (uint32_t i = 0; i < faces && in.ok && i < kFaceCount; i++)
        {
            SceneCamera camera;
            uint32_t face = 0;
            Size size;
            in.get(face);
            in.get(size.width);
            in.get(size.height);
            for (double &v : camera.clip_to_world)
                in.get(v);
            const std::optional<FaceImages> images = face_images(face, camera, size);
            if (!images)
                return std::nullopt;
            file.faces.push_back(*images);
        }
        if (!in.ok || in.at != bytes.size() || faces > kFaceCount || kind > static_cast<uint32_t>(AmbientFit::Kind::pushed)
            || color_source >= static_cast<uint32_t>(LightSource::count) || shade_source >= static_cast<uint32_t>(LightSource::count))
            return std::nullopt;
        light.sky.fallback = fallback != 0;
        light.ambient.kind = static_cast<AmbientFit::Kind>(kind);
        light.sun.color_source = static_cast<LightSource>(color_source);
        light.sun.shade_source = static_cast<LightSource>(shade_source);
        return file;
    }
}
