// Studio lighting for characters: the constants of shaders/character_light_ps.hlsl, from the settings and the camera.
#pragma once

#include "plan.h"
#include "trace.h"

namespace enhancements
{
    // shaders/character_light_ps.hlsl's cbuffer CharacterLight at b1: the lights as core/trace.h's GatherLights rows, but in
    // view space, then the head and the parameters.
    struct CharacterLightConstants
    {
        GatherLights lights;
        float head[4];      // view space; w 1 when known
        float params[4];    // game light, rim, 1 for soft shading, 1 with the world's light
        float world_sun[4];
        float world_sky[4];
        float world_game[4];
        float traced[4];    // set by Gpu::run_character_light: 1 with the trace's sums bound, 1 with the gather's, the balance's
                            // aim and its ease a frame
    };
    static_assert(sizeof(CharacterLightConstants) == 368);

    // The lights parse as a traced shot's (parse_trace_lights) and move into the camera's view space.
    CharacterLightConstants character_light_constants(const CharacterLightSettings &settings, const Camera &camera);
}
