// The game's sun shadow constants: the pixel shader constant buffers b0 to b3 its shadow-mask draw binds, parsed and
// checked. Pure C++17 like recipe.h. shaders/sun_ps.hlsl reads the same rows from its own copies of the four buffers.
#pragma once

#include "recipe.h"

#include <array>
#include <cstdint>

namespace photoreal
{
    // The four buffers' sizes in bytes, the draw's sharpest signature: b0 768, b1 192, b2 352, b3 912.
    constexpr std::array<uint32_t, 4> kSunCbufferBytes = { 768, 192, 352, 912 };
    constexpr uint32_t kSunCbufferTotal = 768 + 192 + 352 + 912;

    // b0 to b3 back to back, as the shell reads them back.
    using SunCbuffers = std::array<uint8_t, kSunCbufferTotal>;

    // b3 holds eight cascades. At the user's normal settings all eight are used; at low settings six are, and the last
    // two entries hold stale values.
    constexpr uint32_t kMaxSunCascades = 8;

    using Vec3 = std::array<float, 3>;
    using Row = std::array<float, 4>;

    struct SunCascade
    {
        Vec3 center;                // b3 row i .xyz, world space
        float radius;               // the square root of b3 row i .w; a point is in the first cascade whose sphere holds it
        // b3 rows 15 + 4i to 18 + 4i. With q = p - origin, (u, v, depth, w) = q.x * matrix[0] + q.y * matrix[1] +
        // q.z * matrix[2] + matrix[3]: u and v in the whole atlas, depth larger toward the sun, w 1.
        std::array<Row, 4> matrix;
        Vec3 origin;                // b3 row 49 + i .xyz
        float depth_range;          // meters toward the sun per unit of depth: 1 / |(matrix[0].z, matrix[1].z, matrix[2].z)|
    };

    struct SunShadowConstants
    {
        uint32_t cascades = 0;      // 1 to kMaxSunCascades; cascade[cascades] onward are unused
        std::array<SunCascade, kMaxSunCascades> cascade {};
        Size atlas;                 // b0 row 0 .zw
        uint32_t columns = 0, rows = 0;     // the atlas's grid of square tiles, cascade i at column i % columns, row i / columns
        float shadow_distance = 0;  // b0 row 10 .x: meters of view distance beyond which the game draws no sun shadow
        Vec3 camera {};             // b1 row 5 .xyz, world space
        std::array<Vec3, 3> basis {};       // b2 rows 18 to 20: the camera's right, up and forward in world space
        // b2 rows 10 to 13: view space (x, y, z, w) = ndc.x * [0] + ndc.y * [1] + ndc.z * [2] + [3], with GL depth
        // ndc.z = 1 - 2 * device depth and ndc.y counting texture rows from the top.
        std::array<Row, 4> inverse_projection {};
        Vec3 toward_sun {};         // unit length, from cascade 0's depth gradient
    };

    // Why a frame's sun-shadow pass cannot use the constants. The names follow in kSunCheckNames.
    enum class SunCheck : uint8_t
    {
        ok,
        unread,         // none read back yet: the pass was just switched on, or the GPU has not finished a copy
        signature,      // this frame's shadow-mask draw bound constant buffers of other sizes
        atlas_changed,  // this frame's atlas is not the size of the atlas the constants describe
        not_finite,     // a value the pass reads is NaN or infinite
        atlas,          // b0 row 0 is not (1 / w, 1 / h, w, h), or the tiles it implies are not square
        tiles,          // a used cascade's tile center is not on the grid cascade 0's center implies
        radii_rows,     // b3 rows 8 and 9 do not repeat the spheres' squared radii
        radii_order,    // the used cascades' radii do not grow
        depth_range,    // b0 rows 12 to 19 .x do not match the matrices' depth ranges
        basis,          // the camera's right, up and forward are not orthonormal
        camera,         // the camera is not at cascade 0's sphere
        count,
    };

    extern const std::array<const char *, static_cast<size_t>(SunCheck::count)> kSunCheckNames;

    // constants holds values only when check is ok.
    struct SunParse
    {
        SunCheck check = SunCheck::unread;
        SunShadowConstants constants;
    };

    // Reads b0 to b3 and checks them as the census analysis found them. Never reports unread, signature or
    // atlas_changed: those are facts of a frame, which sun_check adds.
    SunParse parse_sun_shadows(const SunCbuffers &bytes);

    // This frame's verdict: signature when its draw's buffers did not match, the last parse's check when that is not
    // ok, atlas_changed when this frame's atlas differs from the parsed one, and ok otherwise.
    SunCheck sun_check(const SunParse &last, bool signature_matched, Size atlas);
}
