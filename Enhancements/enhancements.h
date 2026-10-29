// The C interface of CameraToolsEnhancements.addon64, a ReShade add-on that relights Genshin's world inside its own frame.
// ReShade loads the add-on. A mod finds the loaded module by name and calls these exports.
//
// Every function may be called from any thread and returns at once. The add-on applies settings and the camera at the
// next frame boundary on ReShade's render thread, so a whole frame always sees one settings value.
#pragma once

#include <stdint.h>

#define ENHANCEMENTS_VERSION 20

// The native tests compile this header on macOS, where there is nothing to export.
#ifdef _WIN32
#define ENHANCEMENTS_EXPORT __declspec(dllexport)
#else
#define ENHANCEMENTS_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Debug views. Each shows one frame-map entry full screen, decoded for the eye.
enum
{
    ENHANCEMENTS_VIEW_OFF = 0,
    ENHANCEMENTS_VIEW_NORMALS,
    ENHANCEMENTS_VIEW_ALBEDO,
    ENHANCEMENTS_VIEW_SPECULAR,
    ENHANCEMENTS_VIEW_MATERIAL_ID,
    ENHANCEMENTS_VIEW_SMOOTHNESS,
    ENHANCEMENTS_VIEW_CHARACTER,
    ENHANCEMENTS_VIEW_DEPTH,
    ENHANCEMENTS_VIEW_STENCIL,
    ENHANCEMENTS_VIEW_SHADOW_MASK,
    ENHANCEMENTS_VIEW_QUARTER_SHADOW,
    ENHANCEMENTS_VIEW_AMBIENT_DIFFUSE,
    ENHANCEMENTS_VIEW_AMBIENT_SPECULAR,
    ENHANCEMENTS_VIEW_HDR_SCENE,
    ENHANCEMENTS_VIEW_BLOOM_FINAL,
    ENHANCEMENTS_VIEW_SUN_ATLAS,
    ENHANCEMENTS_VIEW_LIGHT_SHAFTS,    // not an entry: the light the light-shaft pass's mist scatters, drawn by the pass for it
    ENHANCEMENTS_VIEW_TRACE,           // not an entry: the trace pass's averages, occlusion red and sun green, blue where a pixel
                                    // has no sample yet; drawn by the pass for it
    ENHANCEMENTS_VIEW_BOUNCE,          // not an entry: the gathered bounce light, unfiltered, tone mapped; drawn by the trace pass
    ENHANCEMENTS_VIEW_SKY_VISIBILITY,  // not an entry: the share of the gather's rays that met the sky, gray, blue where a pixel has
                                    // no sample; drawn by the trace pass
    ENHANCEMENTS_VIEW_SKY_MAP,         // not an entry: the traced scene's sky map stretched over the screen, filled texels dimmed by
                                    // half; drawn by the trace pass
    ENHANCEMENTS_VIEW_COUNT,
};

// Steps of the game's frame the add-on recognizes, as bits of EnhancementsStatus.steps_*.
enum
{
    ENHANCEMENTS_STEP_GBUFFER = 1u << 0,
    ENHANCEMENTS_STEP_QUARTER_SHADOW = 1u << 1,
    ENHANCEMENTS_STEP_SHADOW_MASK = 1u << 2,
    ENHANCEMENTS_STEP_AMBIENT_PAIR = 1u << 3,
    ENHANCEMENTS_STEP_COMBINE = 1u << 4,
    ENHANCEMENTS_STEP_BLOOM = 1u << 5,
    ENHANCEMENTS_STEP_TONEMAP = 1u << 6,
    ENHANCEMENTS_STEP_FORWARD = 1u << 7,   // the first forward draw into the HDR scene, after the deferred combine draws
    ENHANCEMENTS_STEP_GBUFFER_DONE = 1u << 8,  // the first draw that samples the G-buffer's smoothness, after its last write
    ENHANCEMENTS_STEP_TONEMAP_READ = 1u << 9,  // the first draw that samples the tone map's output: before the game's UI
};

// Frame-map entries, as bits of EnhancementsStatus.entries_missing.
enum
{
    ENHANCEMENTS_ENTRY_NORMALS = 1u << 0,
    ENHANCEMENTS_ENTRY_ALBEDO = 1u << 1,
    ENHANCEMENTS_ENTRY_SPECULAR = 1u << 2,
    ENHANCEMENTS_ENTRY_MATERIAL_ID = 1u << 3,
    ENHANCEMENTS_ENTRY_SMOOTHNESS = 1u << 4,
    ENHANCEMENTS_ENTRY_CHARACTER = 1u << 5,
    ENHANCEMENTS_ENTRY_DEPTH = 1u << 6,
    ENHANCEMENTS_ENTRY_QUARTER_SHADOW = 1u << 7,
    ENHANCEMENTS_ENTRY_SHADOW_MASK = 1u << 8,
    ENHANCEMENTS_ENTRY_AMBIENT_SPECULAR = 1u << 9,
    ENHANCEMENTS_ENTRY_AMBIENT_DIFFUSE = 1u << 10,
    ENHANCEMENTS_ENTRY_HDR_SCENE = 1u << 11,
    ENHANCEMENTS_ENTRY_BLOOM = 1u << 12,
    ENHANCEMENTS_ENTRY_TONEMAP_OUT = 1u << 13,
    ENHANCEMENTS_ENTRY_BLOOM_FINAL = 1u << 14,
    ENHANCEMENTS_ENTRY_SUN_ATLAS = 1u << 15,
};

// Our passes, as bits of EnhancementsStatus.passes_*.
enum
{
    ENHANCEMENTS_PASS_AMBIENT = 1u << 0,
    ENHANCEMENTS_PASS_CONTACT_SHADOWS = 1u << 1,
    ENHANCEMENTS_PASS_ATMOSPHERE = 1u << 2,
    ENHANCEMENTS_PASS_TONEMAP = 1u << 3,
    ENHANCEMENTS_PASS_SUN_SHADOWS = 1u << 4,
    ENHANCEMENTS_PASS_WETNESS = 1u << 5,
    ENHANCEMENTS_PASS_LIGHT_SHAFTS = 1u << 6,
    ENHANCEMENTS_PASS_TRACE = 1u << 7,
    ENHANCEMENTS_PASS_DEPTH_OF_FIELD = 1u << 8,
    ENHANCEMENTS_PASS_LENS_FLARE = 1u << 9,
};

// The tonemap pass's curves, values of EnhancementsTonemap.curve.
enum
{
    ENHANCEMENTS_CURVE_GAME = 0,   // the game's filmic x(1.36x + 0.047) / (x(0.93x + 0.56) + 0.14)
    ENHANCEMENTS_CURVE_AGX,        // AgX, after Benjamin Wrensch's minimal fit
    ENHANCEMENTS_CURVE_NEUTRAL,    // Khronos PBR Neutral
    ENHANCEMENTS_CURVE_COUNT,
};

// What the accumulator does with the HDR scene at the bloom step, values of EnhancementsAccumulate.mode.
enum
{
    ENHANCEMENTS_ACCUMULATE_OFF = 0,   // nothing; the sum is kept
    ENHANCEMENTS_ACCUMULATE_ADD,       // adds each marked frame's HDR scene into the sum
    ENHANCEMENTS_ACCUMULATE_PRESENT,   // writes the sum's average over the HDR scene every frame
    ENHANCEMENTS_ACCUMULATE_COUNT,
};

enum
{
    ENHANCEMENTS_ERROR_NONE = 0,
    ENHANCEMENTS_ERROR_NOT_D3D11,  // the game's device is not D3D11; the add-on never arms
    ENHANCEMENTS_ERROR_SHADER,     // a shader or state object of ours failed to create; the add-on never arms
    ENHANCEMENTS_ERROR_TEXTURE,    // a texture of ours failed to create last frame; the pass or view that needed it skipped
    ENHANCEMENTS_ERROR_STATE,      // the game's render target, depth or pixel shader differed after we restored state;
                                // the add-on disarmed until it reloads, and ReShade.log says which binding moved
};

// The ambient pass replaces the game's diffuse irradiance on world pixels (stencil & 0x84 == 0x80) before the combine
// pass reads it. Characters and the sky keep the game's value. contact_shadows follows it in EnhancementsSettings, so this
// block keeps its size.
typedef struct EnhancementsAmbient
{
    uint32_t enabled;           // default 0
    float level;                // multiplies the game's irradiance; default 0.6, clamped to 0..4
    float ao_strength;          // 0 no ambient occlusion, 1 full; default 0.5
    float ao_radius;            // in world units (meters), default 1, clamped to 0.05..10
    float foliage_ao_strength;  // multiplies ao_strength on grass, vegetation and foliage (stencil 129, 136, 137),
                                // whose alpha-tested depth makes noisy occlusion; default 0.5, clamped to 0..1
} EnhancementsAmbient;

// Screen-space contact shadows: each world pixel looks toward the sun through the depth buffer for length meters, and a
// surface in the way lowers the sun's visibility in the game's shadow mask before the combine pass reads it. They add
// the small shadows the game's shadow map is too coarse for, such as a character's feet on the ground. Never brightens.
// Grass (stencil 129) never casts: its blades would shadow each other and the ground between them.
// atmosphere follows it in EnhancementsSettings, so this block keeps its size.
typedef struct EnhancementsContactShadows
{
    uint32_t enabled;           // default 0
    float length;               // how far each pixel looks toward the sun, in meters; default 0.6, clamped to 0.05..5
    float strength;             // 0 no contact shadows, 1 full; default 1, clamped to 0..1
    float thickness;            // how deep a surface in the depth buffer is taken to be, in meters, so a ray passing
                                // behind a thin pole is not shadowed; default 0.25, clamped to 0.01..2
    float foliage_strength;     // multiplies strength on grass, vegetation and foliage (stencil 129, 136, 137), whose
                                // dense alpha-tested blades shadow each other into speckle; default 0, clamped to 0..1
} EnhancementsContactShadows;

// Aerial perspective: haze between the camera and each pixel dims the HDR scene and scatters sky and sun light into the
// view, before the game's bloom and tone map read it. The haze thins with height above the camera, and the sun's part
// gathers around the sun. The sky (depth 0) keeps the game's color. Light comes from EnhancementsCamera's sun and sky colors.
// tonemap follows it in EnhancementsSettings, so this block keeps its size.
typedef struct EnhancementsAtmosphere
{
    uint32_t enabled;           // default 0
    float density;              // extinction per meter at the camera's height; default 0.000325, which hazes a pixel 500 m
                                // away level with the camera by 15%; clamped to 0..0.01
    float height_falloff;       // per meter: the haze thins by e every 1 / height_falloff meters above the camera and
                                // thickens as fast below it; default 0.02, clamped to 0..1
    float sun_scatter;          // multiplies the sun light scattered toward the camera; default 1, clamped to 0..10
    float anisotropy;           // Henyey-Greenstein g: 0 scatters sun light evenly, toward 1 into a glow around the sun;
                                // default 0.7, clamped to -0.95..0.95
} EnhancementsAtmosphere;

// Our tone map in place of the game's: the add-on skips the game's tone map draw and draws into its output instead,
// from the HDR scene, the game's final bloom, and the game's exposure, bloom intensity, color matrix and display gamma,
// read each frame from the skipped draw's constants. With the defaults it reproduces the game's image.
// sun_shadows follows it in EnhancementsSettings, so this block keeps its size.
typedef struct EnhancementsTonemap
{
    uint32_t enabled;           // default 0
    float exposure_ev;          // stops added to the game's exposure; default 0, clamped to -10..10
    uint32_t curve;             // ENHANCEMENTS_CURVE_*; unknown values read as the game's, the default
    float bloom_strength;       // multiplies the game's bloom intensity; default 1, clamped to 0..4
    float saturation;           // 0 gray, 1 unchanged; default 1, clamped to 0..2
    float contrast;             // a power around middle gray (0.18) before the curve; default 1, clamped to 0.5..2
} EnhancementsTonemap;

// Soft sun shadows from the game's own shadow atlas and cascades: on world pixels, the sun's visibility in the shadow
// mask is drawn again with a penumbra that widens with the distance between the shadow and what casts it, and replaces
// the game's, before the combine pass reads it. Characters, the sky, and pixels past the game's cascades or shadow
// distance keep the game's value.
// wetness follows it in EnhancementsSettings, so this block keeps its size.
typedef struct EnhancementsSunShadows
{
    uint32_t enabled;           // default 0
    float light_size;           // the penumbra's width per meter between caster and shadow: 0.0093 is the real sun's; default
                                // 0.03, so a branch 10 m up casts a shadow with a 0.3 m soft edge; clamped to 0..0.2
    float min_penumbra;         // the soft edge's least width in meters, at the caster; default 0.02, clamped to 0..0.5
    float strength;             // 0 no sun shadow, 1 full; default 1, clamped to 0..1
} EnhancementsSunShadows;

// Wet surfaces: on world pixels, after the game's last G-buffer write and before anything reads it, the G-buffer's albedo
// darkens, its smoothness rises and its specular color moves toward water's, more on surfaces that face up, and puddles
// form on flat ground. The game's reflections, sun light and ambient light then all see the wet surface.
// light_shafts follows it in EnhancementsSettings, so this block keeps its size.
typedef struct EnhancementsWetness
{
    uint32_t enabled;           // default 0
    float wetness;              // 0 (the default) follows the weather, which EnhancementsCamera.sky_color[3] carries; above 0
                                // it replaces the weather's: 1 is soaked; clamped to 0..1
    float darkening;            // how much a soaked surface's albedo darkens; default 0.35, clamped to 0..1
    float puddles;              // how much of the flat ground puddles cover when soaked, from none at 0 to all of it at 1;
                                // default 0.3, clamped to 0..1
} EnhancementsWetness;

// Light shafts: sun light scattered toward the camera by a ground mist, bright where the game's sun shadow atlas says the
// sun reaches the mist and dark where trunks, crowns or anything else that casts the game's sun shadows blocks it, on
// screen or off. At the bloom step, after the atmosphere pass, each pixel's view ray is marched through the mist up to
// the pixel or distance, and the HDR scene dims behind the mist and takes its light, before the game's bloom and tone map
// read it. The mist thins with height above the camera and thickens below it, and its patches stay put in the world. Light
// comes from EnhancementsCamera's sun and sky colors and sun direction; the atlas is copied whenever this pass is on.
// trace follows it in EnhancementsSettings, so this block keeps its size.
typedef struct EnhancementsLightShafts
{
    uint32_t enabled;           // default 0
    float strength;             // multiplies the sun light the mist scatters; default 3, clamped to 0..10
    float density;              // the mist's extinction per meter at the camera's height; default 0.006, which hides 26% of a
                                // pixel 50 m away level with the camera; clamped to 0..0.2
    float height;               // meters: the mist thins by e every height meters above the camera and thickens as fast
                                // below it, to at most 20 times; default 5, clamped to 0.5..500
    float anisotropy;           // Henyey-Greenstein g: 0 scatters sun light evenly, toward 1 into shafts that shine when
                                // the camera looks toward the sun; default 0.6, clamped to -0.95..0.95
    float distance;             // meters of mist along each pixel's ray, up to the pixel, and all of it for the sky;
                                // default 150, clamped to 5..900
    float patchiness;           // 0 an even mist, 1 banks and clear holes; default 1, clamped to 0..1
    float patch_size;           // meters across a patch; default 6, clamped to 0.5..100
    float ambient;              // multiplies the sky light the mist scatters, which keeps mist in shadow from going black;
                                // default 0.3, clamped to 0..4
    uint32_t steps;             // samples along each ray; default 48, clamped to 8..128
} EnhancementsLightShafts;

// What the trace pass writes into the frame, as bits of EnhancementsTrace.outputs.
enum
{
    ENHANCEMENTS_TRACE_AO = 1u << 0,   // traced ambient occlusion into the diffuse irradiance
    ENHANCEMENTS_TRACE_SUN = 1u << 1,  // traced sun visibility into the shadow mask
};

// What casts in the traced scene besides the world, as bits of EnhancementsTrace.casters. Bit 1 is kept for grass, which
// the traced scene leaves out.
enum
{
    ENHANCEMENTS_TRACE_CAST_CHARACTERS = 1u << 0,  // characters (stencil 133 and 165), whose own pixels keep the game's values
    ENHANCEMENTS_TRACE_CAST_ALPHA_CARDS = 1u << 2, // vegetation and foliage (136 and 137)
    ENHANCEMENTS_TRACE_CAST_WHOLE_CARDS = 1u << 3, // alpha-tested casters cast their whole triangles: the alpha tests off, as before
                                                // T1b, for comparison
};

// How the gathered light meets the game's sky light, values of EnhancementsGather.sky. With vis the share of a pixel's gather
// rays that met the sky, occ its traced occlusion within trace.ao_distance, B the light its rays met on surfaces and K
// the sky they met, the irradiance on world pixels becomes:
enum
{
    ENHANCEMENTS_GATHER_SKY_OCCLUSION = 0, // level x game x lerp(1, occ, trace.ao_strength) + strength x B: T1's occlusion with the
                                        // bounce on top, as screen-space GI does it
    ENHANCEMENTS_GATHER_SKY_VISIBILITY,    // level x game x lerp(1, vis, trace.ao_strength) + strength x B: each direction counts once
    ENHANCEMENTS_GATHER_SKY_WEIGHTED,      // as VISIBILITY, with vis weighted by how bright the sky it met is against the sky map's mean
    ENHANCEMENTS_GATHER_SKY_MAP,           // level x K + strength x B: the sky map replaces the game's irradiance
    ENHANCEMENTS_GATHER_SKY_COUNT,
};

// What of the traced scene's light came from the game and not from a fallback, as bits of EnhancementsStatus.trace_light.
enum
{
    ENHANCEMENTS_TRACE_LIGHT_ALBEDO = 1u << 0,     // a face's G-buffer gave triangles their albedo
    ENHANCEMENTS_TRACE_LIGHT_SKY = 1u << 1,        // the faces' sky made the sky map
    ENHANCEMENTS_TRACE_LIGHT_SUN = 1u << 2,        // the sun's color and shading direction are the game's combine constants
    ENHANCEMENTS_TRACE_LIGHT_AMBIENT = 1u << 3,    // the ambient light at a hit is a fit of the game's irradiance
};

// The traced scene's state, values of EnhancementsStatus.trace_state.
enum
{
    ENHANCEMENTS_TRACE_STATE_NONE = 0,     // no traced scene
    ENHANCEMENTS_TRACE_STATE_BUILDING,     // an ENHANCEMENTS_SCENE_TRACE capture runs, or its BVH is being built
    ENHANCEMENTS_TRACE_STATE_UPLOADING,    // its buffers are being filled, a part each present
    ENHANCEMENTS_TRACE_STATE_READY,        // the trace pass can trace it
    ENHANCEMENTS_TRACE_STATE_STALE,        // the world moved under it: a world shift, or rays from the camera no longer meet the
                                        // depth buffer; the raster passes run, until another capture or a release
    ENHANCEMENTS_TRACE_STATE_FAILED,       // it could not be built, uploaded or traced; ReShade.log says why
};

// Why the trace's samples last started again, values of EnhancementsStatus.trace_restart_reason: the first of these that
// held on that frame. While one keeps holding, trace_restarts rises and trace_samples stays 0.
enum
{
    ENHANCEMENTS_TRACE_RESTART_NONE = 0,       // they have not started yet
    ENHANCEMENTS_TRACE_RESTART_SCENE,          // a new scene: its first samples
    ENHANCEMENTS_TRACE_RESTART_RENDER_SIZE,
    ENHANCEMENTS_TRACE_RESTART_SETTINGS,       // trace.ao_distance, sun_size or casters, gather.enabled or gather.hit_ambient
    ENHANCEMENTS_TRACE_RESTART_CAMERA,         // the pushed camera's view or projection
    ENHANCEMENTS_TRACE_RESTART_SUN_DIRECTION,  // the direction toward the sun, the game's shadows' or its shading's
    ENHANCEMENTS_TRACE_RESTART_LIGHT,          // the sun's color
    ENHANCEMENTS_TRACE_RESTART_COUNT,
};

// The trace pass: ambient occlusion and sun shadows traced against the scene an ENHANCEMENTS_SCENE_TRACE capture built, at the
// combine step before the game's combine pass reads them. Once every world pixel has a sample, it writes the traced sun
// visibility into the shadow mask and multiplies the diffuse irradiance by ambient.level and the traced occlusion, on world
// pixels, and sun_shadows, contact_shadows and ambient do not run (only those whose output it writes). Each frame it adds
// one sample per pixel to as many 64x64 tiles as budget_ms allows, until every pixel has its samples; a moved camera, a new
// render size, sun or scene, or a change to ao_distance, sun_size or casters starts the samples again.
// The last block of EnhancementsSettings, so a field added here goes at the end and size keeps older callers working.
typedef struct EnhancementsTrace
{
    uint32_t enabled;           // default 0
    uint32_t outputs;           // ENHANCEMENTS_TRACE_AO | ENHANCEMENTS_TRACE_SUN; 0 reads as both
    uint32_t casters;           // ENHANCEMENTS_TRACE_CAST_*; unknown bits are dropped. A caller without this block gets
                                // CHARACTERS | ALPHA_CARDS
    float ao_distance;          // meters an occlusion ray looks; a hit at distance t occludes 1 - (t / ao_distance)^2;
                                // default 2, clamped to 0.1..20
    float ao_strength;          // 0 no traced occlusion, 1 full; default 1, clamped to 0..1
    float sun_size;             // the sun's angular diameter in radians, which is the penumbra's width per meter between
                                // caster and shadow: 0.0093, the real sun's, by default; clamped to 0..0.2
    float sun_strength;         // 0 no traced sun shadow, 1 full; default 1, clamped to 0..1
    uint32_t ao_samples;        // occlusion rays per pixel, then it stops; default 64, clamped to 1..4096
    uint32_t sun_samples;       // sun rays per pixel; default 32, clamped to 1..4096
    float budget_ms;            // GPU time per frame for tracing, measured with timestamp queries; default 100, clamped to
                                // 2..200. Frames are that much longer until every pixel has its samples
} EnhancementsTrace;

// One bounce of diffuse light: each world pixel gathers the light of the surfaces and the sky its hemisphere sees, by rays
// to any distance, and from each hit that faces the sun one more ray toward it. Needs trace.enabled and a scene captured
// with ENHANCEMENTS_SCENE_LIGHT; over a scene without, the trace pass runs as without this block. While on, the gather's rays
// give the occlusion too, and trace.ao_samples is unused. depth_of_field follows it in EnhancementsSettings, so this block
// keeps its size.
typedef struct EnhancementsGather
{
    uint32_t enabled;           // default 0
    uint32_t samples;           // gather samples a pixel, then it stops; default 64, clamped to 1..4096
    uint32_t sky;               // ENHANCEMENTS_GATHER_SKY_*; default VISIBILITY; unknown values read as the default
    uint32_t filter;            // 0 none, 1 one edge-aware 5x5 pass of the far light, 2 two; default 2, clamped to 0..2
    float strength;             // multiplies the bounce; default 1, clamped to 0..4
    float hit_ambient;          // the share of the game's ambient light a hit surface gets; default 0.5, clamped to 0..1
} EnhancementsGather;

// Where the depth of field blurs the picture, values of EnhancementsDepthOfField.place.
enum
{
    ENHANCEMENTS_DOF_PLACE_SCENE = 0,  // in the HDR scene at the bloom step, before the game's bloom and tone map and ReShade's effects
    ENHANCEMENTS_DOF_PLACE_DISPLAY,    // on the back buffer ReShade's effects draw into, decoded to linear light: right after the last
                                    // enabled technique that reads the depth buffer, or before the first when none does, or at
                                    // present when ReShade drew no effects the frame before; from the frame's own depth at the bloom
                                    // step, where only the autofocus runs. While the accumulator adds, it does not blur at all. So
                                    // effects such as RTGI see the sharp frame, and a screenshot ReShade saves after its effects
                                    // has the blur. It blurs whatever the game drew over the scene too, its HUD among it
    ENHANCEMENTS_DOF_PLACE_COUNT,
};

// Depth of field from the depth buffer in one frame: at the bloom step, after the light shafts, each point of the HDR
// scene spreads into its circle of confusion, shaped like the aperture, with spherical aberration's light across it,
// before the game's bloom and tone map read the scene, so highlights become bright discs. The focal length comes from the
// pushed camera's projection and sensor_height_mm, so it follows the field of view. The last block of EnhancementsSettings.
typedef struct EnhancementsDepthOfField
{
    uint32_t enabled;           // default 0
    float f_number;             // default 2.8, clamped to 1..22
    float sensor_height_mm;     // default 24, full frame; clamped to 4..20000. N times taller blurs as a 1:N miniature of the
                                // scene would through the same lens: the focal length grows with it, and the whole optics with
                                // it, which images as every distance divided by N does
    float focus_distance;       // meters along the view axis, which the focus eases to as to the autofocus's; 0 (the default)
                                // focuses on focus_point; clamped to 0..10000
    float focus_point[2];       // the autofocus's point, 0 to 1 from the top left; default the center
    float focus_seconds;        // the focus's ease: the seconds it takes to cover 99% of the way; 0 jumps; default 2, clamped to 0..10
    float max_blur;             // the largest CoC radius as a share of the render height, which bounds the cost; default 0.03,
                                // clamped to 0.002..0.1
    uint32_t blades;            // 0 (the default) round, else 5 to 16; 1 to 4 read as 5
    float blade_roundness;      // 0 straight blades to 1 round; default 0.5
    float blade_rotation;       // degrees; default 0, a corner straight up; clamped to -360..360
    float spherical_aberration; // -1 to 1: above 0 discs behind focus have a bright rim and those in front a soft center (soap
                                // bubble), below 0 the reverse; default 0.6
    float cat_eye;              // 0 to 1: mechanical vignetting cuts discs toward the corners into cat's eyes; default 0
    float swirl;                // 0 to 1: discs squeezed along the line from the center toward the corners; default 0
    float anamorphic;           // the aperture's width over its height is 1 / anamorphic; default 1, clamped to 0.5..2
    float chromatic;            // axial chromatic aberration, 1 a typical achromat's: green fringes behind focus and purple in
                                // front; above 1 exaggerates it; default 0, clamped to 0..5
    uint32_t samples;           // gather samples a pixel on a spiral, rounded to 4k (k + 1) + 1; default 49, clamped to 9..1089
    float highlight_threshold;  // highlights start at this many times the frame's mean luminance (at least 0.25); default 2,
                                // clamped to 1..16
    float highlight_gain;       // how readily a small light that stands out of everything around it grows into a bright disc,
                                // as a real lamp's would; 0 keeps every light as bright as the game drew it; default 2, clamped
                                // to 0..8. Needs scatter
    uint32_t scatter;           // 1: highlights above the threshold that stand out of their surroundings, glints and small
                                // lamps, are drawn as sprites of their disc instead of gathered, so their edges stay smooth and
                                // their rims and colors show; 0 (the default) gathers them with the rest
    float bokeh_contrast;       // how much what stands out of its own surroundings, a grass tip or a leaf's glint, outweighs
                                // them behind focus, so it blurs into a disc as a real lens's would; each place keeps its
                                // light. 0 keeps every source's own light; default 0, clamped to 0..2
    uint32_t place;             // ENHANCEMENTS_DOF_PLACE_*; unknown values read as SCENE, the default
    float eye_distance;         // meters along the view axis to where the caller reckons the surface of an eye under focus_point,
                                // its iris, lies; while above 0 and focus_distance is 0, the autofocus takes the eye's own
                                // surface: the depths of a window eye_size wide around the point that lie from 2.5 cm in front
                                // of that distance to 1.5 cm behind it, the one with a quarter of them nearer; with none there,
                                // the distance itself. 0 (the default) takes the nearest depth around the point; clamped to 0..10000
    float eye_size;             // the window's width in meters at eye_distance; default 0.015, clamped to 0.005..0.1
} EnhancementsDepthOfField;

// Where the lens flare is drawn, values of EnhancementsLensFlare.place.
enum
{
    ENHANCEMENTS_FLARE_PLACE_SCENE = 0,    // on the tone map's output before the game's first draw that reads it, so the game's HUD and
                                        // the mod's own UI, drawn later, stay clear of it; ReShade's effects then see it
    ENHANCEMENTS_FLARE_PLACE_DISPLAY,      // on the back buffer after ReShade's effects and the depth of field's display place, so no
                                        // effect blurs or relights it; it covers whatever the game drew over the scene too
    ENHANCEMENTS_FLARE_PLACE_COUNT,
};

// Where the lens flare's source lies, values of EnhancementsLensFlare.source.
enum
{
    ENHANCEMENTS_FLARE_SOURCE_SUN = 0,     // the sun, from EnhancementsCamera's direction, dimmed where anything blocks it
    ENHANCEMENTS_FLARE_SOURCE_MANUAL,      // a light at position, always unblocked
    ENHANCEMENTS_FLARE_SOURCE_COUNT,
};

// A vintage lens's flare over the finished picture: thin orange-red arcs near the source, soft ghost discs with a bright
// spot on the line from the source through the picture's center, thin streaks along that line, and a warm glare toward
// the source, drawn where place says; when the scene place's step does not come, on the back buffer as in the display place. The sun's flare dims as the sun is hidden: on the picture,
// by the depth buffer around the sun; off it, by the game's sun shadow atlas at the camera. The last block of
// EnhancementsSettings.
typedef struct EnhancementsLensFlare
{
    uint32_t enabled;           // default 0
    uint32_t source;            // ENHANCEMENTS_FLARE_SOURCE_*; unknown values read as SUN, the default
    float position[2];          // the manual source, 0 to 1 from the top left of the picture, and beyond it to -2 and 3; default
                                // 0.8, 0.12
    float strength;             // multiplies the whole flare; default 1, clamped to 0..4
    float arcs;                 // each part's light, clamped to 0..4: arcs, ghosts and streaks default to 1, glare to 0.5
    float ghosts;
    float streaks;
    float glare;
    float size;                 // scales every part; default 1, clamped to 0.25..4
    float warmth;               // 0 cool, 0.5 (the default) the lens's own orange, 1 deeper orange
    float chromatic;            // the red fringe outside each arc and ghost; default 1, clamped to 0..4
    float visibility_seconds;   // how long the flare takes to follow the sun's visibility, 99% of the way; default 0.15,
                                // clamped to 0..5
    float filled_arcs;          // the light of the filled discs with a thick rim, some cut into a D; default 0.5, clamped to 0..4
    uint32_t place;             // ENHANCEMENTS_FLARE_PLACE_*; unknown values read as SCENE, the default
    float rainbow;              // the light of a rainbow band across each arc, violet inside to red outside; default 1, clamped
                                // to 0..4
    float texture;              // how textured the flare is, as a real lens's dust, scratches and coatings make it: striations and
                                // uneven light along the arcs, dust in the ghosts, rays in the glare; 0 smooth, default 1, clamped
                                // to 0..2
} EnhancementsLensFlare;

// The ambient pass's color, a block of its own because EnhancementsAmbient keeps its size: a gray of the same luminance
// in place of the game's irradiance color, which takes the sky's color cast out of the shade and keeps its light.
typedef struct EnhancementsAmbientColor
{
    float neutral;              // 0 (the default) keeps the game's color, 1 is gray; clamped to 0..1
} EnhancementsAmbientColor;

// The tonemap pass's shadows and highlights, a block of its own because EnhancementsTonemap keeps its size: before the
// curve, each pixel's stops move with its luminance, the shadows' fully at 4 stops below middle gray (0.18) and the
// highlights' fully at 4 above, fading to nothing at middle gray, so the midtones keep their exposure.
typedef struct EnhancementsTones
{
    float shadows;              // 1 brightens the shadows by 2 stops, -1 darkens them by 2; default 0, clamped to -1..1
    float highlights;           // the same for the highlights; default 0, clamped to -1..1
} EnhancementsTones;

// Lens softness: every pixel spread a little, the in-focus ones too, as a real lens and sensor never draw an edge on one
// pixel. It runs at the depth of field's place, first in its chain, so the depth of field blurs the softened scene, and
// alone while the depth of field is off. With the depth of field on, apertures past f/8 add their diffraction to it.
// Version 20 added it.
typedef struct EnhancementsLensSoftness
{
    float radius;               // the spread, a Gaussian's sigma in pixels of a picture 2160 pixels tall, scaled to the render
                                // height; 0 (the default) is off; clamped to 0..4
} EnhancementsLensSoftness;

// The whole desired state. A later version appends one block per pass; size tells the add-on which fields the caller
// knows, and fields past it keep their defaults.
typedef struct EnhancementsSettings
{
    uint32_t size;      // sizeof(EnhancementsSettings) as the caller compiled it
    uint32_t enabled;   // the master switch: while 0 the add-on does nothing but one atomic check per event
    uint32_t view;      // ENHANCEMENTS_VIEW_*; unknown values read as off
    uint32_t flip;      // 1 (the default): game targets are stored upside down. Debug views and the ambient pass's
                        // view-space reconstruction both read it, so an upright debug view proves the passes right too
    EnhancementsAmbient ambient;
    EnhancementsContactShadows contact_shadows;
    EnhancementsAtmosphere atmosphere;
    EnhancementsTonemap tonemap;
    EnhancementsSunShadows sun_shadows;
    EnhancementsWetness wetness;
    EnhancementsLightShafts light_shafts;
    EnhancementsTrace trace;
    EnhancementsGather gather;
    EnhancementsDepthOfField depth_of_field;
    EnhancementsLensFlare lens_flare;
    EnhancementsAmbientColor ambient_color;
    EnhancementsTones tones;
    EnhancementsLensSoftness lens_softness;
} EnhancementsSettings;

// Lens-sampled depth of field: over several frames, the caller moves the camera to points of a virtual aperture and
// shears its projection so the focus plane stays put, and marks one frame of each point in the camera's lens_sample.
// The accumulator adds the HDR scene of each marked frame into a 32-bit float sum at the bloom step, after our passes
// and before the game's bloom and tone map, and then writes the sum's average over the HDR scene while the caller takes
// its screenshot, so bloom and the tone map run on the blurred image.
// Separate from EnhancementsSettings so a screenshot can accumulate without knowing the passes another caller set.
typedef struct EnhancementsAccumulate
{
    uint32_t size;              // sizeof(EnhancementsAccumulate) as the caller compiled it
    uint32_t mode;              // ENHANCEMENTS_ACCUMULATE_*; unknown values read as off
    uint32_t generation;        // the sum belongs to one generation: a new value, or a new render size, empties it at the
                                // next bloom step
    float cat_eye;              // optical vignetting: at each pixel, only lens points within 1 of a center that moves out
                                // to cat_eye at the picture's corners count, so bokeh turns to cat's eyes toward the edges
                                // and swirls; 0 (the default) is round everywhere, 0.3 to 0.6 is Helios-like; clamped to 0..1
    float cat_eye_falloff;      // the width of the cut's soft edge in aperture radii; default 0.1, clamped to 0.01..1
} EnhancementsAccumulate;

// A traced shot: while active, every frame runs the trace pass with the gather on over whatever settings are applied,
// also while their master switch is off, when nothing else of them runs. Separate from EnhancementsSettings for the reason
// EnhancementsAccumulate is: a screenshot must not overwrite what another caller applied.
typedef struct EnhancementsShot
{
    uint32_t size;              // sizeof(EnhancementsShot) as the caller compiled it
    uint32_t active;            // default 0
    uint32_t samples;           // gather.samples for the shot; 0 keeps the applied one; clamped to 1..4096
    float budget_ms;            // trace.budget_ms for the shot; 0 keeps the applied one; clamped to 2..200
} EnhancementsShot;

// The directions a scene capture turns the camera to: world-axis cube faces in D3D cube-map order, each with its own up
// (+X, -X, +Z and -Z keep +Y up; +Y has -Z up and -Y has +Z up), at a 90-degree vertical field of view, and the view as
// the camera already is. Values of EnhancementsStatus.scene_face, and bits 1 << face of EnhancementsSceneRequest.faces.
enum
{
    ENHANCEMENTS_FACE_POS_X = 0,
    ENHANCEMENTS_FACE_NEG_X,
    ENHANCEMENTS_FACE_POS_Y,
    ENHANCEMENTS_FACE_NEG_Y,
    ENHANCEMENTS_FACE_POS_Z,
    ENHANCEMENTS_FACE_NEG_Z,
    ENHANCEMENTS_FACE_VIEW,
    ENHANCEMENTS_FACE_COUNT,
};
#define ENHANCEMENTS_FACE_NONE 0xFFFFFFFFu

// What a scene capture writes and keeps besides each face's OBJ, as bits of EnhancementsSceneRequest.flags.
enum
{
    ENHANCEMENTS_SCENE_RAW = 1u << 0,          // also face-N.so: the stream output's clip-space positions as captured
    ENHANCEMENTS_SCENE_PLY = 1u << 1,          // a binary PLY per face instead of the OBJ
    ENHANCEMENTS_SCENE_TRACE = 1u << 2,        // keep the faces' triangles, deduplicate them, build their BVH and upload it for the
                                            // trace pass, and write them as trace-scene.bin. With faces 0, capture nothing and
                                            // trace the 3-triangle self-test scene around the camera instead
    ENHANCEMENTS_SCENE_NO_MESHES = 1u << 3,    // write no OBJ or PLY; the TSVs, pictures and depth still
    ENHANCEMENTS_SCENE_GBUFFER = 1u << 4,      // the view face also writes its normals and stencil, and the game's shadow mask and
                                            // diffuse irradiance as the combine step found them, before any pass of ours
    ENHANCEMENTS_SCENE_RELEASE = 1u << 5,      // capture nothing, stop a scene capture that runs and free the traced scene; faces and
                                            // the other flags are ignored
    ENHANCEMENTS_SCENE_LIGHT = 1u << 6,        // with TRACE: each face also copies its G-buffer albedo, stencil and HDR scene, and the
                                            // traced scene gets its triangles' albedo, a sky map, the game's ambient light and
                                            // sun, for EnhancementsGather; with GBUFFER, each face writes those copies too
};

// A scene capture: the geometry of every G-buffer draw, replayed with stream output, in world space, from each face.
typedef struct EnhancementsSceneRequest
{
    uint32_t size;              // sizeof(EnhancementsSceneRequest) as the caller compiled it
    uint32_t faces;             // bits 1 << ENHANCEMENTS_FACE_*; 0 reads as all seven (0x7F)
    uint32_t settle;            // presents each turned face is held before its frame is captured; 0 reads as 45, clamped
                                // to 2..600. The view face waits 2.
    uint32_t flags;             // ENHANCEMENTS_SCENE_*
    uint32_t max_triangles;     // per face; whole draws past it are left out of the file and counted; 0 reads as 4000000
} EnhancementsSceneRequest;

#define ENHANCEMENTS_COMPARE_MAX 16

// One settings variant of a comparison capture.
typedef struct EnhancementsVariant
{
    char name[32];                  // UTF-8, ended by a NUL or by the array; names the variant's PNG. Bytes other than
                                    // A-Z, a-z, 0-9, '.', '_' and '-' become '-'.
    EnhancementsSettings settings;     // size as for EnhancementsApply
} EnhancementsVariant;

// Unity's main camera for the frame being drawn, as Unity lays out Matrix4x4 in memory (column-major).
// view_to_clip is GL.GetGPUProjectionMatrix(projectionMatrix, false): D3D clip space with reversed Z, not flipped,
// because EnhancementsSettings.flip owns the vertical orientation.
typedef struct EnhancementsCamera
{
    float world_to_view[16];
    float view_to_clip[16];
    float sun_direction[4];     // xyz: the direction toward the sun in world space, such as minus the sun light's
                                // forward; any length. Zero or non-finite means no sun: contact shadows add nothing.
                                // w is unused.
    float sun_color[4];         // rgb: the sun light's color in linear RGB times its intensity; w is unused
    float sky_color[4];         // rgb: the sky's ambient light in linear RGB; w: the weather's wetness, 0 dry to 1 soaked,
                                // clamped to 0..1, which the wetness pass follows while EnhancementsWetness.wetness is 0
    float lens_sample[4];       // xy: the point in the unit aperture disk the camera was moved to, x right and y up, clamped
                                // to -1..1; z: the sample's index, a whole number; w: 1 on the one frame of the sample the
                                // accumulator adds, else 0. All zero when no depth of field is being sampled.
    float sky_sun[4];           // xyz: the direction toward the sun disc the sky draws, which Genshin places apart from the sun
                                // light that casts the shadows; any length, and zero or non-finite means the sky draws it along
                                // sun_direction. Only the lens flare follows it. w is unused. Version 17 added it, so a binding
                                // of an earlier version must not call EnhancementsSetCamera.
} EnhancementsCamera;

// Where the DNG EnhancementsDevelopDng last accepted is, as EnhancementsStatus.dng_state. Version 18 added it.
enum
{
    ENHANCEMENTS_DNG_NONE = 0,          // none asked for since the add-on loaded
    ENHANCEMENTS_DNG_DEVELOPING,        // waiting for the next tone map draw, which develops it, and for its read back
    ENHANCEMENTS_DNG_READY,             // its image is held for EnhancementsWriteDng
    ENHANCEMENTS_DNG_WRITING,           // the add-on's worker thread writes the file
    ENHANCEMENTS_DNG_WRITTEN,
    ENHANCEMENTS_DNG_FAILED,            // dng_failure says why
};

// Why that DNG failed, as EnhancementsStatus.dng_failure. Version 18 added it.
enum
{
    ENHANCEMENTS_DNG_FAILED_NONE = 0,
    ENHANCEMENTS_DNG_FAILED_NO_TONEMAP,     // no tone map draw developed it within 300 presents
    ENHANCEMENTS_DNG_FAILED_DEVELOP,        // the tone map draw had no HDR scene, or a texture of ours failed
    ENHANCEMENTS_DNG_FAILED_READBACK,       // its image could not be read back from the GPU
    ENHANCEMENTS_DNG_FAILED_WRITE,          // the file could not be written; ReShade.log names it
    ENHANCEMENTS_DNG_FAILED_DROPPED,        // no write asked for it within 1800 presents of it being ready, so its image was freed
};

typedef struct EnhancementsStatus
{
    uint32_t size;              // set by the caller; the add-on writes at most this many bytes
    uint32_t armed;             // 1 while the add-on follows the game's frame: settings.enabled, an accumulation or a depth
                                // read reached the render thread, and no error disarmed it
    uint64_t frames;            // presents since the add-on loaded
    uint64_t settings_applied;  // the generation EnhancementsApply returned for the settings the last frame used
    uint32_t render_width;      // main depth size of the last frame, 0 when no G-buffer was found
    uint32_t render_height;
    uint32_t steps_found;       // ENHANCEMENTS_STEP_* matched last frame; a step whose passes ran at a later step's moment
                                // because it did not match is not in it
    uint32_t steps_wanted;      // steps the enabled passes and the view need
    uint32_t passes_ran;        // ENHANCEMENTS_PASS_*
    uint32_t passes_skipped;    // wanted, but an entry, the camera or a texture of ours was missing at their step
    uint32_t entries_missing;   // ENHANCEMENTS_ENTRY_* a wanted pass or the view needed and did not get
    uint32_t view_shown;        // ENHANCEMENTS_VIEW_* composited for the last frame, 0 when none or its entry was missing
    uint32_t camera_age;        // frames since the last EnhancementsSetCamera, UINT32_MAX when never
    uint32_t restarts;          // G-buffer binds that restarted the last frame (another camera drew first)
    uint32_t error;             // ENHANCEMENTS_ERROR_*
    uint32_t compare_remaining; // variants of the comparison capture not yet saved, 0 when none runs
    uint32_t accum_mode;        // ENHANCEMENTS_ACCUMULATE_* the last frame ran
    uint32_t accum_samples;     // frames added into the sum since it was last emptied
    uint32_t accum_generation;  // the generation the sum belongs to, 0 before the first bloom step with an accumulation
    uint32_t scene_face;        // ENHANCEMENTS_FACE_* a scene capture wants the camera to show now; ENHANCEMENTS_FACE_NONE when none
    uint32_t scene_remaining;   // faces of the scene capture not yet written, 0 when none runs
    uint32_t scene_support;     // 0 untried, 1 stream output wrote triangles, 2 CreateGeometryShaderWithStreamOutput failed with
                                // DXGI_ERROR_INVALID_CALL, which DXVK returns without transform feedback
    uint32_t trace_state;       // ENHANCEMENTS_TRACE_STATE_* of the traced scene
    uint32_t trace_triangles;   // triangles in its BVH, 0 while there is none
    uint32_t trace_samples;     // full passes since the samples last started: samples every traced pixel has
    uint32_t trace_mrays_x100;  // the last measured rays a second, in millions times 100
    uint32_t trace_generation;  // the scene's, one more for each scene uploaded since the add-on loaded
    uint32_t trace_target;      // the samples every traced pixel gets: with the gather, the larger of its samples and the sun's
    uint32_t trace_converged;   // 1 when every traced pixel has trace_target samples and the last frame was written from them
    uint32_t trace_error_x10000;    // the gathered light's mean relative standard error at the last full pass, times 10000
    uint32_t trace_light;       // ENHANCEMENTS_TRACE_LIGHT_* of the traced scene
    uint32_t trace_restarts;    // times the trace's samples have started since the add-on's device was made, each scene's first
                                // start among them: while it rises and trace_samples stays 0, no sample accumulates
    uint32_t trace_restart_reason;  // ENHANCEMENTS_TRACE_RESTART_* of the last of them
    uint32_t shot_active;       // 1 when the last frame ran with an EnhancementsSetShot shot's settings, so trace_converged is that
                                // shot's
    uint32_t frames_saved;      // frames EnhancementsSaveFrame has saved since the add-on loaded; the status line names the last folder
    float dof_focus_meters;     // the depth of field's focus distance, read back a few frames late; 0 before the pass ran
    float flare_visibility;     // the lens flare's source's visibility, 0 to 1, read back a few frames late; 0 before the pass ran
    uint32_t dng_id;            // the id of the DNG EnhancementsDevelopDng last accepted, 0 before the first. Version 18 added
                                // the three dng fields, the first in the status's 4 bytes of padding, which makes it 168 bytes
    uint32_t dng_state;         // ENHANCEMENTS_DNG_* of that DNG
    uint32_t dng_failure;       // ENHANCEMENTS_DNG_FAILED_* while dng_state is FAILED, else 0
} EnhancementsStatus;

ENHANCEMENTS_EXPORT uint32_t EnhancementsVersion(void);

// Copies the settings and returns their generation, which EnhancementsStatus.settings_applied reaches once a frame has used
// them. Applying the same settings twice ends in the same state. A null pointer, or a size too small to hold enabled,
// changes nothing and returns the current generation.
ENHANCEMENTS_EXPORT uint64_t EnhancementsApply(const EnhancementsSettings *settings);

ENHANCEMENTS_EXPORT void EnhancementsSetCamera(const EnhancementsCamera *camera);

// Starts a comparison capture: from the next present, the render thread runs each variant's settings in turn, lets 4
// presents settle, copies the back buffer of the next 2 before ReShade's effects, saves the second as a PNG with the
// flicker between the two, and then restores the settings from before the capture. Files go to
// <ReShade base path>\Enhancements\compare-<YYYYMMDD-HHMMSS>\. Returns 1 when accepted, 0 when a capture is still running or
// saving, or the variants are invalid: null, none, more than ENHANCEMENTS_COMPARE_MAX, an empty name, or settings too short
// to hold enabled.
ENHANCEMENTS_EXPORT uint32_t EnhancementsCompare(const EnhancementsVariant *variants, uint32_t count);

// Starts a scene capture from the next present: for each requested face, the view first, EnhancementsStatus.scene_face asks
// the caller to turn the camera there, and once a frame whose own camera shows the face has rendered, every draw of its
// G-buffer is replayed with stream output, and its triangles are written in world space with the frame's picture and
// depth to <ReShade base path>\Enhancements\scene-<YYYYMMDD-HHMMSS>\. Returns 1 when accepted, and 0 while a scene capture
// is still running or saving, or when request is null, too short to hold faces, or names no known face. It works whether
// settings.enabled is on or not. An ENHANCEMENTS_SCENE_RELEASE request is always accepted: at the next present it stops a
// capture asked for before it, whose faces written so far stay, and frees the traced scene.
ENHANCEMENTS_EXPORT uint32_t EnhancementsCaptureScene(const EnhancementsSceneRequest *request);

ENHANCEMENTS_EXPORT void EnhancementsGetStatus(EnhancementsStatus *status);

// Saves the next frame the trace pass writes into <ReShade base path>\Enhancements\trace-<YYYYMMDD-HHMMSS>\: its history,
// depth, stencil and normals, the game's shadow mask and diffuse irradiance as the pass found them, the constants it
// traced with, the shadow-mask constants and trace.tsv, which names the scene folder holding trace-scene.bin, for the
// Mac's reference tracer; the view's G-buffer albedo, its HDR scene before our passes and the irradiance the pass wrote;
// and while it gathers, the gather's sums and constants. The constants are the last ones the trace uploaded, and trace.tsv says whether the scene was
// ready or stale, and why. While the scene is stale the save takes the next frame's images and the history as it was
// when it went stale. Returns 1 when accepted, and 0 while the scene is neither ready nor stale, before a ready scene's
// first full pass, or while a save waits; a save that sees no traced frame within 300 presents is given up, which
// ReShade.log says.
ENHANCEMENTS_EXPORT uint32_t EnhancementsSaveTrace(void);

// Saves the HDR scene and the main depth of the next frame's bloom step, before any pass of ours, with the camera, into
// <ReShade base path>\Enhancements\frame-<YYYYMMDD-HHMMSS>\: hdr.r11g11b10 and depth.r32 as stored, and frame.tsv, for the
// Mac's depth of field harness. It works whether settings.enabled is on or not. Returns 1 when accepted, 0 while a save
// waits; a save that sees no bloom step within 300 presents is given up, which ReShade.log says.
ENHANCEMENTS_EXPORT uint32_t EnhancementsSaveFrame(void);

// Develops a DNG of the next frame, beside a screenshot of it: at the game's tone map draw, the linear light before the tone
// curve, as the tonemap pass draws it. The source is the HDR scene, or while the accumulator presents, its sum's own average
// at full precision; the depth of field that frame defers to the back buffer runs on it in HDR with the frame's settings and
// focus; then the game's final bloom times its bloom intensity times tonemap.bloom_strength, the game's color matrix, and
// the game's exposure times 2^tonemap.exposure_ev, both 1 while the tonemap pass does not run that frame; and the lens flare
// that frame draws, added. Nothing the game, ReShade or the depth of field's focus see changes. The image is read back
// without stalling and held for EnhancementsWriteDng; EnhancementsStatus.dng_id and dng_state follow from the next present.
// Returns 1 when accepted, which drops a held image of an earlier id, and 0 for id 0 or while another is developing or
// being written. It works whether settings.enabled is on or not. Version 18 added it.
ENHANCEMENTS_EXPORT uint32_t EnhancementsDevelopDng(uint32_t id);

// The camera a DNG says it was taken with, in its EXIF. A value of 0, or a date_time that is empty, leaves its tag out.
// Version 19 added it.
typedef struct EnhancementsDngInfo
{
    uint32_t size;              // set by the caller to sizeof(EnhancementsDngInfo)
    float focal_length_mm;      // full-frame focal length; FocalLength and FocalLengthIn35mmFilm
    float f_number;             // the depth of field's, for a DNG it blurs; FNumber
    float subject_distance_m;   // the focus distance; SubjectDistance
    char date_time[20];         // DateTimeOriginal, "YYYY:MM:DD HH:MM:SS" with its NUL
} EnhancementsDngInfo;

// Writes the DNG of id (core/dng.h says what the file holds) to path, UTF-8, on the add-on's worker thread, replacing a file
// there, with info's camera in its EXIF; info may be null or too short, for no camera. dng_state goes to WRITING, then
// WRITTEN or FAILED. Returns 1 when accepted, and 0 unless dng_state is READY for id, or when path is null or empty.
// Version 18 added it, and version 19 info.
ENHANCEMENTS_EXPORT uint32_t EnhancementsWriteDng(uint32_t id, const char *path, const EnhancementsDngInfo *info);

// Copies the accumulation's state; it takes effect at the next frame boundary, like EnhancementsApply. A null pointer, or a
// size too small to hold mode, changes nothing. It works whether settings.enabled is on or not.
ENHANCEMENTS_EXPORT void EnhancementsSetAccumulate(const EnhancementsAccumulate *accumulate);

// Copies the shot's state; it takes effect at the next frame boundary, like EnhancementsApply, and EnhancementsStatus.shot_active
// follows a frame later. A null pointer, a size too small to hold active, or active 0 ends the shot, and the applied
// settings run as before. It works whether settings.enabled is on or not.
ENHANCEMENTS_EXPORT void EnhancementsSetShot(const EnhancementsShot *shot);

// Asks for the view-space depth in meters (the distance along the camera's forward axis) at a point on the screen, u and
// v from 0 at the top left to 1, and returns the newest answer read from a frame that began after the point was first
// asked for, or -1 while there is none, or for a point off the screen. The render thread copies the depth at the bloom
// step and reads it back without waiting, so an answer comes a few presents later: call it once a frame until it is not
// negative. Asking for another point, or not asking for 60 presents, forgets the answer.
ENHANCEMENTS_EXPORT float EnhancementsDepthAt(float u, float v);

// Writes the last frame's report as one UTF-8 line with its terminator, such as
// "1152x720 found gbuffer quarter-shadow shadow-mask ambient-pair combine bloom tonemap; ran ambient@combine; view normals"
// or "found nothing; skipped ambient: missing normals depth ambient-diffuse". The line holds no frame counter, so it
// changes only when the frame map or the passes change, and a caller can log it on change. Returns the length without
// the terminator, 0 when the buffer is too small.
ENHANCEMENTS_EXPORT uint32_t EnhancementsDescribe(char *text, uint32_t size);

#ifdef __cplusplus
}
#endif
