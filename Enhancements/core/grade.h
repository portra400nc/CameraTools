// Where the game's tone map shader keeps the values the tonemap pass takes over. They are found in the shader's own code,
// never at fixed rows, because the rows of its b0 differ between the shader's variants. Pure C++17 like recipe.h.
#pragma once

#include "shex.h"

#include <cstdint>
#include <optional>

namespace enhancements
{
    // One float of the shader's b0.
    struct GradeValue
    {
        uint32_t row = 0;
        uint8_t component = 0;  // 0 to 3

        uint32_t offset() const { return row * 16 + component * 4; }
    };

    struct GradeLayout
    {
        uint32_t color_row = 0;             // the first of the color matrix's three rows: what red, green and blue become
        GradeValue exposure, gamma;
        std::optional<GradeValue> bloom;    // none in a variant that adds no bloom

        // The bytes of b0 the layout reaches into.
        uint32_t bytes() const;
    };

    // The layout a tone map pixel shader's code shows, or none when the code is not the game's tone map as the census
    // found it. The code it looks for, with any registers and rows:
    //
    //   mad r0.xyz, r1.xyzx, cb0[22].y, r0.xyzx        the bloom, before the matrix; a variant may have none
    //   mul r1.yzw, r0.y, cb0[100].xxyz                the color matrix: one row of b0 for each of the scene's channels,
    //   mad r1.yzw, cb0[99].xxyz, r0.x, r1.yyzw        three rows in a run
    //   mad r1.yzw, cb0[101].xxyz, r0.z, r1.yyzw
    //   mul r3.xyz, r1.yzwy, cb0[22].z                 the exposure: the matrix's result times one float
    //   log r1.yzw, ...                                the display gamma: a power by one float
    //   mul r1.yzw, r1.yyzw, cb0[20].y
    //   exp r1.yzw, ...
    std::optional<GradeLayout> find_grade(const Shex &shader);
}
