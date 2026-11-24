// Shared by every shader. Registers stay inside gpu.h's ranges: t0-t7, s0-s3, b0-b4.

cbuffer Constants : register(b0)
{
    float4x4 world_to_view;     // Unity's worldToCameraMatrix, column-major in memory as HLSL's default packing reads it
    float4x4 clip_to_view;      // inverse of GL.GetGPUProjectionMatrix(projection, false)
    float4x4 view_to_clip;      // GL.GetGPUProjectionMatrix(projection, false)
    float4 render_size;         // w, h, 1/w, 1/h; the back buffer's in the debug composite
    float level;
    float ao_strength;
    float ao_radius;
    float flip;                 // 1: game targets are stored upside down, so their row 0 is the bottom of the image
    float projection_scale;     // pixels per world unit at distance 1: 0.5 * h * view_to_clip[1][1]
    uint decode;                // Decode in core/plan.h
    float foliage_ao_strength;  // multiplies ao_strength on stencil 129, 136 and 137
    float contact_length;       // meters
    float3 toward_sun;          // world space, unit length, or zero when there is no sun
    float contact_strength;
    float contact_thickness;    // meters
    uint label_length;          // glyphs in label
    float contact_foliage_strength;  // multiplies contact_strength on stencil 129, 136 and 137
    float ambient_neutral;      // 0 the game's irradiance color, 1 a gray of the same luminance
    uint4 label[2];             // the debug composite's view name, glyph indices from encode_label, four per uint, low byte first
    float3 sun_color;           // linear RGB times intensity
    float atmosphere_density;   // extinction per meter at the camera's height
    float3 sky_color;           // linear RGB
    float height_falloff;       // per meter
    float sun_scatter;
    float anisotropy;           // Henyey-Greenstein g
    float exposure_scale;       // 2 ^ tonemap.exposure_ev
    uint curve;                 // Curve in core/plan.h
    float bloom_strength;       // multiplies the game's bloom intensity
    float saturation;
    float contrast;
    float auto_exposure;        // EnhancementsAutoExposure.strength, for exposure_adapt_ps.hlsl
    uint sun_cascades;          // the cascades the game uses, from SunShadowConstants in core/sun.h
    uint sun_columns;           // the atlas's grid of cascade tiles
    uint sun_rows;
    float sun_light_size;       // penumbra width per meter between caster and receiver
    float sun_min_penumbra;     // meters
    float sun_strength;
    float2 lens_sample;         // this frame's aperture point, x right and y up
    float cat_eye;              // EnhancementsAccumulate.cat_eye
    float cat_eye_falloff;
    float wetness;              // the wetness pass's amount: EnhancementsWetness.wetness, or the weather's while that is 0
    float wet_darkening;        // EnhancementsWetness.darkening
    float puddles;              // EnhancementsWetness.puddles
    float shafts_strength;      // EnhancementsLightShafts.strength
    float shafts_density;       // the mist's extinction per meter at the camera's height
    float shafts_height;        // meters per e of thinning above the camera
    float shafts_anisotropy;    // Henyey-Greenstein g
    float shafts_distance;      // meters marched at most
    float shafts_patchiness;
    float shafts_patch_size;    // meters
    float shafts_ambient;       // multiplies the sky light in the mist
    uint shafts_steps;          // samples per ray
    float2 flare_sun_uv;        // the lens flare's sun, 0 to 1 from the top left of the picture, upright; may lie off it
    float flare_ease;           // the share of the way the flare's visibility moves toward this frame's in one frame
    float flare_disc;           // the radius of the disc around the sun the flare's visibility looks at, over the height
    uint flare_mode;            // 0 the sun in front of the camera, 1 the manual source, 2 a sun behind the camera
    uint flare_atlas;           // 1 when this frame's sun constants passed their checks, so the atlas can be read
    float tone_shadows;         // EnhancementsTones.shadows
    float tone_highlights;      // EnhancementsTones.highlights
    float accum_gain;           // EnhancementsAccumulate.gain, for present_ps.hlsl
    float auto_ease;            // the share of the way the auto exposure moves in this frame, 1 to jump
    float cloud_coverage;       // EnhancementsClouds, and what run_clouds derives from it
    float cloud_base;           // world meters
    float cloud_thickness;
    float cloud_size;
    float cloud_stretch;
    float cloud_density;        // extinction per meter
    float cloud_detail;
    float cloud_sun;
    float2 cloud_map_center;    // the cloud map's center, in cloud space: world x and z plus the wind's drift
    float cloud_map_extent;     // meters the map spans
    float cloud_ambient;
    float2 cloud_wind;          // meters the clouds drifted along world x and z
    float cloud_fade;           // meters per e of fading into the sky behind
    uint cloud_steps;
    float3 cloud_toward_sun;    // the sky's sun while it is up, the main light otherwise
    float cloud_pad;
    float4x4 cloud_prev_world_to_view;  // last frame's camera, for the cloud history's reprojection
    float4x4 cloud_prev_view_to_clip;
    float cloud_phase;          // 0 to 1, the march's sample offset this frame
    float cloud_blend;          // the share of this frame in the cloud history, 1 to start it again
    float cloud_block;          // which pixel of each 2x2 block of the cloud history this frame marches, 0 to 3; 4 all, at half size; 5 all, at the full size
    float cloud_type;           // ENHANCEMENTS_CLOUD_TYPE_*
    float2 cloud_wind_step;     // meters the clouds drifted since last frame
    float cloud_camera_height;  // world meters
    float cloud_pattern;        // 0 our noise, 1 to 6 the pattern texture's ENHANCEMENTS_CLOUD_PATTERN_*
    float3 sky_toward_sun;      // the physical sky's sun, unit, world
    float sky_haze;             // the aerosols' optical depth straight up
    float3 sky_toward_moon;     // unit, world
    float sky_moon_radiance;    // the sunlit moon's radiance over its albedo and its Lommel-Seeliger shading, in sun units
    float3 sky_moon_north;      // unit, world
    float sky_earthshine;       // the earthlit moon's radiance over its albedo
    float3 sky_scale;           // game units per unit of the sun's light above the air, per channel
    float sky_camera_km;        // the viewer's height above the ground
    float3 sky_stars_x;         // the J2000 equatorial axes in the world
    float sky_moon_illuminance; // the moon's illuminance above the air, in sun units
    float3 sky_stars_y;
    float sky_cloud_physical;   // 1: the clouds take the physical sky's light at their height
    float3 sky_stars_z;
    float sky_cloud_illuminance;    // above the air, in sun units, of the light along cloud_toward_sun
    float sky_aerial;           // EnhancementsAerial.distance: the scene's meters times this are the air's
    float sky_vegetation;       // EnhancementsCelestial.vegetation_scale; 1 while the pass has no albedo
    float sky_aerial_lit;       // 1: aerial_lit_ps.hlsl's share of lit air is bound for the haze; 0: all the air is lit
    float sky_dome;             // 1: sky_dome_ps.hlsl drew the sky this frame, which the sky pass keeps
    float water_clarity;        // EnhancementsWater's
    float water_waves;
    float water_time;           // seconds, for the waves' motion
    float water_wind;           // radians from world +x toward +z, the way the wind blows the waves
    float water_size;           // EnhancementsWater.size
    float sky_moon_face;        // a full moon's face at the sky's scale, on average
    float reflection_overcast;  // how far the ambient pass grays the game's reflections, 0 to 1, by the clouds' cover
    float sky_sunset;           // EnhancementsSunset.strength
    float sky_overcast;         // how overcast the clouds make the sky, 0 to 1: the air scatters less sunlight and grays
    float rain_amount;          // the share of rain_drops_vs.hlsl's cells holding a drop, and of rain_ps.hlsl's splashes
    float rain_time;            // seconds on the game's clock
    float rain_wind;            // radians from world +x toward +z, the way the wind blows
    float rain_flash;           // lightning's flash, 0 to 1.5: Lightning in core/sky.h
    float rain_bolt;            // its bolt's brightness, 0 while none shows
    float rain_bolt_azimuth;    // radians from the camera's heading, the way world +x turns toward +z
    float rain_bolt_seed;       // shapes the bolt's path
    float mist_patchiness;      // EnhancementsMist's, for atmosphere_ps.hlsl
    float mist_size;            // meters across a bank
    float2 mist_drift;          // meters the banks drifted along world x and z
    float scene_gain;           // 2 ^ EnhancementsSceneLight.exposure_ev, for scene_light_ps.hlsl
    float transmission_strength;    // EnhancementsTransmission.strength, for transmission_ps.hlsl
    float grass_roots;          // EnhancementsGrass.root_shade, for grass_ps.hlsl
    float leaf_light;           // EnhancementsLeaves.shading, for transmission_ps.hlsl
    float cloud_mid_coverage;   // the layered sky's middle layer, as cloud_coverage to cloud_type shape the main one
    float cloud_mid_base;
    float cloud_mid_thickness;
    float cloud_mid_size;
    float cloud_mid_stretch;
    float cloud_mid_density;
    float cloud_mid_detail;
    float cloud_mid_type;
    float cloud_layers;         // 1 the main layer alone, 2 with the middle one
    float flare_clouds;         // 1 while this frame's volume clouds are in their history, which dims the flare's sun
    uint water_stamp;           // this frame's number for water_ps.hlsl's marks, never 0
    float cloud_layers_pad;
};

SamplerState point_clamp : register(s0);
SamplerState linear_clamp : register(s1);

// The game's character test: bit 0x04 marks characters, 0x80 the lit world. Sky is 0.
bool is_world(uint stencil) { return (stencil & 0x84u) == 0x80u; }

bool is_grass(uint stencil) { return stencil == 129; }

// Grass, vegetation and foliage: alpha-tested, so their depth is noisy at the pixel scale.
bool is_foliage(uint stencil) { return stencil == 129 || stencil == 136 || stencil == 137; }

// The texture coordinate in a stored game target of a point on screen, uv (0, 0) being the top left.
float2 stored_uv(float2 uv) { return flip > 0.5 ? float2(uv.x, 1 - uv.y) : uv; }

// Unity's view space position of a stored pixel, from reversed Z. Reads flip like the debug views, so an upright view
// means this reconstruction is upright too.
float3 view_position(float2 stored, float reversed_depth)
{
    float2 screen = stored_uv(stored);
    float4 clip = float4(screen.x * 2 - 1, 1 - screen.y * 2, reversed_depth, 1);
    float4 view = mul(clip_to_view, clip);
    return view.xyz / view.w;
}

// The stored texture coordinate of a view space point in front of the camera; view_position's inverse.
float2 stored_of(float3 view)
{
    float4 clip = mul(view_to_clip, float4(view, 1));
    float2 ndc = clip.xy / clip.w;
    return stored_uv(float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5));
}
