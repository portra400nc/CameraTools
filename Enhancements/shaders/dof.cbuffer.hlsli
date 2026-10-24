// The depth of field's constants at b1, core/dof.h's DofConstants, shared by the dof_*_ps.hlsl draws. Registers stay
// inside gpu.h's ranges: t0-t7, s0-s3, b0-b4.

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
};

#define LOOP [loop]
#define UNROLL [unroll]
