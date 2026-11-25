// The highlight lift's pass over the HDR scene (lift.hlsli): accum_lift_mode 1 expands it before a lens sample's gap fill
// and sum, 2 compresses it after, so the game's bloom and tone map see it as it was.
#include "common.hlsli"
#include "lift.hlsli"

Texture2D<float3> scene : register(t0);     // mirror of the game's HDR scene
Texture2D<float> key : register(t1);        // lift_key_ps.hlsl's

float3 main(float4 position : SV_Position) : SV_Target0
{
    float3 c = scene.Load(int3(position.xy, 0));
    float t = lift_threshold(key.Load(int3(0, 0, 0)));
    return accum_lift_mode == 1 ? lift_expand(c, t, accum_lift) : lift_compress(c, t, accum_lift);
}
