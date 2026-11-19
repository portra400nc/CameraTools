// This frame's camera turn, from the directions the game's sky dome gave (sky_direction_ps.hlsl): two pixels the same
// distance left and right of the middle of the picture's middle row see directions whose difference lies along the
// camera's right and whose sum along its view, so they give the turn exactly, whatever lag the pushed camera has. It
// keeps the last frame's as well, for the clouds' history. Layout of camera: rows 0 to 2 this frame's right, up and back
// (the rows of world_to_view's turn), 3 to 5 the last frame's, 6.x 1 when this frame's are known and 6.y when the last
// frame's are.
Texture2D<float4> directions : register(t0);
RWBuffer<float4> camera : register(u0);

cbuffer SkyCameraFrame : register(b0)
{
    uint last_known;    // 1 when the last frame computed its turn too
    uint3 pad;
};

[numthreads(1, 1, 1)]
void main()
{
    uint2 size;
    directions.GetDimensions(size.x, size.y);
    float4 rows[3] = { camera[0], camera[1], camera[2] };
    bool was = camera[6].x > 0.5 && last_known != 0;
    camera[3] = rows[0];
    camera[4] = rows[1];
    camera[5] = rows[2];
    int y = size.y / 2;
    float4 left = directions.Load(int3(size.x / 4, y, 0)), right = directions.Load(int3(size.x - 1 - size.x / 4, y, 0));
    bool known = left.w > 0.5 && right.w > 0.5;
    if (known)
    {
        float3 r = normalize(right.xyz - left.xyz), forward = normalize(right.xyz + left.xyz);
        forward = normalize(forward - dot(forward, r) * r);
        float3 back = -forward;
        // Unity's world is left-handed, so up is right x back.
        float3 up = cross(r, back);
        camera[0] = float4(r, 0);
        camera[1] = float4(up, 0);
        camera[2] = float4(back, 0);
    }
    camera[6] = float4(known ? 1 : 0, known && was ? 1 : 0, 0, 0);
}
