// The traced scene's light, which a PHOTOREAL_SCENE_LIGHT capture brings and the gather kernel (shaders/gather.hlsli)
// reads: an albedo for every triangle, baked from the faces' G-buffers into the triangle's light word; the sky as an
// octahedral map of the faces' HDR sky pixels; the game's ambient light as a first-order fit of its irradiance to the
// normal; and the sun's color and shading direction from the combine draw's constants. Pure C++17 like trace_scene.h,
// and without fused multiply-adds or library functions that round differently, so the Mac bakes a saved capture to
// the Deck's bits (tests/bake_light.cpp). The add-on's writer thread runs it once a scene, before the alpha bake.
#pragma once

#include "trace_scene.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace photoreal
{
    // One saved face as the bake and the sky map read it: its camera and its images at size, top row first, as the game
    // stored them. A copy that was not made is empty, and what needs it does without that face.
    struct FaceImages
    {
        uint32_t face = kFaceView;
        Size size;
        Mat4d world_to_clip {};         // the world to the face's D3D clip space, whose w is the distance along its forward axis
        Mat4d clip_to_world {};
        std::vector<float> depth;       // the main depth, reversed, 0 for the sky
        std::vector<uint8_t> albedo;    // G-buffer rt1 at the combine step, sRGB in its first three bytes, 4 bytes a pixel
        std::vector<uint8_t> stencil;   // the main depth's stencil at the combine step
        std::vector<uint32_t> hdr;      // the HDR scene at the bloom step before any pass of ours, R11G11B10_FLOAT
    };

    // A face's images with its camera's matrices; empty when the camera's transform does not invert.
    std::optional<FaceImages> face_images(uint32_t face, const SceneCamera &camera, Size size);

    // Faces of more pixels than this are kept at half size, so seven faces of a 3840x2160 capture hold what seven of
    // 1920x1200 do: 209 MB.
    constexpr uint64_t kFullFacePixels = 2400000;

    // Half the size, rounding odd sizes down: each pixel takes the depth and stencil of the nearest of its four, and the
    // mean of their albedo bytes and of their HDR colors.
    void halve_face(FaceImages &face);

    // R11G11B10_FLOAT to linear RGB, and back to the nearest stored value not above it.
    Vec3f hdr_rgb(uint32_t stored);
    uint32_t hdr_stored(const Vec3f &rgb);
    // An sRGB byte to its linear value.
    float srgb_linear(uint8_t stored);

    // Where a triangle's albedo came from, bits 30 and 31 of its light word.
    enum class AlbedoSource : uint8_t
    {
        direct,         // a face's G-buffer shows the triangle
        covered,        // a face shows grass, vegetation or foliage at most kCoverMeters in front of it
        material,       // the mean of its material's triangles that have either
        class_mean,     // the mean of its class's, else of the scene's, else kNoAlbedo
        count,
    };
    extern const std::array<const char *, static_cast<size_t>(AlbedoSource::count)> kAlbedoSourceNames;

    // The light word: the albedo as 10 bits a channel of the square root of its linear value, red lowest, and the source.
    uint32_t pack_light(const Vec3f &albedo, AlbedoSource source);
    Vec3f light_albedo(uint32_t light);
    constexpr AlbedoSource light_source(uint32_t light) { return static_cast<AlbedoSource>(light >> 30); }

    // How far behind grass, vegetation or foliage a face shows a triangle may lie to take that pixel's albedo: grass is
    // not in the traced scene, so a ray meets the ground under it, and what bounces light there is the grass.
    constexpr double kCoverMeters = 1.5;
    // The albedo of a scene no face gave any: the census's mean, a middle gray.
    constexpr float kNoAlbedo = 0.18f;

    struct AlbedoCounts
    {
        std::array<uint64_t, static_cast<size_t>(AlbedoSource::count)> triangles {};
        std::array<double, static_cast<size_t>(AlbedoSource::count)> area {};   // square meters
    };

    // Sets each triangle's light word. A triangle has 4 sample points, its centroid and the points at barycentric
    // (2/3, 1/6, 1/6) and its two rotations; each is projected into each face with an albedo, and counts where the
    // face's depth buffer holds it within max(0.05 m, 1%) (direct), or holds grass, vegetation or foliage (stencil 129,
    // 136, 137) nearer by at most kCoverMeters (covered). Its albedo is the mean of its direct samples, else of its
    // covered ones, else its material's area-weighted mean, else its class's, else the scene's. materials holds each
    // triangle's material, or nothing, and every triangle is then of one. The triangles are relative to reference. The
    // result does not depend on tasks, the threads it runs on.
    AlbedoCounts bake_albedo(std::vector<TraceTriangle> &triangles, const std::vector<uint32_t> &materials, const std::vector<FaceImages> &faces,
        const Vec3d &reference, uint32_t tasks);

    // The sky map's side, shaders/gather.hlsli's kSkySide.
    constexpr uint32_t kSkyMapSide = 128;

    struct SkyMap
    {
        // kSkyMapSide x kSkyMapSide texels of rgb and 1 for a texel the faces' sky filled, 0 for one filled from its
        // neighbors, row by row: the buffer the gather kernel loads.
        std::vector<float> texels;
        uint32_t real = 0;                  // texels the faces' sky filled
        // The map's light on an unoccluded surface of normal n over pi, as a polynomial in n:
        // [0] + [1] n.y + [2] n.z + [3] n.x + [4] n.x n.y + [5] n.y n.z + [6] (3 n.z^2 - 1) + [7] n.x n.z + [8] (n.x^2 - n.y^2).
        std::array<Vec3f, 9> harmonics {};
        bool fallback = false;              // every texel is the color given: the faces showed too little sky, or had no HDR copy
    };

    // The direction of the sky map's point (u, v), each from 0 to 1 across the map, unit length: the inverse of
    // shaders/gather.hlsli's sky_texel_of.
    Vec3f sky_direction(float u, float v);

    // Each texel averages 16 directions, 4x4 inside it. A direction looks in the cube face of its largest axis, then in
    // the other faces in the order given, in the first that holds it; there it is sky where the depth is 0, and takes
    // the HDR pixel. A texel with at least 4 sky samples is real. A real texel's luminance is capped at 8 times the real
    // texels' median, which bounds the sun's disc. The others are filled from the real ones by a pull-push over the
    // map's mip pyramid. With fewer than 5% real texels, or no face with an HDR copy, every texel is fallback.
    SkyMap build_sky_map(const std::vector<FaceImages> &faces, const Vec3f &fallback);

    // harmonics' polynomial at a unit normal.
    Vec3f sky_irradiance(const SkyMap &sky, const Vec3f &normal);

    // Where a light input came from.
    enum class LightSource : uint8_t { game, pushed, count };
    extern const std::array<const char *, static_cast<size_t>(LightSource::count)> kLightSourceNames;

    // The game's ambient light on a surface of normal n: rows[0] + rows[1] n.x + rows[2] n.y + rows[3] n.z, in the
    // irradiance buffer's unit, what a white surface shows.
    struct AmbientFit
    {
        std::array<Vec3f, 4> rows {};
        Vec3f r2 {};                // each channel's share of the variance the fit explains
        uint32_t pixels = 0;        // world pixels it was fitted to
        enum class Kind : uint8_t { fit, mean, pushed } kind = Kind::pushed;
    };

    // The least-squares fit over a face's world pixels with depth (stencil & 0x84 == 0x80) of its diffuse irradiance
    // (R11G11B10_FLOAT) to its G-buffer normal (R10G10B10A2_UNORM, normalized), both 4 bytes a pixel as stored. With
    // fewer than 10,000 such pixels, or a channel's r2 under 0.9, the irradiance's mean alone; with an image missing or
    // of another size, pushed, the pushed camera's sky color. The rows never go below zero for a unit normal: where the
    // fit would, its slope is scaled down.
    AmbientFit fit_ambient(Size size, const std::vector<uint8_t> &irradiance, const std::vector<uint8_t> &normals, const std::vector<uint8_t> &stencil,
        const std::vector<float> &depth, const Vec3f &pushed);

    // The pixel shader constant buffers of the combine step's first draw, the game's lighting of the world: their sizes,
    // b0 to b3, and row 0 of b0 and of b3.
    constexpr std::array<uint32_t, 4> kCombineCbufferBytes = { 3184, 192, 352, 768 };
    struct CombineConstants
    {
        std::array<uint32_t, 4> bytes {};
        Row b0 {}, b3 {};
    };

    // The sun as the game shades with it: its color, what a white surface facing it shows (b0 row 0 .xyz), and the
    // direction toward it (b3 row 0 .xyz, while its .w is under 0.5). The game casts its shadows from another direction,
    // which it holds at 35 degrees of elevation or more: cascade 0's.
    struct SunLight
    {
        Vec3 color {};
        Vec3 shade {};              // unit length, or zero for no sun
        LightSource color_source = LightSource::pushed, shade_source = LightSource::pushed;
        std::string why;            // why a pushed value stands in: "unread", "sizes", "color", "direction" or "azimuth"; else empty
    };

    // The game's when the constants are there with kCombineCbufferBytes' sizes, the color finite, not negative and under
    // 64, and the direction of unit length and, with cascade, within 2 degrees of its azimuth; else the pushed camera's
    // for what failed.
    SunLight sun_light(const std::optional<CombineConstants> &combine, const std::optional<Vec3> &cascade, const Vec3 &pushed_color,
        const Vec3 &pushed_direction);

    // What of a scene's light came from the game and not from a fallback: PHOTOREAL_TRACE_LIGHT_*.
    constexpr uint32_t kLightAlbedo = 1u << 0, kLightSky = 1u << 1, kLightSun = 1u << 2, kLightAmbient = 1u << 3;

    // A scene's light besides its triangles' words.
    struct SceneLight
    {
        SunLight sun;
        Vec3 cascade {};            // toward the sun as the game's shadows had it at the capture, zero when unread
        AmbientFit ambient;
        SkyMap sky;
        AlbedoCounts albedo;

        // kLightAlbedo when a face's G-buffer gave an albedo, kLightSky unless the sky map fell back, kLightSun when the
        // color and the direction are both the game's, kLightAmbient when the ambient light is the fit.
        uint32_t flags() const;
    };

    // What a PHOTOREAL_SCENE_LIGHT capture brings for its scene's light: every face's images, the first face's G-buffer
    // normals, diffuse irradiance, stencil and depth for the ambient fit, the newest combine constants and cascade 0's
    // direction, and the pushed camera's sun and sky for what the game's cannot give.
    struct LightInputs
    {
        std::vector<FaceImages> faces;
        Size first_size;
        std::vector<uint8_t> first_normals, first_irradiance, first_stencil;
        std::vector<float> first_depth;
        std::optional<CombineConstants> combine;
        std::optional<Vec3> cascade;
        Vec3 pushed_sun_color {}, pushed_sun_direction {}, pushed_sky {};
    };

    // The scene's light from its inputs, as the add-on's writer thread builds it before the alpha bake: each triangle's
    // light word (bake_albedo), the sky map (build_sky_map, the pushed sky where the faces show too little), the ambient
    // fit of the first face (fit_ambient) and the sun (sun_light).
    SceneLight build_light(std::vector<TraceTriangle> &triangles, const std::vector<uint32_t> &materials, const LightInputs &inputs, const Vec3d &reference,
        uint32_t tasks);

    // trace-light.bin, beside trace-scene.bin: b"PRTL", a uint32 count, each triangle's light word and material in
    // trace-scene.bin's order, the sky map's side and its float4 texels, its count of real texels and 1 when it fell
    // back, its 9 harmonics, the ambient fit's 4 rows, r2, pixels and kind, the sun's color, its shading direction,
    // cascade 0's direction and the color's and the direction's sources, and the faces the bake read: their count, and
    // each one's number, its size as baked and its clip_to_world as 16 float64, so the Mac bakes the raw copies to the
    // Deck's bits; little-endian.
    std::string light_bytes(const std::vector<TraceTriangle> &triangles, const std::vector<uint32_t> &materials, const SceneLight &light,
        const std::vector<FaceImages> &faces);
    struct LightFile
    {
        std::vector<uint32_t> words, materials;     // a triangle each
        SceneLight light;                           // without the albedo's counts, which the words and the triangles give
        std::vector<FaceImages> faces;              // the faces' numbers, sizes and cameras, without images
    };
    // Empty when the bytes are not such a file.
    std::optional<LightFile> parse_light(const std::string &bytes);

    // The counts of triangles' light words by source, their areas from the triangles.
    AlbedoCounts count_albedo(const std::vector<TraceTriangle> &triangles);
}
