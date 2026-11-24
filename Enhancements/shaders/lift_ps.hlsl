// The highlight lift's pass over the HDR scene (lift.hlsli): accum_lift_mode 1 expands it before a lens sample's gap fill
// and sum, 2 compresses it after, so the game's bloom and tone map see it as it was.
#include "common.hlsli"
#include "lift.hlsli"

Texture2D<float3> scene : register(t0);     // mirror of the game's HDR scene

float3 main(float4 position : SV_Position) : SV_Target0
{
    float3 c = scene.Load(int3(position.xy, 0));
    return accum_lift_mode == 1 ? lift_expand(c, accum_lift_threshold, accum_lift) : lift_compress(c, accum_lift_threshold, accum_lift);
}
