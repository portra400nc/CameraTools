// The D3D11 side. Render thread only. Everything here works on the native immediate context, which bypasses ReShade's
// hooks, so our own calls never come back to us as events.
//
// The mirror rule: the add-on never creates a view on a resource it does not own. Game data comes in and goes out
// through CopyResource between the game's resource and a Mirror with the same description.
#pragma once

#include "core/compare.h"
#include "core/plan.h"

#include <d3d11_1.h>
#include <wrl/client.h>

namespace photoreal
{
    using Microsoft::WRL::ComPtr;

    // Our shaders declare registers only inside these ranges, and StateGuard saves and restores exactly these.
    constexpr UINT kSrvSlots = 8;       // t0..t7
    constexpr UINT kSamplerSlots = 2;   // s0..s1
    constexpr UINT kCbSlots = 2;        // b0..b1
    constexpr UINT kUavSlots = 2;       // u0..u1, compute only (no v1 pass uses compute; the path tracer will)
    constexpr UINT kSoSlots = D3D11_SO_BUFFER_SLOT_COUNT;
    constexpr UINT kClassInstances = 256;

    // Captures, on construction, the pipeline state our work can change, nulls the stages we never use (GS, HS, DS and
    // stream output), and puts everything back on destruction: IA input layout and topology; VS/PS/GS/HS/DS/CS with
    // class instances; VS/PS/CS SRVs, samplers, and constant buffers through *GetConstantBuffers1 so first-constant
    // offsets survive; CS UAVs; stream-output targets; RTVs and DSV; all viewports and scissor rects; blend state,
    // factor, sample mask; depth-stencil state and stencil ref; rasterizer state.
    //
    // The tripwire: render target 0, the depth view and the pixel shader are sampled before the capture and after the
    // restore. If any differs, moved names it, and the shell disarms the add-on.
    class StateGuard
    {
    public:
        StateGuard(ID3D11DeviceContext1 *context, const char *&moved);
        ~StateGuard();
        StateGuard(const StateGuard &) = delete;
        StateGuard &operator=(const StateGuard &) = delete;

    private:
        struct Stage
        {
            ID3D11ShaderResourceView *srvs[kSrvSlots] = {};
            ID3D11SamplerState *samplers[kSamplerSlots] = {};
            ID3D11Buffer *cbs[kCbSlots] = {};
            UINT cb_first[kCbSlots] = {}, cb_count[kCbSlots] = {};
        };
        template <class Shader>
        struct Bound
        {
            Shader *shader = nullptr;
            ID3D11ClassInstance *instances[kClassInstances] = {};
            UINT count = kClassInstances;
        };

        ID3D11DeviceContext1 *context_;
        const char *&moved_;
        ID3D11RenderTargetView *tripwire_rtv_ = nullptr;
        ID3D11DepthStencilView *tripwire_dsv_ = nullptr;
        ID3D11PixelShader *tripwire_ps_ = nullptr;

        ID3D11InputLayout *input_layout_ = nullptr;
        D3D11_PRIMITIVE_TOPOLOGY topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        Bound<ID3D11VertexShader> vs_;
        Bound<ID3D11PixelShader> ps_;
        Bound<ID3D11GeometryShader> gs_;
        Bound<ID3D11HullShader> hs_;
        Bound<ID3D11DomainShader> ds_;
        Bound<ID3D11ComputeShader> cs_;
        Stage vs_stage_, ps_stage_, cs_stage_;
        ID3D11UnorderedAccessView *cs_uavs_[kUavSlots] = {};
        ID3D11Buffer *so_targets_[kSoSlots] = {};
        ID3D11RenderTargetView *rtvs_[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ID3D11DepthStencilView *dsv_ = nullptr;
        D3D11_VIEWPORT viewports_[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
        UINT viewport_count_ = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        D3D11_RECT scissors_[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
        UINT scissor_count_ = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ID3D11BlendState *blend_ = nullptr;
        FLOAT blend_factor_[4] = {};
        UINT sample_mask_ = 0;
        ID3D11DepthStencilState *depth_stencil_ = nullptr;
        UINT stencil_ref_ = 0;
        ID3D11RasterizerState *rasterizer_ = nullptr;
    };

    // Our texture with a game resource's description, in the typeless family of its format, plus the SRV (and for a
    // render target, RTV) bind flags we need. sync() recreates it when the description changed, which is how
    // render-size changes are followed.
    class Mirror
    {
    public:
        // view_format is the format the game viewed the resource with. false when creation failed; the caller skips its
        // pass and reports PHOTOREAL_ERROR_TEXTURE.
        bool sync(ID3D11Device *device, ID3D11Resource *like, DXGI_FORMAT view_format, bool render_target);
        void copy_from(ID3D11DeviceContext *context, ID3D11Resource *game) const { context->CopyResource(texture_.Get(), game); }
        void copy_to(ID3D11DeviceContext *context, ID3D11Resource *game) const { context->CopyResource(game, texture_.Get()); }

        ID3D11ShaderResourceView *srv() const { return srv_.Get(); }                 // depth: R32_FLOAT_X8X24_TYPELESS
        ID3D11ShaderResourceView *stencil_srv() const { return stencil_srv_.Get(); } // depth only: X32_TYPELESS_G8X24_UINT
        ID3D11RenderTargetView *rtv() const { return rtv_.Get(); }
        Size size() const { return { desc_.Width, desc_.Height }; }

    private:
        D3D11_TEXTURE2D_DESC desc_ {};
        DXGI_FORMAT view_format_ = DXGI_FORMAT_UNKNOWN;
        ComPtr<ID3D11Texture2D> texture_;
        ComPtr<ID3D11ShaderResourceView> srv_, stencil_srv_;
        ComPtr<ID3D11RenderTargetView> rtv_;
    };

    // Our render-size texture with no game counterpart, for results passed between our own draws. sync() recreates it
    // when the size changed.
    class Scratch
    {
    public:
        // false when creation failed; the caller skips its pass and reports PHOTOREAL_ERROR_TEXTURE.
        bool sync(ID3D11Device *device, Size size, DXGI_FORMAT format);
        ID3D11ShaderResourceView *srv() const { return srv_.Get(); }
        ID3D11RenderTargetView *rtv() const { return rtv_.Get(); }

    private:
        Size size_;
        DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
        ComPtr<ID3D11Texture2D> texture_;
        ComPtr<ID3D11ShaderResourceView> srv_;
        ComPtr<ID3D11RenderTargetView> rtv_;
    };

    // A back buffer copy for the comparison capture in a CPU-readable texture, read presents later so the copy is done
    // and mapping it does not wait on the GPU.
    class Staging
    {
    public:
        // false when the back buffer is not 8-bit RGBA or BGRA, is multisampled, or the texture failed.
        bool copy_from(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Resource *back_buffer);
        // Empty when the map failed.
        std::optional<RawFrame> read(ID3D11DeviceContext *context) const;

    private:
        D3D11_TEXTURE2D_DESC desc_ {};
        PixelOrder order_ = PixelOrder::rgba;
        ComPtr<ID3D11Texture2D> texture_;
    };

    // A debug snapshot, decoded by its own view so a view change at a frame boundary cannot misread it.
    // Two exist: pending is filled during a frame, shown is what reshade_present draws. present swaps them, because
    // ReShade's present event (where the frame rolls) fires before reshade_present (where the composite runs).
    struct Snapshot
    {
        View view = View::off;      // off: nothing to show
        bool found = false;         // false with a view: the entry was missing, the composite shows the "missing" pattern
        Mirror image;
    };

    class Gpu
    {
    public:
        // Creates our shaders from the embedded DXBC, the samplers, the constant buffer and the fixed states.
        // false: PHOTOREAL_ERROR_SHADER.
        bool init(ID3D11Device *device, ID3D11DeviceContext *context);
        bool ready() const { return context_ != nullptr; }
        ID3D11Device *device() const { return device_.Get(); }
        ID3D11DeviceContext1 *context() const { return context_.Get(); }

        // The ambient pass: mirror in the irradiance, normals and depth; draw raw occlusion into ao_; draw
        // out = in * level * lerp(1, blurred ao, strength) on world pixels and in elsewhere, where strength is
        // ao_strength, times foliage_ao_strength on grass, vegetation and foliage; copy out over the game's irradiance.
        // Empty when a texture of ours failed.
        std::optional<GameCall> run_ambient(const FrameMap &map, const AmbientSettings &settings, const Camera &camera, bool flip);

        // The contact-shadow pass: mirror in the shadow mask, normals and depth; draw raw sun visibility into contact_ by
        // marching each world pixel toward the sun through the depth buffer; draw out = (min(in.x, lerp(1, blurred
        // visibility, strength)), in.y) on world pixels and in elsewhere; copy out over the game's shadow mask.
        // Empty when a texture of ours failed.
        std::optional<GameCall> run_contact_shadows(const FrameMap &map, const ContactShadowSettings &settings, const Camera &camera, bool flip);

        // The atmosphere pass: mirror in the HDR scene and depth; draw out = in * T + (sky + sun * sun_scatter * phase) *
        // (1 - T) on pixels with depth, where T is the transmittance of the height-fading haze along the view ray, and in
        // on the sky; copy out over the game's HDR scene. Empty when a texture of ours failed.
        std::optional<GameCall> run_atmosphere(const FrameMap &map, const AtmosphereSettings &settings, const Camera &camera, bool flip);

        // The tonemap pass, at the game's tone map draw: copy the draw's constants (b0) into ours; mirror in the HDR scene
        // and the final bloom; draw the tone mapped, encoded image into a mirror of the game's output; copy it over the
        // output, and skip the game's draw. Empty when a texture of ours failed, and the game's draw then runs.
        std::optional<GameCall> run_tonemap(const FrameMap &map, const TonemapSettings &settings);

        // At present: shown = pending, and pending starts empty for next_view (found = false until its step copies it).
        void end_frame(View next_view);

        // At the pending view's snapshot step: copies the entry into pending. false when the copy could not be made.
        bool snapshot(const Texture &entry);

        // At reshade_present: draws shown over the back buffer through a mirror of it, with the view's name in the top-left
        // corner, then copies the mirror over it.
        // Returns the tripwire's finding, null when state came back intact.
        const char *composite(ID3D11Resource *back_buffer, bool flip);

        // The comparison capture's two copies, one per frame of a variant. slot is 0 or 1.
        bool copy_back_buffer(ID3D11Resource *back_buffer, uint32_t slot) { return copies_[slot].copy_from(device_.Get(), context_.Get(), back_buffer); }
        std::optional<RawFrame> read_back_buffer(uint32_t slot) const { return copies_[slot].read(context_.Get()); }

    private:
        void draw_fullscreen(ID3D11PixelShader *shader, ID3D11ShaderResourceView *const *srvs, UINT srv_count, ID3D11RenderTargetView *target, Size size);
        void upload(const struct Constants &constants);
        // Copies the normals and depth into their mirrors and syncs scratch, the pass's raw result, to the render size.
        // false when a texture of ours failed.
        bool mirror_gbuffer(const FrameMap &map, Scratch &scratch);
        // Copies the game's bound pixel shader constants at b0 into game_tonemap_. false when none are bound or they are
        // too small for the rows the tonemap shader reads.
        bool copy_game_tonemap_constants();

        ComPtr<ID3D11Device> device_;
        ComPtr<ID3D11DeviceContext1> context_;
        ComPtr<ID3D11VertexShader> fullscreen_vs_;
        ComPtr<ID3D11PixelShader> ao_ps_, ambient_ps_, contact_ps_, contact_shadows_ps_, atmosphere_ps_, tonemap_ps_, view_ps_;
        ComPtr<ID3D11SamplerState> point_, linear_;
        ComPtr<ID3D11Buffer> constants_;
        ComPtr<ID3D11Buffer> game_tonemap_;    // the game's tone map constants, copied in at each tonemap pass
        ComPtr<ID3D11BlendState> opaque_;
        ComPtr<ID3D11DepthStencilState> no_depth_;
        ComPtr<ID3D11RasterizerState> no_cull_;
        Mirror irradiance_in_, irradiance_out_, shadow_mask_in_, shadow_mask_out_, scene_in_, scene_out_, bloom_in_, tonemap_out_, normals_, depth_,
            back_buffer_;
        Scratch ao_;                // R8_UNORM raw occlusion, 1 = open
        Scratch contact_;           // R8_UNORM raw sun visibility, 1 = lit
        Snapshot pending_, shown_;
        Staging copies_[2];
    };
}
