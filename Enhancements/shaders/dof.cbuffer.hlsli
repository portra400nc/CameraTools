// The depth of field's constants at b1, core/dof.h's DofConstants, shared by the dof_*.hlsl shaders. Registers stay
// inside gpu.h's ranges: t0-t7, s0-s3, b0-b4, u0-u3.

cbuffer Dof : register(b1)
{
    float4 dof_optics;      // focal length in mm, f-number, half-size pixels per mm of sensor, largest CoC radius in half-size pixels
    float4 dof_focus;       // focus distance in meters (0: autofocus), the focus point's stored uv, the autofocus's step this frame
    float4 dof_depth;       // 1 / view depth of a reversed depth d: -(x d + y) / (z d + w)
    float4 dof_shape;       // blades (0 round), blade roundness, blade rotation in radians, area scale
    float4 dof_lens;        // anamorphic squeeze, cat's eye, swirl, chromatic aberration
    float4 dof_bokeh;       // spherical aberration, its normalization behind and in front of focus, the gather radius's margin
    float4 dof_sampling;    // rings, the frame's turn, +1 when stored rows run up the screen (flip) else -1, width over height
    uint4 dof_sizes;        // the half size, and the tile count
    uint4 dof_counts;       // tiles the dilation reaches, the render size, pixels between the autofocus's depth samples
    float4 dof_highlight;   // the highlights' threshold over the frame's mean luminance, gain, the sprites a field may draw
                            // (0: no scatter), and the near field's first slot
    float4 dof_contrast;    // the bokeh contrast's lift's power (0: off), and the share of the light it may move
    float4 dof_display;     // on the display: the main depth's size, 1 when its rows run up the screen, 1 when the back buffer
                            // holds sRGB-encoded values (0: linear); all 0 in the scene
    float4 dof_eye;         // the eye's center in meters along the view axis (0: no eye), and the pixels between its window's samples
};

#define LOOP [loop]
#define UNROLL [unroll]
