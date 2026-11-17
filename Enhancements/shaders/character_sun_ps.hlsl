// The character light pass's sun draw: the sun's raw visibility at each character pixel from the game's sun atlas, 1
// elsewhere. The game's mask holds 1 on characters, as the game shades them itself, so a character in a cliff's shade
// stays lit as in the sun. The lookup starts kLift toward the sun from the pixel, past the character's own body, which the
// atlas holds too: the game's toon shading already darkens the side away from the sun, and the casters that matter, trees,
// walls and other characters, stand farther off. The taps turn by the 4x4 pattern; character_light_ps.hlsl averages them.
#include "sun_visibility.hlsli"

static const float kLift = 0.6;     // meters

float main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    if ((stencil.Load(int3(pixel, 0)).g & 0x04u) == 0)
        return 1;
    float3 view = game_view(pixel);
    if (-view.z >= game_b0[10].x)
        return 1;                           // past the game's shadow distance
    float3 p = game_world(view);
    float edge;
    int c = cascade_of(p, 0, edge);
    if (c < 0)
        return 1;
    CascadeTransform t = cascade_transform(c);
    p += normalize(float3(t.m0.z, t.m1.z, t.m2.z)) * kLift;
    c = cascade_of(p, 0, edge);
    return c < 0 ? 1 : soft_visibility(project(c, p, 0), pattern(pixel));
}
