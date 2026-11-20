// Shared by the rain shaders, after common.hlsli: the hashes, the noise and the gusts that thicken and thin the splashes
// and the drops alike, and the drops' streak from their vertex shader to their pixel shader.

static const float kShutter = 1.0 / 80;     // the seconds a streak shows of a drop's fall: 9 to 11 cm, as a 1/80 s photo shows rain

float2 hash2(float2 p)
{
    p = frac(p * float2(0.1031, 0.1030));
    p += dot(p, p.yx + 33.33);
    return frac((p.xx + p.yx) * p.yx);
}

float noise1(float x)
{
    float i = floor(x), f = frac(x);
    return lerp(hash2(float2(i, 3.7)).x, hash2(float2(i + 1, 3.7)).x, f * f * (3 - 2 * f));
}

float noise2(float2 p)
{
    float2 i = floor(p), f = frac(p);
    f = f * f * (3 - 2 * f);
    return lerp(lerp(hash2(i).x, hash2(i + float2(1, 0)).x, f.x), lerp(hash2(i + float2(0, 1)).x, hash2(i + 1).x, f.x), f.y);
}

// The shelter map, shelter_move's in core/sky.h, as rain_shelter_cs.hlsl writes it: in each cell the highest surface seen,
// in steps of 1/64 meter from 16384 meters down, so a uint orders the heights and 0 marks a cell where nothing was seen.
Texture2D<uint> shelter : register(t4);
static const float kShelterNone = -1e9;     // the height of a cell that holds nothing, or off the map

int2 shelter_cell_of(float2 world_xz) { return int2(floor((world_xz - shelter_origin) / shelter_cell)); }

uint shelter_encode(float height) { return uint(clamp((height + 16384) * 64, 1, 2097151)); }

// The highest surface seen in the column through a world point. A frame without the map, unbound with no cell size, has
// none anywhere.
float shelter_height(float2 world_xz)
{
    uint w, h;
    shelter.GetDimensions(w, h);
    int2 cell = shelter_cell_of(world_xz);
    if (shelter_cell <= 0 || any(cell < 0) || any(cell >= int2(w, h)))
        return kShelterNone;
    uint v = shelter.Load(int3(cell, 0));
    return v == 0 ? kShelterNone : v / 64.0 - 16384;
}

// How far a surface the map saw covers a world point, 0 open to 1 under it: covered from band.x meters above the point,
// fading in over band.y more. The depth buffer shows only what the camera looked at, so right over the camera, or over a
// floor it looked down on, the map may hold nothing above the point. There the columns around stand in: the point is
// under cover when most of the directions in which the map holds anything within kShelterReach hold a surface over it,
// as a ceiling, a canopy or walls around do, and a lone wall or roof beside open ground does not.
static const float kShelterReach[4] = { 1, 2.5, 4, 6 };    // meters

float shelter_cover(float3 world, float2 band)
{
    float cover = smoothstep(band.x, band.x + band.y, shelter_height(world.xz) - world.y);
    float seen = 0, over = 0;
    [unroll] for (int k = 0; k < 8; k++)
    {
        float2 direction;
        sincos(k * 0.7853982, direction.y, direction.x);
        float highest = kShelterNone;
        [unroll] for (int r = 0; r < 4; r++)
            highest = max(highest, shelter_height(world.xz + direction * kShelterReach[r]));
        seen += highest > kShelterNone ? 1 : 0;
        over += highest - world.y > band.x + band.y ? 1 : 0;
    }
    return seen >= 3 && over >= 0.75 * seen ? 1 : cover;
}

// Gusts: the rain thickens and thins in sheets drifting around the camera, azimuth radians from world +x toward +z.
float rain_gust(float azimuth)
{
    return lerp(0.45, 1.35, noise2(float2(azimuth * 1.6 - rain_time * 0.12, rain_time * 0.2)));
}

// A drop's streak, from rain_drops_vs.hlsl to rain_drops_ps.hlsl. Its quad's pixels are those of the stored target.
struct RainStreak
{
    float4 position : SV_Position;
    noperspective float2 at : TEXCOORD0;        // pixels along the streak from its head, and across it from its middle line
    noperspective float inverse_meters : TEXCOORD1; // 1 over the view depth, which interpolates linearly on the screen
    nointerpolation float4 shape : TEXCOORD2;   // its length and drawn width in pixels, and the unit direction across it
    nointerpolation float4 light : TEXCOORD3;   // rgb its own light; a the share of a pixel it takes across its drawn width
};
