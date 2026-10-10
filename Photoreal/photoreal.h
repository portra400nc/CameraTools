// The C interface of CameraToolsPhotoreal.addon64, a ReShade add-on that relights Genshin's world inside its own frame.
// ReShade loads the add-on. A mod finds the loaded module by name and calls these exports.
//
// Every function may be called from any thread and returns at once. The add-on applies settings and the camera at the
// next frame boundary on ReShade's render thread, so a whole frame always sees one settings value.
#pragma once

#include <stdint.h>

#define PHOTOREAL_VERSION 5

// The native tests compile this header on macOS, where there is nothing to export.
#ifdef _WIN32
#define PHOTOREAL_EXPORT __declspec(dllexport)
#else
#define PHOTOREAL_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Debug views. Each shows one frame-map entry full screen, decoded for the eye.
enum
{
    PHOTOREAL_VIEW_OFF = 0,
    PHOTOREAL_VIEW_NORMALS,
    PHOTOREAL_VIEW_ALBEDO,
    PHOTOREAL_VIEW_SPECULAR,
    PHOTOREAL_VIEW_MATERIAL_ID,
    PHOTOREAL_VIEW_SMOOTHNESS,
    PHOTOREAL_VIEW_CHARACTER,
    PHOTOREAL_VIEW_DEPTH,
    PHOTOREAL_VIEW_STENCIL,
    PHOTOREAL_VIEW_SHADOW_MASK,
    PHOTOREAL_VIEW_QUARTER_SHADOW,
    PHOTOREAL_VIEW_AMBIENT_DIFFUSE,
    PHOTOREAL_VIEW_AMBIENT_SPECULAR,
    PHOTOREAL_VIEW_HDR_SCENE,
    PHOTOREAL_VIEW_BLOOM_FINAL,
    PHOTOREAL_VIEW_SUN_ATLAS,
    PHOTOREAL_VIEW_COUNT,
};

// Steps of the game's frame the add-on recognizes, as bits of PhotorealStatus.steps_*.
enum
{
    PHOTOREAL_STEP_GBUFFER = 1u << 0,
    PHOTOREAL_STEP_QUARTER_SHADOW = 1u << 1,
    PHOTOREAL_STEP_SHADOW_MASK = 1u << 2,
    PHOTOREAL_STEP_AMBIENT_PAIR = 1u << 3,
    PHOTOREAL_STEP_COMBINE = 1u << 4,
    PHOTOREAL_STEP_BLOOM = 1u << 5,
    PHOTOREAL_STEP_TONEMAP = 1u << 6,
    PHOTOREAL_STEP_FORWARD = 1u << 7,   // the first forward draw into the HDR scene, after the deferred combine draws
    PHOTOREAL_STEP_GBUFFER_DONE = 1u << 8,  // the first draw that samples the G-buffer's smoothness, after its last write
};

// Frame-map entries, as bits of PhotorealStatus.entries_missing.
enum
{
    PHOTOREAL_ENTRY_NORMALS = 1u << 0,
    PHOTOREAL_ENTRY_ALBEDO = 1u << 1,
    PHOTOREAL_ENTRY_SPECULAR = 1u << 2,
    PHOTOREAL_ENTRY_MATERIAL_ID = 1u << 3,
    PHOTOREAL_ENTRY_SMOOTHNESS = 1u << 4,
    PHOTOREAL_ENTRY_CHARACTER = 1u << 5,
    PHOTOREAL_ENTRY_DEPTH = 1u << 6,
    PHOTOREAL_ENTRY_QUARTER_SHADOW = 1u << 7,
    PHOTOREAL_ENTRY_SHADOW_MASK = 1u << 8,
    PHOTOREAL_ENTRY_AMBIENT_SPECULAR = 1u << 9,
    PHOTOREAL_ENTRY_AMBIENT_DIFFUSE = 1u << 10,
    PHOTOREAL_ENTRY_HDR_SCENE = 1u << 11,
    PHOTOREAL_ENTRY_BLOOM = 1u << 12,
    PHOTOREAL_ENTRY_TONEMAP_OUT = 1u << 13,
    PHOTOREAL_ENTRY_BLOOM_FINAL = 1u << 14,
    PHOTOREAL_ENTRY_SUN_ATLAS = 1u << 15,
};

// Our passes, as bits of PhotorealStatus.passes_*.
enum
{
    PHOTOREAL_PASS_AMBIENT = 1u << 0,
    PHOTOREAL_PASS_CONTACT_SHADOWS = 1u << 1,
    PHOTOREAL_PASS_ATMOSPHERE = 1u << 2,
    PHOTOREAL_PASS_TONEMAP = 1u << 3,
    PHOTOREAL_PASS_SUN_SHADOWS = 1u << 4,
    PHOTOREAL_PASS_LEAVES = 1u << 5,
};

// The tonemap pass's curves, values of PhotorealTonemap.curve.
enum
{
    PHOTOREAL_CURVE_GAME = 0,   // the game's filmic x(1.36x + 0.047) / (x(0.93x + 0.56) + 0.14)
    PHOTOREAL_CURVE_AGX,        // AgX, after Benjamin Wrensch's minimal fit
    PHOTOREAL_CURVE_NEUTRAL,    // Khronos PBR Neutral
    PHOTOREAL_CURVE_COUNT,
};

// What the accumulator does with the HDR scene at the bloom step, values of PhotorealAccumulate.mode.
enum
{
    PHOTOREAL_ACCUMULATE_OFF = 0,   // nothing; the sum is kept
    PHOTOREAL_ACCUMULATE_ADD,       // adds each marked frame's HDR scene into the sum
    PHOTOREAL_ACCUMULATE_PRESENT,   // writes the sum's average over the HDR scene every frame
    PHOTOREAL_ACCUMULATE_COUNT,
};

enum
{
    PHOTOREAL_ERROR_NONE = 0,
    PHOTOREAL_ERROR_NOT_D3D11,  // the game's device is not D3D11; the add-on never arms
    PHOTOREAL_ERROR_SHADER,     // a shader or state object of ours failed to create; the add-on never arms
    PHOTOREAL_ERROR_TEXTURE,    // a texture of ours failed to create last frame; the pass or view that needed it skipped
    PHOTOREAL_ERROR_STATE,      // the game's render target, depth or pixel shader differed after we restored state;
                                // the add-on disarmed until it reloads, and ReShade.log says which binding moved
};

// The ambient pass replaces the game's diffuse irradiance on world pixels (stencil & 0x84 == 0x80) before the combine
// pass reads it. Characters and the sky keep the game's value. contact_shadows follows it in PhotorealSettings, so this
// block keeps its size.
typedef struct PhotorealAmbient
{
    uint32_t enabled;           // default 0
    float level;                // multiplies the game's irradiance; default 0.6, clamped to 0..4
    float ao_strength;          // 0 no ambient occlusion, 1 full; default 0.5
    float ao_radius;            // in world units (meters), default 1, clamped to 0.05..10
    float foliage_ao_strength;  // multiplies ao_strength on grass, vegetation and foliage (stencil 129, 136, 137),
                                // whose alpha-tested depth makes noisy occlusion; default 0.5, clamped to 0..1
} PhotorealAmbient;

// Screen-space contact shadows: each world pixel looks toward the sun through the depth buffer for length meters, and a
// surface in the way lowers the sun's visibility in the game's shadow mask before the combine pass reads it. They add
// the small shadows the game's shadow map is too coarse for, such as a character's feet on the ground. Never brightens.
// Grass (stencil 129) never casts: its blades would shadow each other and the ground between them.
// atmosphere follows it in PhotorealSettings, so this block keeps its size.
typedef struct PhotorealContactShadows
{
    uint32_t enabled;           // default 0
    float length;               // how far each pixel looks toward the sun, in meters; default 0.6, clamped to 0.05..5
    float strength;             // 0 no contact shadows, 1 full; default 1, clamped to 0..1
    float thickness;            // how deep a surface in the depth buffer is taken to be, in meters, so a ray passing
                                // behind a thin pole is not shadowed; default 0.25, clamped to 0.01..2
    float foliage_strength;     // multiplies strength on grass, vegetation and foliage (stencil 129, 136, 137), whose
                                // dense alpha-tested blades shadow each other into speckle; default 0, clamped to 0..1
} PhotorealContactShadows;

// Aerial perspective: haze between the camera and each pixel dims the HDR scene and scatters sky and sun light into the
// view, before the game's bloom and tone map read it. The haze thins with height above the camera, and the sun's part
// gathers around the sun. The sky (depth 0) keeps the game's color. Light comes from PhotorealCamera's sun and sky colors.
// tonemap follows it in PhotorealSettings, so this block keeps its size.
typedef struct PhotorealAtmosphere
{
    uint32_t enabled;           // default 0
    float density;              // extinction per meter at the camera's height; default 0.000325, which hazes a pixel 500 m
                                // away level with the camera by 15%; clamped to 0..0.01
    float height_falloff;       // per meter: the haze thins by e every 1 / height_falloff meters above the camera and
                                // thickens as fast below it; default 0.02, clamped to 0..1
    float sun_scatter;          // multiplies the sun light scattered toward the camera; default 1, clamped to 0..10
    float anisotropy;           // Henyey-Greenstein g: 0 scatters sun light evenly, toward 1 into a glow around the sun;
                                // default 0.7, clamped to -0.95..0.95
} PhotorealAtmosphere;

// Our tone map in place of the game's: the add-on skips the game's tone map draw and draws into its output instead,
// from the HDR scene, the game's final bloom, and the game's exposure, bloom intensity, color matrix and display gamma,
// read each frame from the skipped draw's constants. With the defaults it reproduces the game's image.
// sun_shadows follows it in PhotorealSettings, so this block keeps its size.
typedef struct PhotorealTonemap
{
    uint32_t enabled;           // default 0
    float exposure_ev;          // stops added to the game's exposure; default 0, clamped to -10..10
    uint32_t curve;             // PHOTOREAL_CURVE_*; unknown values read as the game's, the default
    float bloom_strength;       // multiplies the game's bloom intensity; default 1, clamped to 0..4
    float saturation;           // 0 gray, 1 unchanged; default 1, clamped to 0..2
    float contrast;             // a power around middle gray (0.18) before the curve; default 1, clamped to 0.5..2
} PhotorealTonemap;

// Soft sun shadows from the game's own shadow atlas and cascades: on world pixels, the sun's visibility in the shadow
// mask is drawn again with a penumbra that widens with the distance between the shadow and what casts it, and replaces
// the game's, before the combine pass reads it. Characters, the sky, and pixels past the game's cascades or shadow
// distance keep the game's value.
// leaves follows it in PhotorealSettings, so this block keeps its size.
typedef struct PhotorealSunShadows
{
    uint32_t enabled;           // default 0
    float light_size;           // the penumbra's width per meter between caster and shadow: 0.0093 is the real sun's; default
                                // 0.03, so a branch 10 m up casts a shadow with a 0.3 m soft edge; clamped to 0..0.2
    float min_penumbra;         // the soft edge's least width in meters, at the caster; default 0.02, clamped to 0..0.5
    float strength;             // 0 no sun shadow, 1 full; default 1, clamped to 0..1
} PhotorealSunShadows;

// Light through leaves: on leaf and grass pixels (stencil 129, 136 or 137 with material id 2, 15 or 3), the sun light a
// leaf passes on toward the camera is added to the HDR scene after the game's deferred lighting and before its sky,
// transparent and fog draws. A leaf glows where the camera looks toward the sun through its back, and leaves lit from the
// front are unchanged. The light is the sun's color and direction from PhotorealCamera, times the leaf's albedo and the
// sun's visibility in the shadow mask.
// wetness follows it in PhotorealSettings, so this block keeps its size.
typedef struct PhotorealLeaves
{
    uint32_t enabled;           // default 0
    float strength;             // multiplies the light let through; default 0.6, clamped to 0..4
    float scatter_sharpness;    // the power of the glow's falloff away from the sun: 1 is broad, higher gathers it closer
                                // around the sun; default 4, clamped to 1..32
} PhotorealLeaves;

// Wet surfaces: on world pixels, after the game's last G-buffer write and before anything reads it, the G-buffer's albedo
// darkens, its smoothness rises and its specular color moves toward water's, more on surfaces that face up, and puddles
// form on flat ground. The game's reflections, sun light and ambient light then all see the wet surface.
// The last block of PhotorealSettings, so a field added here goes at the end and size keeps older callers working.
typedef struct PhotorealWetness
{
    uint32_t enabled;           // default 0
    float wetness;              // 0 (the default) follows the weather, which PhotorealCamera.sky_color[3] carries; above 0
                                // it replaces the weather's: 1 is soaked; clamped to 0..1
    float darkening;            // how much a soaked surface's albedo darkens; default 0.35, clamped to 0..1
    float puddles;              // the share of flat ground puddles cover when soaked; default 0.3, clamped to 0..1
} PhotorealWetness;

// The whole desired state. A later version appends one block per pass; size tells the add-on which fields the caller
// knows, and fields past it keep their defaults.
typedef struct PhotorealSettings
{
    uint32_t size;      // sizeof(PhotorealSettings) as the caller compiled it
    uint32_t enabled;   // the master switch: while 0 the add-on does nothing but one atomic check per event
    uint32_t view;      // PHOTOREAL_VIEW_*; unknown values read as off
    uint32_t flip;      // 1 (the default): game targets are stored upside down. Debug views and the ambient pass's
                        // view-space reconstruction both read it, so an upright debug view proves the passes right too
    PhotorealAmbient ambient;
    PhotorealContactShadows contact_shadows;
    PhotorealAtmosphere atmosphere;
    PhotorealTonemap tonemap;
    PhotorealSunShadows sun_shadows;
    PhotorealLeaves leaves;
    PhotorealWetness wetness;
} PhotorealSettings;

// Lens-sampled depth of field: over several frames, the caller moves the camera to points of a virtual aperture and
// shears its projection so the focus plane stays put, and marks one frame of each point in the camera's lens_sample.
// The accumulator adds the HDR scene of each marked frame into a 32-bit float sum at the bloom step, after our passes
// and before the game's bloom and tone map, and then writes the sum's average over the HDR scene while the caller takes
// its screenshot, so bloom and the tone map run on the blurred image.
// Separate from PhotorealSettings so a screenshot can accumulate without knowing the passes another caller set.
typedef struct PhotorealAccumulate
{
    uint32_t size;              // sizeof(PhotorealAccumulate) as the caller compiled it
    uint32_t mode;              // PHOTOREAL_ACCUMULATE_*; unknown values read as off
    uint32_t generation;        // the sum belongs to one generation: a new value, or a new render size, empties it at the
                                // next bloom step
    float cat_eye;              // optical vignetting: at each pixel, only lens points within 1 of a center that moves out
                                // to cat_eye at the picture's corners count, so bokeh turns to cat's eyes toward the edges
                                // and swirls; 0 (the default) is round everywhere, 0.3 to 0.6 is Helios-like; clamped to 0..1
    float cat_eye_falloff;      // the width of the cut's soft edge in aperture radii; default 0.1, clamped to 0.01..1
} PhotorealAccumulate;

#define PHOTOREAL_COMPARE_MAX 16

// One settings variant of a comparison capture.
typedef struct PhotorealVariant
{
    char name[32];                  // UTF-8, ended by a NUL or by the array; names the variant's PNG. Bytes other than
                                    // A-Z, a-z, 0-9, '.', '_' and '-' become '-'.
    PhotorealSettings settings;     // size as for PhotorealApply
} PhotorealVariant;

// Unity's main camera for the frame being drawn, as Unity lays out Matrix4x4 in memory (column-major).
// view_to_clip is GL.GetGPUProjectionMatrix(projectionMatrix, false): D3D clip space with reversed Z, not flipped,
// because PhotorealSettings.flip owns the vertical orientation.
typedef struct PhotorealCamera
{
    float world_to_view[16];
    float view_to_clip[16];
    float sun_direction[4];     // xyz: the direction toward the sun in world space, such as minus the sun light's
                                // forward; any length. Zero or non-finite means no sun: contact shadows add nothing.
                                // w is unused.
    float sun_color[4];         // rgb: the sun light's color in linear RGB times its intensity; w is unused
    float sky_color[4];         // rgb: the sky's ambient light in linear RGB; w is unused
    float lens_sample[4];       // xy: the point in the unit aperture disk the camera was moved to, x right and y up, clamped
                                // to -1..1; z: the sample's index, a whole number; w: 1 on the one frame of the sample the
                                // accumulator adds, else 0. All zero when no depth of field is being sampled.
} PhotorealCamera;

typedef struct PhotorealStatus
{
    uint32_t size;              // set by the caller; the add-on writes at most this many bytes
    uint32_t armed;             // 1 while the add-on follows the game's frame: settings.enabled, an accumulation or a depth
                                // read reached the render thread, and no error disarmed it
    uint64_t frames;            // presents since the add-on loaded
    uint64_t settings_applied;  // the generation PhotorealApply returned for the settings the last frame used
    uint32_t render_width;      // main depth size of the last frame, 0 when no G-buffer was found
    uint32_t render_height;
    uint32_t steps_found;       // PHOTOREAL_STEP_* matched last frame
    uint32_t steps_wanted;      // steps the enabled passes and the view need
    uint32_t passes_ran;        // PHOTOREAL_PASS_*
    uint32_t passes_skipped;    // wanted, but an entry, the camera or a texture of ours was missing at their step
    uint32_t entries_missing;   // PHOTOREAL_ENTRY_* a wanted pass or the view needed and did not get
    uint32_t view_shown;        // PHOTOREAL_VIEW_* composited for the last frame, 0 when none or its entry was missing
    uint32_t camera_age;        // frames since the last PhotorealSetCamera, UINT32_MAX when never
    uint32_t restarts;          // G-buffer binds that restarted the last frame (another camera drew first)
    uint32_t error;             // PHOTOREAL_ERROR_*
    uint32_t compare_remaining; // variants of the comparison capture not yet saved, 0 when none runs
    uint32_t accum_mode;        // PHOTOREAL_ACCUMULATE_* the last frame ran
    uint32_t accum_samples;     // frames added into the sum since it was last emptied
    uint32_t accum_generation;  // the generation the sum belongs to, 0 before the first bloom step with an accumulation
} PhotorealStatus;

PHOTOREAL_EXPORT uint32_t PhotorealVersion(void);

// Copies the settings and returns their generation, which PhotorealStatus.settings_applied reaches once a frame has used
// them. Applying the same settings twice ends in the same state. A null pointer, or a size too small to hold enabled,
// changes nothing and returns the current generation.
PHOTOREAL_EXPORT uint64_t PhotorealApply(const PhotorealSettings *settings);

PHOTOREAL_EXPORT void PhotorealSetCamera(const PhotorealCamera *camera);

// Starts a comparison capture: from the next present, the render thread runs each variant's settings in turn, lets 4
// presents settle, copies the back buffer of the next 2 before ReShade's effects, saves the second as a PNG with the
// flicker between the two, and then restores the settings from before the capture. Files go to
// <ReShade base path>\Photoreal\compare-<YYYYMMDD-HHMMSS>\. Returns 1 when accepted, 0 when a capture is still running or
// saving, or the variants are invalid: null, none, more than PHOTOREAL_COMPARE_MAX, an empty name, or settings too short
// to hold enabled.
PHOTOREAL_EXPORT uint32_t PhotorealCompare(const PhotorealVariant *variants, uint32_t count);

PHOTOREAL_EXPORT void PhotorealGetStatus(PhotorealStatus *status);

// Copies the accumulation's state; it takes effect at the next frame boundary, like PhotorealApply. A null pointer, or a
// size too small to hold mode, changes nothing. It works whether settings.enabled is on or not.
PHOTOREAL_EXPORT void PhotorealSetAccumulate(const PhotorealAccumulate *accumulate);

// Asks for the view-space depth in meters (the distance along the camera's forward axis) at a point on the screen, u and
// v from 0 at the top left to 1, and returns the newest answer read from a frame that began after the point was first
// asked for, or -1 while there is none, or for a point off the screen. The render thread copies the depth at the bloom
// step and reads it back without waiting, so an answer comes a few presents later: call it once a frame until it is not
// negative. Asking for another point, or not asking for 60 presents, forgets the answer.
PHOTOREAL_EXPORT float PhotorealDepthAt(float u, float v);

// Writes the last frame's report as one UTF-8 line with its terminator, such as
// "1152x720 found gbuffer quarter-shadow shadow-mask ambient-pair combine bloom tonemap; ran ambient@combine; view normals"
// or "found nothing; skipped ambient: missing normals depth ambient-diffuse". The line holds no frame counter, so it
// changes only when the frame map or the passes change, and a caller can log it on change. Returns the length without
// the terminator, 0 when the buffer is too small.
PHOTOREAL_EXPORT uint32_t PhotorealDescribe(char *text, uint32_t size);

#ifdef __cplusplus
}
#endif
