// Auto exposure's 1x1 target: the stops added to the game's exposure, eased from the last frame's toward the meter's.
// tonemap_ps.hlsl and dng_ps.hlsl multiply by 2 to its power.
#include "common.hlsli"
#include "exposure.hlsli"
#include "grade.hlsli"

Texture2D<float2> meter : register(t0);     // exposure_meter_ps.hlsl's target with its whole mip chain
Texture2D<float> previous : register(t1);   // the last frame's stops

float main(float4 position : SV_Position) : SV_Target0
{
    uint width, height, levels;
    meter.GetDimensions(0, width, height, levels);
    float2 mean = meter.Load(int3(0, 0, levels - 1));
    float last = previous.Load(int3(0, 0, 0));
    // A frame with no point at all keeps the last frame's.
    float target = mean.y > 0 ? exposure_target_ev(mean.x / mean.y, game_grade().exposure, auto_exposure) : last;
    return exposure_next_ev(last, target, auto_ease);
}
