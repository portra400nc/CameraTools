// The sun shadow constants: the four buffers of each census capture's shadow-mask draw, parsed, and broken one way at a
// time to check each rejection.
#include "../core/sun.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

using namespace photoreal;

namespace
{
    int failures = 0;

    void check(const char *name, bool ok)
    {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        failures += ok ? 0 : 1;
    }

    // fixtures/<capture>-sun-b0.bin to b3.bin, the census's dumps of the draw's buffers, back to back.
    SunCbuffers load(const std::string &capture)
    {
        SunCbuffers bytes {};
        size_t offset = 0;
        for (int i = 0; i < 4; i++)
        {
            const std::string path = "fixtures/" + capture + "-sun-b" + std::to_string(i) + ".bin";
            std::ifstream file(path, std::ios::binary);
            const std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (data.size() != kSunCbufferBytes[i])
                throw std::runtime_error("unexpected size of " + path);
            std::memcpy(bytes.data() + offset, data.data(), data.size());
            offset += data.size();
        }
        return bytes;
    }

    bool close(float a, float b) { return std::fabs(a - b) <= 1e-6f * std::fmax(1.0f, std::fabs(b)); }

    // Overwrites one float: row of the whole SunCbuffers, in rows of 16 bytes, and component 0 to 3.
    void poke(SunCbuffers &bytes, uint32_t row, uint32_t component, float value)
    {
        std::memcpy(bytes.data() + row * 16 + component * 4, &value, sizeof value);
    }

    float peek(const SunCbuffers &bytes, uint32_t row, uint32_t component)
    {
        float value;
        std::memcpy(&value, bytes.data() + row * 16 + component * 4, sizeof value);
        return value;
    }

    // The first row of each buffer in SunCbuffers.
    constexpr uint32_t B0 = 0, B1 = 48, B2 = 60, B3 = 82;

    SunCheck parsed(const SunCbuffers &bytes) { return parse_sun_shadows(bytes).check; }
}

int main()
{
    const SunCbuffers normal = load("capture-20261010-111824");
    const SunCbuffers pcss = load("capture-20261009-214859");
    {
        const SunParse p = parse_sun_shadows(normal);
        const SunShadowConstants &c = p.constants;
        check("111824: the constants pass every check", p.check == SunCheck::ok);
        check("111824: 8 cascades in a 4x2 grid of a 4096x2048 atlas", c.cascades == 8 && c.columns == 4 && c.rows == 2
            && c.atlas == Size { 4096, 2048 });
        check("111824: cascade radii 3.627 m and 107.9 m for cascades 0 and 5",
            close(c.cascade[0].radius, 3.62714076f) && close(c.cascade[5].radius, 107.919777f));
        check("111824: cascade 0's matrix rows (0.0323, -0.0118, 0.00283, 0) and (0, 0.0556, 0.00652, 0)",
            close(c.cascade[0].matrix[0][0], 0.032324817f) && close(c.cascade[0].matrix[0][1], -0.0118387109f)
                && close(c.cascade[0].matrix[0][2], 0.00283172959f) && c.cascade[0].matrix[0][3] == 0
                && close(c.cascade[0].matrix[1][1], 0.0555913523f) && close(c.cascade[0].matrix[1][2], 0.0065194075f));
        check("111824: cascade 0's tile center (0.125, 0.25) and cascade 7's (0.875, 0.75)", c.cascade[0].matrix[3][0] == 0.125f
            && c.cascade[0].matrix[3][1] == 0.25f && c.cascade[7].matrix[3][0] == 0.875f && c.cascade[7].matrix[3][1] == 0.75f);
        check("111824: cascade 0's origin (1.995, 225.39, 1.582)", close(c.cascade[0].origin[0], 1.99487722f)
            && close(c.cascade[0].origin[1], 225.392853f) && close(c.cascade[0].origin[2], 1.58201766f));
        check("111824: cascade 0's depth range 87.98 m", std::fabs(c.cascade[0].depth_range - 87.97984f) < 1e-3f);
        check("111824: shadow distance 900 m", c.shadow_distance == 900.0f);
        check("111824: camera at (1.0697, 223.675, 8.770)", close(c.camera[0], 1.06969583f) && close(c.camera[1], 223.675491f)
            && close(c.camera[2], 8.7703476f));
        check("111824: camera right (-0.99917, 0, 0.04086) and inverse projection x 0.6627, y 0.4142",
            std::fabs(c.basis[0][0] + 0.999165f) < 1e-5f && std::fabs(c.basis[0][2] - 0.040859f) < 1e-5f
                && std::fabs(c.inverse_projection[0][0] - 0.662742f) < 1e-5f && std::fabs(c.inverse_projection[1][1] - 0.414214f) < 1e-5f);
        check("111824: the sun lies along (0.249, 0.574, -0.780)", std::fabs(c.toward_sun[0] - 0.2491f) < 1e-3f
            && std::fabs(c.toward_sun[1] - 0.5736f) < 1e-3f && std::fabs(c.toward_sun[2] + 0.7803f) < 1e-3f);
    }
    {
        const SunParse p = parse_sun_shadows(pcss);
        const SunShadowConstants &c = p.constants;
        check("214859, PCSS High: the constants pass every check", p.check == SunCheck::ok);
        check("214859: 6 cascades in a 3x2 grid of a 6144x4096 atlas; entries 6 and 7 are stale", c.cascades == 6 && c.columns == 3
            && c.rows == 2 && c.atlas == Size { 6144, 4096 });
        check("214859: cascade radii 1.466 m and 168.4 m for cascades 0 and 5",
            close(c.cascade[0].radius, 1.46577704f) && close(c.cascade[5].radius, 168.392639f));
        check("214859: cascade 0's first matrix row (0.1093, -0.0242, 0.00245, 0)", close(c.cascade[0].matrix[0][0], 0.109276235f)
            && close(c.cascade[0].matrix[0][1], -0.0241512675f) && close(c.cascade[0].matrix[0][2], 0.00244888244f));
        check("214859: shadow distance 350 m, depth range 83.22 m", c.shadow_distance == 350.0f
            && std::fabs(c.cascade[0].depth_range - 83.22468f) < 1e-3f);
        check("214859: camera at (17.538, 254.689, -26.771)", close(c.camera[0], 17.5377598f) && close(c.camera[1], 254.689468f)
            && close(c.camera[2], -26.7708664f));
    }
    {
        SunCbuffers bytes = normal;
        poke(bytes, B3 + 9, 1, 11000);
        check("a squared radius in row 9 that does not repeat cascade 5's: radii-rows", parsed(bytes) == SunCheck::radii_rows);
        bytes = normal;
        poke(bytes, B3 + 2, 3, 40);
        poke(bytes, B3 + 8, 2, 40);
        check("cascade 2 smaller than cascade 1: radii-order", parsed(bytes) == SunCheck::radii_order);
        bytes = normal;
        poke(bytes, B3 + 18 + 4 * 3, 0, 1.3f);
        const SunParse three = parse_sun_shadows(bytes);
        check("cascade 3's tile center off the grid: only cascades 0 to 2 are used", three.check == SunCheck::ok
            && three.constants.cascades == 3);
        bytes = normal;
        poke(bytes, B3 + 18, 0, -0.125f);
        check("cascade 0's tile center outside 0..1: tiles", parsed(bytes) == SunCheck::tiles);
        bytes = normal;
        poke(bytes, B0, 2, 4000);
        check("an atlas width that is not 1 / texel: atlas", parsed(bytes) == SunCheck::atlas);
        bytes = normal;
        poke(bytes, B0, 0, 1.0f / 8192);
        poke(bytes, B0, 2, 8192);
        check("an 8192x2048 atlas with 4x2 tiles, which are not square: atlas", parsed(bytes) == SunCheck::atlas);
        bytes = normal;
        poke(bytes, B0 + 12 + 4, 0, 150);
        check("cascade 4's depth range in b0 differs from its matrix: depth-range", parsed(bytes) == SunCheck::depth_range);
        bytes = normal;
        poke(bytes, B2 + 19, 1, 1.5f);
        check("an up vector 1.5 long: basis", parsed(bytes) == SunCheck::basis);
        bytes = normal;
        poke(bytes, B1 + 5, 0, peek(bytes, B1 + 5, 0) + 10);
        check("the camera 10 m from cascade 0: camera", parsed(bytes) == SunCheck::camera);
        bytes = normal;
        poke(bytes, B3 + 15 + 4 * 6 + 2, 1, NAN);
        check("a NaN in cascade 6's matrix: not-finite", parsed(bytes) == SunCheck::not_finite);
        bytes = normal;
        poke(bytes, B0 + 10, 0, INFINITY);
        check("an infinite shadow distance: not-finite", parsed(bytes) == SunCheck::not_finite);
        bytes = pcss;
        poke(bytes, B3 + 15 + 4 * 7, 0, NAN);
        check("a NaN in a stale cascade does not matter", parsed(bytes) == SunCheck::ok);
        check("all zero bytes: atlas", parsed(SunCbuffers {}) == SunCheck::atlas);
    }
    {
        const SunParse good = parse_sun_shadows(normal);
        check("a frame whose draw matched and whose atlas is the parsed size: ok", sun_check(good, true, { 4096, 2048 }) == SunCheck::ok);
        check("a draw whose buffers did not match: signature", sun_check(good, false, { 4096, 2048 }) == SunCheck::signature);
        check("an atlas of another size this frame: atlas-changed", sun_check(good, true, { 6144, 4096 }) == SunCheck::atlas_changed);
        check("nothing read back yet: unread", sun_check(SunParse {}, true, { 4096, 2048 }) == SunCheck::unread);
        SunCbuffers bytes = normal;
        poke(bytes, B2 + 18, 0, 3);
        check("the last parse failed: its reason", sun_check(parse_sun_shadows(bytes), true, { 4096, 2048 }) == SunCheck::basis);
        check("the reasons' names", std::string(kSunCheckNames[static_cast<size_t>(SunCheck::atlas_changed)]) == "atlas-changed"
            && std::string(kSunCheckNames[static_cast<size_t>(SunCheck::radii_order)]) == "radii-order");
    }
    std::printf("%s: %d failed\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
