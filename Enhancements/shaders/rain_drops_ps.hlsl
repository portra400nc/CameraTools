// A drop's streak, blended premultiplied over rain_ps.hlsl's output: it takes light.a of a pixel across its drawn width,
// less at its sides and its ends, and gives that share the drop's own light and what lies behind the drop, bent through
// it and mostly let through. Nearer scenery hides it, softly just in front of the drop.
#include "common.hlsli"
#include "rain.hlsli"

Texture2D<float3> scene : register(t0);     // mirror of the HDR scene before rain_ps.hlsl
Texture2D<float> depth : register(t2);      // mirror of the main depth, reversed Z

static const float kTransmit = 0.4;         // the share of what lies behind a drop that comes through it, the rest is kSkyThrough's
static const float kBend = 1.5;             // what lies behind shows mirrored across the streak, this many times as far out
static const float kTaper = 0.15;           // the share of a streak's length over which each end fades
static const float3 kFlashTint = float3(0.85, 0.92, 1.1);   // as rain_ps.hlsl's

float4 main(RainStreak s) : SV_Target0
{
    float length_px = s.shape.x, drawn = s.shape.y;
    // A pixel's overlap with the drawn width, which sums to it across the streak.
    float across = saturate(0.5 * drawn + 0.5 - abs(s.at.y));
    float taper = max(kTaper * length_px, 1);
    float ends = saturate((s.at.x + 0.5) / taper) * saturate((length_px - s.at.x + 0.5) / taper);
    float meters = 1 / s.inverse_meters;
    float d = depth.Load(int3(s.position.xy, 0));
    float scene_meters = d > 0 ? -view_position(s.position.xy * render_size.zw, d).z : 1e9;
    float shown = saturate((scene_meters - meters) / (0.05 + 0.03 * meters));
    float cover = s.light.a * across * ends * shown;
    float2 behind_at = s.position.xy - s.shape.zw * s.at.y * kBend;
    // Lit by rain_ps.hlsl's flash, as it lights what is not sky.
    float3 behind = scene.SampleLevel(linear_clamp, behind_at * render_size.zw, 0) * (1 + rain_flash * 1.2 * kFlashTint);
    return float4((s.light.rgb + kTransmit * behind) * cover, cover);
}
