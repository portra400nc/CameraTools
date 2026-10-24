// The D3D11 side. Render thread only. Everything here works on the native immediate context, which bypasses ReShade's
// hooks, so our own calls never come back to us as events.
//
// The mirror rule: the add-on never creates a view on a resource it does not own. Game data comes in and goes out
// through CopyResource between the game's resource and a Mirror with the same description.
#pragma once

#include "core/accumulate.h"
#include "core/compare.h"
#include "core/dof.h"
#include "core/grade.h"
#include "core/light.h"
#include "core/plan.h"

#include <d3d11_1.h>
#include <wrl/client.h>

#include <string>
#include <vector>

namespace enhancements
{
    using Microsoft::WRL::ComPtr;

    class TraceGpu;     // trace_gpu.h
    struct TraceFrame;
    struct TraceSave;   // core/trace_files.h

    // Our shaders declare registers only inside these ranges, and StateGuard saves and restores exactly these.
    constexpr UINT kSrvSlots = 8;       // t0..t7; the trace kernel uses t0..t6, the gather kernel t0..t7
    constexpr UINT kSamplerSlots = 4;   // s0..s3
    constexpr UINT kCbSlots = 5;        // b0..b4
    constexpr UINT kUavSlots = 4;       // u0..u3, compute only: the trace kernels' history and counters, and the gather's two buffers
    constexpr UINT kSoSlots = D3D11_SO_BUFFER_SLOT_COUNT;
    constexpr UINT kClassInstances = 256;

    // Captures, on construction, the pipeline state our work can change, nulls the stages we never use (GS, HS, DS and
    // stream output), and puts everything back on destruction: IA input layout and topology; VS/PS/GS/HS/DS/CS with
    // class instances; VS/PS/CS SRVs, samplers, and constant buffers through *GetConstantBuffers1 so first-constant
    // offsets survive; CS UAVs; stream-output targets; RTVs and DSV; all viewports and scissor rects; blend state,
    // factor, sample mask; depth-stencil state and stencil ref; rasterizer state.
    //
    // The tripwire: render target 0, the depth view, the pixel shader and the compute shader are sampled before the
    // capture and after the restore. If any differs, moved names it, and the shell disarms the add-on.
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
        ID3D11ComputeShader *tripwire_cs_ = nullptr;

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
        // pass and reports ENHANCEMENTS_ERROR_TEXTURE.
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
        ID3D11Texture2D *texture() const { return texture_.Get(); }
        Size size() const { return { desc_.Width, desc_.Height }; }

    private:
        D3D11_TEXTURE2D_DESC desc_ {};
        DXGI_FORMAT view_format_ = DXGI_FORMAT_UNKNOWN;
        ComPtr<ID3D11Texture2D> texture_;
        ComPtr<ID3D11ShaderResourceView> srv_, stencil_srv_;
        ComPtr<ID3D11RenderTargetView> rtv_;
    };

    // Our texture with no game counterpart, at the render size or a fraction of it, for results passed between our own
    // draws. sync() recreates it when the size changed.
    class Scratch
    {
    public:
        // false when creation failed; the caller skips its pass and reports ENHANCEMENTS_ERROR_TEXTURE. With mips, the
        // texture has its whole mip chain for GenerateMips, its SRV all of it, and its RTV level 0. With uav, it also has a
        // UAV of level 0 for a compute shader.
        bool sync(ID3D11Device *device, Size size, DXGI_FORMAT format, bool mips = false, bool uav = false);
        ID3D11ShaderResourceView *srv() const { return srv_.Get(); }
        ID3D11RenderTargetView *rtv() const { return rtv_.Get(); }
        ID3D11UnorderedAccessView *uav() const { return uav_.Get(); }
        ID3D11Texture2D *texture() const { return texture_.Get(); }

    private:
        Size size_;
        DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
        bool mips_ = false, uav_wanted_ = false;
        ComPtr<ID3D11Texture2D> texture_;
        ComPtr<ID3D11ShaderResourceView> srv_;
        ComPtr<ID3D11RenderTargetView> rtv_;
        ComPtr<ID3D11UnorderedAccessView> uav_;
    };

    // A back buffer copy for the comparison capture in a CPU-readable texture, read presents later so the copy is done
    // and mapping it does not wait on the GPU.
    class Staging
    {
    public:
        // false when the back buffer is not 8-bit RGBA or BGRA, is multisampled, or the texture failed.
        bool copy_from(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Resource *back_buffer);
        // Empty when the map failed, or with D3D11_MAP_FLAG_DO_NOT_WAIT in flags while the GPU has not finished the copy.
        std::optional<RawFrame> read(ID3D11DeviceContext *context, UINT flags = 0) const;
        bool copied() const { return texture_ != nullptr; }

    private:
        D3D11_TEXTURE2D_DESC desc_ {};
        PixelOrder order_ = PixelOrder::rgba;
        ComPtr<ID3D11Texture2D> texture_;
    };

    // A texture of ours copied into a CPU-readable texture of its description, read presents later so the copy is done
    // and mapping it does not wait on the GPU. Its rows come back packed, top row first.
    class Readback
    {
    public:
        enum class Read : uint8_t { ok, not_ready, failed };

        // false when the format's texel size is unknown or the staging texture could not be made.
        bool copy_from(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *source);
        // not_ready while the GPU has not finished the copy; failed when nothing was copied or the map failed.
        Read read(ID3D11DeviceContext *context, std::vector<uint8_t> &out) const;
        Size size() const { return { desc_.Width, desc_.Height }; }
        bool copied() const { return texture_ != nullptr; }
        void reset() { *this = Readback {}; }

    private:
        D3D11_TEXTURE2D_DESC desc_ {};
        uint32_t texel_bytes_ = 0;
        ComPtr<ID3D11Texture2D> texture_;
    };

    // Bytes a texel of a mirror's typeless format: 0 for a format Readback does not know.
    uint32_t texel_bytes(DXGI_FORMAT format);

    // "Call 0x887A0001", for logs.
    std::string hresult_text(const char *call, HRESULT hr);

    // A debug snapshot, decoded by its own view so a view change at a frame boundary cannot misread it.
    // Two exist: pending is filled during a frame, shown is what reshade_present draws. present swaps them, because
    // ReShade's present event (where the frame rolls) fires before reshade_present (where the composite runs).
    struct Snapshot
    {
        View view = View::off;      // off: nothing to show
        bool found = false;         // false with a view: the entry was missing, the composite shows the "missing" pattern
        std::string why;            // with found false: why, for the label, as the status line says it; empty for "missing"
        Mirror image;
    };

    class Gpu
    {
    public:
        // Creates our shaders from the embedded DXBC, the samplers, the constant buffer and the fixed states.
        // false: ENHANCEMENTS_ERROR_SHADER.
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

        // At the combine step's first draw, before it runs: copies row 0 of the draw's pixel shader constant buffers b0 and
        // b3, the sun's color and shading direction (core/light.h), into frame's staging buffer, and keeps the four
        // buffers' sizes. combine_constants reads the newest the GPU has finished, without waiting, as sun_constants does.
        void gather_combine(uint64_t frame);
        std::optional<CombineConstants> combine_constants(uint64_t frame);

        // The constants gather_sun staged at frame, read without waiting, for the scene capture, which reads its frame's two
        // presents later. Empty while the GPU has not finished the copy, or when frame's are no longer staged: the
        // shadow-mask draw did not match that frame, or a frame since took its stage. sun_staged says which.
        std::optional<SunCbuffers> sun_constants_of(uint64_t frame);
        bool sun_staged(uint64_t frame) const;

        // Our shared fixed states: a blend state that writes no render target and a depth-stencil state with depth and
        // stencil off.
        ID3D11BlendState *no_write() const { return no_write_.Get(); }
        ID3D11DepthStencilState *no_depth() const { return no_depth_.Get(); }

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

        // The wetness pass, at the first draw that reads the finished G-buffer: mirror in the albedo, specular, smoothness,
        // material id, normals and depth; draw them wet, by amount, into mirrors of the albedo, specular and smoothness on
        // world pixels and as they were elsewhere; copy those over the game's. Does nothing while amount is 0. Empty when a
        // texture of ours failed.
        std::optional<GameCall> run_wetness(const FrameMap &map, const WetnessSettings &settings, float amount, const Camera &camera, bool flip);

        // The light-shaft pass: mirror in the HDR scene and depth; march each half-size ray through the mist into shafts_
        // and shafts_meters_, with the sun's visibility at each step from the atlas and the constants gather_sun copied this
        // frame; draw out = in * T + in-scatter at full size from a 4x4 window of the march; copy out over the game's HDR
        // scene. sun gives the cascades in use and the atlas's grid. With view, the in-scatter alone also goes into
        // shafts_view_ for snapshot_light_shafts. Empty when gather_sun copied nothing this frame or a texture of ours failed.
        std::optional<GameCall> run_light_shafts(const FrameMap &map, const LightShaftSettings &settings, const Camera &camera,
            const SunShadowConstants &sun, bool flip, bool view);

        // The atmosphere pass: mirror in the HDR scene and depth; draw out = in * T + (sky + sun * sun_scatter * phase) *
        // (1 - T) on pixels with depth, where T is the transmittance of the height-fading haze along the view ray, and in
        // on the sky; copy out over the game's HDR scene. Empty when a texture of ours failed.
        std::optional<GameCall> run_atmosphere(const FrameMap &map, const AtmosphereSettings &settings, const Camera &camera, bool flip);

        // The depth of field pass, last at the bloom step: mirror in the HDR scene and depth; draw the focus distance into
        // one of two 1x1 textures from the other, the setting's or the autofocus eased over seconds; the half-size source,
        // with scatter its highlights moved into the sprites' buffer by dof_scatter_cs.hlsl, and its mips, with the bokeh
        // contrast its lifted copy and that one's mips, its tiles and their dilation, the gather's far and near fields (the
        // far one's mips with the contrast), their post-filter, the sprites added into those, and the
        // composite at full size; copy it over the game's HDR scene. frame turns the gather's spiral. The focus is copied for
        // read_dof_focus when no copy waits. Empty when the projection has no focal length or a texture of ours failed.
        std::optional<GameCall> run_depth_of_field(const FrameMap &map, const DepthOfFieldSettings &settings, const Camera &camera, bool flip,
            float seconds, uint64_t frame);
        // The focus distance run_depth_of_field last copied, once the GPU has it, read without waiting.
        std::optional<float> read_dof_focus();

        // The pixel shader bound now, as the handle ReShade's pipeline events name it by, or 0.
        uint64_t bound_pixel_shader() const;
        // Copies the grade a tone map shader's layout names from the game's bound pixel shader constants at b0 into
        // game_grade_. Called at the game's tone map draw before any pass of ours draws, since a pass binds its own b0.
        // false when no constants are bound or they are too small for the layout.
        bool copy_game_grade(const GradeLayout &layout);
        // Asks for the grade copy_game_grade just copied, for the log; read_game_grade returns it some frames later.
        void request_game_grade();
        // The grade last asked for, once the GPU has it: the three rows of the color matrix, then exposure, bloom, gamma
        // and an unused float. Empty until then and after it was returned.
        std::optional<std::array<float, 16>> read_game_grade();
        // The tonemap pass, at the game's tone map draw: mirror in the HDR scene and the final bloom; draw the tone mapped,
        // encoded image into a mirror of the game's output; copy it over the output, and skip the game's draw. grade says
        // whether copy_game_grade copied this draw's grade; without it the shader uses its own. bloom says whether the
        // draw's shader adds the bloom. Empty when a texture of ours failed, and the game's draw then runs.
        std::optional<GameCall> run_tonemap(const FrameMap &map, const TonemapSettings &settings, bool grade, bool bloom);

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
        // At the pending view's snapshot step, when it has nothing to show: why, for its label (view_missing_why).
        void note_view_why(std::string why) { pending_.why = std::move(why); }

        // The trace pass, first at the combine step (trace_gpu.h): mirror in the normals, depth, shadow mask and diffuse
        // irradiance; dispatch frame's tiles of the trace kernel over trace's history; then, when frame.traced, draw
        // apply_trace_ps.hlsl's sun visibility into a mirror of the shadow mask (with settings.sun and a sun) and its
        // occlusion times level into a mirror of the irradiance (with settings.ao), and copy them over the game's. With
        // view, the history's averages also go into trace_view_ for snapshot_trace. With save, once the frame is traced, its
        // history, normals, depth and stencil, and the game's mask and irradiance as found, are copied for trace.read_save,
        // with save's other fields, and saved says whether they were. Empty when a texture of ours failed or the trace
        // failed, which then says why.
        // While frame gathers, the occlusion is the gather's: the apply draw writes only the sun, and the gather's draws
        // (gather_average_ps.hlsl, gather_filter_ps.hlsl and apply_gather_ps.hlsl) write the irradiance, and for a gather
        // view its picture into gather_view_. A save then also copies the view's albedo and what the draws wrote.
        std::optional<GameCall> run_trace(const FrameMap &map, const TraceFrame &frame, const TraceSettings &settings, const GatherSettings &gather,
            const Camera &camera, bool flip, View view, const TraceSave *save, bool &saved, TraceGpu &trace);
        // A save while the trace pass does not run, its scene STALE: mirrors in this frame's normals, depth, shadow mask and
        // diffuse irradiance, and copies them with the history as it was for trace.read_save. Copies only; no pipeline
        // state changes. false when a mirror or a copy could not be made, or the map lacks an entry.
        bool save_trace(const FrameMap &map, const TraceSave &save, TraceGpu &trace);
        // At the bloom step, before any pass of ours: the HDR scene into the save the trace copied this frame.
        void save_trace_hdr(const FrameMap &map, TraceGpu &trace);

        // At the bloom step, before any pass of ours: mirrors in the HDR scene and the main depth and copies them for
        // read_frame. false when the map lacks one, a copy waits to be read, or a texture failed.
        bool copy_frame(const FrameMap &map);
        // The copies, once the GPU has them, read without waiting: R11G11B10_FLOAT and the depth's 8 bytes a texel, rows
        // packed, row 0 first. not_ready until then, failed when nothing was copied or a map failed; either way after ok
        // or failed the next copy_frame may copy again.
        Readback::Read read_frame(std::vector<uint8_t> &hdr, std::vector<uint8_t> &depth);

        // At the pending view's snapshot step: copies the entry into pending. false when the copy could not be made.
        bool snapshot(const Texture &entry);
        // The same for the light-shafts view, from what run_light_shafts drew for it this frame.
        bool snapshot_light_shafts();
        // The same for the trace pass's views, from what run_trace drew for view this frame.
        bool snapshot_trace(View view);

        // At reshade_present: draws shown over the back buffer through a mirror of it, with the view's name in the top-left
        // corner, then copies the mirror over it.
        // Returns the tripwire's finding, null when state came back intact.
        const char *composite(ID3D11Resource *back_buffer, bool flip);

        // The comparison capture's two copies, one per frame of a variant. slot is 0 or 1.
        bool copy_back_buffer(ID3D11Resource *back_buffer, uint32_t slot) { return copies_[slot].copy_from(device_.Get(), context_.Get(), back_buffer); }
        std::optional<RawFrame> read_back_buffer(uint32_t slot) const { return copies_[slot].read(context_.Get()); }
        // The view the frame that just ended snapshot, drawn as the composite draws it into a texture of size of our own,
        // and copied for read_view: a comparison variant's view, which the capture's back buffer copies do not show.
        // false, with moved null, when there is no view or a texture failed; moved names a binding the restore missed.
        bool copy_view(Size size, bool flip, const char *&moved);
        std::optional<RawFrame> read_view() const { return copies_[2].read(context_.Get()); }

    private:
        void draw_fullscreen(ID3D11PixelShader *shader, ID3D11ShaderResourceView *const *srvs, UINT srv_count, ID3D11RenderTargetView *target, Size size,
            ID3D11BlendState *blend = nullptr);
        void draw_fullscreen(ID3D11PixelShader *shader, ID3D11ShaderResourceView *const *srvs, UINT srv_count, ID3D11RenderTargetView *const *targets,
            UINT target_count, Size size, ID3D11BlendState *blend = nullptr);
        void upload(const struct Constants &constants);
        // Copies the normals and depth into their mirrors and syncs scratch, the pass's raw result, to the render size.
        // false when a texture of ours failed.
        bool mirror_gbuffer(const FrameMap &map, Scratch &scratch);
        bool mirror_gbuffer(const FrameMap &map);

        ComPtr<ID3D11Device> device_;
        ComPtr<ID3D11DeviceContext1> context_;
        ComPtr<ID3D11VertexShader> fullscreen_vs_, dof_sprite_vs_;
        ComPtr<ID3D11ComputeShader> dof_scatter_cs_;
        ComPtr<ID3D11PixelShader> ao_ps_, ambient_ps_, contact_ps_, contact_shadows_ps_, sun_ps_, sun_shadows_ps_, atmosphere_ps_,
            tonemap_ps_, view_ps_, accumulate_ps_, present_ps_, wetness_ps_, shafts_ps_, light_shafts_ps_, apply_trace_ps_, gather_average_ps_, gather_filter_ps_,
            apply_gather_ps_, dof_focus_ps_, dof_prefilter_ps_, dof_tiles_ps_, dof_dilate_ps_, dof_gather_ps_, dof_post_ps_, dof_composite_ps_,
            dof_sprite_ps_, dof_lift_ps_;
        ComPtr<ID3D11SamplerState> point_, linear_, lit_compare_, wrap_;
        ComPtr<ID3D11Texture3D> lattice_;                   // noise_lattice(), R8_UNORM
        ComPtr<ID3D11ShaderResourceView> lattice_srv_;
        ComPtr<ID3D11Buffer> constants_;
        ComPtr<ID3D11Buffer> gather_apply_;    // core/trace.h's GatherApplyConstants, at b2 for the gather's draws
        ComPtr<ID3D11Buffer> dof_constants_;   // core/dof.h's DofConstants, at b1 for the depth of field's draws
        ComPtr<ID3D11Buffer> game_grade_;      // shaders/tonemap_ps.hlsl's GameGrade, filled by copy_game_grade
        ComPtr<ID3D11Buffer> grade_stage_;     // game_grade_ for the CPU to read
        bool grade_staged_ = false;
        ComPtr<ID3D11Buffer> game_sun_[4];     // the game's b0 to b3 at its shadow-mask draw, bound at b1 to b4
        // Three, so a frame reads one of the two before it while it writes its own.
        struct SunStage
        {
            ComPtr<ID3D11Buffer> buffer;        // b0 to b3 back to back, CPU-readable
            uint64_t frame = 0;
            bool pending = false;               // copied and not yet read
        };
        std::array<SunStage, 3> sun_stages_;
        // The combine draw's row 0 of b0 and b3, 32 bytes, likewise, with the buffers' sizes.
        struct CombineStage
        {
            ComPtr<ID3D11Buffer> buffer;        // CPU-readable
            uint64_t frame = 0;
            bool pending = false;
            std::array<uint32_t, 4> bytes {};
        };
        std::array<CombineStage, 3> combine_stages_;
        std::optional<CombineConstants> combine_;   // the newest read
        bool sun_ready_ = false;                // gather_sun copied the constants and the atlas this frame
        ComPtr<ID3D11BlendState> opaque_, additive_, no_write_;
        ComPtr<ID3D11DepthStencilState> no_depth_;
        ComPtr<ID3D11RasterizerState> no_cull_;
        Mirror irradiance_in_, irradiance_out_, shadow_mask_in_, shadow_mask_out_, scene_in_, scene_out_, bloom_in_, tonemap_out_, normals_, depth_,
            back_buffer_, sun_atlas_, albedo_, material_id_, albedo_out_, specular_in_, specular_out_, smoothness_in_, smoothness_out_;
        Scratch ao_;                // R8_UNORM raw occlusion, 1 = open
        Scratch contact_;           // R8_UNORM raw sun visibility, 1 = lit
        Scratch sun_;               // R8_UNORM raw soft sun visibility, 1 = lit
        Scratch sum_;               // R32G32B32A32_FLOAT: rgb the weighted HDR sum, a the weight sum
        Scratch shafts_;            // half size, R16G16B16A16_FLOAT: rgb the mist's raw in-scatter, a its transmittance
        Scratch shafts_meters_;     // half size, R16_FLOAT: the meters each ray marched
        Scratch shafts_view_;       // R16G16B16A16_FLOAT: the mist's in-scatter at full size, for the light-shafts view
        Scratch trace_view_;        // R8G8B8A8_UNORM: the trace's averages, for the trace view
        // R16G16B16A16_FLOAT: the gather's averages (the bounce and the far term; the sky met and the occlusion), the
        // filter's other pair, and the gather views' picture.
        Scratch gather_bounce_, gather_sky_, gather_bounce_filtered_, gather_sky_filtered_, gather_view_;
        bool gather_drawn_ = false;     // run_trace drew a gather view into gather_view_ this frame
        ComPtr<ID3D11Texture2D> depth_texel_;   // 1x1 R32G8X24_TYPELESS, CPU-readable
        bool depth_texel_pending_ = false;      // copied and not yet read
        Snapshot pending_, shown_;
        Scratch dof_focus_[2];      // 1x1 R32G32_FLOAT: the focus distance in meters and the frame's mean luminance, this frame's and the last's in turn
        uint32_t dof_focus_at_ = 0; // the one this frame writes
        Readback dof_focus_read_;   // the focus for the status
        bool dof_focus_pending_ = false;
        Scratch dof_prefilter_;     // half size, R16G16B16A16_FLOAT: the scene and the green CoC before the scatter
        Scratch dof_source_;        // the same with mips and a UAV of level 0: what the gather reads
        Scratch dof_lifted_;        // half size with mips, R16G16B16A16_FLOAT: the bokeh contrast's lifted source and its luminance
        // core/dof.h's kDofSpriteCap DofSprites of the far field, then as many of the near field, with an SRV of each half;
        // and their two DrawInstancedIndirect argument sets, R32_UINT, which the scatter counts the sprites into.
        ComPtr<ID3D11Buffer> dof_sprites_, dof_args_;
        ComPtr<ID3D11UnorderedAccessView> dof_sprites_uav_, dof_args_uav_;
        ComPtr<ID3D11ShaderResourceView> dof_sprites_srv_[2];
        Scratch dof_tiles_, dof_dilated_;   // R16G16_FLOAT, one texel per 8x8 half-size pixels: far and near CoC maxima
        Scratch dof_far_, dof_near_, dof_far_post_, dof_near_post_;     // half size, R16G16B16A16_FLOAT: the gather's fields, the
                                                                        // far one with mips under the bokeh contrast
        Scratch dof_cover_, dof_cover_post_;    // half size, R16G16_FLOAT: the near field's red and blue coverage
        Readback frame_hdr_, frame_depth_;  // EnhancementsSaveFrame's copies
        bool frame_pending_ = false;        // copied and not yet read
        Staging copies_[3];         // the comparison capture's two frames, and its variant's view
        Scratch view_copy_;         // R8G8B8A8_UNORM at the back buffer's size: a comparison variant's view
    };
}
