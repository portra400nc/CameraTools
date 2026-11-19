// Surface detail's pick: every G-buffer draw of one frame drawn again with its pixel shader's number, so the number under
// the screen's middle names the shader that drew it there.
cbuffer SurfacePick : register(b13)
{
    uint pick_id;
};

uint main(float4 position : SV_Position) : SV_Target0
{
    return pick_id;
}
