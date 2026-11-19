// This frame's camera from the game's own constants: a G-buffer draw's vertex shader b1, whose rows 5 to 8 hold the
// projection's columns (D3D clip as the game rasterizes it: y down, reversed depth, the frame's jitter in column 2) and
// rows 13 to 16 the camera-to-world matrix's (right, up, back, position). It writes the three matrices that open the
// shared constants (common.hlsli), each as its columns, as HLSL's column-major packing stores them: world_to_view,
// clip_to_view and view_to_clip, with y up as GL.GetGPUProjectionMatrix(projection, false) has it. A copy that is not such
// a camera writes nothing, so the last good one stays.
Buffer<float4> game : register(t0);
RWBuffer<float4> matrices : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    float4 c0 = game[5], c1 = game[6], c2 = game[7], c3 = game[8];
    float3 r = game[13].xyz, u = game[14].xyz, b = game[15].xyz, p = game[16].xyz;
    bool camera = abs(length(r) - 1) < 1e-3 && abs(length(u) - 1) < 1e-3 && abs(length(b) - 1) < 1e-3 && abs(dot(r, u)) < 1e-3 && abs(dot(r, b)) < 1e-3
        && abs(dot(u, b)) < 1e-3 && c0.x > 0 && c1.y != 0 && c2.w == -1 && c3.z > 0 && c3.w == 0 && game[16].w == 1;
    if (!camera)
        return;
    matrices[0] = float4(r.x, u.x, b.x, 0);
    matrices[1] = float4(r.y, u.y, b.y, 0);
    matrices[2] = float4(r.z, u.z, b.z, 0);
    matrices[3] = float4(-dot(r, p), -dot(u, p), -dot(b, p), 1);
    // view_to_clip is the game's projection with y up: x' = a x + jx z, y' = by y + jy z, z' = c z + d, w' = -z.
    float a = c0.x, by = -c1.y, jx = c2.x, jy = -c2.y, c = c2.z, d = c3.z;
    matrices[4] = float4(1 / a, 0, 0, 0);
    matrices[5] = float4(0, 1 / by, 0, 0);
    matrices[6] = float4(0, 0, 0, 1 / d);
    matrices[7] = float4(jx / a, jy / by, -1, c / d);
    matrices[8] = float4(a, 0, 0, 0);
    matrices[9] = float4(0, by, 0, 0);
    matrices[10] = float4(jx, jy, c, -1);
    matrices[11] = float4(0, 0, d, 0);
}
