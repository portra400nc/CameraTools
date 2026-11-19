// Surface detail's mask: a rock draw drawn again with the game's vertex shader and this pixel shader in place of its own,
// with an equal depth test, so it writes where the rock shows at that moment. It takes the rock's world position from
// the vertex shader's TEXCOORD3 to TEXCOORD5 .w, the rows of Unity's tangent-to-world matrix with the position after
// them, so the detail sits on the rock exactly as the game drew it this frame, and its view depth, by which the detail
// pass tells rock that later draws covered. Its inputs repeat the vertex shader's outputs up to TEXCOORD5 in the same
// registers, as D3D11 links the stages: WITH_TEXCOORD2 for the layout with TEXCOORD2 in register 4 (core/dxbc.h,
// world_position_layout 1), without it for the one without (2).
struct Rock
{
    float4 position : SV_Position;
    float4 uv : TEXCOORD0;
    float4 color : COLOR0;
    float4 screen : TEXCOORD1;
#ifdef WITH_TEXCOORD2
    float4 uv2 : TEXCOORD2;
#endif
    float4 row0 : TEXCOORD3;
    float4 row1 : TEXCOORD4;
    float4 row2 : TEXCOORD5;
};

float4 main(Rock rock) : SV_Target0
{
    return float4(rock.row0.w, rock.row1.w, rock.row2.w, rock.position.w);
}
