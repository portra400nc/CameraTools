// shaders/exposure.hlsli compiled as C++ through hlsl.h.
#include "exposure.h"
#include "hlsl.h"

namespace enhancements::shader
{
#include "../shaders/exposure.hlsli"
}

namespace enhancements
{
    float exposure_log_luminance(float r, float g, float b) { return shader::exposure_log_luminance(shader::float3(r, g, b)); }

    float exposure_target_ev(float mean_log2, float game_exposure, float strength, float night_ev)
    {
        return shader::exposure_target_ev(mean_log2, game_exposure, strength, night_ev);
    }

    float exposure_next_ev(float previous, float target, float ease) { return shader::exposure_next_ev(previous, target, ease); }
}
