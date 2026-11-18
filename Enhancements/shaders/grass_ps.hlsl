// The grass pass's first draw at the bloom step: root shade for grass_apply_ps.hlsl to smooth and apply to the HDR scene.
// Each blade darkens toward its foot by its height over the lowest grass or ground within kAround meters of it across,
// as blades in dense grass shade each other's lower parts. Grass hides the ground it stands on, so the lowest blade or
// ground nearby stands for it; a blade measured only against visible ground kept its light where none showed, and
// flickered in blocks as the grass swayed. The neighbors are the pixels within kReach taps, a tap every kStep pixels at
// 720 lines (wider in proportion above), that grass_capture_ps.hlsl marked at the combine step.
#include "common.hlsli"

Texture2D<float> depth : register(t2);      // mirror of the main depth, reversed Z
Texture2D<float> grass : register(t4);      // grass_capture_ps.hlsl's: 1 a blade, 0.5 ground

static const int kReach = 6;
static const float kStep = 3;
static const float kAround = 0.3;           // meters across: a blade's lowest neighbors this far off say where its ground is
static const float kBladeHeight = 0.35;     // meters from the ground where a blade stops darkening
static const float kRootLight = 0.3;        // a blade's light at its foot against its tip's

float4 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float kind = grass.Load(int3(pixel, 0));
    float d = depth.Load(int3(pixel, 0));
    if (kind <= 0 || d <= 0)
        return float4(1, 1, 1, 0);
    if (kind < 0.75)
        return float4(1, 1, 1, 1);
    float3 p = view_position((pixel + 0.5) * render_size.zw, d);
    float3 up = normalize(mul(world_to_view, float4(0, 1, 0, 0)).xyz);
    int stride = max(1, int(kStep * render_size.y / 720 + 0.5));
    float height = 0;
    [loop] for (int y = -kReach; y <= kReach; y++)
    {
        [loop] for (int x = -kReach; x <= kReach; x++)
        {
            int2 q = pixel + int2(x, y) * stride;
            if (any(q < 0) || any(q >= int2(render_size.xy)) || grass.Load(int3(q, 0)) <= 0)
                continue;
            float dq = depth.Load(int3(q, 0));
            if (dq <= 0)
                continue;
            float3 offset = view_position((q + 0.5) * render_size.zw, dq) - p;
            float rise = dot(offset, up);
            if (length(offset - rise * up) <= kAround)
                height = max(height, -rise);
        }
    }
    float shade = lerp(kRootLight, 1, smoothstep(0, kBladeHeight, height));
    return float4(lerp(1, shade, grass_roots).xxx, 1);
}
