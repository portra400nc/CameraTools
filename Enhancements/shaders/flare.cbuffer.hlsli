// The lens flare's constants at b1, core/flare.h's FlareConstants, shared by flare_ps.hlsl and flare_visibility_ps.hlsl.
// Registers stay inside gpu.h's ranges: t0-t7, s0-s3, b0-b4, u0-u3.

cbuffer Flare : register(b1)
{
    float4 flare_sun;       // the source in picture units (height 1, center 0, x right, y down), its visibility times the strength
                            // when it does not come from the texture, the picture's width over its height
    float4 flare_amounts;   // arcs, ghosts, streaks, glare
    float4 flare_tint;      // rgb the flare's color, linear; w its size
    float4 flare_frame;     // the back buffer's width and height, 1 when it holds sRGB-encoded values (0: linear), chromatic spread
    float4 flare_more;      // filled arcs, 1 when the target's rows run up the screen, rainbow; one unused
};

#define LOOP [loop]
#define UNROLL [unroll]
