// The light-shaft pass's second draw, at full size: brings shafts_ps.hlsl's half-size march to each pixel through a 4x4
// window of it, one whole period of the pattern its steps are offset by, and lays the mist over the HDR scene. Writes
// every pixel, because the result is copied over the game's HDR scene whole.
#include "sun.hlsli"
#include "scatter.hlsli"

Texture2D<float3> scene : register(t0);     // mirror of the game's HDR scene
Texture2D<float4> rays : register(t5);      // the march: rgb the light scattered toward the camera, a the transmittance
Texture2D<float> ray_meters : register(t6); // the meters each half-size ray marched

struct Output
{
    float3 scene : SV_Target0;
    float4 shafts : SV_Target1;     // the light the mist scatters alone, for the light-shafts view; bound only for it
};

// A sample's weight falls to its floor at this difference between its ray's length and the pixel's, relative to the
// pixel's, as kDepthTolerance does for the AO blur.
static const float kLengthTolerance = 0.1;
// Where no sample's ray is about as long as the pixel's, such as a twig one pixel wide against the sky, every sample
// counts this little and the pixel takes their average, each first rescaled to its length.
static const float kWeightFloor = 1e-3;

Output main(float4 position : SV_Position)
{
    int2 pixel = int2(position.xy);
    float d = depth.Load(int3(pixel, 0));
    float pixel_meters = d > 0 ? min(length(game_view(pixel)), shafts_distance) : shafts_distance;
    int2 half_size = (int2(render_size.xy) + 1) / 2;
    int2 corner = int2(floor((pixel + 0.5) * 0.5 - 0.5)) - 1;
    float3 inscatter = 0;
    float transmittance = 0, total = 0;
    [unroll] for (int y = 0; y < 4; y++)
    {
        [unroll] for (int x = 0; x < 4; x++)
        {
            int2 q = clamp(corner + int2(x, y), 0, half_size - 1);
            float4 ray = rays.Load(int3(q, 0));
            float meters = max(ray_meters.Load(int3(q, 0)), 1e-3);
            Mist m;
            m.inscatter = ray.rgb;
            m.transmittance = ray.a;
            // Rescaled to the pixel's own length, so a sample along a longer or shorter ray still says how much mist the
            // pixel's ray crosses.
            m = mist_rescale(m, pixel_meters / meters);
            float weight = kWeightFloor + saturate(1 - abs(meters - pixel_meters) / (pixel_meters * kLengthTolerance));
            inscatter += m.inscatter * weight;
            transmittance += m.transmittance * weight;
            total += weight;
        }
    }
    Mist mist;
    mist.inscatter = inscatter / total;
    mist.transmittance = transmittance / total;
    Output o;
    o.scene = mist_over(scene.Load(int3(pixel, 0)), mist);
    o.shafts = float4(mist.inscatter, 1);
    return o;
}
