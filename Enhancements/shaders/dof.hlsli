// The depth of field's arithmetic and passes: each point's circle of confusion per color channel, the aperture's shape
// and the light across it, and the prefilter, tiles, gather, post-filter and composite over textures the includer reads.
// Shared by the dof_*.hlsl shaders and, compiled as C++ through core/hlsl.h, by core/dof.cpp, so the tests and the Mac's
// harness (tests/render_dof.cpp) run the very same passes. It keeps to what core/hlsl.h supports.
//
// Sizes: CoC radii are in half-size pixels, signed, positive behind the focus plane. Offsets on screen have y up.
//
// The pass functions read the Dof constants (dof.cbuffer.hlsli, core/dof.h's DofConstants), and a pass's block needs
// its includer to define, before the include:
//   DOF_FOCUS:      float dof_load_depth(int2 pixel)        the main depth, reversed, clamped to the render size
//                   float3 dof_sample_scene(float2 at)      the HDR scene bilinear at full-size pixels (centers at .5)
//   DOF_PREFILTER:  float3 dof_load_scene(int2 pixel), dof_load_depth
//   DOF_SCATTER:    float4 dof_load_prefilter(int2 pixel)  the prefilter's texel, clamped to its size
//   DOF_TILES:      float4 dof_load_source(int2 pixel)      the prefilter's level 0: rgb and the green CoC, clamped to its size,
//                   dof_load_focus
//   DOF_DILATE:     float2 dof_load_tile(int2 tile)         the tiles' far and near maxima, clamped to the tile count
//   DOF_LIFT:       dof_load_source, dof_sample_source
//   DOF_BEHIND:     dof_load_source
//   DOF_GATHER:     dof_load_source, float4 dof_sample_source(float2 at, float lod), at in half-size pixels (centers at
//                   .5), bilinear between the prefilter's levels; float4 dof_sample_lifted(float2 at, float lod), the same
//                   over dof_lift's levels (read only with the bokeh contrast on); float4 dof_sample_behind(float2 at, float lod),
//                   the same over dof_behind's levels; float2 dof_load_dilated(int2 tile)
//   DOF_POST:       float4 dof_load_far(int2 pixel), float4 dof_load_near(int2 pixel), float2 dof_load_cover(int2 pixel), the
//                   gather's, clamped;
//                   float4 dof_sample_far(float2 at, float lod), bilinear between the gather's far field's levels (read
//                   only with the bokeh contrast on); dof_load_dilated; dof_load_source
//   DOF_SPRITE:     dof_load_source
//   DOF_NEAR_MASK:  dof_load_source
//   DOF_NEAR:       dof_load_source, float4 dof_sample_near_mask(float2 at, float lod) (dof_near_mask's, bilinear between
//                   its levels), dof_load_dilated (the post-filter's, with the far
//                   sprites)
//   DOF_SOFTEN:     dof_load_scene
//   DOF_COMPOSITE:  dof_load_scene, dof_load_depth, dof_load_far, dof_load_near (the post-filter's, with the sprites),
//                   float4 dof_load_near_layer(int2 pixel), float2 dof_load_near_cover(int2 pixel) (dof_near_layer's)
//   and every pass but DOF_FOCUS: float2 dof_load_focus()   the focus distance in meters and the frame's mean luminance,
//                                                           which the autofocus draw wrote
// and LOOP and UNROLL for HLSL's [loop] and [unroll].
#include "cat_eye.hlsli"

static const float kDofPi = 3.14159265f;
// Axial chromatic aberration at chromatic 1: red's and blue's focal lengths are longer than green's by these shares, an
// achromat's secondary spectrum. A crown and flint doublet corrected at the C and F lines focuses them f / 2000 behind its
// shortest focus near 550 nm; the red channel's band lies near C, while the blue channel's reaches past F toward the g line,
// where the curve climbs to about f / 450, so blue's mean shift is over twice red's. The focus shift on the sensor's side
// grows with the focal length, and its blur with the aperture: about f d / (2 N) on the sensor whatever the focus distance.
// Red and blue focus farther than green, so green has the largest disc behind the focus plane and the smallest in front:
// bokeh fringes green behind focus and purple in front, as achromats' do.
static const float kDofChromaticRed = 0.0005f;
static const float kDofChromaticBlue = 0.0012f;
// The CoC radius in half-size pixels below which a point is in focus: half a full-size pixel.
static const float kDofInFocus = 0.25f;
// At a pixel in front of focus, the CoC in half-size pixels by which a source in front of focus must differ from the
// pixel's own to lie in front of its surface, occluding it, or behind it.
static const float kDofLayer = 1.0f;
// The share of a pixel's own raw CoC within which a source in front of focus lies on the pixel's own surface: a face's
// depth varies by a few percent, a shoulder in front of it by much more. A fixed band split a face far out of focus into
// thin contours of equal depth, each averaged on its own, which kept its detail: a sharp face under a translucent blur.
static const float kDofLayerShare = 0.15f;
// The share of a foreground pixel's own CoC under which a source in front of focus counts as hidden behind it.
static const float kDofHiddenShare = 0.5f;
// The full-size pixels of CoC past the first over which the composite turns from a foreground pixel's own color to the
// estimate of what it hides, under the near plane.
static const float kDofHiddenFade = 2.0f;
// The CoC in half-size pixels behind focus from which the post-filter starts averaging a pixel's far field with its
// neighbors', fully at twice it.
static const float kDofSmoothFrom = 2.0f;
// The share by which the mean CoC of a filtered read of the sources may differ from its source's own before the gather
// takes the source's texel instead: the read averaged in a surface at another depth, whose light would spread with the
// source's disc, as a subject in focus beside the background would spread over its blur as a halo around it.
static const float kDofMixed = 0.05f;
// How much swirl at 1 compresses a disc's radial axis at the picture's corners.
static const float kDofSwirl = 1.5f;
static const float3 kDofLuma = float3(0.2126f, 0.7152f, 0.0722f);
// The most a highlight's growth may add to its disc's light a half-size pixel, in thresholds a texel: a lamp of several
// texels lands where the game's tone map still shows its disc's rim and color, and overlapping discs keep their gaps
// instead of merging into a white mass. At most kDofHighlightMax over its disc, far below R16F's 65504.
static const float kDofHighlightDisc = 0.08f;
static const float kDofHighlightMax = 8192.0f;
// The contrast, the mean of a highlight's 3x3 texels over the brightest texel of the ring four away, from which it grows:
// a small lamp stands well above everything around it, while a glint of a sunlit sea, the inside of a large lamp and the
// edge of a bright sky have bright texels four away.
static const float kDofHighlightContrast = 1.25f;
// The least mean luminance the highlights' curve scales to. A night frame's mean falls with its dark sky and street while
// its lamps and lit walls stay near day levels; scaled to that mean, a lit white shirt would glow like the lantern.
static const float kDofHighlightKeyFloor = 0.25f;
// Scatter. A half-size source above the highlights' threshold and at least kDofSpriteRadius half-size pixels out of focus
// that is kDofSpritePeak times the mean of the ring two texels away, or has the contrast to grow, is a highlight: it
// leaves the gather and is drawn as a sprite of its disc. A flat bright area such as the sky and its edges stay in the
// gather, as do sources nearer focus, whose small discs the gather draws well enough.
static const float kDofSpritePeak = 2.0f;
static const float kDofSpriteRadius = 1.0f;
// The most one sprite adds to a channel of a pixel. A texel as bright as R11G11B10 holds, on a disc at the floor's radius,
// would add 1.27 times R16F's 65504; at this cap a dozen overlapping sprites still fit.
static const float kDofSpriteMostAdded = 4096.0f;

// Bokeh contrast. The game's HDR scene is shallow: a grass tip or a leaf's glint lies little above what is around it, so
// spread over its disc it drowns in it, where a real lens's would show. dof_lift lifts each source by how far it stands
// above the mean of the kDofContrastLevel's texels around it: its luminance over kDofContrastFrom times that mean, at least
// 1 and at most kDofContrastCap, to the power dof_contrast.x, faded in over its disc's radius from kDofContrastFade.x to .y
// half-size pixels so what is near focus stays as drawn. A thin bright thing stands out of its own surroundings, while a
// lit wall does not, so the wall keeps its soft blur and only its edge lifts a little. The far field gathers the lifted
// sources, and the post-filter takes back what the lift added over the dilated radius's kDofContrastSpan levels up. The
// lift may move at most the share dof_contrast.y of the light there: where it would add more, as everywhere on a sea of
// glints, the lift is cut to fit. So each place keeps its light, within a few percent where the texture is dense, and it
// gathers into the discs of what stands out.
static const float kDofContrastFrom = 1.15f;
static const float kDofContrastCap = 1.6f;
static const float2 kDofContrastFade = float2(2.0f, 3.0f);
static const float kDofContrastLevel = 3.0f;
static const float kDofContrastSpan = 2.0f;

// The lens at one focus distance: green's CoC at infinity in half-size pixels, and how much less red's and blue's CoC is
// than green's at every depth.
struct DofLens
{
    float scale;
    float3 shift;
    float max_radius;
};

// focal and sensor sizes in millimeters, focus in meters; pixels_per_mm is half-size pixels per millimeter of sensor.
// Through a thin lens of focal length f and aperture f / N, with the sensor v = f S / (S - f) behind it where green
// focuses at S, a channel of focal length f (1 + d) images a point at z at v_c = 1 / (1 / (f (1 + d)) - 1 / z), and its
// CoC radius is f / (2 N) (v / v_c - 1) = f / (2 N) (v / (f (1 + d)) - 1 - v / z): every channel's falls off with 1 / z
// alike, and a channel's is green's less v d / (2 N (1 + d)), which is scale S / f d / (1 + d).
DofLens dof_lens_at(float focal, float f_number, float focus, float pixels_per_mm, float chromatic, float max_radius)
{
    DofLens l;
    float s = focus * 1000.0f;
    l.scale = focal * focal / f_number * 0.5f * pixels_per_mm / max(s - focal, 0.001f);
    float2 d = float2(chromatic * kDofChromaticRed, chromatic * kDofChromaticBlue);
    float k = l.scale * s / focal;
    l.shift = float3(k * d.x / (1.0f + d.x), 0.0f, k * d.y / (1.0f + d.y));
    l.max_radius = max_radius;
    return l;
}

// The green CoC of a point at 1 / inverse_depth meters: (f^2 / (N (S - f))) (z - S) / z / 2 on the sensor.
float dof_coc(float inverse_depth, float focus, DofLens l) { return l.scale * (1.0f - focus * inverse_depth); }

// Each channel's CoC from green's. Not clamped.
float3 dof_channel_coc(float coc, DofLens l) { return float3(coc - l.shift.x, coc, coc - l.shift.z); }

// Each channel's CoC from green's, capped at the largest radius as one disc: past it all three scale by the widest
// channel's ratio to the cap, so a capped disc keeps its channels' proportions and its fringe. Clamping each channel on its
// own would give every channel past the cap the same disc, and most of a strongly blurred frame lies past it.
float3 dof_capped_coc(float coc, DofLens l)
{
    float3 c = dof_channel_coc(coc, l);
    float widest = max(max(abs(c.x), abs(c.y)), abs(c.z));
    return widest > l.max_radius ? c * (l.max_radius / widest) : c;
}

// A capped CoC's disc radii, at least half a half-size pixel.
float3 dof_radii(float3 capped) { return float3(max(abs(capped.x), 0.5f), max(abs(capped.y), 0.5f), max(abs(capped.z), 0.5f)); }

// 1 / view depth of a reversed depth through the constants' inverse projection: -(x d + y) / (z d + w).
float dof_inverse_depth(float reversed, float4 k) { return max(-(k.x * reversed + k.y) / (k.z * reversed + k.w), 0.0f); }

struct DofShape
{
    float blades;       // 0 round, else 5 to 16
    float roundness;    // 0 straight blades to 1 round
    float rotation;     // radians
    float area_scale;   // the blades' and the anamorphic squeeze's area over a disc's, square-rooted: keeps the area pi
    float anamorphic;
    float cat_eye;
    float swirl;
    float aspect;       // width over height
};

// Where a source sits on the picture, scaled so its corners lie at distance 1, y up.
float2 dof_field(float2 ndc, float aspect) { return float2(ndc.x * aspect, ndc.y) / sqrt(aspect * aspect + 1.0f); }

// The distance of a receiver at u from a source's center, in u's units, scaled so the aperture's edge lies at the
// source's radius: a regular polygon of blades with rounded sides, rotated, squeezed, and with swirl squeezed along the
// line from the picture's center toward the source. It is homogeneous in u, so it is taken once in pixels for all
// channels' radii.
float dof_shape_distance(float2 u, float2 ndc, DofShape s)
{
    float2 at = dof_field(ndc, s.aspect);
    float m = length(at);
    if (s.swirl > 0.0f && m > 0.0001f)
    {
        float2 e = at / m;
        float radial = dot(u, e);
        u = u + e * (radial * kDofSwirl * s.swirl * m * m);
    }
    u.x = u.x * s.anamorphic;
    float rho = length(u);
    float d = rho;
    if (s.blades >= 3.0f && rho > 0.000001f)
    {
        float sector = 2.0f * kDofPi / s.blades;
        float t = atan2(u.y, u.x) - s.rotation - kDofPi * 0.5f - sector * 0.5f;
        float phi = t - sector * floor(t / sector + 0.5f);
        d = lerp(rho * cos(phi) / cos(sector * 0.5f), rho, s.roundness);
    }
    return d * s.area_scale;
}

// How much brighter the disc's light lies where swirl squeezed it, so the disc keeps its energy.
float dof_swirl_density(float2 ndc, DofShape s)
{
    float2 at = dof_field(ndc, s.aspect);
    return 1.0f + kDofSwirl * s.swirl * dot(at, at);
}

// The cat's eye's cut of a source of radius r at a receiver at u: the second disc's weight in the aperture, its edge
// smoothed over the pixel on either side of the radius, as the disc's own edge is.
float dof_cat_eye_cut(float2 u, float r, float2 ndc, DofShape s)
{
    return s.cat_eye > 0.0f ? cat_eye_weight(u / (r + 0.5f), ndc, s.aspect, s.cat_eye, 1.0f / (r + 0.5f)) : 1.0f;
}

// The cut of each channel's disc, of radii r: a channel whose disc is wider than green's is cut at its own radius.
float3 dof_cat_eye_cuts(float2 u, float3 r, float2 ndc, DofShape s)
{
    return float3(dof_cat_eye_cut(u, r.x, ndc, s), dof_cat_eye_cut(u, r.y, ndc, s), dof_cat_eye_cut(u, r.z, ndc, s));
}

// The share of a source of radius r at ndc that reaches a receiver at u, both in pixels: inside the shape, its edge
// smoothed over one pixel, and the cat's eye's cut.
float dof_aperture(float2 u, float r, float2 ndc, DofShape s)
{
    return saturate(r - dof_shape_distance(u, ndc, s) + 0.5f) * dof_cat_eye_cut(u, r, ndc, s);
}

// Spherical aberration: the light across the disc at rho (0 center, 1 edge) is 1 + aberration (3 rho^4 - 1), whose mean
// over the disc is 1, at least 0, times norm, which restores the mean where the clamp cut it. aberration is the
// setting's behind focus and its negative in front: at 1 behind focus the center is dark and the rim 3 (soap bubble).
float dof_bokeh_intensity(float rho, float aberration, float norm)
{
    float r2 = rho * rho;
    return max(1.0f + aberration * (3.0f * r2 * r2 - 1.0f), 0.0f) * norm;
}

// What one source of radius r gives a receiver at shape distance d, per area unit of the source plane: coverage times the
// light across the disc over the disc's area. No disc is smaller than the in-focus band's, half a half-size pixel, so a
// channel near its own focus plane has a bounded weight; and the light across a disc of a pixel or two is not resolved,
// so it is flat at that floor and the lens's whole profile from a pixel past it.
float dof_channel_weight(float r, float d, float aberration, float norm)
{
    r = max(r, 0.5f);
    float light = lerp(1.0f, dof_bokeh_intensity(min(d / r, 1.0f), aberration, norm), saturate(r - 0.5f));
    return saturate(r - d + 0.5f) * light / (kDofPi * r * r);
}

// Each channel's dof_channel_weight, times scale.
float3 dof_weights(float3 r, float d, float scale, float aberration, float norm)
{
    return float3(dof_channel_weight(r.x, d, aberration, norm), dof_channel_weight(r.y, d, aberration, norm), dof_channel_weight(r.z, d, aberration, norm)) * scale;
}

// Highlights. The game's HDR scene is shallow: a small lamp lies barely above what is around it, so spread over its disc
// it drowns in it. A sprite of luminance l and contrast c grows by room (1 - exp(-s (c - kDofHighlightContrast))) for gain
// s, where room, curve.z times its disc's area, adds at most curve.z to each pixel of its disc, so a lamp of a few texels
// stays below the tone map's clip and overlapping discs keep their gaps. It is l itself at gain 0 and below that contrast.
// curve is dof_highlight_curve's threshold, gain and most added light a pixel.
float dof_grow_highlight(float l, float contrast, float3 curve, float radius)
{
    float room = min(curve.z * kDofPi * radius * radius, kDofHighlightMax);
    return l + room * (1.0f - exp(-curve.y * max(contrast - kDofHighlightContrast, 0.0f)));
}

// The curve in the scene's units: the threshold the setting's times the frame's mean luminance key, so the same settings
// pick out the lamps of a dim hall and the glints of a bright sea, and kDofHighlightDisc thresholds.
float3 dof_highlight_curve(float key)
{
    float t = dof_highlight.x * max(key, kDofHighlightKeyFloor);
    return float3(t, dof_highlight.y, kDofHighlightDisc * t);
}

// Interleaved gradient noise, Jimenez 2014: a per-pixel turn of the gather's spiral.
float dof_noise(int2 p) { return frac(52.9829189f * frac(0.06711056f * float(p.x) + 0.00583715f * float(p.y))); }

// The lens of this frame, from the constants and the focus the autofocus draw wrote.
DofLens dof_frame_lens(float focus)
{
    return dof_lens_at(dof_optics.x, dof_optics.y, focus, dof_optics.z, dof_lens.w, dof_optics.w);
}

DofShape dof_frame_shape()
{
    DofShape s;
    s.blades = dof_shape.x;
    s.roundness = dof_shape.y;
    s.rotation = dof_shape.z;
    s.area_scale = dof_shape.w;
    s.anamorphic = dof_lens.x;
    s.cat_eye = dof_lens.y;
    s.swirl = dof_lens.z;
    s.aspect = dof_sampling.w;
    return s;
}

// A highlight drawn as a quad of its disc: at is its center in half-size pixels, its green CoC and the quad's half side,
// light what it took out of the gather.
struct DofSprite
{
    float4 at;
    float4 light;
};

// The half side of the quad a source of green CoC coc needs, in half-size pixels: its widest channel's disc grown by the
// shape's reach, and the edge's pixel.
float dof_sprite_extent(float coc, DofLens l)
{
    float3 c = dof_capped_coc(coc, l);
    float r = max(max(abs(c.x), abs(c.y)), abs(c.z));
    return max(r, 0.5f) * dof_bokeh.w + 1.0f;
}

struct DofFields
{
    float4 far;     // rgb the far field's weighted mean, a the luminance of its mean without the lift (at least 1e-4 where it has weight)
    float4 near;    // rgb premultiplied, a green's coverage
    float2 cover;   // red's and blue's coverage of the near field: with chromatic aberration their discs differ from green's
};

#ifdef DOF_FOCUS
// The view depth in meters of the main depth at a pixel, 10000 where it is infinitely far.
float dof_meters_at(int2 pixel)
{
    float inverse = dof_inverse_depth(dof_load_depth(pixel), dof_depth);
    return inverse > 0.0f ? 1.0f / inverse : 10000.0f;
}

// The autofocus on an eye whose surface the caller reckons dof_eye.x meters away under the focus point, from the eye's
// bones and its size, so a centimeter or so out: of a 9x9 window of depths around the point, dof_eye.y pixels apart
// (core/dof.h's kDofEyeGrid), those that can be the eye's visible surface, from kDofEyeFront in front of the estimate to
// kDofEyeBack behind it, and of them the one with a quarter nearer. Genshin's iris sits up to about a centimeter behind
// the lids and brow around it, the bangs that cross an eye hang 4 cm and more in front of it, and the head behind it lies
// farther than its eyeball's 1.2 cm or so. So the band keeps the lids, the iris and the face around them with the
// estimate's error either way and drops the bangs and the head behind, and the quarter keeps a lid's rim or a strand that
// still falls in the band from deciding alone. With none in the band, the estimate.
static const int kDofEyeHalfGrid = 4;
static const float kDofEyeFront = 0.025f;
static const float kDofEyeBack = 0.015f;
static const float kDofEyeRank = 0.25f;

float dof_eye_target()
{
    float cx = dof_focus.y * float(dof_counts.y), cy = dof_focus.z * float(dof_counts.z);
    float nearest = dof_eye.x - kDofEyeFront, farthest = dof_eye.x + kDofEyeBack;
    float band[81];
    int count = 0;
    LOOP for (int y = -kDofEyeHalfGrid; y <= kDofEyeHalfGrid; y++)
        LOOP for (int x = -kDofEyeHalfGrid; x <= kDofEyeHalfGrid; x++)
        {
            float meters = dof_meters_at(int2(int(floor(cx + float(x) * dof_eye.y)), int(floor(cy + float(y) * dof_eye.y))));
            if (meters >= nearest && meters <= farthest)
            {
                band[count] = meters;
                count = count + 1;
            }
        }
    if (count == 0)
        return dof_eye.x;
    // The sample with exactly rank others before it, nearer or as near and earlier.
    int rank = int(kDofEyeRank * float(count - 1));
    float found = band[0];
    LOOP for (int i = 0; i < count; i++)
    {
        int before = 0;
        LOOP for (int j = 0; j < count; j++)
            before = before + ((band[j] < band[i] || (band[j] == band[i] && j < i)) ? 1 : 0);
        if (before == rank)
            found = band[i];
    }
    return found;
}

// The nearest of a 3x3 window of depths around the focus point, dof_counts.w pixels apart, in meters.
float dof_focus_target()
{
    int2 center = int2(int(dof_focus.y * float(dof_counts.y)), int(dof_focus.z * float(dof_counts.z)));
    int step = int(dof_counts.w);
    float nearest = 0.0f;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
            nearest = max(nearest, dof_load_depth(int2(center.x + x * step, center.y + y * step)));
    float inverse = dof_inverse_depth(nearest, dof_depth);
    return inverse > 0.0f ? 1.0f / inverse : 10000.0f;
}

// The frame's mean luminance over 16x9 points, each the mean of the 2x2 pixels around it.
float dof_frame_luminance()
{
    float sum = 0.0f;
    float2 size = float2(float(dof_counts.y), float(dof_counts.z));
    LOOP for (int y = 0; y < 9; y++)
        UNROLL for (int x = 0; x < 16; x++)
            sum = sum + dot(dof_sample_scene(float2(floor((float(x) + 0.5f) / 16.0f * size.x), floor((float(y) + 0.5f) / 9.0f * size.y))), kDofLuma);
    return sum / 144.0f;
}

// The focus distance this frame: eased from previous by dof_focus.w toward the setting's distance or the autofocus's
// target, on the eye or around the point, and at once when previous is no distance yet.
float dof_focus_next(float previous)
{
    float target = dof_focus.x > 0.0f ? dof_focus.x : dof_eye.x > 0.0f ? dof_eye_target() : dof_focus_target();
    if (!(previous > 0.0f && previous < 100000.0f))
        return target;
    return lerp(previous, target, dof_focus.w);
}

// What the autofocus draw writes: the focus distance from the last frame's, and the frame's mean luminance.
float2 dof_focus_pass(float previous) { return float2(dof_focus_next(previous), dof_frame_luminance()); }
#endif

#ifdef DOF_PREFILTER
// A half-size pixel: the mean of its 2x2 full-size pixels, and the green CoC of the nearest of them, so a thin foreground does not vanish into the background's
// CoC. The mean leaves out a pixel beside one nearer by more than a pixel of CoC while any other remains: an anti-aliased
// outline blends the nearer surface into it, whose light would spread over the background's blur as a halo around a
// subject in focus, or, as the background a blurred foreground hides, darken or lighten the inside of its outline in steps.
float4 dof_prefilter(int2 h)
{
    int2 p = int2(h.x * 2, h.y * 2);
    float2 lens_key = dof_load_focus();
    float focus = lens_key.x;
    DofLens lens = dof_frame_lens(focus);
    // The full-size CoCs of the 4x4 pixels around the 2x2.
    float cocs[16];
    UNROLL for (int i = 0; i < 16; i++)
        cocs[i] = 2.0f * dof_coc(dof_inverse_depth(dof_load_depth(int2(p.x + i % 4 - 1, p.y + i / 4 - 1)), dof_depth), focus, lens);
    float3 all = float3(0.0f, 0.0f, 0.0f), kept = float3(0.0f, 0.0f, 0.0f);
    float count = 0.0f;
    UNROLL for (int j = 0; j < 4; j++)
    {
        int at = (j / 2 + 1) * 4 + j % 2 + 1;
        bool outline = false;
        UNROLL for (int k = 0; k < 9; k++)
            outline = outline || (k != 4 && cocs[at + (k / 3 - 1) * 4 + k % 3 - 1] < cocs[at] - 1.0f);
        float3 c = dof_load_scene(int2(p.x + j % 2, p.y + j / 2));
        all = all + c;
        kept = kept + (outline ? float3(0.0f, 0.0f, 0.0f) : c);
        count = count + (outline ? 0.0f : 1.0f);
    }
    float3 color = count > 0.0f ? kept / count : all * 0.25f;
    float d = max(max(dof_load_depth(p), dof_load_depth(int2(p.x + 1, p.y))), max(dof_load_depth(int2(p.x, p.y + 1)), dof_load_depth(int2(p.x + 1, p.y + 1))));
    float coc = dof_coc(dof_inverse_depth(d, dof_depth), focus, lens);
    return float4(color.x, color.y, color.z, clamp(coc, -10000.0f, 10000.0f));
}
#endif

#ifdef DOF_SCATTER
struct DofDetect
{
    float4 source;  // the prefilter's texel
    bool sprite;    // whether it stands out enough to be drawn as one
    DofSprite as;   // the sprite
    float4 rest;    // what the gather keeps of the texel once the sprite is drawn
};

// The 16 texels two away from h, i from 0 to 15: the rows above and below, then the middle three of either side.
int2 dof_ring(int2 h, int i)
{
    return i < 5 ? int2(h.x + i - 2, h.y - 2) : i < 10 ? int2(h.x + i - 7, h.y + 2) : i < 13 ? int2(h.x - 2, h.y + i - 11) : int2(h.x + 2, h.y + i - 14);
}

// Whether the prefilter's texel at h is a highlight to draw as a sprite: out of focus by kDofSpriteRadius, above the
// threshold, and kDofSpritePeak times the mean luminance of the ring two texels away, which a glint or a cluster of them up
// to three texels across passes and a flat bright area and its edges do not, or with the contrast to grow, which a small
// lamp has. The sprite takes the light above the ring's least of each channel, which stays in the gather as the
// surroundings' color there, grown by dof_grow_highlight.
DofDetect dof_detect(int2 h)
{
    DofDetect o;
    o.source = dof_load_prefilter(h);
    o.sprite = false;
    o.as.at = float4(float(h.x) + 0.5f, float(h.y) + 0.5f, o.source.w, 0.0f);
    o.as.light = float4(0.0f, 0.0f, 0.0f, 0.0f);
    o.rest = o.source;
    float3 c = float3(o.source.x, o.source.y, o.source.z);
    float l = dot(c, kDofLuma);
    float2 lens_key = dof_load_focus();
    float3 curve = dof_highlight_curve(lens_key.y);
    if (abs(o.source.w) < kDofSpriteRadius || l <= curve.x)
        return o;
    float near_ring = 0.0f, far_ring = 0.0f;
    float3 least = c;
    LOOP for (int i = 0; i < 16; i++)
    {
        float4 n = dof_load_prefilter(dof_ring(h, i));
        near_ring = near_ring + dot(float3(n.x, n.y, n.z), kDofLuma);
        least = float3(min(least.x, n.x), min(least.y, n.y), min(least.z, n.z));
        int2 q = dof_ring(int2(0, 0), i);
        float4 f = dof_load_prefilter(int2(h.x + q.x * 2, h.y + q.y * 2));
        far_ring = max(far_ring, dot(float3(f.x, f.y, f.z), kDofLuma));
    }
    float inner = 0.0f;
    LOOP for (int j = 0; j < 9; j++)
    {
        float4 n = dof_load_prefilter(int2(h.x + j % 3 - 1, h.y + j / 3 - 1));
        inner = inner + dot(float3(n.x, n.y, n.z), kDofLuma);
    }
    float contrast = inner / 9.0f / max(far_ring, 1e-6f);
    if (l < kDofSpritePeak * near_ring / 16.0f && contrast < kDofHighlightContrast)
        return o;
    DofLens lens = dof_frame_lens(lens_key.x);
    float3 grown = c * (dof_grow_highlight(l, contrast, curve, max(abs(dof_capped_coc(o.source.w, lens).y), 0.5f)) / l);
    o.sprite = true;
    o.as.at.w = dof_sprite_extent(o.source.w, lens);
    o.as.light = float4(grown.x - least.x, grown.y - least.y, grown.z - least.z, 0.0f);
    o.rest = float4(least.x, least.y, least.z, o.source.w);
    return o;
}
#endif

#ifdef DOF_LIFT
// A half-size source lifted for the far field: rgb its light times the bokeh contrast's lift, a its own luminance, so the
// gather can count the light the lift added.
float4 dof_lift(int2 h)
{
    float4 source = dof_load_source(h);
    float3 c = float3(source.x, source.y, source.z);
    float light = dot(c, kDofLuma);
    float4 mean = dof_sample_source(float2(float(h.x) + 0.5f, float(h.y) + 0.5f), kDofContrastLevel);
    float around = max(dot(float3(mean.x, mean.y, mean.z), kDofLuma), 1e-4f);
    float r = min(abs(source.w), dof_optics.w);
    float lift = 1.0f + (pow(clamp(light / (kDofContrastFrom * around), 1.0f, kDofContrastCap), dof_contrast.x) - 1.0f)
        * saturate((r - kDofContrastFade.x) / (kDofContrastFade.y - kDofContrastFade.x));
    return float4(c.x * lift, c.y * lift, c.z * lift, light);
}
#endif

#ifdef DOF_BEHIND
// A half-size source weighted by how far behind focus it lies, its CoC over the largest to the fourth power, premultiplied:
// a filtered read of the mips averages what lies deepest around a point, the background a nearer surface hides, in which
// that surface itself, with a disc a few times smaller, all but vanishes. Nothing in front of focus counts.
float4 dof_behind(int2 h)
{
    float4 s = dof_load_source(h);
    float depth_share = s.w > kDofInFocus ? saturate(s.w / dof_optics.w) : 0.0f;
    float w = depth_share * depth_share * depth_share * depth_share;
    return float4(s.x * w, s.y * w, s.z * w, w);
}
#endif

#ifdef DOF_TILES
// A tile's widest disc behind and in front of focus over its 8x8 half-size pixels, capped at the largest radius: of each
// out-of-focus source, its widest channel's. With chromatic aberration that is not green's in front of focus, nor near it,
// where green's disc is under a pixel and red's spans several at a long focal length; a gather out to green's reach only
// would find a fraction of red's and blue's discs, and so a fraction of their light and coverage.
float2 dof_tile(int2 tile)
{
    DofLens lens = dof_frame_lens(dof_load_focus().x);
    float far = 0.0f, near = 0.0f;
    LOOP for (int y = 0; y < 8; y++)
        UNROLL for (int x = 0; x < 8; x++)
        {
            float c = dof_load_source(int2(tile.x * 8 + x, tile.y * 8 + y)).w;
            float3 r = dof_capped_coc(c, lens);
            float widest = max(max(abs(r.x), abs(r.y)), abs(r.z));
            if (c > kDofInFocus)
                far = max(far, widest);
            else if (c < -kDofInFocus)
                near = max(near, widest);
        }
    return float2(min(far, dof_optics.w), min(near, dof_optics.w));
}
#endif

#ifdef DOF_DILATE
// The tiles' maxima over the dof_counts.x tiles around, so a disc reaches every pixel it covers.
float2 dof_dilate(int2 tile)
{
    int reach = int(dof_counts.x);
    float2 m = float2(0.0f, 0.0f);
    LOOP for (int y = -reach; y <= reach; y++)
        LOOP for (int x = -reach; x <= reach; x++)
        {
            float2 t = dof_load_tile(int2(tile.x + x, tile.y + y));
            m = float2(max(m.x, t.x), max(m.y, t.y));
        }
    return m;
}
#endif

#ifdef DOF_GATHER
// Scatter as gather at a half-size pixel: as many samples as dof_sampling.x rings of 8k around one would have, on a
// golden-angle spiral out to half a ring past its tile's dilated radius, turned by the frame's seed and the pixel's
// noise. Each sample is a source whose disc may cover the pixel: its light per channel is coverage times the light
// across its disc over the disc's area, times the area the sample stands for. Sources behind the focus plane go to the
// far field, normalized, with the bokeh contrast's lifted light and, in a, the plain luminance the post-filter weighs it
// against; a source deeper than a pixel behind focus spreads no wider than that pixel's own disc, so the background does
// not spill over a nearer blurred surface. Sources in front of focus but behind a pixel's own surface by more than its
// layer go to the far field too, and at such a pixel every source beneath spreads as wide as the pixel's own disc or the
// spiral's spacing, nearest first, so what lies around an out-of-focus foreground fills in behind it. Sources on or in front of the pixel's surface are dof_near_layer's. The near field and
// its cover stay empty, the near sprites' targets.
DofFields dof_gather(int2 p)
{
    DofFields o;
    o.far = float4(0.0f, 0.0f, 0.0f, 0.0f);
    o.near = float4(0.0f, 0.0f, 0.0f, 0.0f);
    o.cover = float2(0.0f, 0.0f);
    float2 reach = dof_load_dilated(int2(p.x / 8, p.y / 8));
    float radius = max(reach.x, reach.y) * dof_bokeh.w;
    float4 own_source = dof_load_source(p);
    // Every disc a tile this small holds lies within its pixel, so all the gather would find is the pixel's own source.
    // That is its far field then, so the post-filter treats the pixel as it does its neighbors in a wider tile.
    if (radius < 0.5f)
    {
        if (own_source.w > kDofInFocus)
            o.far = float4(own_source.x, own_source.y, own_source.z, max(dot(float3(own_source.x, own_source.y, own_source.z), kDofLuma), 1e-4f));
        return o;
    }
    float2 lens_key = dof_load_focus();
    float focus = lens_key.x;
    DofLens lens = dof_frame_lens(focus);
    DofShape shape = dof_frame_shape();
    float own = own_source.w;
    float3 own_c = dof_capped_coc(own, lens);
    float3 own_far = own > 0.0f ? float3(max(own_c.x, 0.5f), max(own_c.y, 0.5f), max(own_c.z, 0.5f)) : float3(100000.0f, 100000.0f, 100000.0f);
    float own_near = own < -kDofInFocus ? -own_c.y : 0.0f;
    // A pixel behind focus takes light only from discs no wider than its own, as the sources behind it spread no wider and
    // those in front are dof_near_layer's, so its spiral spans its own disc: spread over the tile's widest disc, a slightly
    // blurred pixel's disc would fall between the samples, and it would show its half-size texels, then jump to the blur.
    if (own > kDofInFocus)
        radius = min(radius, (max(max(own_far.x, own_far.y), own_far.z) + 0.5f) * dof_bokeh.w);
    int rings = int(dof_sampling.x);
    int count = 4 * rings * (rings + 1) + 1;
    float spacing = radius / float(rings);
    float reach_px = radius + 0.5f * spacing;
    float area = kDofPi * reach_px * reach_px / float(count + 1);
    float turn = dof_sampling.y + dof_noise(p);
    float up = dof_sampling.z;
    float2 half_size = float2(float(dof_sizes.x), float(dof_sizes.y));
    float3 far_sum = float3(0.0f, 0.0f, 0.0f), far_w = float3(0.0f, 0.0f, 0.0f);
    float2 far_by_green = float2(0.0f, 0.0f);
    float far_light = 0.0f;
    bool lifted = dof_contrast.x > 0.0f;
    // The pixel's own texel, kept aside when the spiral could find its disc: it counts only when the spiral found nothing.
    float3 own_far_color = float3(0.0f, 0.0f, 0.0f), own_far_w = float3(0.0f, 0.0f, 0.0f);
    float own_far_light = 0.0f;
    LOOP for (int i = 0; i <= count; i++)
    {
        // The golden angle apart, at radii that split the disc into equal areas, so every sample has its own radius: rings
        // put every pixel's samples at the same few radii, which blurs an edge into steps a ring apart. The last is the
        // pixel itself: a disc smaller than the spiral's spacing is found there whatever its channel's radius, so no
        // channel of a field is left without weight where another has some.
        float angle = 2.0f * kDofPi * (turn + float(i) * 0.38196601f);
        float2 o_px = i < count ? float2(cos(angle), sin(angle)) * (reach_px * sqrt((float(i) + 0.5f) / float(count))) : float2(0.0f, 0.0f);
        float2 q = float2(float(p.x) + 0.5f + o_px.x, float(p.y) + 0.5f + o_px.y);
        float4 at_q = dof_load_source(int2(int(floor(q.x)), int(floor(q.y))));
        float c = at_q.w;
        float3 r = dof_radii(dof_capped_coc(c, lens));
        float source_radius = r.y;
        // In a disc the spiral finds, the pixel's own texel would be a sample at the center on top of the spiral's,
        // and the pixel's own detail would show through the blur: it is kept aside.
        bool aside = i == count && min(min(r.x, r.y), r.z) >= spacing;
        // A source within half a full-size pixel of focus belongs to neither field, the composite keeping it sharp, as its
        // small disc's weight would swamp the fields' wider ones; but it fills in behind a pixel in front of focus.
        if (abs(c) <= kDofInFocus && own_near == 0.0f)
            continue;
        bool behind = c > -kDofInFocus;
        float layer = max(kDofLayer, kDofLayerShare * abs(own));
        bool occluder = !behind && (own_near == 0.0f || c < own - layer);
        bool level = !behind && !occluder && c <= own + layer;
        bool beneath = behind || (!occluder && !level);
        if (behind && c > own)
            r = float3(min(r.x, own_far.x), min(r.y, own_far.y), min(r.z, own_far.z));
        // What a pixel in front of focus hides is the nearest of the sources beneath it, whatever their own discs: a surface
        // just behind it, sharp or nearly, has discs under the spiral's spacing, which the spiral would miss, and the far
        // background's wide discs would stand in for it, showing through a strand of hair over a face.
        if (beneath && own_near > 0.0f)
        {
            // As far as the widest channel's disc and a spacing past it, where the foreground's blur starts to let it through.
            float hidden = max(max(abs(own_c.x), own_near), abs(own_c.z)) + spacing;
            r = float3(hidden, hidden, hidden);
        }
        // Sources in front of the pixel's own surface belong to dof_near_layer's foreground blur.
        if (!beneath)
            continue;
        // Where the pixel lies in the source's disc, y up; a source in front of focus shows the aperture mirrored.
        float2 u = behind ? float2(-o_px.x, -o_px.y * up) : float2(o_px.x, o_px.y * up);
        float2 ndc = float2(q.x / half_size.x * 2.0f - 1.0f, up * (q.y / half_size.y * 2.0f - 1.0f));
        // What a pixel in front of focus hides is filled in from all around it, whatever the aperture's shape: cut by the
        // cat's eye or squeezed, the fill stopped short of where the foreground's own blur turns translucent, and there the
        // pixel showed its own color through it, in a step that followed the half-size texels.
        bool fill = beneath && own_near > 0.0f;
        float d = fill ? length(o_px) : dof_shape_distance(u, ndc, shape);
        if (d > max(max(r.x, r.y), r.z) + 0.5f)
            continue;
        float3 w = fill ? float3(area, area, area)
                        : dof_weights(r, d, dof_swirl_density(ndc, shape) * area, behind ? dof_bokeh.x : -dof_bokeh.x, behind ? dof_bokeh.y : dof_bokeh.z)
                            * dof_cat_eye_cuts(u, r, ndc, shape);
        float lod = log2(clamp(min(spacing, r.y), 1.0f, 256.0f));
        float4 filtered = dof_sample_source(q, lod);
        bool mixed = abs(filtered.w - c) > kDofMixed * abs(c);
        bool lift = lifted && behind;
        float2 lit_at = mixed ? float2(floor(q.x) + 0.5f, floor(q.y) + 0.5f) : q;
        float4 lit = lift ? dof_sample_lifted(lit_at, mixed ? 0.0f : lod) : float4(0.0f, 0.0f, 0.0f, 0.0f);
        // What shows through a blurred foreground at a pixel in front of focus is taken texel by texel: a filtered
        // sample beside the foreground's silhouette would average its light in, and the background seen through the
        // foreground would brighten inside its outline, a step at the silhouette.
        // A source out of focus behind a pixel in front of focus, or well behind the pixel's own surface out of focus, shows as
        // blurred as it is: the deepest light averaged over its own disc. Taken texel by texel, a background far behind showed
        // its sharp detail through a foreground's translucent edge, and darkened a nearer blurred surface's rim with it, which
        // drew that surface's outline crisp against the background's blur just outside it.
        bool deeper = c > kDofInFocus && (own_near > 0.0f || (own > kDofInFocus && c > own + max(kDofLayer, kDofLayerShare * own)));
        float4 deep = deeper ? dof_sample_behind(q, log2(clamp(source_radius, 1.0f, 256.0f))) : float4(0.0f, 0.0f, 0.0f, 0.0f);
        float3 color = lift ? float3(lit.x, lit.y, lit.z)
                     : deep.w > 0.0f ? float3(deep.x, deep.y, deep.z) / deep.w
                     : own_near > 0.0f || mixed ? float3(at_q.x, at_q.y, at_q.z) : float3(filtered.x, filtered.y, filtered.z);
        // What a blurred foreground hides is seen around its edge as a lens sees it, sharp: the nearest visible background
        // stands for it, so it meets the background beside the outline without a seam of blur.
        if (own_near > 0.0f)
            w = w / (1.0f + d * d * d * d);
        if (aside)
        {
            own_far_color = color;
            own_far_w = w;
            own_far_light = (lift ? lit.w : dot(color, kDofLuma)) * w.y;
            continue;
        }
        far_sum = far_sum + color * w;
        far_w = far_w + w;
        far_by_green = far_by_green + float2(color.x, color.z) * w.y;
        far_light = far_light + (lift ? lit.w : dot(color, kDofLuma)) * w.y;
    }
    if (far_w.y <= 0.0f)
    {
        far_sum = own_far_color * own_far_w;
        far_w = own_far_w;
        far_by_green = float2(own_far_color.x, own_far_color.z) * own_far_w.y;
        far_light = own_far_light;
    }
    // Behind a pixel in front of focus, red's or blue's discs of the sources behind may be too small to reach it where
    // green's do: that channel then takes its sources' light by green's weights, not none.
    if (far_w.y > 0.0f)
        o.far = float4(far_w.x > 0.0f ? far_sum.x / far_w.x : far_by_green.x / far_w.y, far_sum.y / far_w.y,
            far_w.z > 0.0f ? far_sum.z / far_w.z : far_by_green.y / far_w.y, max(far_light / far_w.y, 1e-4f));
    return o;
}
#endif

#ifdef DOF_POST
// The cubic B-spline's two bilinear reads along one axis that make up its four texels' weights (Sigg and Hadwiger, GPU
// Gems 2, ch. 20): at u in texels, centers at .5; returns the reads' positions in texels and the first's weight.
float3 dof_bspline_axis(float u)
{
    float c = u - 0.5f;
    float i = floor(c);
    float f = c - i;
    float w0 = (1.0f - f) * (1.0f - f) * (1.0f - f) / 6.0f;
    float w1 = (4.0f - 6.0f * f * f + 3.0f * f * f * f) / 6.0f;
    float w3 = f * f * f / 6.0f;
    float w2 = 1.0f - w0 - w1 - w3;
    float g0 = w0 + w1;
    return float3(i - 0.5f + w1 / g0, i + 1.5f + w3 / (w2 + w3), g0);
}

// The far field's level at at, half-size pixels, through a cubic B-spline over its texels from four bilinear reads: smooth
// across the texels' centers, where a bilinear read bends.
float4 dof_far_smooth(float2 at, float level)
{
    float2 half_size = float2(float(dof_sizes.x), float(dof_sizes.y));
    float2 size = float2(max(floor(half_size.x / exp2(level)), 1.0f), max(floor(half_size.y / exp2(level)), 1.0f));
    float3 x = dof_bspline_axis(at.x / half_size.x * size.x);
    float3 y = dof_bspline_axis(at.y / half_size.y * size.y);
    float2 scale = float2(half_size.x / size.x, half_size.y / size.y);
    float4 top = dof_sample_far(float2(x.x * scale.x, y.x * scale.y), level) * x.z + dof_sample_far(float2(x.y * scale.x, y.x * scale.y), level) * (1.0f - x.z);
    float4 bottom = dof_sample_far(float2(x.x * scale.x, y.y * scale.y), level) * x.z + dof_sample_far(float2(x.y * scale.x, y.y * scale.y), level) * (1.0f - x.z);
    return top * y.z + bottom * (1.0f - y.z);
}

// The gather's sampling holes filled: each channel of each field clamped to the least and most of its 8 neighbors, which
// takes out a lone outlier but keeps the discs' rims and the light; the far field's neighbors without weight take the
// pixel's own value. With the bokeh contrast, the far field then gives back what the lift added around it (see the
// constants).
DofFields dof_post(int2 p)
{
    float4 own = dof_load_far(p);
    float4 f_lo = float4(100000.0f, 100000.0f, 100000.0f, 100000.0f), f_hi = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 n_lo = f_lo, n_hi = f_hi;
    float2 c_lo = float2(1.0f, 1.0f), c_hi = float2(0.0f, 0.0f);
    UNROLL for (int i = 0; i < 9; i++)
    {
        if (i == 4)
            continue;
        int2 q = int2(p.x + i % 3 - 1, p.y + i / 3 - 1);
        float4 f = dof_load_far(q);
        if (f.w <= 0.0f)
            f = own;
        float4 n = dof_load_near(q);
        float2 v = dof_load_cover(q);
        c_lo = float2(min(c_lo.x, v.x), min(c_lo.y, v.y));
        c_hi = float2(max(c_hi.x, v.x), max(c_hi.y, v.y));
        f_lo = float4(min(f_lo.x, f.x), min(f_lo.y, f.y), min(f_lo.z, f.z), min(f_lo.w, f.w));
        f_hi = float4(max(f_hi.x, f.x), max(f_hi.y, f.y), max(f_hi.z, f.z), max(f_hi.w, f.w));
        n_lo = float4(min(n_lo.x, n.x), min(n_lo.y, n.y), min(n_lo.z, n.z), min(n_lo.w, n.w));
        n_hi = float4(max(n_hi.x, n.x), max(n_hi.y, n.y), max(n_hi.z, n.z), max(n_hi.w, n.w));
    }
    float4 near = dof_load_near(p);
    DofFields o;
    o.far = float4(clamp(own.x, f_lo.x, f_hi.x), clamp(own.y, f_lo.y, f_hi.y), clamp(own.z, f_lo.z, f_hi.z), clamp(own.w, f_lo.w, f_hi.w));
    // A pixel far behind focus averages its far field with its neighbors' as far out of focus, by a tent over the 3x3
    // around it: a texel's shift is lost in its disc, and a sparse gather finds a disc smaller than its own at a few of its
    // samples, a different few for each pixel and frame, which speckles and shimmers where a sharper surface meets it.
    float own_coc = min(dof_load_source(p).w, dof_optics.w);
    float smooth_share = saturate((own_coc - kDofSmoothFrom) / kDofSmoothFrom);
    if (smooth_share > 0.0f)
    {
        float4 sum = o.far * 4.0f;
        float weight = 4.0f;
        UNROLL for (int j = 0; j < 9; j++)
        {
            if (j == 4)
                continue;
            int2 q = int2(p.x + j % 3 - 1, p.y + j / 3 - 1);
            float4 f = dof_load_far(q);
            float c = min(dof_load_source(q).w, dof_optics.w);
            float w = (j % 2 == 0 ? 1.0f : 2.0f) * (f.w > 0.0f && c >= 0.5f * own_coc && c <= 2.0f * own_coc ? 1.0f : 0.0f);
            sum = sum + f * w;
            weight = weight + w;
        }
        o.far = o.far * (1.0f - smooth_share) + sum * (smooth_share / weight);
    }
    o.near = float4(clamp(near.x, n_lo.x, n_hi.x), clamp(near.y, n_lo.y, n_hi.y), clamp(near.z, n_lo.z, n_hi.z), clamp(near.w, n_lo.w, n_hi.w));
    float2 cover = dof_load_cover(p);
    o.cover = float2(clamp(cover.x, c_lo.x, c_hi.x), clamp(cover.y, c_lo.y, c_hi.y));
    if (dof_contrast.x > 0.0f && o.far.w > 0.0f)
    {
        // gain, what the lift multiplied the far field's light by over the span, from the far field's two levels around
        // the span's, each through a cubic B-spline: a bilinear read bends at the texels' centers, and a bright source
        // nearby turns each bend into a line, which on the span's usual levels lies on the tiles' grid. The span grows from the dilated far radius taken bilinearly between the
        // tiles' centers: a tile's own radius would change the level, and so the gain, in a step at every tile's edge. Past
        // dof_contrast.y the lift's share of each pixel's light is cut to fit, from the lifted light toward the plain, whose
        // luminance the gather kept in a.
        float2 tile = float2((float(p.x) + 0.5f) / 8.0f - 0.5f, (float(p.y) + 0.5f) / 8.0f - 0.5f);
        int2 t = int2(int(floor(tile.x)), int(floor(tile.y)));
        float tx = tile.x - floor(tile.x), ty = tile.y - floor(tile.y);
        float reach = lerp(lerp(dof_load_dilated(t).x, dof_load_dilated(int2(t.x + 1, t.y)).x, tx),
            lerp(dof_load_dilated(int2(t.x, t.y + 1)).x, dof_load_dilated(int2(t.x + 1, t.y + 1)).x, tx), ty);
        float lod = log2(max(reach, 1.0f)) + kDofContrastSpan;
        float level = floor(lod);
        float2 at = float2(float(p.x) + 0.5f, float(p.y) + 0.5f);
        float blend = lod - level;
        float4 m = dof_far_smooth(at, level) * (1.0f - blend) + dof_far_smooth(at, level + 1.0f) * blend;
        float gain = max(dot(float3(m.x, m.y, m.z), kDofLuma) / max(m.w, 1e-4f), 1.0f);
        float share = gain > 1.0f + dof_contrast.y ? dof_contrast.y / (gain - 1.0f) : 1.0f;
        float3 lit = float3(o.far.x, o.far.y, o.far.z);
        float3 plain = lit * (o.far.w / max(dot(lit, kDofLuma), 1e-4f));
        float3 v = (plain + (lit - plain) * share) / (1.0f + share * (gain - 1.0f));
        o.far = float4(v.x, v.y, v.z, o.far.w);
    }
    return o;
}
#endif

#ifdef DOF_NEAR_MASK
// Whether a half-size pixel lies in front of focus, softened over its first half-pixel of CoC, in a, and its color times
// that in rgb: the mips give the share of any area in front of focus and the foreground's own light there.
float4 dof_near_mask(int2 p)
{
    float4 s = dof_load_source(p);
    float m = saturate(-s.w - kDofInFocus);
    return float4(s.x * m, s.y * m, s.z * m, m);
}
#endif

#ifdef DOF_NEAR
// The near plane's blur, after Cinematic DOF's: Copyright (c) 2018-2022 Frans Bouma, released under the 2-clause BSD license
// (OtisFX, CinematicDOF.fx, PerformSingleValueGaussianBlur and PerformNearPlaneDiscBlur). Instead of gathering the
// foreground by its sources' discs, it blurs the foreground's CoC: the dilated tiles' near maxima, over a Gaussian a
// little narrower than the largest disc, give each pixel a blurred near radius that falls off smoothly across a
// silhouette on both sides. A disc of that radius averages what the far field made of the scene, the foreground
// unblurred, evenly; and the blurred radius, boosted by the max blur, is the alpha it covers the rest with: whole inside
// a foreground, fading out beyond its outline, with nothing of what lies behind it to guess.

// The near plane at a half-size pixel: its light, premultiplied, with green's coverage in a, and red's and blue's.
struct DofNear
{
    float4 layer;
    float2 cover;
};

DofNear dof_near_none()
{
    DofNear o;
    o.layer = float4(0.0f, 0.0f, 0.0f, 0.0f);
    o.cover = float2(0.0f, 0.0f);
    return o;
}

DofNear dof_near_layer(int2 p)
{
    // The largest near disc around the pixel, from the dilated tiles taken bilinearly between their centers: a tile's
    // own would change the disc, and so the coverage, in a step at every tile's edge. None, and nothing near is near.
    float2 tc = float2((float(p.x) + 0.5f) / 8.0f - 0.5f, (float(p.y) + 0.5f) / 8.0f - 0.5f);
    int2 tb = int2(int(floor(tc.x)), int(floor(tc.y)));
    float tx = tc.x - floor(tc.x), ty = tc.y - floor(tc.y);
    float local = lerp(lerp(dof_load_dilated(tb).y, dof_load_dilated(int2(tb.x + 1, tb.y)).y, tx),
        lerp(dof_load_dilated(int2(tb.x, tb.y + 1)).y, dof_load_dilated(int2(tb.x + 1, tb.y + 1)).y, tx), ty);
    if (local <= 0.0f)
        return dof_near_none();
    // A disc of the local near radius in the lens's shape, as the gather's discs are: the blades, the anamorphic squeeze,
    // the cat's eye and the swirl at the source's place on the picture, the soap bubble's light across it, mirrored as in
    // front of focus, and each channel's own radius under chromatic aberration. Behind focus, its average of dof_near_mask
    // is the share of the pixel the foreground's discs cover, a half at its outline and none a disc beyond, and of the
    // mask's premultiplied color the foreground's own light: laid over the background's sharp self, a veil as a lens
    // shows it, where an average with the background beside it drew a halo of blurred background. A small bright
    // foreground so blurs into a disc of the lens's shape.
    DofLens lens = dof_frame_lens(dof_load_focus().x);
    DofShape shape = dof_frame_shape();
    float3 r = dof_radii(dof_channel_coc(-local, lens));
    float reach = max(max(r.x, r.y), r.z) * dof_bokeh.w + 0.5f;
    int rings = int(dof_sampling.x) + 1;
    int count = 4 * rings * (rings + 1) + 1;
    float spacing = reach / float(rings);
    float lod = log2(clamp(spacing, 1.0f, 256.0f));
    float turn = dof_sampling.y + dof_noise(p);
    float up = dof_sampling.z;
    float2 half_size = float2(float(dof_sizes.x), float(dof_sizes.y));
    float3 sum3 = float3(0.0f, 0.0f, 0.0f), w3 = float3(0.0f, 0.0f, 0.0f), disc = float3(0.0f, 0.0f, 0.0f);
    // At a pixel itself in front of focus, sources much farther than its own surface, blurred by under half its disc,
    // are what it hides, which the composite's far estimate stands for: a foreground's edge over a nearer-than-focus wall
    // keeps the foreground's light instead of averaging the wall in. The rest, the foreground's own surfaces, its hair
    // a little behind its shoulder among them, and anything nearer, cover it as the mask says: leaving out what lay only
    // a little behind let the far estimate show through a blurred foreground in shards.
    float own = dof_load_source(p).w;
    bool layered = own < -kDofInFocus;
    LOOP for (int k = 0; k < count; k++)
    {
        float angle = 2.0f * kDofPi * (turn + float(k) * 0.38196601f);
        float2 o_px = float2(cos(angle), sin(angle)) * (reach * sqrt((float(k) + 0.5f) / float(count)));
        float2 q = float2(float(p.x) + 0.5f + o_px.x, float(p.y) + 0.5f + o_px.y);
        float2 u = float2(o_px.x, o_px.y * up);
        float2 ndc = float2(q.x / half_size.x * 2.0f - 1.0f, up * (q.y / half_size.y * 2.0f - 1.0f));
        float d = dof_shape_distance(u, ndc, shape);
        if (d > max(max(r.x, r.y), r.z) + 0.5f)
            continue;
        float3 w = dof_weights(r, d, dof_swirl_density(ndc, shape), -dof_bokeh.x, dof_bokeh.z) * dof_cat_eye_cuts(u, r, ndc, shape);
        disc = disc + w;
        if (layered && dof_load_source(int2(int(floor(q.x)), int(floor(q.y)))).w > kDofHiddenShare * own)
            continue;
        // The foreground's own light, filtered to the samples' spacing so a small bright foreground blurs smoothly: beyond
        // its outline the composite lays it over the background's sharp self by the coverage, a veil of the foreground
        // as a lens shows it, where an average with the background beside it drew a halo of blurred background.
        float4 fg = dof_sample_near_mask(q, lod);
        sum3 = sum3 + float3(fg.x, fg.y, fg.z) * w;
        w3 = w3 + w * fg.w;
    }
    if (w3.y <= 0.0f || disc.y <= 0.0f)
        return dof_near_none();
    // Each channel's coverage and light over its own disc. Premultiplied, so the composite's bilinear read between a
    // covered texel and an uncovered one fades the light, not the color.
    float3 cover = float3(disc.x > 0.0f ? saturate(w3.x / disc.x) : saturate(w3.y / disc.y), saturate(w3.y / disc.y),
        disc.z > 0.0f ? saturate(w3.z / disc.z) : saturate(w3.y / disc.y));
    float3 light = float3(disc.x > 0.0f ? sum3.x / disc.x : sum3.y / disc.y, sum3.y / disc.y, disc.z > 0.0f ? sum3.z / disc.z : sum3.y / disc.y);
    DofNear o;
    o.layer = float4(light.x, light.y, light.z, cover.y);
    o.cover = float2(cover.x, cover.z);
    return o;
}
#endif

#ifdef DOF_SPRITE
// What sprite s adds to the half-size pixel p of its field: per channel its light times dof_weights' share of its disc at
// p, with the aperture's shape, the light across the disc, the cat's eye and the swirl, as the gather would have spread it.
// Behind focus it reaches a receiver nearer but also behind focus only within that receiver's own disc, as the gather's far
// field does, while keeping its own disc's light a pixel: the nearer surface hides the rest.
float3 dof_sprite_light(DofSprite s, int2 p)
{
    DofLens lens = dof_frame_lens(dof_load_focus().x);
    DofShape shape = dof_frame_shape();
    float c = s.at.z;
    bool behind = c > 0.0f;
    float3 r = dof_radii(dof_capped_coc(c, lens));
    float up = dof_sampling.z;
    float2 o = float2(float(p.x) + 0.5f - s.at.x, float(p.y) + 0.5f - s.at.y);
    float2 u = behind ? float2(o.x, o.y * up) : float2(-o.x, -o.y * up);
    float2 ndc = float2(s.at.x / float(dof_sizes.x) * 2.0f - 1.0f, up * (s.at.y / float(dof_sizes.y) * 2.0f - 1.0f));
    float d = dof_shape_distance(u, ndc, shape);
    float own = dof_load_source(p).w;
    float reach = behind && own > kDofInFocus && own < c ? max(dof_capped_coc(own, lens).y, 0.5f) : 100000.0f;
    float scale = dof_swirl_density(ndc, shape) * saturate(reach - d + 0.5f);
    float3 w = dof_weights(r, d, scale, behind ? dof_bokeh.x : -dof_bokeh.x, behind ? dof_bokeh.y : dof_bokeh.z) * dof_cat_eye_cuts(u, r, ndc, shape);
    return float3(min(s.light.x * w.x, kDofSpriteMostAdded), min(s.light.y * w.y, kDofSpriteMostAdded), min(s.light.z * w.z, kDofSpriteMostAdded));
}
#endif

#ifdef DOF_SOFTEN
// The farthest a lens softness's Gaussian reaches, in pixels: three of its sigmas, at most this many.
static const int kDofSoftReach = 6;

// A full-size pixel through the lens softness: the scene under a Gaussian of dof_eye.z pixels.
float3 dof_soften(int2 pixel)
{
    float sigma = dof_eye.z;
    int reach = min(int(ceil(3.0f * sigma)), kDofSoftReach);
    float k = -0.5f / (sigma * sigma);
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    float weight = 0.0f;
    LOOP for (int y = -reach; y <= reach; y++)
    {
        LOOP for (int x = -reach; x <= reach; x++)
        {
            float w = exp(float(x * x + y * y) * k);
            sum = sum + dof_load_scene(int2(pixel.x + x, pixel.y + y)) * w;
            weight = weight + w;
        }
    }
    return sum / weight;
}
#endif

#ifdef DOF_COMPOSITE
// What a pixel behind focus shows of a surface in focus beside it. The game's SMAA and a supersampled shot blend an
// outline's pixels by the share each surface covers, while the depth holds only the surface under the pixel's center: a
// pixel whose depth lies behind can still be half the subject. Its color then lies between the subject's and the sharp
// background's, which the blurred background under it stands for, so the share comes back as how far along that line it
// lies, and the pixel keeps that much of the subject over the blurred background. The subject's color is that of the
// neighbor in focus farthest from the background, the least blended.
float3 dof_outline(int2 pixel, float3 sharp, float3 under, float focus, DofLens lens)
{
    float3 subject = under;
    float spread = 0.0f;
    UNROLL for (int i = 0; i < 9; i++)
    {
        int2 q = int2(pixel.x + i % 3 - 1, pixel.y + i / 3 - 1);
        float coc = 2.0f * dof_coc(dof_inverse_depth(dof_load_depth(q), dof_depth), focus, lens);
        float3 c = dof_load_scene(q);
        float3 d = c - under;
        if (i != 4 && abs(coc) <= 1.0f && dot(d, d) > spread)
        {
            subject = c;
            spread = dot(d, d);
        }
    }
    if (spread <= 0.0f)
        return under;
    return under + (subject - under) * saturate(dot(sharp - under, subject - under) / spread);
}

// What a pixel in focus beside a blurred foreground shows of its own surface. Anti-aliasing blends the foreground into
// the outline's pixels on the sharp side too, and kept sharp under the foreground's translucent blur, they would trace the
// outline through it as a line. The pixel's color lies between the foreground's, its neighbor in front of focus farthest
// from it, and its surface's, its neighbor in focus farthest from the foreground; what it holds of the foreground comes
// back as how far along that line it lies, and is taken out.
float3 dof_without_near(int2 pixel, float3 sharp, float focus, DofLens lens)
{
    float cocs[9];
    float3 colors[9];
    float3 front = sharp;
    float spread = 0.0f;
    UNROLL for (int i = 0; i < 9; i++)
    {
        int2 q = int2(pixel.x + i % 3 - 1, pixel.y + i / 3 - 1);
        cocs[i] = 2.0f * dof_coc(dof_inverse_depth(dof_load_depth(q), dof_depth), focus, lens);
        colors[i] = dof_load_scene(q);
        float3 d = colors[i] - sharp;
        if (cocs[i] < -1.0f && dot(d, d) > spread)
        {
            front = colors[i];
            spread = dot(d, d);
        }
    }
    if (spread <= 0.0f)
        return sharp;
    float3 surface = sharp;
    spread = 0.0f;
    UNROLL for (int j = 0; j < 9; j++)
    {
        float3 d = colors[j] - front;
        if (j != 4 && abs(cocs[j]) <= 1.0f && dot(d, d) > spread)
        {
            surface = colors[j];
            spread = dot(d, d);
        }
    }
    if (spread <= 0.0f)
        return sharp;
    return sharp + (surface - front) * saturate(dot(sharp - surface, front - surface) / spread);
}

// A full-size pixel: its sharp color while its own green CoC is under half a pixel, the far field blended in behind focus
// above that, the near layer over it by its alpha, as Cinematic DOF's combiner lays it, and the near sprites' light on top.
// Each of the four half-size texels around the pixel gives its far field or, without one, the mean of those that have it,
// or the sharp color; a pixel in front of focus keeps its sharp color under the near layer.
float3 dof_composite(int2 pixel)
{
    float3 sharp = dof_load_scene(pixel);
    float focus = dof_load_focus().x;
    DofLens lens = dof_frame_lens(focus);
    float coc = 2.0f * dof_coc(dof_inverse_depth(dof_load_depth(pixel), dof_depth), focus, lens);
    float2 at = float2((float(pixel.x) + 0.5f) * 0.5f - 0.5f, (float(pixel.y) + 0.5f) * 0.5f - 0.5f);
    int2 base = int2(int(floor(at.x)), int(floor(at.y)));
    float fx = at.x - floor(at.x), fy = at.y - floor(at.y);
    float3 far = float3(0.0f, 0.0f, 0.0f);
    float far_weight = 0.0f;
    UNROLL for (int i = 0; i < 4; i++)
    {
        int2 q = int2(base.x + i % 2, base.y + i / 2);
        float b = (i % 2 == 0 ? 1.0f - fx : fx) * (i / 2 == 0 ? 1.0f - fy : fy);
        float4 f = dof_load_far(q);
        float w = f.w > 0.0f ? b : 0.0f;
        far = far + float3(f.x, f.y, f.z) * w;
        far_weight = far_weight + w;
    }
    float t = coc > 0.0f ? saturate(coc - 0.5f) : 0.0f;
    float hidden = saturate((-coc - 1.0f) / kDofHiddenFade);
    float3 pooled = far_weight > 0.0f ? far / far_weight : sharp;
    float3 under = float3(0.0f, 0.0f, 0.0f);
    float4 layer = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float2 near_cover = float2(0.0f, 0.0f);
    float3 sprites = float3(0.0f, 0.0f, 0.0f);
    UNROLL for (int j = 0; j < 4; j++)
    {
        int2 q = int2(base.x + j % 2, base.y + j / 2);
        float b = (j % 2 == 0 ? 1.0f - fx : fx) * (j / 2 == 0 ? 1.0f - fy : fy);
        float4 f = dof_load_far(q);
        under = under + (f.w > 0.0f ? float3(f.x, f.y, f.z) : pooled) * b;
        layer = layer + dof_load_near_layer(q) * b;
        near_cover = near_cover + dof_load_near_cover(q) * b;
        float4 n = dof_load_near(q);
        sprites = sprites + float3(n.x, n.y, n.z) * b;
    }
    // Under a pixel in front of focus lies what it hides, the far field's estimate, which the near plane covers but for its
    // edge, faded in over its first kDofHiddenFade pixels of CoC: a pixel barely out of focus keeps its own color there,
    // which its blur hardly differs from; elsewhere the pixel's own color, blurred behind focus.
    float3 color = sharp + (under - sharp) * hidden;
    if (t > 0.0f)
        color = sharp + (dof_outline(pixel, sharp, under, focus, lens) - sharp) * t;
    else if (hidden <= 0.0f)
        color = dof_without_near(pixel, sharp, focus, lens);
    return color * float3(1.0f - near_cover.x, 1.0f - layer.w, 1.0f - near_cover.y) + float3(layer.x, layer.y, layer.z) + sprites;
}
#endif
