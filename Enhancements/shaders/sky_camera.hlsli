// The camera's turn found from the game's sky dome this frame (sky_camera_cs.hlsl), for the clouds and the stars, so they
// stay put as the camera turns; the pushed camera's while it is not known, as where no dome is drawn.
cbuffer SkyCamera : register(b7)
{
    float4 sky_camera_rows[7];
};

bool sky_camera_known() { return sky_camera_rows[6].x > 0.5; }

float3 sky_world_from_view(float3 v)
{
    return sky_camera_known() ? v.x * sky_camera_rows[0].xyz + v.y * sky_camera_rows[1].xyz + v.z * sky_camera_rows[2].xyz
        : mul(v, (float3x3)world_to_view);
}

float3 sky_view_from_world(float3 w)
{
    return sky_camera_known() ? float3(dot(w, sky_camera_rows[0].xyz), dot(w, sky_camera_rows[1].xyz), dot(w, sky_camera_rows[2].xyz))
        : mul((float3x3)world_to_view, w);
}

// The last frame's view of w, for the clouds' history: from the dome when both frames had it, else the pushed camera's.
float3 sky_last_view_from_world(float3 w)
{
    return sky_camera_rows[6].y > 0.5 ? float3(dot(w, sky_camera_rows[3].xyz), dot(w, sky_camera_rows[4].xyz), dot(w, sky_camera_rows[5].xyz))
        : mul((float3x3)cloud_prev_world_to_view, w);
}
