// The game's lamps for the physical sky's night on vegetation (shaders/vegetation.hlsli): the game lights vegetation with a
// light of its own that the night does not darken, so the night takes it away and gives back what its lamps shed.
#pragma once

#include "plan.h"
#include "trace.h"

struct EnhancementsShotLight;

namespace enhancements
{
    constexpr uint32_t kNightLamps = 32;

    // The cbuffer NightLamps at b4: per lamp core/trace.h's GatherLights rows, in view space; then the count.
    struct NightLamps
    {
        float rows[kNightLamps][4][4];
        uint32_t info[4];           // the count, 0, 0, 0
    };
    static_assert(sizeof(NightLamps) == kNightLamps * 64 + 16);

    // The lamps parse as a traced shot's (parse_trace_lights). The kNightLamps whose reach comes nearest the camera move into
    // its view space, nearest first.
    NightLamps night_lamps(const std::vector<EnhancementsShotLight> &lights, const Camera &camera);
}
