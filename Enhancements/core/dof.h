// The depth of field: its constants as the shaders read them, and shaders/dof.hlsli compiled as C++ (core/dof.cpp), so the
// tests check the shaders' own circles of confusion, aperture shape and light across the disc, and the Mac's harness runs
// their passes; and the frames EnhancementsSaveFrame writes for it. Pure C++17 like plan.h. The GPU side is in gpu.cpp.
#pragma once

#include "plan.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace enhancements
{
    // The lens as dof.hlsli's dof_lens_at takes it.
    struct DofOptics
    {
        float focal_mm = 50;
        float f_number = 2;
        float pixels_per_mm = 1;    // half-size pixels per millimeter of sensor: render height / 2 / sensor height
        float chromatic = 0;
        float max_radius = 1e9f;    // half-size pixels
    };

    // The aperture as dof.hlsli's DofShape holds it, before the area scale.
    struct DofShapeParams
    {
        uint32_t blades = 0;        // 0 round, else 3 or more
        float roundness = 0;
        float rotation_degrees = 0;
        float anamorphic = 1;
        float cat_eye = 0;
        float swirl = 0;
        float aspect = 1.6f;
    };

    // The signed CoC radius of each channel, red, green and blue, in half-size pixels, of a point depth meters away with
    // the lens focused at focus meters, through dof_channel_coc from green's as the passes take it. Not clamped.
    std::array<float, 3> dof_channel_cocs(float depth, float focus, const DofOptics &optics);

    // The green CoC straight from dof_coc.
    float dof_green_coc(float depth, float focus, const DofOptics &optics);

    // How much of a source of radius r half-size pixels at ndc (y up) reaches a receiver u pixels from its center (y up):
    // dof_aperture, with the shape's area scale.
    float dof_aperture(float ux, float uy, float r, float ndc_x, float ndc_y, const DofShapeParams &shape);

    // dof_bokeh_intensity at rho with aberration (the setting's behind focus, its negative in front) and its
    // normalization.
    float dof_bokeh_intensity(float rho, float aberration);

    // dof_channel_weight: what a source of radius r half-size pixels gives a receiver d pixels from its center, per area
    // unit, with aberration (the setting's behind focus, its negative in front).
    float dof_source_weight(float r, float d, float aberration);

    // 1 over the mean of 1 + aberration (3 rho^4 - 1) clamped at 0 over the unit disc: 1 for aberration from -0.5 up.
    float dof_bokeh_norm(float aberration);

    // dof_grow_highlight: a sprite's luminance grown at gain from the contrast of its 3x3 texels to their surroundings, for
    // a threshold in the scene's units and a disc of radius half-size pixels.
    float dof_grow_highlight(float luminance, float contrast, float threshold, float gain, float radius);

    // The shape's area over the unit disc's, square-rooted: the blades, their roundness and the anamorphic squeeze.
    float dof_area_scale(uint32_t blades, float roundness, float anamorphic);

    // The farthest the shape reaches from its center, in units of the radius, after the area scale: the gather's radius
    // grows by it.
    float dof_shape_reach(uint32_t blades, float roundness, float anamorphic);

    // The coefficients that take a reversed depth d to 1 / view depth, -(x d + y) / (z d + w), from the inverse of the
    // camera's projection. Empty when it does not invert.
    std::optional<std::array<float, 4>> dof_depth_coefficients(const Camera &camera);

    // The view depth in meters of a reversed depth through those coefficients: the near plane at 1, the far plane at 0.
    float dof_view_depth(const std::array<float, 4> &coefficients, float reversed);

    // A frame as EnhancementsSaveFrame keeps it: the HDR scene and the main depth at the bloom step before any pass of
    // ours, and the camera that drew it.
    struct FrameSave
    {
        Size size;                      // the render size; each image is size's pixels, row 0 first as stored
        bool flip = true;               // stored rows run up the screen
        uint64_t frame = 0;             // the add-on's frame count
        Camera camera {};               // the pushed camera of the frame
        std::vector<uint32_t> hdr;      // R11G11B10_FLOAT as stored
        std::vector<float> depth;       // reversed depth as stored
    };

    // frame.tsv's text: the size, flip, frame, the camera's matrices, and its near and far planes.
    std::string frame_tsv(const FrameSave &save);

    // hdr.r11g11b10, depth.r32 and frame.tsv. Creates the folder. Returns the names of the files that could not be written.
    std::vector<std::string> write_frame_folder(const std::filesystem::path &folder, const FrameSave &save);

    // The folder back. Empty, with why, when a file is missing or of the wrong size.
    std::optional<FrameSave> read_frame_folder(const std::filesystem::path &folder, std::string &why);

    // shaders/dof.cbuffer.hlsli's cbuffer Dof at b1: 160 bytes.
    struct DofConstants
    {
        float optics[4];    // focal length in mm, f-number, half-size pixels per mm of sensor, largest CoC radius in half-size pixels
        float focus[4];     // focus distance in meters (0: autofocus), the focus point's stored uv, the autofocus's step this frame
        float depth[4];     // dof_depth_coefficients
        float shape[4];     // blades, blade roundness, blade rotation in radians, dof_area_scale
        float lens[4];      // anamorphic, cat's eye, swirl, chromatic
        float bokeh[4];     // spherical aberration, dof_bokeh_norm behind and in front of focus, the gather radius's margin
        float sampling[4];  // rings, the frame's turn of them, +1 when stored rows run up the screen else -1, width over height
        uint32_t sizes[4];  // the half size, and the tile count
        uint32_t counts[4]; // tiles the dilation reaches, the render size, pixels between the autofocus's depth samples
        float highlight[4]; // the highlights' threshold over the frame's mean luminance, gain, the sprites a field may draw (0: no
                            // scatter), and the near field's first slot in the sprites' buffer
    };
    static_assert(sizeof(DofConstants) == 160);

    // Pixels a tile holds along each side of the half-size picture; shaders/dof.hlsli's tiles say the same.
    constexpr uint32_t kDofTile = 8;

    // The sprites each field's buffer holds, and the half-size pictures' worth of pixels each field's sprites may cover at
    // most: a frame draws no more sprites than both allow at its largest quad, and leaves the rest in the gather.
    constexpr uint32_t kDofSpriteCap = 4096;
    constexpr uint32_t kDofSpriteOverdraw = 8;

    // The k whose 4k (k + 1) + 1 samples, as many as rings of 8k around one, come nearest samples: at least 2, at most 16.
    uint32_t dof_rings(uint32_t samples);

    // The constants of one frame: the focal length from the camera's projection and the sensor, the focus point stored as
    // flip says, the autofocus's step after seconds of real time, and the spiral turned by the frame's count. Empty when
    // the projection does not invert or has no focal length.
    std::optional<DofConstants> dof_constants(const DepthOfFieldSettings &settings, const Camera &camera, Size render, bool flip, float seconds,
        uint64_t frame);

    // A frame as the passes read it: linear RGB, three floats a pixel, and the reversed depth, rows as stored.
    struct DofImage
    {
        Size size;
        std::vector<float> rgb;
        std::vector<float> depth;
    };

    struct DofOutput
    {
        std::vector<float> rgb;     // the frame with depth of field, as stored
        float focus = 0;            // the focus distance the frame used, meters
        float luminance = 0;        // the frame's mean luminance, which scales the highlights' curve
        uint32_t sprites[2] = {};   // the far and the near field's sprites
        uint32_t nonfinite = 0;     // channels of the half-float targets that held infinity or NaN once stored
    };

    // Every pass of shaders/dof.hlsli over the image, as the GPU runs them, on the machine's threads: the autofocus from
    // previous_focus, the prefilter, its highlights' detection into sprites, its levels, the tiles and their dilation, the
    // gather, the post-filter, the sprites added into the fields, and the composite. The sprites take their buffers' slots
    // in row order, where the GPU's take them in the order its threads come. Every half-float target is rounded as R16F
    // stores it, overflow to infinity included.
    DofOutput render_dof(const DofImage &image, const DofConstants &constants, float previous_focus);
}
