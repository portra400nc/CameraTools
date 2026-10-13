// The trace pass's D3D11 side. Render thread only, on the native immediate context like gpu.h.
//
// It holds the traced scene's BVH in two raw buffers and its alpha buffer (core/alpha.h) in a third, filled a part each
// present after the writer thread built them, and for a scene with light its sky map (core/light.h) in a fourth; the
// per-pixel history (a structured buffer of float4 at the render size: occlusion sum, sun sum and their counts), and
// while the trace gathers the gather's two (the hits' light and the sun-lit hits; the sky met and the misses); the
// kernels' counters; the Morton list of tiles; and per dispatch a set of timestamp queries and a CPU-readable copy of the
// counters, read presents later without waiting, which set the next frame's tiles from the time budget. Every D3D11 call
// that returns an HRESULT is checked: a failure puts the scene in FAILED with the call and its code, which the shell logs,
// and the raster passes run as before.
#pragma once

#include "core/trace.h"
#include "core/trace_files.h"
#include "gpu.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace photoreal
{
    // What one frame's trace does, from TraceGpu::preview at the combine step, before plan(), which reads traced, and
    // Gpu::run_trace, which does it.
    struct TraceFrame
    {
        TraceKey key;
        TraceSchedule schedule;                 // after this frame's tiles
        std::optional<TraceDispatch> dispatch;  // none on the frame the samples start again, and once every pixel has them
        bool reset = false;                     // the samples start again: the history is cleared first
        bool traced = false;                    // every traced pixel has a sample after this frame's tiles
        TraceConstants constants {};            // tiles left 0; dispatch sets them
        // While it gathers: the gather kernel runs over the frame's tiles after the trace kernel, which runs only while
        // trace_kernel; the trace kernel's constants then ask for no occlusion ray.
        bool gathering = false;
        bool trace_kernel = true;
        GatherConstants gather {};
        uint32_t target = 0;                    // the samples every traced pixel gets
    };

    // Bytes of the scene uploaded at each present.
    constexpr uint64_t kTraceUploadBytes = 32ull << 20;

    class TraceGpu
    {
    public:
        // The device and context, from Gpu::init. Creates nothing.
        void init(ID3D11Device *device, ID3D11DeviceContext1 *context);

        // The scene's life, as PHOTOREAL_TRACE_STATE_* reads it.
        void building();                        // BUILDING: a scene is coming; frees the old one
        void fail(const std::string &why);      // FAILED: frees the scene and keeps why
        void release();                         // NONE: frees the scene, history and copies
        void stale(const std::string &why);     // STALE: stops tracing, keeps the buffers until the next scene or a release

        // Starts uploading a built scene as generation: makes the compute shader the first time, the node, triangle and
        // alpha buffers (16 bytes of zeros for a scene without alpha tests), and moves to UPLOADING. false, in FAILED with
        // why, when one could not be made; buffer says whether it was a buffer, which a smaller scene may fit.
        // light, for a scene captured with PHOTOREAL_SCENE_LIGHT, gives its sky map a buffer of its own.
        bool begin_upload(std::shared_ptr<const Bvh> bvh, std::shared_ptr<const std::vector<uint32_t>> alpha, std::shared_ptr<const SceneLight> light,
            uint64_t generation, bool &buffer);
        // Whether rays from the camera must meet the scene where the depth buffer does; off for the self-test scene, which
        // no frame shows and which would go stale after its first pass.
        void check_consistency(bool on) { check_consistency_ = on; }
        // At present while UPLOADING: the next kTraceUploadBytes. true when the scene became READY at this present.
        bool upload_step();
        uint32_t upload_presents() const { return upload_presents_; }

        TraceState state() const { return state_; }
        const std::string &why() const { return why_; }
        uint64_t generation() const { return generation_; }
        uint32_t triangles() const { return triangles_; }
        uint64_t alpha_bytes() const { return alpha_bytes_; }
        uint32_t nodes() const { return node_count_; }
        uint64_t hash() const { return hash_; }
        uint32_t passes() const { return trace_passes(schedule_); }
        // The scene's light, empty for a scene captured without it.
        const SceneLight *light() const { return light_.get(); }
        double error() const { return error_; }
        // The samples the last dispatched frame asked every traced pixel to get, and whether it gathered.
        uint32_t target() const { return target_; }
        bool gathering() const { return gathering_; }
        uint32_t batch() const { return schedule_.batch; }
        double mrays() const { return mrays_; }
        const TraceCounts &totals() const { return totals_; }

        // What this frame's trace would do with key, settings and constants: changes nothing. The samples start again when
        // key differs from the key they started with; that frame dispatches no tiles, so a moving camera costs none. A
        // frame that goes on traces with the start's camera, sun and light (core/trace.h's held_constants). With gather,
        // the frame gathers with its constants, and the trace kernel's constants ask for no occlusion ray.
        TraceFrame preview(const TraceKey &key, const TraceSettings &settings, const TraceConstants &constants, const GatherSettings &gather_settings,
            const GatherConstants *gather) const;

        // When the trace pass runs, after the shell mirrored the G-buffer: makes the history, counters and tile list for
        // the render size when needed, clears the history when frame.reset, uploads the constants (also for the apply
        // draw, at b1), and dispatches frame's tiles with the timestamp queries around them; then frame's schedule is this
        // one's. false, in FAILED with why, when a resource or call failed.
        bool dispatch(const TraceFrame &frame, Size render, ID3D11ShaderResourceView *normals, ID3D11ShaderResourceView *depth,
            ID3D11ShaderResourceView *stencil);

        ID3D11ShaderResourceView *history_srv() const { return history_srv_.Get(); }
        ID3D11ShaderResourceView *gather_hit_srv() const { return gather_hit_srv_.Get(); }
        ID3D11ShaderResourceView *gather_sky_srv() const { return gather_sky_srv_.Get(); }
        ID3D11ShaderResourceView *sky_srv() const { return sky_srv_.Get(); }
        ID3D11Buffer *constants() const { return constants_.Get(); }

        // At present: what the finished dispatches measured, read without waiting. The next frame's tiles follow
        // settings.budget_ms; their counters add up since the samples last started; and at each completed pass the
        // consistency grid of that pass may make the scene STALE. log holds lines for ReShade.log.
        struct Poll
        {
            std::vector<std::string> log;
            bool went_stale = false;
        };
        Poll poll(const TraceSettings &settings);

        // A save of this frame: copies the history, the normals, the depth with its stencil, and the game's shadow mask and
        // irradiance mirrors (as the trace pass found them) into CPU-readable copies, keeping save's other fields but the
        // constants, which become the last ones a dispatch uploaded, and the state, why, passes and counters, which become the
        // trace's. Works while READY and while STALE, whose history stays as it was. false when there is no history or
        // constants yet, or a copy could not be made.
        // While it gathers, its two buffers and the gather's constants too; and albedo, the view's G-buffer albedo, and
        // irradiance_out, what the frame's draw wrote, when given.
        bool copy_for_save(const TraceSave &save, ID3D11Texture2D *normals, ID3D11Texture2D *depth, ID3D11Texture2D *mask, ID3D11Texture2D *irradiance,
            ID3D11Texture2D *albedo = nullptr, ID3D11Texture2D *irradiance_out = nullptr);
        bool saving() const { return save_ != nullptr; }
        // At the bloom step of the frame a save copied, before any pass of ours: its HDR scene. Once per save.
        void copy_hdr_for_save(ID3D11Texture2D *hdr);
        // At present: the save once the GPU has finished its copies, else empty; failed when a map failed, and the save is
        // dropped. Throws std::bad_alloc when its images cannot be held, and the save is dropped too.
        std::optional<TraceSave> read_save(bool &failed);

    private:
        void free_scene();
        bool make_shader();
        bool sync(Size render, bool gathering);

        // One dispatch's measurements in flight.
        struct Flight
        {
            ComPtr<ID3D11Query> disjoint, begin, end;
            ComPtr<ID3D11Buffer> counters;      // CPU-readable copy of counters_
            bool pending = false;
            uint32_t tiles = 0;                 // the frame's
            uint32_t measured = 0;              // the first submission's, which the timestamps bracket
            uint32_t passes_before = 0, passes_after = 0;
            uint64_t epoch = 0;                 // the samples' start it belongs to
        };

        struct PendingSave
        {
            TraceSave save;
            ComPtr<ID3D11Buffer> history, gather_hit, gather_sky;   // CPU-readable
            Readback normals, depth, mask, irradiance, albedo, irradiance_out, hdr;
            bool hdr_copied = false;
        };

        ComPtr<ID3D11Device> device_;
        ComPtr<ID3D11DeviceContext1> context_;
        ComPtr<ID3D11ComputeShader> cs_, gather_cs_;
        ComPtr<ID3D11Buffer> constants_, gather_constants_;
        ComPtr<ID3D11Buffer> node_buffer_, triangle_buffer_, alpha_buffer_;
        ComPtr<ID3D11ShaderResourceView> node_srv_, triangle_srv_, alpha_srv_;
        std::shared_ptr<const Bvh> uploading_;
        std::shared_ptr<const std::vector<uint32_t>> uploading_alpha_;
        uint64_t alpha_bytes_ = 0;
        uint64_t uploaded_ = 0;
        uint32_t upload_presents_ = 0;
        ComPtr<ID3D11Buffer> tiles_;
        ComPtr<ID3D11ShaderResourceView> tiles_srv_;
        ComPtr<ID3D11Buffer> history_;
        ComPtr<ID3D11UnorderedAccessView> history_uav_;
        ComPtr<ID3D11ShaderResourceView> history_srv_;
        ComPtr<ID3D11Buffer> counters_;
        ComPtr<ID3D11UnorderedAccessView> counters_uav_;
        // The gather's, made at the first frame that gathers at a render size and kept with the history.
        ComPtr<ID3D11Buffer> gather_hit_, gather_sky_;
        ComPtr<ID3D11UnorderedAccessView> gather_hit_uav_, gather_sky_uav_;
        ComPtr<ID3D11ShaderResourceView> gather_hit_srv_, gather_sky_srv_;
        // The scene's light and its sky map's buffer.
        std::shared_ptr<const SceneLight> light_;
        ComPtr<ID3D11Buffer> sky_buffer_;
        ComPtr<ID3D11ShaderResourceView> sky_srv_;
        Size size_;                             // of the history and tile list
        uint32_t tile_count_ = 0;
        std::array<Flight, 4> flights_;
        uint32_t next_flight_ = 0;

        TraceState state_ = TraceState::none;
        std::string why_;
        uint64_t generation_ = 0;
        uint32_t triangles_ = 0, node_count_ = 0;
        uint64_t hash_ = 0;
        std::optional<TraceKey> key_;           // what the samples started with
        std::optional<TraceConstants> started_constants_;   // the constants of the frame that started them, and its gather's,
        GatherConstants started_gather_ {};                 // which every later frame's camera, sun and light are
        std::optional<TraceConstants> uploaded_constants_;  // what the last dispatch put in the constant buffer, tiles and all
        GatherConstants uploaded_gather_ {};    // what the last gathering dispatch put in the gather's
        TraceSchedule schedule_;
        uint64_t epoch_ = 0;
        TraceCounts totals_ {};
        TraceCounts window_ {};                 // since the last pass the consistency grid judged
        bool check_consistency_ = true;
        uint32_t window_pass_ = 0, logged_pass_ = 0;
        uint32_t measures_logged_ = 0;         // measurements of these samples logged, before any pass
        double mrays_ = 0;
        bool gathering_ = false;                // the last dispatched frame's
        uint32_t target_ = 0;
        // The gather's noise sums since the last full pass, and the error they gave at it.
        uint64_t noise_deviation_ = 0, noise_mean_ = 0;
        uint32_t noise_pass_ = 0;
        double error_ = 0;
        std::unique_ptr<PendingSave> save_;
    };
}
