// The highlight lift's key: the HDR scene's mean luminance over a 16 x 9 grid of its pixels, as the depth of field's
// autofocus measures it (dof.hlsli, dof_frame_luminance), drawn once when a sum's samples start, before the first
// expands, so the samples and their average share one threshold (lift.hlsli).
#include "common.hlsli"
#include "lift.hlsli"

Texture2D<float3> scene : register(t0);     // mirror of the game's HDR scene

float main(float4 position : SV_Position) : SV_Target0
{
    float sum = 0;
    [loop] for (int y = 0; y < 9; y++)
        [unroll] for (int x = 0; x < 16; x++)
            sum += dot(scene.Load(int3(int((x + 0.5) / 16 * render_size.x), int((y + 0.5) / 9 * render_size.y), 0)), kLiftLuma);
    return sum / 144;
}
