// The traced foliage pass at the bloom step: grass and leaves in the HDR scene times the shade the trace's apply draw
// wrote for them at the combine step, where the stencil still marked them. Grass's shade is blurred across nearby
// foliage at about the same depth, a few pixels wide at 720 lines and wider in proportion above, so the thin shadows
// blades cast on each other merge into a clump's soft shade instead of drawing dark lines. Leaves keep their own: a
// canopy's leaves sit at about one depth, so the blur smeared the shade of inner leaves over the edges of the ones in
// front. Writes the whole target, because the result is copied over the game's HDR scene whole.
#include "common.hlsli"

Texture2D<float3> scene : register(t0);     // mirror of the HDR scene
Texture2D<float> depth : register(t2);      // mirror of the main depth, reversed Z
Texture2D<float2> shade : register(t4);     // the apply draw's foliage shade: below 1 on foliage, 1 elsewhere; y is 1 on grass

static const int kReach = 3;                // taps each way, kReach * stride pixels
static const float kDepthTolerance = 0.05;  // a tap counts while its depth is within this share of the pixel's

float view_z(int2 q) { return view_position((q + 0.5) * render_size.zw, depth.Load(int3(q, 0))).z; }

float4 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float3 color = scene.Load(int3(pixel, 0));
    float2 own = shade.Load(int3(pixel, 0));
    if (own.x >= 1 || depth.Load(int3(pixel, 0)) <= 0)
        return float4(color, 1);
    if (own.y < 0.5)
        return float4(color * own.x, 1);
    int stride = max(1, int(render_size.y / 720 + 0.5));
    float z = view_z(pixel), sum = 0, total = 0;
    [loop] for (int y = -kReach; y <= kReach; y++)
    {
        [loop] for (int x = -kReach; x <= kReach; x++)
        {
            int2 q = pixel + int2(x, y) * stride;
            if (any(q < 0) || any(q >= int2(render_size.xy)))
                continue;
            float s = shade.Load(int3(q, 0)).x;
            if (s >= 1 || depth.Load(int3(q, 0)) <= 0)
                continue;       // not foliage
            float weight = saturate(1 - abs(view_z(q) - z) / (max(-z, 0.05) * kDepthTolerance)) * exp(-0.25 * (x * x + y * y));
            sum += s * weight;
            total += weight;
        }
    }
    return float4(color * (total > 0 ? sum / total : own.x), 1);
}
