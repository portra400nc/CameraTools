// The scene capture's D3D11 side. Render thread only, on the native immediate context like gpu.h, so our replays never
// come back to us as events.
//
// Each G-buffer draw of a captured frame is replayed just before the game's own, with every binding the game made for it
// untouched: its input assembler, vertex, hull and domain shaders, constant buffers and resources. Only a geometry shader
// built with stream output from the draw's own vertex or domain shader bytecode, our stream-output target at the draw's
// own offset, a blend state that writes nothing and a depth-stencil state with both off are swapped in, and a triangle
// list's topology becomes a point list; ReplayGuard puts the game's back.
//
// Why points: DXVK builds that geometry shader as a pass-through that takes and emits one point (DxbcCompiler::
// processXfbPassthrough). Fed triangles, the Deck's driver ran it once per triangle and streamed only its first corner,
// 16 bytes where D3D11 writes 48, while the query still counted one primitive per triangle (build 198). Fed the same
// indices as points, the vertex shader runs per index as before and every corner is streamed in index order, which is the
// triangle list's corners three by three, under DXVK and under D3D11 alike.
#pragma once

#include "core/scene.h"
#include "core/shaders.h"
#include "gpu.h"

#include <unordered_map>

namespace photoreal
{
    // Captures, on construction, the state a replay changes and puts it back on destruction: the geometry shader with its
    // class instances, the blend state with its factor and sample mask, and the depth-stencil state with its stencil
    // reference, and the primitive topology. The game's hull and domain shaders stay bound, unlike under StateGuard, so
    // tessellated draws replay.
    //
    // The tripwire: after the restore, render target 0, the depth view, the pixel shader, the geometry shader, the blend
    // and depth-stencil states, the topology and stream-output slot 0 must be what they were. If any differs, moved names it, and the
    // shell disarms the add-on.
    class ReplayGuard
    {
    public:
        ReplayGuard(ID3D11DeviceContext *context, const char *&moved);
        ~ReplayGuard();
        ReplayGuard(const ReplayGuard &) = delete;
        ReplayGuard &operator=(const ReplayGuard &) = delete;

        // The game had a stream-output target bound. SOGetTargets gives no offsets, so a replay would lose the game's
        // and must not run.
        bool game_so() const { return so_[0] || so_[1] || so_[2] || so_[3]; }

    private:
        ID3D11DeviceContext *context_;
        const char *&moved_;
        ID3D11RenderTargetView *rtv_ = nullptr;
        ID3D11DepthStencilView *dsv_ = nullptr;
        ID3D11PixelShader *ps_ = nullptr;
        ID3D11GeometryShader *gs_ = nullptr;
        ID3D11ClassInstance *gs_instances_[kClassInstances] = {};
        UINT gs_count_ = kClassInstances;
        ID3D11BlendState *blend_ = nullptr;
        FLOAT blend_factor_[4] = {};
        UINT sample_mask_ = 0;
        ID3D11DepthStencilState *depth_stencil_ = nullptr;
        UINT stencil_ref_ = 0;
        D3D11_PRIMITIVE_TOPOLOGY topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        ID3D11Buffer *so_[kSoSlots] = {};
    };

    // Replays past this many in one frame are skipped with query-pool. B drew 521 G-buffer draws.
    constexpr uint32_t kSceneQueries = 2048;

    // A game draw as ReShade reported it.
    struct ReplayCall
    {
        ReplayKind kind = ReplayKind::indexed;
        uint32_t count = 0, instances = 0, first = 0;   // direct calls: indices or vertices, instances, the first of them
        int32_t base_vertex = 0;
        uint32_t first_instance = 0;
        ID3D11Buffer *args = nullptr;                   // indirect calls: the game's argument buffer and offset
        uint32_t args_offset = 0;
    };

    // What read() found.
    struct SceneRead
    {
        enum class State : uint8_t { not_ready, failed, ok } state = State::not_ready;
        std::string why;                        // failed only
        std::vector<SceneDrawRecord> records;   // with each draw's written and needed triangles
        std::vector<float> clip;                // the streamed draws' triangles, 12 floats each, in draw order
        std::vector<float> depth;               // the main depth's reversed depth, top row first; empty when not copied
        Size render;                            // its size
        std::optional<RawFrame> picture;        // the back buffer before ReShade's effects, when it was 8-bit
        uint64_t wanted = 0;                    // corners the frame's draws would have reserved had they all fit
        bool full = false;                      // a draw did not fit in the buffer: buffer-full
        uint32_t overran = 0;                   // draws that wrote past their reservation; their next one is larger
        uint32_t unwritten = 0;                 // draws whose reservation kept corners stream output never wrote
        // copy_gbuffer's, when it copied that frame, at render, top row first: the stencil, and the normals, shadow mask
        // and diffuse irradiance as stored; each empty when it was not copied.
        std::vector<uint8_t> stencil, normals, mask, irradiance;
    };

    class SceneGpu
    {
    public:
        // The device, the context and our shared states, from Gpu::init. Creates nothing.
        void init(ID3D11Device *device, ID3D11DeviceContext1 *context, ID3D11BlendState *no_write, ID3D11DepthStencilState *no_depth);
        bool ready() const { return context_ != nullptr; }

        // At the start of a scene capture: the stream-output buffer and its CPU-readable twin at the default size. Empty
        // when they were made, else why not. end() frees them, so an idle add-on holds neither.
        std::optional<std::string> begin();
        void end();

        // At the present before a captured frame, and at a G-buffer restart within it: forget the frame's draws and clear
        // the buffer to kUnwrittenBits, so the next replay is placed at the buffer's start and a corner no replay wrote can
        // be told apart.
        void begin_frame();

        // One G-buffer draw, before the game's own: records it, and replays it unless a DrawSkip says why not. Returns why
        // the capture must stop, such as stream output being unsupported, or empty.
        std::optional<std::string> replay(const ReplayCall &call, const ShaderRegistry &shaders, const char *&moved);

        // At the combine step of a captured frame whose request keeps the G-buffer, before any pass of ours: copies the
        // normals, the shadow mask and the diffuse irradiance the frame map found into mirrors, which copy_at_present
        // passes on. A mirror that cannot be made leaves its copy out.
        void copy_gbuffer(const FrameMap &map);

        // At the present that ends a captured frame: copies the stream-output buffer, the main depth and the back buffer
        // into CPU-readable resources, and copy_gbuffer's mirrors when it copied this frame, and keeps the frame's
        // records for read(). false when a copy of ours could not be made; the picture and the G-buffer copies are
        // optional and their failure is not one.
        bool copy_at_present(const std::optional<Texture> &depth, ID3D11Resource *back_buffer);

        // Reads the queries and maps the copies without waiting. not_ready while any is unfinished.
        SceneRead read();

        // After a draw did not fit: makes both buffers 1.25 times the corners wanted, rounded up to 16 MB, at most 512 MB.
        // false when they are at that size already or the larger ones could not be made, and the old ones are kept.
        bool grow(uint64_t wanted_corners);

        // PhotorealStatus.scene_support: 0 untried, 1 stream output wrote triangles, 2 CreateGeometryShaderWithStreamOutput
        // failed with DXGI_ERROR_INVALID_CALL.
        uint32_t support() const { return support_; }

    private:
        // One stream-output geometry shader per source bytecode, made at its first replay and kept for the device's life:
        // the same bytecode always gives the same shader.
        struct CachedGs
        {
            ComPtr<ID3D11GeometryShader> shader;
            DrawSkip skip = DrawSkip::none;     // no_position or gs_create_failed when there is none
            HRESULT result = S_OK;
        };

        const CachedGs &geometry_shader(const ShaderCode &code);
        bool make_buffers(uint64_t bytes);

        ComPtr<ID3D11Device> device_;
        ComPtr<ID3D11DeviceContext1> context_;
        ComPtr<ID3D11BlendState> no_write_;
        ComPtr<ID3D11DepthStencilState> no_depth_;
        std::unordered_map<uint64_t, CachedGs> gs_cache_;
        ComPtr<ID3D11Buffer> so_buffer_, so_staging_;
        ComPtr<ID3D11Buffer> marker_;                   // filled with kUnwrittenBits, the source begin_frame clears from
        uint64_t buffer_bytes_ = 0;
        ReserveHints hints_;                            // for the whole scene capture
        std::vector<ComPtr<ID3D11Query>> queries_;     // made as replays need them, up to kSceneQueries
        // The frame being replayed.
        std::vector<SceneDrawRecord> records_;
        uint32_t replays_ = 0;
        StreamLayout layout_;
        // The frame copy_at_present copied.
        std::vector<SceneDrawRecord> copied_;
        uint32_t copied_replays_ = 0;
        uint64_t copied_wanted_ = 0;
        bool copied_frame_ = false;
        Mirror depth_;
        ComPtr<ID3D11Texture2D> depth_staging_;         // R32G8X24_TYPELESS, CPU-readable
        Size depth_size_;                               // of the copy, {} when none was made
        Staging picture_;
        bool picture_copied_ = false;
        // copy_gbuffer's mirrors of the frame being replayed, and their copies at present.
        Mirror normals_, mask_, irradiance_;
        bool gbuffer_copied_ = false;
        Readback normals_back_, mask_back_, irradiance_back_;
        bool gbuffer_back_ = false;
        uint32_t support_ = 0;
    };
}
