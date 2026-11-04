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
        float params[4];    // game light, rim, 1 for soft shading, 0
    };
    static_assert(sizeof(CharacterLightConstants) == 304);

    // The lights parse as a traced shot's (parse_trace_lights) and move into the camera's view space.
    CharacterLightConstants character_light_constants(const CharacterLightSettings &settings, const Camera &camera);
}
