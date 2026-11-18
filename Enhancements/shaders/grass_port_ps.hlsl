// The grass port's pixel shader: the clump's alpha tested shape and shading, in the game's colors, into the G-buffer as the game's grass pixel shaders
// (253eb0505615eed5 and its variants) write it: the normal at half scale and bias, the albedo, a specular target of the
// normal again, material id 3 (the game's tall grass) and the material's smoothness.
cbuffer GrassMaterial : register(b0) { float4 material[98]; };
cbuffer GrassCopies : register(b4) { uint4 copies; float4 look; };  // look.x how far normals tilt to the sky

Texture2D<float4> albedo : register(t0);
SamplerState linear_wrap : register(s0);

struct Blade
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float height : TEXCOORD1;       // meters over the ground
    float rise : TEXCOORD2;         // 0 at the clump's foot, 1 at its top
    float3 tip : TEXCOORD3;         // the game's colors for the blade, as its grass shaders blend them
    float3 root : TEXCOORD4;
};

struct GBuffer
{
    float4 normal : SV_Target0;
    float4 albedo : SV_Target1;
    float4 specular : SV_Target2;
    float material : SV_Target3;
    float smoothness : SV_Target4;
    float character : SV_Target5;
};

static const float kAlphaCut = 0.5;
static const float kMeanLuminance = 0.1;    // a Skoglendi blade's mean, linear

GBuffer main(Blade b, bool front : SV_IsFrontFace)
{
    float4 color = albedo.Sample(linear_wrap, b.uv);
    clip(color.a - kAlphaCut);
    // Grass cards' own normals face sideways, and their back faces' down: lit by them, a meadow goes dark. Tilted toward
    // the sky as real-time grass is, both faces light as the ground under them, a little by their own facing. The game's
    // own blades tilt less.
    float3 n = normalize(b.normal) * (front ? 1 : -1);
    n = normalize(lerp(n, float3(0, 1, 0), look.x));
    GBuffer o;
    o.normal = float4(n * 0.5 + 0.5, 0);
    // The game's own colors, root to tip by the material's ramp, with the texture's light and dark: the clump keeps the
    // meadow's art.
    float3 game = lerp(b.root, b.tip, saturate(b.rise / material[97].y + material[97].z));
    float detail = clamp(dot(color.rgb, float3(0.2126, 0.7152, 0.0722)) / kMeanLuminance, 0.5, 1.5);
    o.albedo = float4(game * detail, floor(b.rise * 127) / 255);
    // The game's grass writes a second normal here, which its vegetation lighting reads, not a specular color.
    o.specular = float4(n * 0.5 + 0.5, material[50].y);
    o.material = 3.0 / 255;
    o.smoothness = material[50].y;
    o.character = 0;
    return o;
}
