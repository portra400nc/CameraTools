// The depth of field's arithmetic and passes: each point's circle of confusion per color channel, the aperture's shape
// and the light across it, and the prefilter, tiles, gather, post-filter and composite over textures the includer reads.
// Shared by the dof_*_ps.hlsl draws and, compiled as C++ through core/hlsl.h, by core/dof.cpp, so the tests and the Mac's
// harness (tests/render_dof.cpp) run the very same passes. It keeps to what core/hlsl.h supports.
//
// Sizes: CoC radii are in half-size pixels, signed, positive behind the focus plane. Offsets on screen have y up.
//
// The pass functions read the Dof constants (dof.cbuffer.hlsli, core/dof.h's DofConstants), and a pass's block needs
// its includer to define, before the include:
//   DOF_FOCUS:      float dof_load_depth(int2 pixel)        the main depth, reversed, clamped to the render size
//   DOF_PREFILTER:  float3 dof_load_scene(int2 pixel), dof_load_depth
//   DOF_TILES:      float4 dof_load_source(int2 pixel)      the prefilter's level 0: rgb and the green CoC, clamped to its size
//   DOF_DILATE:     float2 dof_load_tile(int2 tile)         the tiles' far and near maxima, clamped to the tile count
//   DOF_GATHER:     dof_load_source, float3 dof_sample_source(float2 at, float lod), at in half-size pixels (centers at
//                   .5), bilinear between the prefilter's levels; float2 dof_load_dilated(int2 tile)
//   DOF_POST:       float4 dof_load_far(int2 pixel), float4 dof_load_near(int2 pixel), the gather's, clamped
//   DOF_COMPOSITE:  dof_load_scene, dof_load_depth, dof_load_far, dof_load_near (the post-filter's)
//   and every pass but DOF_FOCUS: float dof_load_focus()    the focus distance in meters the autofocus draw wrote
// and LOOP and UNROLL for HLSL's [loop] and [unroll].
#include "cat_eye.hlsli"

static const float kDofPi = 3.14159265f;
// Axial chromatic aberration at chromatic 1: red and blue focus this much farther than green, as a share of the focus
// distance, which shifts their CoC by that share of the CoC at infinity. Green then has the largest disc behind the focus
// plane and the smallest in front, so bokeh fringes green behind focus and magenta in front, as achromats' do.
static const float kDofChromaticRed = 0.04f;
static const float kDofChromaticBlue = 0.02f;
// The CoC radius in half-size pixels below which a point is in focus: half a full-size pixel.
static const float kDofInFocus = 0.25f;
// How much swirl at 1 compresses a disc's radial axis at the picture's corners.
static const float kDofSwirl = 1.5f;

// The lens at one focus distance: each channel's CoC at infinity in half-size pixels, and its focus over green's.
struct DofLens
{
    float3 scale;
    float3 ratio;
    float max_radius;
};

// focal and sensor sizes in millimeters, focus in meters; pixels_per_mm is half-size pixels per millimeter of sensor.
DofLens dof_lens_at(float focal, float f_number, float focus, float pixels_per_mm, float chromatic, float max_radius)
{
    DofLens l;
    l.ratio = float3(1.0f + chromatic * kDofChromaticRed, 1.0f, 1.0f + chromatic * kDofChromaticBlue);
    float s = focus * 1000.0f;
    float k = focal * focal / f_number * 0.5f * pixels_per_mm;
    l.scale = float3(k / max(s * l.ratio.x - focal, 0.001f), k / max(s - focal, 0.001f), k / max(s * l.ratio.z - focal, 0.001f));
    l.max_radius = max_radius;
    return l;
}

// The green CoC of a point at 1 / inverse_depth meters: (f^2 / (N (S - f))) (z - S) / z / 2 on the sensor.
float dof_coc(float inverse_depth, float focus, DofLens l) { return l.scale.y * (1.0f - focus * inverse_depth); }

// Each channel's CoC from green's, through S / z = 1 - c / scale, so no depth is needed. Not clamped.
float3 dof_channel_coc(float coc, DofLens l)
{
    float rest = 1.0f - coc / l.scale.y;
    return float3(l.scale.x * (1.0f - l.ratio.x * rest), coc, l.scale.z * (1.0f - l.ratio.z * rest));
}

float dof_clamp_coc(float coc, DofLens l) { return clamp(coc, -l.max_radius, l.max_radius); }

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

// What one source of each channel's radius r gives a receiver at shape distance d, per area unit of the source plane:
// coverage times the light across the disc over the disc's area, times scale.
float3 dof_weights(float3 r, float d, float scale, float aberration, float norm)
{
    float3 w;
    w.x = saturate(r.x - d + 0.5f) * dof_bokeh_intensity(min(d / r.x, 1.0f), aberration, norm) / (kDofPi * r.x * r.x);
    w.y = saturate(r.y - d + 0.5f) * dof_bokeh_intensity(min(d / r.y, 1.0f), aberration, norm) / (kDofPi * r.y * r.y);
    w.z = saturate(r.z - d + 0.5f) * dof_bokeh_intensity(min(d / r.z, 1.0f), aberration, norm) / (kDofPi * r.z * r.z);
    return w * scale;
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

struct DofFields
{
    float4 far;     // rgb the far field's weighted mean, a its green weight
    float4 near;    // rgb premultiplied, a its coverage
};

#ifdef DOF_FOCUS
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

// The focus distance this frame: the setting's, or the autofocus eased from previous toward its target by dof_focus.w,
// and at once when previous is no distance yet.
float dof_focus_next(float previous)
{
    if (dof_focus.x > 0.0f)
        return dof_focus.x;
    float target = dof_focus_target();
    if (!(previous > 0.0f && previous < 100000.0f))
        return target;
    return lerp(previous, target, dof_focus.w);
}
#endif

#ifdef DOF_PREFILTER
// A half-size pixel: the mean of its 2x2 full-size pixels, and the green CoC of the nearest of them, so a thin
// foreground does not vanish into the background's CoC.
float4 dof_prefilter(int2 h)
{
    int2 p = int2(h.x * 2, h.y * 2);
    float3 color = (dof_load_scene(p) + dof_load_scene(int2(p.x + 1, p.y)) + dof_load_scene(int2(p.x, p.y + 1)) + dof_load_scene(int2(p.x + 1, p.y + 1))) * 0.25f;
    float d = max(max(dof_load_depth(p), dof_load_depth(int2(p.x + 1, p.y))), max(dof_load_depth(int2(p.x, p.y + 1)), dof_load_depth(int2(p.x + 1, p.y + 1))));
    float focus = dof_load_focus();
    float coc = dof_coc(dof_inverse_depth(d, dof_depth), focus, dof_frame_lens(focus));
    return float4(color.x, color.y, color.z, clamp(coc, -10000.0f, 10000.0f));
}
#endif

#ifdef DOF_TILES
// A tile's largest far CoC and largest near |CoC| over its 8x8 half-size pixels, clamped to the largest radius.
float2 dof_tile(int2 tile)
{
    float far = 0.0f, near = 0.0f;
    LOOP for (int y = 0; y < 8; y++)
        UNROLL for (int x = 0; x < 8; x++)
        {
            float c = dof_load_source(int2(tile.x * 8 + x, tile.y * 8 + y)).w;
            far = max(far, c);
            near = max(near, -c);
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
// far field, normalized, and a source deeper than a pixel behind focus spreads no wider than that pixel's own disc, so
// the background does not spill over a nearer blurred surface. Sources in front go to the near field, premultiplied,
// whose coverage is their summed weight. At a pixel in front of focus the far field is what shows through its blurred
// edge, so there every source not in front spreads at least as wide as the pixel's own disc: the background around an
// out-of-focus foreground fills in behind it.
DofFields dof_gather(int2 p)
{
    DofFields o;
    o.far = float4(0.0f, 0.0f, 0.0f, 0.0f);
    o.near = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float2 reach = dof_load_dilated(int2(p.x / 8, p.y / 8));
    float radius = max(reach.x, reach.y) * dof_bokeh.w;
    if (radius < 0.5f)
        return o;
    float focus = dof_load_focus();
    DofLens lens = dof_frame_lens(focus);
    DofShape shape = dof_frame_shape();
    float own = dof_load_source(p).w;
    float3 own_c = dof_channel_coc(own, lens);
    float3 own_far = own > 0.0f ? float3(max(dof_clamp_coc(own_c.x, lens), 0.5f), max(dof_clamp_coc(own_c.y, lens), 0.5f), max(dof_clamp_coc(own_c.z, lens), 0.5f))
                                : float3(100000.0f, 100000.0f, 100000.0f);
    float own_near = own < -kDofInFocus ? min(-own, lens.max_radius) : 0.0f;
    int rings = int(dof_sampling.x);
    int count = 4 * rings * (rings + 1) + 1;
    float spacing = radius / float(rings);
    float reach_px = radius + 0.5f * spacing;
    float area = kDofPi * reach_px * reach_px / float(count);
    float turn = dof_sampling.y + dof_noise(p);
    float up = dof_sampling.z;
    float2 half_size = float2(float(dof_sizes.x), float(dof_sizes.y));
    float3 far_sum = float3(0.0f, 0.0f, 0.0f), far_w = float3(0.0f, 0.0f, 0.0f);
    float3 near_sum = float3(0.0f, 0.0f, 0.0f), near_w = float3(0.0f, 0.0f, 0.0f);
    LOOP for (int i = 0; i < count; i++)
    {
        // The golden angle apart, at radii that split the disc into equal areas, so every sample has its own radius: rings
        // put every pixel's samples at the same few radii, which blurs an edge into steps a ring apart.
        float angle = 2.0f * kDofPi * (turn + float(i) * 0.38196601f);
        float2 o_px = float2(cos(angle), sin(angle)) * (reach_px * sqrt((float(i) + 0.5f) / float(count)));
        float2 q = float2(float(p.x) + 0.5f + o_px.x, float(p.y) + 0.5f + o_px.y);
        float c = dof_load_source(int2(int(floor(q.x)), int(floor(q.y)))).w;
        float3 cq = dof_channel_coc(c, lens);
        float3 r = float3(max(abs(dof_clamp_coc(cq.x, lens)), 0.5f), max(abs(dof_clamp_coc(cq.y, lens)), 0.5f), max(abs(dof_clamp_coc(cq.z, lens)), 0.5f));
        // A source within half a full-size pixel of focus belongs to neither field, the composite keeping it sharp, as its
        // small disc's weight would swamp the fields' wider ones; but it fills in behind a pixel in front of focus.
        if (abs(c) <= kDofInFocus && own_near == 0.0f)
            continue;
        bool behind = c > -kDofInFocus;
        if (behind && c > own)
            r = float3(min(r.x, own_far.x), min(r.y, own_far.y), min(r.z, own_far.z));
        if (behind)
            r = float3(max(r.x, own_near), max(r.y, own_near), max(r.z, own_near));
        // Where the pixel lies in the source's disc, y up; a source in front of focus shows the aperture mirrored.
        float2 u = behind ? float2(-o_px.x, -o_px.y * up) : float2(o_px.x, o_px.y * up);
        float2 ndc = float2(q.x / half_size.x * 2.0f - 1.0f, up * (q.y / half_size.y * 2.0f - 1.0f));
        float d = dof_shape_distance(u, ndc, shape);
        if (d > max(max(r.x, r.y), r.z) + 0.5f)
            continue;
        float scale = dof_cat_eye_cut(u, r.y, ndc, shape) * dof_swirl_density(ndc, shape) * area;
        float3 w = dof_weights(r, d, scale, behind ? dof_bokeh.x : -dof_bokeh.x, behind ? dof_bokeh.y : dof_bokeh.z);
        float3 color = dof_sample_source(q, log2(clamp(min(spacing, r.y), 1.0f, 256.0f)));
        if (behind)
        {
            far_sum = far_sum + color * w;
            far_w = far_w + w;
        }
        else
        {
            near_sum = near_sum + color * w;
            near_w = near_w + w;
        }
    }
    if (far_w.y > 0.0f)
        o.far = float4(far_w.x > 0.0f ? far_sum.x / far_w.x : 0.0f, far_sum.y / far_w.y, far_w.z > 0.0f ? far_sum.z / far_w.z : 0.0f, far_w.y);
    o.near = float4(near_sum.x / max(near_w.x, 1.0f), near_sum.y / max(near_w.y, 1.0f), near_sum.z / max(near_w.z, 1.0f), min(near_w.y, 1.0f));
    return o;
}
#endif

#ifdef DOF_POST
// The median of nine values, by Smith's exchange network of 19 min-max pairs.
float dof_median9(float a0, float a1, float a2, float a3, float a4, float a5, float a6, float a7, float a8)
{
    float t;
    t = min(a1, a2); a2 = max(a1, a2); a1 = t;
    t = min(a4, a5); a5 = max(a4, a5); a4 = t;
    t = min(a7, a8); a8 = max(a7, a8); a7 = t;
    t = min(a0, a1); a1 = max(a0, a1); a0 = t;
    t = min(a3, a4); a4 = max(a3, a4); a3 = t;
    t = min(a6, a7); a7 = max(a6, a7); a6 = t;
    t = min(a1, a2); a2 = max(a1, a2); a1 = t;
    t = min(a4, a5); a5 = max(a4, a5); a4 = t;
    t = min(a7, a8); a8 = max(a7, a8); a7 = t;
    a3 = max(a0, a3);
    a5 = min(a5, a8);
    t = min(a4, a7); a7 = max(a4, a7); a4 = t;
    a6 = max(a3, a6);
    a4 = max(a1, a4);
    a2 = min(a2, a5);
    a4 = min(a4, a7);
    t = min(a4, a2); a2 = max(a4, a2); a4 = t;
    a4 = max(a6, a4);
    return min(a4, a2);
}

// The gather's sampling holes filled: each channel of each field the median of its 3x3 neighborhood, where the far
// field's neighbors without weight take the pixel's own value.
DofFields dof_post(int2 p)
{
    float4 f[9];
    float4 n[9];
    float4 own = dof_load_far(p);
    UNROLL for (int i = 0; i < 9; i++)
    {
        int2 q = int2(p.x + i % 3 - 1, p.y + i / 3 - 1);
        f[i] = dof_load_far(q);
        if (f[i].w <= 0.0f)
            f[i] = own;
        n[i] = dof_load_near(q);
    }
    DofFields o;
    o.far = float4(dof_median9(f[0].x, f[1].x, f[2].x, f[3].x, f[4].x, f[5].x, f[6].x, f[7].x, f[8].x),
        dof_median9(f[0].y, f[1].y, f[2].y, f[3].y, f[4].y, f[5].y, f[6].y, f[7].y, f[8].y),
        dof_median9(f[0].z, f[1].z, f[2].z, f[3].z, f[4].z, f[5].z, f[6].z, f[7].z, f[8].z),
        dof_median9(f[0].w, f[1].w, f[2].w, f[3].w, f[4].w, f[5].w, f[6].w, f[7].w, f[8].w));
    o.near = float4(dof_median9(n[0].x, n[1].x, n[2].x, n[3].x, n[4].x, n[5].x, n[6].x, n[7].x, n[8].x),
        dof_median9(n[0].y, n[1].y, n[2].y, n[3].y, n[4].y, n[5].y, n[6].y, n[7].y, n[8].y),
        dof_median9(n[0].z, n[1].z, n[2].z, n[3].z, n[4].z, n[5].z, n[6].z, n[7].z, n[8].z),
        dof_median9(n[0].w, n[1].w, n[2].w, n[3].w, n[4].w, n[5].w, n[6].w, n[7].w, n[8].w));
    return o;
}
#endif

#ifdef DOF_COMPOSITE
// A full-size pixel: its sharp color while its own green CoC is under half a pixel, the far field blended in above that
// (bilinear over the half-size texels with weight), and the near field over both by its coverage.
float3 dof_composite(int2 pixel)
{
    float3 sharp = dof_load_scene(pixel);
    float focus = dof_load_focus();
    float coc = 2.0f * dof_coc(dof_inverse_depth(dof_load_depth(pixel), dof_depth), focus, dof_frame_lens(focus));
    float2 at = float2((float(pixel.x) + 0.5f) * 0.5f - 0.5f, (float(pixel.y) + 0.5f) * 0.5f - 0.5f);
    int2 base = int2(int(floor(at.x)), int(floor(at.y)));
    float fx = at.x - floor(at.x), fy = at.y - floor(at.y);
    float3 far = float3(0.0f, 0.0f, 0.0f);
    float far_weight = 0.0f;
    float4 near = float4(0.0f, 0.0f, 0.0f, 0.0f);
    UNROLL for (int i = 0; i < 4; i++)
    {
        int2 q = int2(base.x + i % 2, base.y + i / 2);
        float b = (i % 2 == 0 ? 1.0f - fx : fx) * (i / 2 == 0 ? 1.0f - fy : fy);
        float4 f = dof_load_far(q);
        float w = f.w > 0.0f ? b : 0.0f;
        far = far + float3(f.x, f.y, f.z) * w;
        far_weight = far_weight + w;
        near = near + dof_load_near(q) * b;
    }
    float t = saturate(abs(coc) - 0.5f);
    float3 color = far_weight > 0.0f ? sharp + (far / far_weight - sharp) * t : sharp;
    return color * (1.0f - near.w) + float3(near.x, near.y, near.z);
}
#endif
