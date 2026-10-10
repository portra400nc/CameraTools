// The D3D11 side. Render thread only. Everything here works on the native immediate context, which bypasses ReShade's
// hooks, so our own calls never come back to us as events.
//
// The mirror rule: the add-on never creates a view on a resource it does not own. Game data comes in and goes out
// through CopyResource between the game's resource and a Mirror with the same description.
#pragma once

#include "core/accumulate.h"
#include "core/compare.h"
#include "core/plan.h"

#include <d3d11_1.h>
#include <wrl/client.h>

namespace photoreal
{
    using Microsoft::WRL::ComPtr;

    // Our shaders declare registers only inside these ranges, and StateGuard saves and restores exactly these.
    constexpr UINT kSrvSlots = 8;       // t0..t7
    constexpr UINT kSamplerSlots = 3;   // s0..s2
    constexpr UINT kCbSlots = 5;        // b0..b4
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
        // One texel into a texture of the same format family. The mirror is never a depth-stencil resource, so unlike the
        // game's depth it may be copied from in part.
        void copy_texel_to(ID3D11DeviceContext *context, ID3D11Resource *destination, Pixel at) const
        {
            const D3D11_BOX box = { at.x, at.y, 0, at.x + 1, at.y + 1, 1 };
            context->CopySubresourceRegion(destination, 0, 0, 0, 0, texture_.Get(), 0, &box);
        }

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

        // At the game's shadow-mask draw, before it runs: copies the draw's pixel shader constants b0 to b3 into our own
        // buffers for this frame's sun-shadow pass and into frame's staging buffer for sun_constants, and the atlas into
        // its mirror. false when the draw's buffers are not the sizes kSunCbufferBytes names; then nothing is copied.
        bool gather_sun(const Texture &atlas, uint64_t frame);

        // The newest constants staged one or two frames before frame whose copy the GPU has finished, read without
        // waiting. Empty when neither has finished.
        std::optional<SunCbuffers> sun_constants(uint64_t frame);

        // The sun-shadow pass: mirror in the shadow mask, normals and depth; draw raw sun visibility into sun_ from the
        // atlas and the constants gather_sun copied this frame, with a penumbra that widens with the distance to the
        // blockers; draw out = (blurred visibility, in.y) on world pixels and in elsewhere; copy out over the game's
        // shadow mask. sun gives the cascades in use and the atlas's grid. Empty when gather_sun copied nothing this frame
        // or a texture of ours failed.
        std::optional<GameCall> run_sun_shadows(const FrameMap &map, const SunShadowSettings &settings, const Camera &camera,
            const SunShadowConstants &sun, bool flip);

        // The contact-shadow pass: mirror in the shadow mask, normals and depth; draw raw sun visibility into contact_ by
        // marching each world pixel toward the sun through the depth buffer; draw out = (min(in.x, lerp(1, blurred
        // visibility, strength)), in.y) on world pixels and in elsewhere; copy out over the game's shadow mask.
        // Empty when a texture of ours failed.
        std::optional<GameCall> run_contact_shadows(const FrameMap &map, const ContactShadowSettings &settings, const Camera &camera, bool flip);

        // The leaves pass: mirror in the HDR scene, shadow mask, normals, albedo, material id and depth; add the sun light let
        // through leaves and grass toward the camera into the scene's mirror; copy it over the game's HDR scene. Empty when
        // a texture of ours failed.
        std::optional<GameCall> run_leaves(const FrameMap &map, const LeavesSettings &settings, const Camera &camera, bool flip);

        // The wetness pass, at the first draw that reads the finished G-buffer: mirror in the albedo, specular, smoothness,
        // material id, normals and depth; draw them wet, by amount, into mirrors of the albedo, specular and smoothness on
        // world pixels and as they were elsewhere; copy those over the game's. Does nothing while amount is 0. Empty when a
        // texture of ours failed.
        std::optional<GameCall> run_wetness(const FrameMap &map, const WetnessSettings &settings, float amount, const Camera &camera, bool flip);

        // The atmosphere pass: mirror in the HDR scene and depth; draw out = in * T + (sky + sun * sun_scatter * phase) *
        // (1 - T) on pixels with depth, where T is the transmittance of the height-fading haze along the view ray, and in
        // on the sky; copy out over the game's HDR scene. Empty when a texture of ours failed.
        std::optional<GameCall> run_atmosphere(const FrameMap &map, const AtmosphereSettings &settings, const Camera &camera, bool flip);

        // The tonemap pass, at the game's tone map draw: copy the draw's constants (b0) into ours; mirror in the HDR scene
        // and the final bloom; draw the tone mapped, encoded image into a mirror of the game's output; copy it over the
        // output, and skip the game's draw. Empty when a texture of ours failed, and the game's draw then runs.
        std::optional<GameCall> run_tonemap(const FrameMap &map, const TonemapSettings &settings);

        // The accumulator at the bloom step, in tick's order: clear empties sum_; add mirrors in the HDR scene and draws it
        // times the cat's-eye weight of lens, with the weight in alpha, additively into sum_; present draws sum.rgb / sum.a
        // where sum.a > 0, and the scene elsewhere, and copies it over the game's HDR scene. Empty when a texture of ours
        // failed; then the sum is not to be trusted.
        std::optional<GameCall> run_accumulate(const FrameMap &map, const AccumulateTick &tick, const AccumulateSettings &settings,
            const LensSample &lens, bool flip);

        // At the bloom step: copies the main depth's texel at into a CPU-readable texture for read_depth_texel. false when
        // a copy is still unread or a texture of ours failed.
        bool copy_depth_texel(const FrameMap &map, Pixel at);

        // The reversed depth copy_depth_texel copied, once the GPU has finished the copy, read without waiting. Empty while
        // it has not, or when nothing was copied.
        std::optional<float> read_depth_texel();

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
        void draw_fullscreen(ID3D11PixelShader *shader, ID3D11ShaderResourceView *const *srvs, UINT srv_count, ID3D11RenderTargetView *target, Size size,
            ID3D11BlendState *blend = nullptr);
        void draw_fullscreen(ID3D11PixelShader *shader, ID3D11ShaderResourceView *const *srvs, UINT srv_count, ID3D11RenderTargetView *const *targets,
            UINT target_count, Size size, ID3D11BlendState *blend = nullptr);
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
        ComPtr<ID3D11PixelShader> ao_ps_, ambient_ps_, contact_ps_, contact_shadows_ps_, sun_ps_, sun_shadows_ps_, leaves_ps_, atmosphere_ps_,
            tonemap_ps_, view_ps_, accumulate_ps_, present_ps_, wetness_ps_;
        ComPtr<ID3D11SamplerState> point_, linear_, lit_compare_;
        ComPtr<ID3D11Buffer> constants_;
        ComPtr<ID3D11Buffer> game_tonemap_;    // the game's tone map constants, copied in at each tonemap pass
        ComPtr<ID3D11Buffer> game_sun_[4];     // the game's b0 to b3 at its shadow-mask draw, bound at b1 to b4
        // Three, so a frame reads one of the two before it while it writes its own.
        struct SunStage
        {
            ComPtr<ID3D11Buffer> buffer;        // b0 to b3 back to back, CPU-readable
            uint64_t frame = 0;
            bool pending = false;               // copied and not yet read
        };
        std::array<SunStage, 3> sun_stages_;
        bool sun_ready_ = false;                // gather_sun copied the constants and the atlas this frame
        ComPtr<ID3D11BlendState> opaque_, additive_;
        ComPtr<ID3D11DepthStencilState> no_depth_;
        ComPtr<ID3D11RasterizerState> no_cull_;
        Mirror irradiance_in_, irradiance_out_, shadow_mask_in_, shadow_mask_out_, scene_in_, scene_out_, bloom_in_, tonemap_out_, normals_, depth_,
            back_buffer_, sun_atlas_, albedo_, material_id_, albedo_out_, specular_in_, specular_out_, smoothness_in_, smoothness_out_;
        Scratch ao_;                // R8_UNORM raw occlusion, 1 = open
        Scratch contact_;           // R8_UNORM raw sun visibility, 1 = lit
        Scratch sun_;               // R8_UNORM raw soft sun visibility, 1 = lit
        Scratch sum_;               // R32G32B32A32_FLOAT: rgb the weighted HDR sum, a the weight sum
        ComPtr<ID3D11Texture2D> depth_texel_;   // 1x1 R32G8X24_TYPELESS, CPU-readable
        bool depth_texel_pending_ = false;      // copied and not yet read
        Snapshot pending_, shown_;
        Staging copies_[2];
    };
}
