// The sun-shadow pass's first draw: raw sun visibility into our render-size scratch, drawn again from the game's own
// shadow atlas and cascades with soft edges (PCSS): a blocker search finds how far above a pixel its casters are, and
// the penumbra widens with that distance. Each pixel turns its taps by the 4x4 pattern, so the raw result is noisy;
// sun_shadows_ps.hlsl's 4x4 blur averages it away. Pixels that are not world, lie past the shadow distance or outside
// every cascade keep the game's value.
#include "sun_visibility.hlsli"

Texture2D<float2> shadow_mask : register(t0);       // mirror of the game's shadow mask: .x sun visibility

float3 game_world_at(int2 pixel) { return game_world(game_view(clamp(pixel, 0, int2(render_size.xy) - 1))); }

// The surface's normal from the depth buffer, across the neighbor nearer in depth on each axis so an edge does not bend
// it. The atlas holds geometry, which the geometric normal predicts better than the G-buffer's. 0 when degenerate.
float3 receiver_normal(int2 pixel, float3 p)
{
    float3 right = game_world_at(pixel + int2(1, 0)) - p, left = p - game_world_at(pixel - int2(1, 0));
    float3 down = game_world_at(pixel + int2(0, 1)) - p, up = p - game_world_at(pixel - int2(0, 1));
    float3 across = dot(left, left) == 0 || (dot(right, right) > 0 && dot(right, right) < dot(left, left)) ? right : left;
    float3 along = dot(up, up) == 0 || (dot(down, down) > 0 && dot(down, down) < dot(up, up)) ? down : up;
    float3 n = cross(across, along);
    return dot(n, n) > 1e-20 ? normalize(n) : 0;
}

float main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float game = shadow_mask.Load(int3(pixel, 0)).x;
    if (!is_world(stencil.Load(int3(pixel, 0)).g))
        return game;
    float3 view = game_view(pixel);
    if (-view.z >= game_b0[10].x)
        return game;                        // past the game's shadow distance
    float3 p = game_world(view);
    float edge, next_edge;
    int c = cascade_of(p, 0, edge);
    if (c < 0)
        return game;                        // past the cascades, where the game reads its far map
    // In a cascade's outer band, a growing share of pixels take the next cascade; the blur turns the share into a fade.
    if (edge > pattern(pixel.yx) && cascade_of(p, c + 1, next_edge) == c + 1)
        c++;
    float visibility = soft_visibility(project(c, p, receiver_normal(pixel, p)), pattern(pixel));
    return lerp(1, visibility, sun_strength);
}
