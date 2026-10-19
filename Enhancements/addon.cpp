// The shell: ReShade events in, FrameEvents to the tracker, plans to the GPU, reports to the status mailbox.
// Everything that decides lives in core/; everything that draws lives in gpu.cpp.
#include "enhancements.h"
#include "core/alpha_bake.h"
#include "core/compare.h"
#include "core/light.h"
#include "core/plan.h"
#include "core/png.h"
#include "core/scene_files.h"
#include "core/shaders.h"
#include "core/trace_files.h"
#include "gpu.h"
#include "scene_gpu.h"
#include "trace_gpu.h"

#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace reshade::api;
namespace fs = std::filesystem;

namespace enhancements
{
    namespace
    {
        // One writer each: the C# thread writes settings and camera, the render thread writes status and armed.
        // A mailbox is a value behind a mutex plus a generation the reader polls without the lock.
        template <class T>
        struct Mailbox
        {
            std::mutex mutex;
            T value {};
            std::atomic<uint64_t> generation { 0 };

            uint64_t put(const T &v)
            {
                std::lock_guard lock(mutex);
                value = v;
                return ++generation;
            }

            // Copies the value only when the generation moved since seen; returns whether it did.
            bool take(T &out, uint64_t &seen)
            {
                if (generation.load(std::memory_order_acquire) == seen)
                    return false;
                std::lock_guard lock(mutex);
                out = value;
                seen = generation.load(std::memory_order_relaxed);
                return true;
            }
        };

        struct Published
        {
            // No scene face until the first present says otherwise: a binding that read 0 would turn the camera to +X.
            EnhancementsStatus status = [] {
                EnhancementsStatus s {};
                s.scene_face = ENHANCEMENTS_FACE_NONE;
                return s;
            }();
            std::string line = "off";
        };

        Mailbox<Settings> settings_box;
        Mailbox<Camera> camera_box;
        Mailbox<Published> status_box;
        Mailbox<std::vector<Variant>> compare_box;
        Mailbox<AccumulateSettings> accumulate_box;
        Mailbox<SceneRequest> scene_box;
        Mailbox<ShotSettings> shot_box;

        // The point EnhancementsDepthAt asks about and its answer, shared by the C# thread and the render thread.
        struct DepthAsk
        {
            bool active = false;
            float u = 0, v = 0;
            uint64_t asked = 0;         // presents when this point was first asked for
            uint64_t last = 0;          // presents at the newest call
            float meters = -1;          // the newest answer for this point, -1 while none
            uint64_t answered = 0;      // presents when the answer's depth was copied
        };
        // Presents without a call after which the point is forgotten and the add-on stops reading it.
        constexpr uint64_t kDepthForget = 60;
        std::mutex depth_mutex;
        DepthAsk depth_ask;
        // presents since the add-on loaded, for the C# thread's calls.
        std::atomic<uint64_t> presents { 0 };

        // Variants of the comparison capture not yet saved. EnhancementsCompare raises it from 0, and the worker lowers it
        // as it saves each variant's files, or to 0 when a stopped capture's note is written, so while it is not 0 no
        // second capture is accepted.
        std::atomic<uint32_t> compare_remaining { 0 };

        // Faces of the scene capture not yet written, which EnhancementsCaptureScene raises from 0 and the worker lowers like
        // compare_remaining, and faces handed to the worker and not yet written, at which the schedule holds its next
        // capture.
        std::atomic<uint32_t> scene_remaining { 0 };
        std::atomic<uint32_t> scene_backlog { 0 };
        // An ENHANCEMENTS_SCENE_RELEASE request: 0 while there is none, else one more than scene_box's generation when it came, so
        // the render thread stops what was asked for up to then and leaves a capture asked for after it alone.
        std::atomic<uint64_t> scene_release { 0 };

        // Every vertex and domain shader's bytecode and every pixel shader's hash, from the events of their creation on
        // the game's threads.
        ShaderRegistry shaders;

        // The hot path's only check. Written by the render thread at present, so tracking never starts mid-frame.
        std::atomic<bool> armed { false };

        // Render-thread state.
        command_list *immediate = nullptr;
        Gpu gpu;
        SceneGpu scene_gpu;
        FrameTracker tracker;
        FrameReport report;
        Settings settings;                  // this frame's, set at present: the applied ones under the shot
        Settings applied;                   // EnhancementsApply's newest
        ShotSettings shot;                  // EnhancementsSetShot's newest
        bool shot_frame = false;            // this frame runs with the shot
        uint64_t settings_seen = 0, camera_seen = 0, compare_seen = 0, accumulate_seen = 0, scene_seen = 0, shot_seen = 0;
        AccumulateSettings accumulate;      // this frame's, taken at present
        Accumulator accumulator;
        // The depth texel in flight: the point it answers, and the frame and camera it was copied with.
        struct DepthCopy
        {
            float u, v;
            uint64_t frame;
            Camera camera;
        };
        std::optional<DepthCopy> depth_copy;
        Camera camera {};
        uint32_t camera_age = UINT32_MAX;
        bool scene_frame = false;           // the scene capture replays this frame's G-buffer draws
        bool scene_gbuffer = false;         // and copies its G-buffer at the combine step: the view face of a GBUFFER request
        bool scene_light = false;           // and its albedo, stencil and HDR: every face of a LIGHT request
        bool scene_first = false;           // the face is the capture's first, whose normals and irradiance the ambient fit takes
        // The combine step's first draw's constants, the sun's color and shading direction: copied at that draw while the
        // gather or a LIGHT capture wants them, and the newest read back.
        bool combine_copied = false;
        std::optional<CombineConstants> combine_read;
        bool capture_light = false;         // a LIGHT scene capture runs
        SunParse sun_parse;                 // the newest constants read back, from one or two frames before
        SunCheck sun_frame = SunCheck::unread;  // this frame's verdict on them
        uint64_t frames = 0;
        uint32_t sticky_error = ENHANCEMENTS_ERROR_NONE;   // NOT_D3D11, SHADER or STATE: the add-on stays disarmed
        uint32_t frame_error = ENHANCEMENTS_ERROR_NONE;    // TEXTURE: this frame only
        HMODULE addon_module = nullptr;

        // The trace pass's scene on the render thread; the writer thread builds it. trace_scene describes the scene being
        // uploaded or traced.
        TraceGpu trace_gpu;
        struct TraceScene
        {
            std::string folder;                     // the scene folder's name, which holds trace-scene.bin
            Vec3d reference {};                     // the frame its triangles are relative to
            std::optional<Vec3> floating_origin;    // b1 row 10 when it was captured, or when it was first traced
            SceneBuilder::Counts counts;
            AlphaBakeCounts bake;                   // how its alpha programs baked into masks
            double bake_seconds = 0;
            double seconds = 0;                     // its BVH's build
            bool self_test = false;                 // the self-test scene, which the depth buffer never shows
            std::shared_ptr<const SceneLight> light;    // an ENHANCEMENTS_SCENE_LIGHT capture's
            double light_seconds = 0;               // its bake
            uint64_t asked = 0;                     // trace_asked when its capture began
        };
        TraceScene trace_scene;
        TraceState trace_logged = TraceState::none; // the state the log last said

        // An ENHANCEMENTS_SCENE_TRACE capture's triangles as the writer thread writes its faces.
        struct TraceBuild
        {
            SceneBuilder builder;
            Vec3d reference {};
            std::optional<Vec3> floating_origin;    // b1 row 10 of the first face with shadow-mask constants
            bool light = false;                     // ENHANCEMENTS_SCENE_LIGHT: the faces' images, kept until the scene is built
            LightInputs inputs;
        };

        // What the writer thread built, for the render thread to upload.
        struct TraceBuilt
        {
            std::shared_ptr<const Bvh> bvh;         // empty when error says why
            std::shared_ptr<const std::vector<uint32_t>> alpha;  // its alpha buffer, empty without alpha tests
            std::string error;
            TraceScene scene;
        };
        Mailbox<TraceBuilt> trace_box;
        uint64_t trace_seen = 0;
        // scene_box's generation of the newest request that began a traced scene. A scene built for an older one, whose
        // capture was released or followed by another while the writer thread still built it, is dropped.
        uint64_t trace_asked = 0;
        uint64_t trace_generations = 0;
        // A scene of more triangles than this is cut to its nearest; halved each time a scene's buffers could not be made.
        uint64_t trace_max_triangles = 4000000;
        std::optional<TraceFrame> trace_frame;      // this frame's, from the combine step
        RestartBurst restart_burst;                 // the samples' starts since a full pass, for the log
        uint64_t restart_burst_frame = 0;           // the frame of the burst's first
        bool trace_traced = false;                  // the trace pass writes this frame
        bool trace_wrote = false;                   // the trace pass wrote the last frame from its samples
        bool gather_without_light = false;          // the gather is on over a scene captured without light
        std::string gather_sun_source;              // where the gather's sun came from at the last frame, for the log
        std::string trace_sun_source = "none";      // where the trace's sun came from at the last frame, for the log
        std::optional<SunCbuffers> sun_bytes;       // the newest shadow-mask constants read back
        std::optional<Vec3> logged_origin;          // b1 row 10 as the log last said it

        // EnhancementsSaveTrace's request: set by the C# thread, cleared by the render thread once the frame is copied.
        std::atomic<bool> trace_save_wanted { false };
        std::atomic<uint64_t> trace_save_asked { 0 };   // presents at the request
        // Presents a save waits for a traced frame before it is given up.
        constexpr uint64_t kTraceSaveWait = 300;

        void log(reshade::log::level level, const std::string &message)
        {
            reshade::log::message(level, ("CameraTools Enhancements: " + message).c_str());
        }

        // Why the game's bloom sometimes does not match is unknown, so a frame whose bloom step fell back turns on the
        // tracker's trace for the frames after it, and the next such frame's draws go to ReShade.log, at most kBloomTraces
        // times a session. The trace stops kBloomTraceFrames presents after the last fall-back, because it asks every draw
        // for its inputs.
        constexpr uint32_t kBloomTraces = 3;
        constexpr uint64_t kBloomTraceFrames = 3600;
        bool bloom_noted = false;
        uint32_t bloom_traces = 0;
        uint64_t bloom_trace_until = 0;

        // At present, for a frame whose bloom step fell back.
        void diagnose_bloom()
        {
            if (bloom_traces == kBloomTraces)
                return;
            bloom_trace_until = frames + kBloomTraceFrames;
            if (!tracker.tracing())
            {
                if (!bloom_noted)
                    log(reshade::log::level::info, "the game's bloom did not match in frame " + std::to_string(frames)
                        + ", so the bloom step's passes ran at the tonemap draw. The next such frame's draws go to this log.");
                bloom_noted = true;
                return;
            }
            const std::vector<std::string> lines = describe_trace(tracker);
            log(reshade::log::level::info, "the game's bloom did not match in frame " + std::to_string(frames) + ". Its "
                + std::to_string(tracker.traced().size()) + " draws from the combine step to the tonemap draw, with render targets (rt), depth (ds) "
                "and pixel shader inputs (t), each texture by frame-map entry or as #number DXGI_FORMAT:size:");
            for (const std::string &line : lines)
                log(reshade::log::level::info, "  " + line);
            bloom_traces++;
        }

        // A restore that left the game's bindings different would corrupt the rest of every frame, so the add-on stops
        // for good.
        void trip(const char *moved)
        {
            sticky_error = ENHANCEMENTS_ERROR_STATE;
            armed.store(false, std::memory_order_relaxed);
            log(reshade::log::level::error, std::string("the game's ") + moved + " differed after restoring state. Enhancements are off until the game restarts.");
        }

        // Runs jobs on its own thread in the order they came. The thread ends when it runs out of work.
        class Worker
        {
        public:
            void push(std::function<void()> job)
            {
                std::lock_guard lock(mutex_);
                jobs_.push_back(std::move(job));
                if (!running_)
                {
                    running_ = true;
                    std::thread([this] { run(); }).detach();
                }
            }

        private:
            void run()
            {
                for (;;)
                {
                    std::function<void()> job;
                    {
                        std::lock_guard lock(mutex_);
                        if (jobs_.empty())
                        {
                            running_ = false;
                            return;
                        }
                        job = std::move(jobs_.front());
                        jobs_.pop_front();
                    }
                    job();
                }
            }

            std::mutex mutex_;
            std::deque<std::function<void()>> jobs_;
            bool running_ = false;
        };

        Worker worker;

        void write_file(const fs::path &path, const std::string &bytes)
        {
            std::error_code error;
            fs::create_directories(path.parent_path(), error);
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!out)
                log(reshade::log::level::warning, "could not write " + path.u8string() + ".");
        }

        // ReShade's base path, which RESHADE_BASE_PATH_OVERRIDE sets to C:\ReShade on the Deck, as FrameCensus finds it.
        fs::path output_root()
        {
            fs::path base;
            using GetBasePath = bool (*)(char *, size_t *);
            const auto get_base_path = reinterpret_cast<GetBasePath>(GetProcAddress(reshade::internal::get_reshade_module_handle(), "ReShadeGetBasePath"));
            if (get_base_path != nullptr)
            {
                size_t size = 0;
                get_base_path(nullptr, &size);
                std::string path(size, '\0');
                get_base_path(path.data(), &size);
                path.resize(std::strlen(path.c_str()));
                base = fs::u8path(path);
            }
            if (base.empty())
            {
                wchar_t module_path[MAX_PATH] = {};
                GetModuleFileNameW(addon_module, module_path, MAX_PATH);
                base = fs::path(module_path).parent_path().parent_path();
            }
            return base / "Enhancements";
        }

        // The game's grade at one tone map draw, for the tonemap pass.
        struct GameGrade
        {
            bool copied = false;    // the draw's own grade is in the pass's constants
            bool bloom = true;      // the draw's shader adds the bloom
        };

        // By the hash of a pixel shader the game drew its tone map with: where its code keeps the grade, or none for a
        // shader that is not the tone map the add-on knows. Render thread only.
        std::unordered_map<uint64_t, std::optional<GradeLayout>> grade_layouts;
        // The shader of the last tone map draw, named in the log when it changes, at most kGradeNotes times.
        constexpr uint32_t kGradeNotes = 16;
        uint64_t grade_shader = 0;
        uint32_t grade_notes = 0;

        std::string shader_name(uint64_t hash)
        {
            char name[17];
            std::snprintf(name, sizeof name, "%016llx", static_cast<unsigned long long>(hash));
            return name;
        }

        std::string place(const GradeValue &value) { return std::to_string(value.row) + "." + "xyzw"[value.component]; }

        // At the game's tone map draw, before any pass of ours draws there: a pass binds its own shader and constants, and
        // the grade is in the draw's. Finds where the draw's shader keeps the grade and copies it for the tonemap pass.
        // The rows differ between the shader's variants (the game's Bloom setting picks another), so each shader is read
        // once, and the log names each one the game changes to, with the values it held.
        GameGrade take_game_grade()
        {
            if (const std::optional<std::array<float, 16>> g = gpu.read_game_grade())
            {
                char text[320];
                std::snprintf(text, sizeof text, "red becomes (%.4f, %.4f, %.4f), green (%.4f, %.4f, %.4f) and blue (%.4f, %.4f, %.4f); "
                    "exposure %.4f, bloom %.4f, gamma %.4f", (*g)[0], (*g)[1], (*g)[2], (*g)[4], (*g)[5], (*g)[6], (*g)[8], (*g)[9], (*g)[10],
                    (*g)[12], (*g)[13], (*g)[14]);
                log(reshade::log::level::info, std::string("the game's grade at that tone map draw: ") + text + ".");
            }
            const std::optional<ShaderCode> code = shaders.find(gpu.bound_pixel_shader());
            const uint64_t hash = code ? code->hash : 0;
            auto known = grade_layouts.find(hash);
            const bool first = known == grade_layouts.end();
            if (first)
            {
                std::optional<GradeLayout> layout;
                if (code)
                    if (const std::optional<Shex> shex = decode_shex(reinterpret_cast<const uint8_t *>(code->code->data()), code->code->size()))
                        layout = find_grade(*shex);
                known = grade_layouts.emplace(hash, layout).first;
            }
            const std::optional<GradeLayout> &layout = known->second;
            const bool copied = layout && gpu.copy_game_grade(*layout);
            if (hash != grade_shader && grade_notes < kGradeNotes)
            {
                grade_notes++;
                std::string note = "the game's tone map draw of frame " + std::to_string(frames) + " uses pixel shader " + shader_name(hash);
                if (layout)
                {
                    note += ", which keeps its color matrix in rows " + std::to_string(layout->color_row) + " to " + std::to_string(layout->color_row + 2)
                        + " of its constants, its exposure at " + place(layout->exposure) + ", its gamma at " + place(layout->gamma)
                        + (layout->bloom ? " and its bloom at " + place(*layout->bloom) + "." : " and adds no bloom.");
                    if (copied)
                        gpu.request_game_grade();
                    else
                        note += " Its constants could not be copied, so the tonemap pass uses exposure 1, no color matrix and gamma 1.";
                }
                else
                {
                    note += code ? ", which is not the tone map the add-on knows" : ", whose code the add-on never saw";
                    if (code && first)
                    {
                        const fs::path path = output_root() / ("tonemap-" + shader_name(hash) + ".ps.dxbc");
                        note += " and is saved as " + path.u8string();
                        worker.push([path, bytes = code->code] { write_file(path, *bytes); });
                    }
                    note += ". The tonemap pass uses exposure 1, no color matrix and gamma 1 in place of the game's.";
                }
                log(reshade::log::level::info, note);
            }
            grade_shader = hash;
            return { copied, !layout || layout->bloom.has_value() };
        }

        std::string timestamp()
        {
            SYSTEMTIME time;
            GetLocalTime(&time);
            char text[32];
            std::snprintf(text, sizeof text, "%04u%02u%02u-%02u%02u%02u", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
            return text;
        }

        // What the worker writes into: touched only by worker jobs once the capture starts.
        struct CompareFiles
        {
            fs::path folder;
            std::vector<CompareRow> rows;
        };

        // A comparison capture in progress, on the render thread. compare_tick() says what each present does.
        struct Capture
        {
            std::vector<Variant> variants;
            Settings before;
            uint32_t present = 0;
            std::optional<Size> render;         // the first armed frame's, which every later armed frame must match
            std::vector<std::string> lines;     // describe() at each variant's second copy
            uint32_t read = 0;                  // variants read back and handed to the worker
            std::shared_ptr<CompareFiles> files;
            uint32_t waited = 0;                // presents the variant about to be copied waited for its trace's samples
            // The trace's passes when that wait began, its passes at the last present, and the presents waited when the
            // newest of them came in: the pace trace_wait_limit sets the wait's limit by.
            std::optional<uint32_t> wait_first;
            uint32_t wait_passes = 0, wait_pass_presents = 0;
            std::vector<CompareRow> trace;      // each copied variant's trace fields, at its second copy
            std::vector<bool> views;            // whether each copied variant's view was copied too
        };

        std::optional<Capture> capture;
        uint32_t compare_count = 0;  // the last capture's variants, for describe's progress until all are saved

        Texture resolve(device *dev, resource_view view)
        {
            if (view.handle == 0)
                return {};
            const resource res = dev->get_resource_from_view(view);
            const resource_desc desc = dev->get_resource_desc(res);
            if (desc.type != resource_type::texture_2d)
                return { static_cast<ResourceId>(res.handle), Format::unknown, {} };
            return { static_cast<ResourceId>(res.handle), static_cast<Format>(dev->get_resource_view_desc(view).format), { desc.texture.width, desc.texture.height } };
        }

        std::array<Texture, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> draw_inputs;

        // Answers DrawQueries from the native context on first call. Matchers ask only when the targets fit a step.
        class NativeDraw final : public DrawQueries
        {
        public:
            explicit NativeDraw(ID3D11DeviceContext *context) : context_(context) {}

            const Texture *ps_inputs(uint32_t *count) override
            {
                if (!read_)
                {
                    ID3D11ShaderResourceView *srvs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
                    context_->PSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, srvs);
                    for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; slot++)
                    {
                        ID3D11ShaderResourceView *srv = srvs[slot];
                        Texture &t = draw_inputs[slot];
                        t = {};
                        if (srv == nullptr)
                            continue;
                        count_ = slot + 1;
                        ID3D11Resource *resource = nullptr;
                        srv->GetResource(&resource);
                        t.id = static_cast<ResourceId>(reinterpret_cast<uintptr_t>(resource));
                        D3D11_RESOURCE_DIMENSION dimension;
                        resource->GetType(&dimension);
                        if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
                        {
                            D3D11_TEXTURE2D_DESC desc;
                            static_cast<ID3D11Texture2D *>(resource)->GetDesc(&desc);
                            t.format = static_cast<Format>(desc.Format);
                            t.size = { desc.Width, desc.Height };
                        }
                        resource->Release();
                        srv->Release();
                    }
                    read_ = true;
                }
                *count = count_;
                return draw_inputs.data();
            }

        private:
            ID3D11DeviceContext *context_;
            bool read_ = false;
            uint32_t count_ = 0;
        };

        bool camera_known() { return camera_age != UINT32_MAX; }

        Inputs inputs() { return { camera_known(), sun_frame, trace_gpu.state(), trace_traced }; }

        // The trace pass reads the game's sun direction and floating origin from the shadow-mask constants while it traces.
        bool trace_tracing() { return settings.enabled && settings.trace.enabled && trace_gpu.state() == TraceState::ready; }

        // At the shadow-mask draw, before it runs: parse the newest finished readback of an earlier frame's constants,
        // then copy this draw's. The passes draw with this frame's copies on the GPU, so they stay exact while the camera
        // moves. The readback only decides whether they use them and gives the cascade layout, which changes only with
        // the graphics settings, and with it the atlas's size, which sun_check compares. A frame the scene capture replays
        // copies them too, so the capture can place its geometry with that frame's own camera.
        // Each change of the parse's verdict saves the constants it read, at most kSunDumps times a session.
        constexpr uint32_t kSunDumps = 16;
        uint32_t sun_dumps = 0;

        void gather_sun(const FrameMap &map)
        {
            if (!(wants_sun(settings) || scene_frame || trace_tracing()) || !map[Entry::sun_atlas])
                return;
            if (const std::optional<SunCbuffers> bytes = gpu.sun_constants(frames))
            {
                const SunCheck before = sun_parse.check;
                sun_parse = parse_sun_shadows(*bytes);
                sun_bytes = bytes;
                if (sun_parse.check != before && sun_dumps < kSunDumps)
                {
                    sun_dumps++;
                    const fs::path path = output_root() / ("sun-" + std::to_string(frames) + "-" + kSunCheckNames[static_cast<size_t>(sun_parse.check)] + ".bin");
                    worker.push([path, data = std::string(reinterpret_cast<const char *>(bytes->data()), bytes->size())] { write_file(path, data); });
                }
            }
            sun_frame = sun_check(sun_parse, gpu.gather_sun(*map[Entry::sun_atlas], frames), map[Entry::sun_atlas]->size);
        }

        // At the bloom step, after our passes: the accumulator, so each sample's scene includes their effects.
        void accumulate_at_bloom(const FrameMap &map)
        {
            if (accumulate.mode == AccumulateMode::off || !map[Entry::hdr_scene])
                return;
            const AccumulateTick tick = accumulate_tick(accumulator, accumulate, camera.lens, camera_age == 0, map.render);
            if (!tick.clear && !tick.add && !tick.present)
                return;
            const char *moved = nullptr;
            std::optional<GameCall> call;
            {
                StateGuard guard(gpu.context(), moved);
                call = gpu.run_accumulate(map, tick, accumulate, camera.lens, settings.flip);
            }
            if (moved)
                trip(moved);
            else if (!call)
            {
                frame_error = ENHANCEMENTS_ERROR_TEXTURE;
                accumulator = {};
            }
            else
                accumulator = tick.next;
        }

        // At the bloom step: copies the asked point's depth while no copy is in flight. Needs a camera to turn it into
        // meters when it is read.
        void copy_depth_at_bloom(const FrameMap &map)
        {
            if (depth_copy || !camera_known() || !map[Entry::depth])
                return;
            float u, v;
            {
                std::lock_guard lock(depth_mutex);
                if (!depth_ask.active)
                    return;
                u = depth_ask.u;
                v = depth_ask.v;
            }
            const std::optional<Pixel> at = depth_pixel(map.render, u, v, settings.flip);
            if (at && gpu.copy_depth_texel(map, *at))
                depth_copy = DepthCopy { u, v, frames, camera };
        }

        // At present: reads the texel in flight once the GPU is done with it, and answers the point it was copied for.
        void read_depth()
        {
            if (!depth_copy)
                return;
            const std::optional<float> raw = gpu.read_depth_texel();
            if (!raw)
                return;
            const DepthCopy copy = *depth_copy;
            depth_copy.reset();
            const std::optional<float> meters = view_depth(copy.camera, copy.u, copy.v, *raw);
            std::lock_guard lock(depth_mutex);
            if (meters && depth_ask.active && depth_ask.u == copy.u && depth_ask.v == copy.v)
            {
                depth_ask.meters = *meters;
                depth_ask.answered = copy.frame;
            }
        }

        std::string vec_text(const Vec3 &v)
        {
            char text[96];
            std::snprintf(text, sizeof text, "(%.3f, %.3f, %.3f)", v[0], v[1], v[2]);
            return text;
        }

        // A change of b1 row 10, the game's floating origin, is a world shift under the traced scene, which has no way to
        // follow it yet: the scene goes stale. Every change is logged.
        void check_world_shift(const Vec3 &now)
        {
            if (!logged_origin || origin_moved(*logged_origin, now))
                log(reshade::log::level::info, "b1 row 10, the game's floating origin, is " + vec_text(now) + " in frame " + std::to_string(frames) + ".");
            logged_origin = now;
            if (!trace_scene.floating_origin)
                trace_scene.floating_origin = now;
            else if (origin_moved(*trace_scene.floating_origin, now))
                trace_gpu.stale("the world shifted: b1 row 10 moved from " + vec_text(*trace_scene.floating_origin) + " to " + vec_text(now));
        }

        // At the combine step, before the plan: what the trace pass does this frame, which says whether it writes the
        // frame. The sun comes from the game's cascade 0 when its constants were read back, else from the pushed camera.
        void prepare_trace(const FrameMap &map)
        {
            trace_frame.reset();
            trace_traced = false;
            if (!trace_tracing() || !camera_known() || map.render.width == 0)
                return;
            Vec3 toward = { camera.toward_sun[0], camera.toward_sun[1], camera.toward_sun[2] };
            std::string source = toward == Vec3 {} ? "none" : "pushed";
            if (sun_bytes)
            {
                const SunFacts facts = sun_facts(*sun_bytes);
                if (facts.toward_sun)
                {
                    toward = *facts.toward_sun;
                    source = "game";
                }
                if (facts.floating_origin)
                    check_world_shift(*facts.floating_origin);
                if (trace_gpu.state() != TraceState::ready)
                    return;
            }
            if (source != trace_sun_source)
            {
                // Cascade 0 is what the game's own sun shadows follow: on build 202's capture, sun rays along it agreed with
                // the game's shadow mask on 94.7% of the world pixels within 60 m, and along the pushed camera's, 5.4
                // degrees off, on 74.8% (README, "Tracing").
                const Vec3 pushed = { camera.toward_sun[0], camera.toward_sun[1], camera.toward_sun[2] };
                const double cosine = double(toward[0]) * pushed[0] + double(toward[1]) * pushed[1] + double(toward[2]) * pushed[2];
                char apart[64];
                std::snprintf(apart, sizeof apart, ", %.2f degrees apart", std::acos(std::clamp(cosine, -1.0, 1.0)) * 57.29577951308232);
                log(reshade::log::level::info, "the trace's sun comes from " + (source == "game" ? "the game's cascade 0, which the game's own sun shadows follow: " + vec_text(toward)
                    : source == "pushed" ? "the pushed camera, as the game's shadow-mask constants were not read: " + vec_text(toward)
                    : std::string("nowhere: there is no sun")) + "; the pushed camera's is " + vec_text(pushed) + (source == "game" && pushed != Vec3 {} ? apart : "") + ".");
                trace_sun_source = source;
            }
            const std::optional<TraceConstants> constants = trace_constants(settings.trace, settings.ambient.level, camera, settings.flip, map.render,
                trace_scene.reference, toward, static_cast<uint32_t>(trace_gpu.generation()));
            if (!constants)
                return;
            // The gather over a scene with light: the sun as the game shades with it now, and the ambient light the scene
            // was built with.
            gather_without_light = settings.gather.enabled && !trace_gpu.light();
            std::optional<GatherConstants> gather;
            if (settings.gather.enabled && trace_gpu.light())
            {
                const Vec3 pushed_color = { camera.sun_color[0], camera.sun_color[1], camera.sun_color[2] };
                const Vec3 pushed_direction = { camera.toward_sun[0], camera.toward_sun[1], camera.toward_sun[2] };
                const SunLight sun = sun_light(combine_read, source == "game" ? std::optional<Vec3>(toward) : std::nullopt, pushed_color, pushed_direction);
                const std::string said = std::string(kLightSourceNames[static_cast<size_t>(sun.color_source)]) + kLightSourceNames[static_cast<size_t>(sun.shade_source)]
                    + sun.why;
                if (said != gather_sun_source)
                {
                    const double cosine = double(sun.shade[0]) * toward[0] + double(sun.shade[1]) * toward[1] + double(sun.shade[2]) * toward[2];
                    char apart[64];
                    std::snprintf(apart, sizeof apart, "%.2f degrees", std::acos(std::clamp(cosine, -1.0, 1.0)) * 57.29577951308232);
                    log(reshade::log::level::info, "the gather's sun: its color " + vec_text(sun.color) + " from the " + (sun.color_source == LightSource::game
                        ? std::string("game's combine constants") : "pushed camera") + ", its shading direction " + vec_text(sun.shade) + " from the "
                        + (sun.shade_source == LightSource::game ? std::string("game's combine constants") : "pushed camera") + (sun.why.empty() ? "" : " (" + sun.why + ")")
                        + ", " + apart + " from the shadows' " + vec_text(toward) + "; the pushed camera's color " + vec_text(pushed_color) + " and direction "
                        + vec_text(pushed_direction) + ".");
                    gather_sun_source = said;
                }
                gather = gather_constants(settings.gather, sun, trace_gpu.light()->ambient);
            }
            const GatherConstants *gathered = gather ? &*gather : nullptr;
            trace_frame = trace_gpu.preview(trace_key(trace_gpu.generation(), map.render, camera, toward, settings.trace, gathered), settings.trace, *constants,
                settings.gather, gathered);
            trace_traced = trace_frame->traced;
        }

        // What a save holds besides the frame's images, as the trace pass has it now; TraceGpu::copy_for_save adds the
        // constants it last uploaded, its state, passes and counters.
        TraceSave trace_save_meta()
        {
            TraceSave s;
            s.settings = settings.trace;
            s.level = settings.ambient.level;
            s.camera = camera;
            s.reference = trace_scene.reference;
            s.sun = sun_bytes;
            s.sun_source = trace_sun_source;
            s.floating_origin = trace_scene.floating_origin;
            s.scene = trace_scene.folder;
            s.generation = trace_gpu.generation();
            s.bvh_hash = trace_gpu.hash();
            s.triangles = trace_gpu.triangles();
            s.nodes = trace_gpu.nodes();
            s.mrays = trace_gpu.mrays();
            s.frame = frames;
            s.gather = settings.gather;
            return s;
        }

        // The work at one moment. Returns the event's return value: true skips the game's call.
        bool on_step(Step step)
        {
            const FrameMap &map = tracker.map();
            if (step == Step::shadow_mask)
                gather_sun(map);
            if (step == Step::combine)
            {
                // Before any pass of ours writes the shadow mask or the irradiance.
                if (scene_gbuffer || scene_light)
                    scene_gpu.copy_gbuffer(map, { scene_gbuffer || (scene_light && scene_first), scene_gbuffer, scene_light });
                prepare_trace(map);
                // A save while the scene is stale, which the trace pass no longer runs for: this frame's images with the
                // history as it was when the scene went stale.
                if (trace_save_wanted.load() && !trace_gpu.saving() && trace_gpu.state() == TraceState::stale && gpu.save_trace(map, trace_save_meta(), trace_gpu))
                    trace_save_wanted = false;
            }
            // The HDR scene before any pass of ours: a LIGHT capture's face's, and a trace save's.
            if (step == Step::bloom)
            {
                if (scene_light)
                    scene_gpu.copy_hdr(map);
                gpu.save_trace_hdr(map, trace_gpu);
            }
            const Plan p = plan(step, map, settings, inputs());
            report.note(p);
            bool skip = false;
            const char *moved = nullptr;
            GameGrade grade;
            for (uint8_t i = 0; i < p.count; i++)
                if (p.items[i].kind == Decision::Kind::run && p.items[i].pass == PassId::tonemap)
                    grade = take_game_grade();
            {
                // Captured once, before the first pass that draws, restored at the end of this block. Copies (the debug
                // snapshot) change no pipeline state, so a step with no running pass never pays for it.
                std::optional<StateGuard> guard;
                for (uint8_t i = 0; i < p.count; i++)
                {
                    const Decision &d = p.items[i];
                    if (d.kind != Decision::Kind::run)
                        continue;
                    if (!guard)
                        guard.emplace(gpu.context(), moved);
                    std::optional<GameCall> call;
                    switch (d.pass)  // exhaustive: a new PassId warns here until it has a run function
                    {
                    case PassId::ambient:
                        call = gpu.run_ambient(map, settings.ambient, camera, settings.flip);
                        break;
                    case PassId::sun_shadows:
                        call = gpu.run_sun_shadows(map, settings.sun_shadows, camera, sun_parse.constants, settings.flip);
                        break;
                    case PassId::contact_shadows:
                        call = gpu.run_contact_shadows(map, settings.contact_shadows, camera, settings.flip);
                        break;
                    case PassId::wetness:
                        call = gpu.run_wetness(map, settings.wetness, wetness_amount(settings.wetness, camera), camera, settings.flip);
                        break;
                    case PassId::atmosphere:
                        call = gpu.run_atmosphere(map, settings.atmosphere, camera, settings.flip);
                        break;
                    case PassId::light_shafts:
                        call = gpu.run_light_shafts(map, settings.light_shafts, camera, sun_parse.constants, settings.flip,
                            report.view == View::light_shafts);
                        break;
                    case PassId::tonemap:
                        call = gpu.run_tonemap(map, settings.tonemap, grade.copied, grade.bloom);
                        break;
                    case PassId::trace:
                    {
                        // A save waits for a frame the trace writes; run_trace copies it then.
                        const std::optional<TraceSave> save = trace_save_wanted.load() && !trace_gpu.saving() ? std::optional(trace_save_meta()) : std::nullopt;
                        bool saved = false;
                        // Why this frame's samples start again, when the log names it: read before the start replaces what
                        // the samples began with.
                        std::string restart;
                        if (trace_frame && trace_frame->reset)
                        {
                            if (restart_burst.count == 0)
                                restart_burst_frame = frames;
                            if (restart_logged(restart_burst, trace_frame->restart))
                                restart = restart_text(trace_frame->restart, trace_gpu.started_key(), trace_frame->key);
                        }
                        if (trace_frame)
                            call = gpu.run_trace(map, *trace_frame, settings.trace, settings.gather, camera, settings.flip, report.view, save ? &*save : nullptr,
                                saved, trace_gpu);
                        if (!restart.empty())
                            log(reshade::log::level::info, "the trace's samples start again (" + std::string(kTraceRestartNames[static_cast<size_t>(trace_frame->restart)])
                                + ", start " + std::to_string(trace_gpu.starts()) + (restart_burst.count > 1 ? ", " + std::to_string(restart_burst.count)
                                + " starts now without a full pass" : std::string()) + "): " + restart + ".");
                        if (saved)
                            trace_save_wanted = false;
                        trace_wrote = call && trace_frame && trace_frame->traced;
                        const bool gathering = trace_frame && trace_frame->gathering;
                        report.trace_ao = gathering ? 0 : std::min(trace_gpu.passes(), settings.trace.ao_samples);
                        report.trace_gather = gathering ? std::min(trace_gpu.passes(), settings.gather.samples) : 0;
                        report.trace_sun = std::min(trace_gpu.passes(), settings.trace.sun_samples);
                        report.gather_without_light = gather_without_light;
                        break;
                    }
                    case PassId::count:
                        break;
                    }
                    if (!call)
                    {
                        report.note_failed(d.pass);
                        frame_error = ENHANCEMENTS_ERROR_TEXTURE;
                    }
                    skip |= call == GameCall::skip;
                }
            }
            if (moved)
            {
                trip(moved);
                return false;
            }
            if (step == Step::bloom)
            {
                accumulate_at_bloom(map);
                copy_depth_at_bloom(map);
            }
            const ViewSpec &view = kViews[static_cast<size_t>(report.view)];
            // A gather view shows only while the trace gathers.
            const bool gather_shown = gather_view(report.view) == GatherView::none || (trace_frame && trace_frame->gathering);
            if (report.note_view(step, map) && !gather_shown)
            {
                report.view_found = false;
                gpu.note_view_why(gather_without_light ? "scene without light" : "gather off");
            }
            else if (report.view_found && view.snapshot_at == step
                && !(view.from == PassId::light_shafts ? gpu.snapshot_light_shafts() : view.from == PassId::trace ? gpu.snapshot_trace(report.view) : gpu.snapshot(*map[view.entry])))
            {
                report.view_found = false;
                frame_error = ENHANCEMENTS_ERROR_TEXTURE;
            }
            else if (report.view != View::off && view.snapshot_at == step && !report.view_found)
                gpu.note_view_why(view_missing_why(report));
            return skip;
        }

        // The gather's sun comes from the combine step's first draw, and a LIGHT capture's scene keeps the newest.
        bool wants_combine() { return (trace_tracing() && settings.gather.enabled) || capture_light; }

        bool feed(command_list *cmd_list, const FrameEvent &event)
        {
            if (cmd_list != immediate)
                return false;
            const StepSet moments = tracker.feed(event);
            // Before the draw runs, with its own constants bound: the first draw after the combine step's clear.
            if (!combine_copied && std::holds_alternative<Draw>(event) && tracker.matched().has(Step::combine) && wants_combine() && gpu.ready())
            {
                gpu.gather_combine(frames);
                combine_copied = true;
            }
            if (moments.empty())
                return false;
            bool skip = false;
            for (const StepSpec &spec : kRecipe)
                if (moments.has(spec.step))
                    skip |= on_step(spec.step);
            return skip;
        }

        // The G-buffer is bound now, in a frame the scene capture replays.
        bool in_gbuffer = false;

        // A scene capture in progress, on the render thread. scene_tick() says what each present does.
        struct SceneRun
        {
            SceneSchedule schedule;
            std::shared_ptr<SceneFolder> folder;    // touched only by worker jobs
            std::optional<Vec3d> reference;         // the first saved face's camera position, which every mesh is relative to
            // The frame copy_at_present copied last.
            uint64_t frame = 0;
            std::string status;                     // describe() of it
            bool gbuffer = false;                   // it found the gbuffer step
            std::optional<Camera> pushed;           // the camera CameraTools pushed for it
            std::optional<SunCbuffers> sun;         // its shadow-mask draw's constants, once read
            std::optional<std::string> stop;        // a replay's reason to stop, taken at the next present
            std::shared_ptr<TraceBuild> trace;      // an ENHANCEMENTS_SCENE_TRACE capture's triangles, touched only by worker jobs
        };

        // How a traced scene's alpha programs baked into masks, for the log.
        static std::string bake_summary(const TraceScene &scene)
        {
            const AlphaBakeCounts &b = scene.bake;
            char seconds[32];
            std::snprintf(seconds, sizeof seconds, "%.2f", scene.bake_seconds);
            return "their programs baked into " + std::to_string(b.records) + " mask records and " + std::to_string(b.masks) + " masks in " + seconds + " s: "
                + std::to_string(b.triangles[0]) + " exact, " + std::to_string(b.triangles[1]) + " approximate, " + std::to_string(b.triangles[2])
                + " whole as they cannot be baked" + (b.why.empty() ? std::string() : " (first: " + b.why + ")") + ", " + std::to_string(b.always)
                + " whole as their test keeps every point, " + std::to_string(b.never) + " discarded everywhere, " + std::to_string(b.past_max)
                + " whole past the records' cap; draws " + std::to_string(b.draws[0]) + " exact, " + std::to_string(b.draws[1]) + " approximate, "
                + std::to_string(b.draws[2]) + " with a whole triangle";
        }

        // On the writer thread, after a traced scene's last face: writes its triangles and alpha programs to
        // trace-scene.bin in its folder, bakes the programs into the masks the trace kernel fetches (core/alpha_bake.h),
        // builds the triangles' BVH, and hands both to the render thread through trace_box. Never throws.
        void build_trace(std::vector<TraceTriangle> triangles, std::vector<uint32_t> alpha, TraceScene scene, const fs::path &folder)
        {
            TraceBuilt built;
            try
            {
                write_file(folder / "trace-scene.bin", trace_scene_bytes(triangles, scene.reference, alpha));
                if (!alpha.empty())
                {
                    const auto baking = std::chrono::steady_clock::now();
                    AlphaMasks masks = bake_alpha(alpha, triangles, BvhOptions {}.tasks);
                    scene.bake = masks.counts;
                    scene.bake_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - baking).count();
                    if (!masks.words.empty())
                        built.alpha = std::make_shared<const std::vector<uint32_t>>(std::move(masks.words));
                }
                const auto start = std::chrono::steady_clock::now();
                Bvh bvh = build_bvh(triangles, BvhOptions {});
                scene.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                if (bvh.nodes.empty())
                    built.error = "the scene holds no triangles to trace";
                else
                    built.bvh = std::make_shared<const Bvh>(std::move(bvh));
            }
            catch (const std::exception &e)
            {
                built.error = std::string("its BVH could not be built: ") + e.what();
            }
            built.scene = std::move(scene);
            trace_box.put(built);
        }

        // ENHANCEMENTS_SCENE_TRACE with no faces: the self-test scene around the pushed camera, horizontal toward its forward,
        // traced like a captured one, with its trace-scene.bin in a scene folder of its own.
        void begin_self_test(bool light)
        {
            if (!camera_known())
            {
                trace_gpu.fail("the self-test scene needs a camera from EnhancementsSetCamera");
                return;
            }
            const fs::path folder = output_root() / ("scene-" + timestamp() + "-self-test");
            TraceScene scene;
            scene.folder = folder.filename().u8string();
            scene.reference = camera_position(camera);
            scene.self_test = true;
            scene.asked = trace_asked;
            // world_to_view's third row is minus the camera's forward; Unity's matrices are column-major.
            Vec3d forward = { -camera.world_to_view[2], 0, -camera.world_to_view[10] };
            const double length = std::sqrt(forward[0] * forward[0] + forward[2] * forward[2]);
            forward = length > 1e-6 ? Vec3d { forward[0] / length, 0, forward[2] / length } : Vec3d { 0, 0, 1 };
            trace_gpu.building();
            log(reshade::log::level::info, "tracing the self-test scene around the camera" + std::string(light ? ", with its light," : "") + "; its triangles go to "
                + folder.u8string() + ".");
            LightInputs inputs;
            inputs.combine = combine_read;
            if (sun_bytes)
                inputs.cascade = sun_facts(*sun_bytes).toward_sun;
            inputs.pushed_sun_color = { camera.sun_color[0], camera.sun_color[1], camera.sun_color[2] };
            inputs.pushed_sun_direction = { camera.toward_sun[0], camera.toward_sun[1], camera.toward_sun[2] };
            worker.push([folder, scene, forward, light, inputs]() mutable {
                std::error_code error;
                fs::create_directories(folder, error);
                std::vector<TraceTriangle> triangles = self_test_scene(forward);
                if (light)
                {
                    scene.light = std::make_shared<SceneLight>(self_test_light(triangles, inputs));
                    write_file(folder / "trace-light.bin", light_bytes(triangles, std::vector<uint32_t>(triangles.size(), 0), *scene.light, {}));
                }
                build_trace(std::move(triangles), {}, scene, folder);
            });
        }

        std::optional<SceneRun> scene;
        uint32_t scene_count = 0;           // the last scene capture's faces, for describe's progress until all are written
        std::string scene_stopped;          // why the last scene capture stopped, for describe until the next one starts
        uint32_t scene_face = ENHANCEMENTS_FACE_NONE;  // the face the last present published

        // Before the game's draw, in a replayed frame while the G-buffer is bound. A trip or a reason to stop ends the
        // frame's replays.
        void replay_scene(const ReplayCall &call)
        {
            const char *moved = nullptr;
            std::optional<std::string> stop = scene_gpu.replay(call, shaders, moved);
            if (moved)
            {
                scene_frame = false;
                trip(moved);
            }
            if (stop)
            {
                scene_frame = false;
                if (scene && !scene->stop)
                    scene->stop = std::move(stop);
            }
        }

        void on_bind_render_targets(command_list *cmd_list, uint32_t count, const resource_view *rtvs, resource_view dsv)
        {
            if (!armed.load(std::memory_order_relaxed))
                return;
            device *const dev = cmd_list->get_device();
            BindTargets bind;
            count = std::min<uint32_t>(count, kMaxTargets);
            while (count > 0 && rtvs[count - 1].handle == 0)
                count--;
            for (uint32_t i = 0; i < count; i++)
                bind.targets.color[i] = resolve(dev, rtvs[i]);
            bind.targets.count = static_cast<uint8_t>(count);
            if (dsv.handle != 0)
                bind.targets.depth = resolve(dev, dsv);
            const uint32_t restarts = tracker.restarts();
            feed(cmd_list, bind);
            if (!scene_frame || cmd_list != immediate)
                return;
            // A restart drops another camera's G-buffer: only the frame's last one, which the passes use too, is captured.
            if (tracker.restarts() != restarts)
                scene_gpu.begin_frame();
            const FrameMap &map = tracker.map();
            in_gbuffer = tracker.matched().has(Step::gbuffer) && !tracker.matched().has(Step::combine) && bind.targets.count == 6 && bind.targets.depth
                && bind.targets.color[0].id == map[Entry::normals]->id && bind.targets.depth->id == map[Entry::depth]->id;
        }

        bool on_clear_rtv(command_list *cmd_list, resource_view rtv, const float color[4], uint32_t, const rect *)
        {
            if (!armed.load(std::memory_order_relaxed))
                return false;
            return feed(cmd_list, ClearTarget { resolve(cmd_list->get_device(), rtv), { color[0], color[1], color[2], color[3] } });
        }

        bool on_draw_any(command_list *cmd_list)
        {
            if (!armed.load(std::memory_order_relaxed))
                return false;
            NativeDraw native(reinterpret_cast<ID3D11DeviceContext *>(cmd_list->get_native()));
            return feed(cmd_list, Draw { native });
        }

        // The scene capture's checks cost one branch while it does not run. DrawAuto comes as a draw of 0 vertices, which
        // draws nothing to replay.
        bool on_draw(command_list *cmd_list, uint32_t vertices, uint32_t instances, uint32_t first_vertex, uint32_t first_instance)
        {
            if (armed.load(std::memory_order_relaxed) && cmd_list == immediate && scene_frame && in_gbuffer && vertices != 0)
                replay_scene({ ReplayKind::draw, vertices, instances, first_vertex, 0, first_instance, nullptr, 0 });
            return on_draw_any(cmd_list);
        }

        bool on_draw_indexed(command_list *cmd_list, uint32_t indices, uint32_t instances, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance)
        {
            if (armed.load(std::memory_order_relaxed) && cmd_list == immediate && scene_frame && in_gbuffer)
                replay_scene({ ReplayKind::indexed, indices, instances, first_index, vertex_offset, first_instance, nullptr, 0 });
            return on_draw_any(cmd_list);
        }

        // Indirect draws go to the scene capture only: the frame recipe never matched them, so it still sees only direct
        // draws.
        bool on_draw_or_dispatch_indirect(command_list *cmd_list, indirect_command type, resource buffer, uint64_t offset, uint32_t draw_count, uint32_t)
        {
            if (!armed.load(std::memory_order_relaxed) || cmd_list != immediate || !scene_frame || !in_gbuffer || type == indirect_command::dispatch
                || draw_count != 1)
                return false;
            replay_scene({ type == indirect_command::draw_indexed ? ReplayKind::indirect_indexed : ReplayKind::indirect, 0, 0, 0, 0, 0,
                reinterpret_cast<ID3D11Buffer *>(buffer.handle), static_cast<uint32_t>(offset) });
            return false;
        }

        void start_capture(std::vector<Variant> variants)
        {
            auto files = std::make_shared<CompareFiles>();
            files->folder = output_root() / ("compare-" + timestamp());
            for (int i = 2; fs::exists(files->folder); i++)
                files->folder = output_root() / ("compare-" + timestamp() + "-" + std::to_string(i));
            log(reshade::log::level::info, "comparing " + std::to_string(variants.size()) + " settings variants into " + files->folder.u8string() + ".");
            worker.push([files, text = settings_tsv(variants)] { write_file(files->folder / "settings.tsv", text); });
            compare_count = static_cast<uint32_t>(variants.size());
            capture = Capture { std::move(variants), settings, 0, std::nullopt, {}, 0, files, 0, std::nullopt, 0, 0, {}, {} };
        }

        // Reads the next variant's two copies and hands them to the worker, which saves the PNG and rewrites
        // compare.tsv. false when a map failed or the two frames differ in size.
        bool read_variant()
        {
            Capture &c = *capture;
            std::optional<RawFrame> first = gpu.read_back_buffer(0), second = gpu.read_back_buffer(1);
            if (!first || !second || first->size != second->size)
                return false;
            const uint32_t index = c.read++;
            // A variant with a view: the view's own picture, which the copies of the back buffer, taken before the composite, lack.
            std::optional<RawFrame> view = index < c.views.size() && c.views[index] ? gpu.read_view() : std::nullopt;
            worker.push([files = c.files, index, name = c.variants[index].name, line = c.lines[index], trace = c.trace[index], first = std::move(*first),
                            second = std::move(*second), view = std::move(view)] {
                if (view)
                {
                    std::string file = png_name(index, name);
                    file.replace(file.size() - 4, 4, ".view.png");
                    const std::vector<uint8_t> rgb = to_rgb(*view);
                    write_file(files->folder / file, encode_png(rgb.data(), view->size.width, view->size.height));
                }
                const std::vector<uint8_t> a = to_rgb(first), b = to_rgb(second);
                CompareRow row = trace;
                row.index = index;
                row.name = name;
                row.line = line;
                row.flicker = flicker(a, b);
                row.size = second.size;
                files->rows.push_back(row);
                write_file(files->folder / png_name(index, name), encode_png(b.data(), second.size.width, second.size.height));
                write_file(files->folder / "compare.tsv", compare_tsv(files->rows));
                compare_remaining--;
            });
            return true;
        }

        // Saves what was captured, a variant copied but not yet read too while the device allows, and puts the settings
        // from before the capture back.
        void stop_capture(const std::string &why)
        {
            Capture &c = *capture;
            if (gpu.ready() && c.read < c.lines.size())
                read_variant();
            const std::string note = "stopped after " + std::to_string(c.read) + " of " + std::to_string(c.variants.size()) + " variants: " + why + ".";
            log(reshade::log::level::warning, "the comparison capture " + note);
            worker.push([files = c.files, note] {
                write_file(files->folder / "stopped.txt", note + "\n");
                compare_remaining = 0;
            });
            settings = c.before;
            capture.reset();
        }

        // One present of the capture, after the frame's report is finished. Returns why it must stop, or empty.
        std::string tick_capture(swapchain *chain)
        {
            Capture &c = *capture;
            if (sticky_error != ENHANCEMENTS_ERROR_NONE)
                return "the add-on is off after an error";
            if (!gpu.ready())
                return "there is no D3D11 device";
            if (c.present > 0 && report.armed)
            {
                // Optional steps come and go between paused frames (the Deck showed bloom missing for single frames), so
                // only a new render size or a frame without the G-buffer and combine steps stops the capture.
                if (!c.render)
                    c.render = report.render;
                else if (report.render != *c.render)
                    return "the render size changed";
                if (!report.found.has(Step::gbuffer) || !report.found.has(Step::combine))
                    return "the frame had no G-buffer or combine step";
            }
            const uint32_t count = static_cast<uint32_t>(c.variants.size());
            // A variant that traces is copied once its trace has every sample it asks for, or after kTraceWaitPresents.
            const std::optional<CompareTick::Copy> next = compare_tick(c.present, count).copy;
            const Settings *variant = next ? &c.variants[next->variant].settings : nullptr;
            const bool traces = variant && variant->enabled && variant->trace.enabled && trace_gpu.state() == TraceState::ready;
            const uint32_t target = variant ? trace_target(variant->trace, variant->gather, variant->gather.enabled && trace_gpu.light() != nullptr) : 0;
            // The wait's limit follows the pace of the trace's passes, so a slow trace is waited for while it advances. A
            // variant whose samples start again during the wait starts its pace again too.
            const uint32_t passes = trace_gpu.passes();
            if (!c.wait_first || passes < c.wait_passes)
            {
                c.wait_first = passes;
                c.wait_pass_presents = c.waited;
            }
            else if (passes > c.wait_passes)
                c.wait_pass_presents = c.waited;
            c.wait_passes = passes;
            const uint32_t limit = trace_wait_limit(*c.wait_first, passes, c.wait_pass_presents, target);
            if (compare_waits(c.present, count, c.waited, traces && passes < target, limit))
            {
                c.waited++;
                return {};
            }
            const CompareTick tick = compare_tick(c.present++, count);
            if (tick.copy)
            {
                const resource back_buffer = chain->get_current_back_buffer();
                if (!gpu.copy_back_buffer(reinterpret_cast<ID3D11Resource *>(back_buffer.handle), tick.copy->frame))
                    return "the back buffer is not 8-bit RGBA or BGRA, or its copy failed";
                if (tick.copy->frame == 1)
                {
                    const char *moved = nullptr;
                    const resource_desc desc = chain->get_device()->get_resource_desc(back_buffer);
                    c.views.push_back(c.variants[tick.copy->variant].settings.view != View::off
                        && gpu.copy_view({ desc.texture.width, desc.texture.height }, settings.flip, moved));
                    if (moved)
                    {
                        trip(moved);
                        return "the state did not come back after the view's copy";
                    }
                    c.lines.push_back(describe(report));
                    CompareRow &row = c.trace.emplace_back();
                    if (traces)
                    {
                        row.trace_samples = trace_gpu.passes();
                        row.trace_target = target;
                    }
                    row.trace_wait = c.waited;
                    row.trace_timeout = traces && c.waited >= limit && passes < target;
                    c.waited = 0;
                    c.wait_first.reset();
                }
            }
            if (tick.read && !read_variant())
                return "the back buffer copies could not be read, or changed size";
            if (tick.apply)
                settings = c.variants[*tick.apply].settings;
            if (tick.restore)
                settings = c.before;
            if (tick.done)
                capture.reset();
            return {};
        }

        void log_unwritten(const std::vector<std::string> &files)
        {
            for (const std::string &file : files)
                log(reshade::log::level::warning, "could not write " + file + ".");
        }

        void begin_scene_capture(const SceneRequest &request)
        {
            if (request.trace)
                trace_asked = scene_seen;
            if (request.self_test())
            {
                begin_self_test(request.light);
                return;
            }
            auto folder = std::make_shared<SceneFolder>();
            folder->path = output_root() / ("scene-" + timestamp());
            for (int i = 2; fs::exists(folder->path); i++)
                folder->path = output_root() / ("scene-" + timestamp() + "-" + std::to_string(i));
            SceneRun run;
            run.schedule = start_scene(request);
            run.folder = folder;
            scene_count = static_cast<uint32_t>(run.schedule.order.size());
            scene_stopped.clear();
            log(reshade::log::level::info, "capturing the scene from " + std::to_string(scene_count) + " faces into " + folder->path.u8string() + ".");
            worker.push([folder] { log_unwritten(begin_scene_folder(*folder)); });
            if (request.trace)
            {
                run.trace = std::make_shared<TraceBuild>();
                run.trace->light = request.light;
                capture_light = request.light;
                trace_gpu.building();
            }
            scene = std::move(run);
            if (gpu.ready())
                if (const std::optional<std::string> why = scene_gpu.begin())
                    scene->stop = why;
        }

        // Ends the scene capture: what was handed to the worker is still written, then stopped.txt with why, when there
        // is a reason.
        void end_scene_capture(const std::optional<std::string> &why)
        {
            scene_light = scene_first = capture_light = false;
            SceneRun &run = *scene;
            if (why)
            {
                const std::string note = "stopped after " + std::to_string(run.schedule.saved) + " of " + std::to_string(run.schedule.order.size())
                    + " faces: " + *why + ".";
                log(reshade::log::level::warning, "the scene capture " + note);
                worker.push([folder = run.folder, note] {
                    log_unwritten(write_scene_stopped(*folder, note));
                    scene_remaining = 0;
                });
                scene_stopped = *why;
            }
            else
                log(reshade::log::level::info, "the scene capture's " + std::to_string(run.schedule.saved) + " faces go to " + run.folder->path.u8string()
                    + ".");
            if (run.trace && why)
                trace_gpu.fail("the scene capture stopped: " + *why);
            else if (run.trace)
            {
                // After the faces' jobs, so every face is in the builder. The light's inputs the render thread has: the
                // newest combine constants, cascade 0's direction and the pushed camera's sun and sky.
                const uint64_t cap = std::min<uint64_t>(run.schedule.request.max_triangles, trace_max_triangles);
                LightInputs light;
                light.combine = combine_read;
                if (sun_bytes)
                    light.cascade = sun_facts(*sun_bytes).toward_sun;
                light.pushed_sun_color = { camera.sun_color[0], camera.sun_color[1], camera.sun_color[2] };
                light.pushed_sun_direction = { camera.toward_sun[0], camera.toward_sun[1], camera.toward_sun[2] };
                light.pushed_sky = { camera.sky_color[0], camera.sky_color[1], camera.sky_color[2] };
                worker.push([this_build = run.trace, folder = run.folder->path, cap, light, asked = trace_asked] {
                    TraceScene scene;
                    scene.asked = asked;
                    scene.folder = folder.filename().u8string();
                    scene.reference = this_build->reference;
                    scene.floating_origin = this_build->floating_origin;
                    std::vector<TraceTriangle> triangles;
                    std::vector<uint32_t> alpha, materials;
                    try
                    {
                        triangles = this_build->builder.take(cap, &alpha, &materials);
                        if (this_build->light)
                        {
                            // Before the alpha bake, which needs none of it; the faces' images go once the light is built.
                            LightInputs inputs = std::move(this_build->inputs);
                            inputs.combine = light.combine;
                            inputs.cascade = light.cascade;
                            inputs.pushed_sun_color = light.pushed_sun_color;
                            inputs.pushed_sun_direction = light.pushed_sun_direction;
                            inputs.pushed_sky = light.pushed_sky;
                            const auto start = std::chrono::steady_clock::now();
                            auto built = std::make_shared<SceneLight>(build_light(triangles, materials, inputs, scene.reference, BvhOptions {}.tasks));
                            scene.light_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                            write_file(folder / "trace-light.bin", light_bytes(triangles, materials, *built, inputs.faces));
                            scene.light = std::move(built);
                        }
                    }
                    catch (const std::exception &e)
                    {
                        TraceBuilt failed;
                        failed.scene.asked = asked;
                        failed.error = std::string("its triangles or their light could not be gathered: ") + e.what();
                        trace_box.put(failed);
                        return;
                    }
                    scene.counts = this_build->builder.counts();
                    build_trace(std::move(triangles), std::move(alpha), std::move(scene), folder);
                });
            }
            scene_gpu.end();
            scene.reset();
            scene_frame = false;
            scene_gbuffer = false;
            scene_face = ENHANCEMENTS_FACE_NONE;
        }

        // At the read present: the captured frame's camera, stream output, depth and picture, judged, and on ok handed to
        // the worker. Sets facts.read, or facts.stop when a read failed.
        void read_scene_face(SceneRun &run, SceneFacts &facts)
        {
            // Their stage still holds the captured frame's constants at the first read, two presents after it.
            if (!run.sun && gpu.sun_staged(run.frame))
            {
                run.sun = gpu.sun_constants_of(run.frame);
                if (!run.sun)
                {
                    facts.read = Judgement { FaceVerdict::not_ready, {} };
                    return;
                }
            }
            SceneRead got = scene_gpu.read();
            if (got.state == SceneRead::State::not_ready)
            {
                facts.read = Judgement { FaceVerdict::not_ready, {} };
                return;
            }
            if (got.state == SceneRead::State::failed)
            {
                facts.stop = got.why;
                return;
            }
            // The game's own camera of the frame when its shadow-mask constants hold one, else the pushed one. Only b1 and
            // b2 are checked: the cascades in b3 are the sun-shadow pass's business, not the camera's.
            std::optional<SceneCamera> camera;
            std::string note = "no shadow-mask constants";
            if (run.sun)
            {
                std::string why;
                camera = scene_camera(*run.sun, why);
                note = "shadow-mask camera " + why;
            }
            if (!camera && run.pushed)
                camera = scene_camera(*run.pushed, settings.flip);
            if (camera && camera->source == SceneCamera::Source::game)
                note.clear();
            // The buffer grows before the verdict, which captures again only when it did. A draw that overran its
            // reservation has a larger one next time, so that frame is captured again too.
            const bool grown = got.full && scene_gpu.grow(got.wanted);
            const uint32_t face = run.schedule.face();
            const Judgement judgement = judge_face(face, got.records, run.gbuffer, got.full || got.overran != 0, grown || got.overran != 0, camera,
                run.reference);
            facts.read = judgement;
            if (judgement.verdict != FaceVerdict::ok)
                return;
            if (!run.reference)
                run.reference = camera->position;
            // Stream output under DXVK writing what D3D11 promises is this capture's first unknown, so each face says how
            // many draws wrote another count than their call implies, and how many left corners of their reservation
            // unwritten, which build 198's triangle replays did for two thirds of every draw.
            uint32_t replayed = 0, mismatched = 0;
            uint64_t triangles = 0;
            for (const SceneDrawRecord &r : got.records)
            {
                replayed += r.skip == DrawSkip::none;
                triangles += r.written;
                const std::optional<uint64_t> expected = expected_triangles(r);
                mismatched += r.skip == DrawSkip::none && expected && *expected != r.written && r.needed == r.written;
            }
            log(reshade::log::level::info, std::string("the scene capture's ") + kFaces[face].label + " face: " + std::to_string(replayed) + " of "
                + std::to_string(got.records.size()) + " G-buffer draws replayed, " + std::to_string(triangles) + " triangles, "
                + std::to_string(mismatched) + " draws whose triangle count differs from their call's, " + std::to_string(got.unwritten)
                + " draws with unwritten corners left out, camera " + kCameraSourceNames[static_cast<size_t>(camera->source)]
                + (note.empty() ? "" : " (" + note + ")") + (got.full ? ", saved partial at the largest buffer." : "."));
            SceneFaceData data;
            data.face = face;
            data.frame = run.frame;
            data.attempts = run.schedule.attempts;
            data.camera = *camera;
            data.camera_note = note;
            data.sun = run.sun;
            data.pushed = run.pushed;
            data.status = run.status;
            data.render = got.render;
            data.records = std::move(got.records);
            data.clip = std::move(got.clip);
            data.attributes = std::move(got.attributes);
            data.constants = std::move(got.constants);
            data.atlas = std::move(got.atlas);
            data.depth = std::move(got.depth);
            data.overflowed = got.full;
            data.origin = *run.reference;
            scene_backlog++;
            data.stencil = std::move(got.stencil);
            data.normals = std::move(got.normals);
            data.mask = std::move(got.mask);
            data.irradiance = std::move(got.irradiance);
            data.albedo = std::move(got.albedo);
            data.hdr = std::move(got.hdr);
            if (run.schedule.request.light)
            {
                const size_t pixels = size_t(got.render.width) * got.render.height;
                data.light_why = data.albedo.size() != pixels * 4 ? "no albedo" : data.hdr.size() != pixels ? "no hdr" : data.stencil.size() != pixels ? "no stencil" : "";
            }
            worker.push([folder = run.folder, request = run.schedule.request, data = std::move(data), picture = std::move(got.picture), trace = run.trace]() mutable {
                if (picture)
                {
                    data.rgb = to_rgb(*picture);
                    data.picture = picture->size;
                }
                log_unwritten(write_scene_face(*folder, data, request));
                if (trace)
                {
                    try
                    {
                        trace->reference = data.origin;
                        if (!trace->floating_origin && data.sun)
                            trace->floating_origin = sun_facts(*data.sun).floating_origin;
                        add_face(trace->builder, data.camera, data.origin, data.records, data.clip, data.attributes, data.constants);
                        // Every face's read holds the atlas whole, so the last one's is the capture's.
                        if (!data.atlas.empty())
                            trace->builder.set_atlas(data.atlas);
                        // The light's images, at half size past kFullFacePixels; the first face's G-buffer for the ambient fit.
                        if (trace->light)
                        {
                            if (std::optional<FaceImages> images = face_images(data.face, data.camera, data.render))
                            {
                                if (trace->inputs.faces.empty())
                                {
                                    trace->inputs.first_size = data.render;
                                    trace->inputs.first_normals = data.normals;
                                    trace->inputs.first_irradiance = data.irradiance;
                                    trace->inputs.first_stencil = data.stencil;
                                    trace->inputs.first_depth = data.depth;
                                }
                                images->depth = std::move(data.depth);
                                images->albedo = std::move(data.albedo);
                                images->stencil = std::move(data.stencil);
                                images->hdr = std::move(data.hdr);
                                if (uint64_t(data.render.width) * data.render.height > kFullFacePixels)
                                    halve_face(*images);
                                trace->inputs.faces.push_back(std::move(*images));
                            }
                        }
                    }
                    catch (const std::exception &e)
                    {
                        log(reshade::log::level::error, std::string("the traced scene could not take the face's triangles: ") + e.what());
                    }
                }
                scene_backlog--;
                scene_remaining--;
            });
        }

        // One present of the scene capture, after the frame's report is finished.
        void tick_scene(swapchain *chain)
        {
            SceneRun &run = *scene;
            SceneFacts facts;
            facts.backlog = scene_backlog.load();
            if (sticky_error != ENHANCEMENTS_ERROR_NONE)
                facts.stop = "the add-on is off after an error";
            else if (!gpu.ready())
                facts.stop = "there is no D3D11 device";
            else if (run.stop)
                facts.stop = run.stop;
            else if (run.schedule.wants_read())
                read_scene_face(run, facts);
            const SceneTick tick = scene_tick(run.schedule, facts);
            scene_face = tick.publish_face;
            scene_frame = tick.capture_next_frame;
            scene_gbuffer = scene_frame && run.schedule.request.gbuffer && tick.publish_face == kFaceView;
            scene_light = scene_frame && run.schedule.request.light;
            scene_first = scene_frame && run.schedule.saved == 0;
            if (tick.copy_at_present)
            {
                // The frame that just ended: its report, its camera, which the mailbox has not replaced yet, and its map.
                run.frame = frames - 1;
                run.status = describe(report);
                run.gbuffer = report.found.has(Step::gbuffer);
                run.pushed = camera_known() ? std::optional<Camera>(camera) : std::nullopt;
                run.sun.reset();
                const resource back_buffer = chain->get_current_back_buffer();
                const char *moved = nullptr;
                const bool copied = scene_gpu.copy_at_present(tracker.map()[Entry::depth], reinterpret_cast<ID3D11Resource *>(back_buffer.handle), moved);
                if (moved)
                    trip(moved);
                if (!copied)
                {
                    end_scene_capture(std::string("the captured frame's depth could not be copied"));
                    return;
                }
            }
            if (tick.stop)
                end_scene_capture(tick.stop);
            else if (tick.done)
                end_scene_capture(std::nullopt);
        }

        void init(command_queue *queue)
        {
            device *const dev = queue->get_device();
            if (dev->get_api() != device_api::d3d11)
            {
                sticky_error = ENHANCEMENTS_ERROR_NOT_D3D11;
                log(reshade::log::level::warning, "only D3D11 is supported.");
                return;
            }
            immediate = queue->get_immediate_command_list();
            if (!gpu.init(reinterpret_cast<ID3D11Device *>(dev->get_native()), reinterpret_cast<ID3D11DeviceContext *>(immediate->get_native())))
            {
                sticky_error = ENHANCEMENTS_ERROR_SHADER;
                log(reshade::log::level::error, "could not create its shaders or states. Enhancements are off.");
                return;
            }
            scene_gpu.init(gpu.device(), gpu.context(), gpu.no_write(), gpu.no_depth());
            trace_gpu.init(gpu.device(), gpu.context());
            log(reshade::log::level::info, "ready.");
        }

        std::string megabytes(uint64_t bytes)
        {
            char text[32];
            std::snprintf(text, sizeof text, "%.1f MB", bytes / 1e6);
            return text;
        }

        // The traced scene's state into ReShade.log when it changed.
        void log_trace_state()
        {
            const TraceState state = trace_gpu.state();
            if (state == trace_logged)
                return;
            trace_logged = state;
            const SceneBuilder::Counts &c = trace_scene.counts;
            char hash[24], seconds[24];
            std::snprintf(hash, sizeof hash, "%016llx", static_cast<unsigned long long>(trace_gpu.hash()));
            std::snprintf(seconds, sizeof seconds, "%.2f", trace_scene.seconds);
            switch (state)
            {
            case TraceState::none:
                log(reshade::log::level::info, "the traced scene is released.");
                break;
            case TraceState::building:
                log(reshade::log::level::info, "building a traced scene; the raster passes run meanwhile.");
                break;
            case TraceState::uploading:
                log(reshade::log::level::info, "the traced scene: " + std::to_string(trace_gpu.triangles()) + " triangles of " + std::to_string(c.in)
                    + " captured, or of the self-test scene when none were (" + std::to_string(c.unique) + " unique on the 1 mm grid, " + std::to_string(c.replaced) + " prepass or equal-depth copies "
                    "replaced by a main draw's, " + std::to_string(c.pre_only) + " drawn by the prepass alone, " + std::to_string(c.dropped_class)
                    + " of grass and " + std::to_string(c.dropped_no_depth) + " of draws that write no depth left out, " + std::to_string(c.dropped_equal_alone)
                    + " equal-depth copies without an earlier twin left out, " + std::to_string(c.dropped_degenerate) + " degenerate, " + std::to_string(c.past_max)
                    + " farthest past the cap; " + std::to_string(c.alpha) + " alpha-tested, " + std::to_string(c.inherited) + " of them equal-depth copies with their prepass copy's test, "
                    + std::to_string(c.alpha_past_max) + " whole past the alpha records' 64 MB; " + bake_summary(trace_scene) + "), a BVH of "
                    + std::to_string(trace_gpu.nodes()) + " nodes built in " + seconds + " s, hash " + hash + "; uploading " + megabytes(uint64_t(trace_gpu.nodes()) * sizeof(BvhNode) + uint64_t(trace_gpu.triangles())
                    * sizeof(BvhTriangle) + trace_gpu.alpha_bytes()) + " (" + megabytes(trace_gpu.alpha_bytes()) + " of alpha tests) a part of " + megabytes(kTraceUploadBytes)
                    + " each present. trace-scene.bin is in " + trace_scene.folder + ".");
                if (const SceneLight *light = trace_scene.light.get())
                {
                    const AlbedoCounts &a = light->albedo;
                    const double area = a.area[0] + a.area[1] + a.area[2] + a.area[3];
                    char line[640];
                    std::snprintf(line, sizeof line, "the traced scene's light, built in %.2f s: albedo %llu direct, %llu covered, %llu by material, %llu by class "
                        "(by area %.1f%%, %.1f%%, %.1f%%, %.1f%%); a sky map of %u real texels of %u%s; the ambient light %s (r2 %.4f %.4f %.4f over %u pixels: "
                        "%.3f %.3f %.3f + n.y %.3f %.3f %.3f); the sun's color %s from the %s, shading direction %s from the %s%s, cascade 0's %s; "
                        "trace-light.bin is in %s.", trace_scene.light_seconds, static_cast<unsigned long long>(a.triangles[0]),
                        static_cast<unsigned long long>(a.triangles[1]), static_cast<unsigned long long>(a.triangles[2]), static_cast<unsigned long long>(a.triangles[3]),
                        area > 0 ? 100 * a.area[0] / area : 0.0, area > 0 ? 100 * a.area[1] / area : 0.0, area > 0 ? 100 * a.area[2] / area : 0.0,
                        area > 0 ? 100 * a.area[3] / area : 0.0, light->sky.real, kSkyMapSide * kSkyMapSide, light->sky.fallback ? ", fell back to the pushed sky" : "",
                        light->ambient.kind == AmbientFit::Kind::fit ? "fit" : light->ambient.kind == AmbientFit::Kind::mean ? "the irradiance's mean" : "the pushed sky",
                        light->ambient.r2[0], light->ambient.r2[1], light->ambient.r2[2], light->ambient.pixels, light->ambient.rows[0][0], light->ambient.rows[0][1],
                        light->ambient.rows[0][2], light->ambient.rows[2][0], light->ambient.rows[2][1], light->ambient.rows[2][2], vec_text(light->sun.color).c_str(),
                        light->sun.color_source == LightSource::game ? "combine constants" : "pushed camera", vec_text(light->sun.shade).c_str(),
                        light->sun.shade_source == LightSource::game ? "combine constants" : "pushed camera",
                        light->sun.why.empty() ? "" : (" (" + light->sun.why + ")").c_str(), vec_text(light->cascade).c_str(), trace_scene.folder.c_str());
                    log(reshade::log::level::info, line);
                }
                break;
            case TraceState::ready:
                log(reshade::log::level::info, "the traced scene is ready after " + std::to_string(trace_gpu.upload_presents()) + " presents of upload, generation "
                    + std::to_string(trace_gpu.generation()) + ".");
                break;
            case TraceState::stale:
                log(reshade::log::level::warning, "the traced scene is stale: " + trace_gpu.why() + ". The raster passes run until another capture or a release.");
                break;
            case TraceState::failed:
                log(reshade::log::level::error, "the traced scene failed: " + trace_gpu.why() + ". The raster passes run.");
                break;
            case TraceState::count:
                break;
            }
        }

        // At present: the writer thread's built scene into its upload, the upload's next part, the dispatches'
        // measurements, a finished save to the writer thread, and the scene's state into the log when it changed.
        void tick_trace()
        {
            TraceBuilt built;
            if (trace_box.take(built, trace_seen) && trace_gpu.state() == TraceState::building && built.scene.asked == trace_asked)
            {
                if (!built.bvh)
                    trace_gpu.fail(built.error);
                else
                {
                    bool buffer = false;
                    trace_scene = built.scene;
                    trace_gpu.check_consistency(!trace_scene.self_test);
                    if (!trace_gpu.begin_upload(built.bvh, built.alpha, trace_scene.light, ++trace_generations, buffer) && buffer)
                    {
                        trace_max_triangles = std::max<uint64_t>(trace_max_triangles / 2, 1);
                        trace_gpu.fail(trace_gpu.why() + "; the next traced capture keeps its nearest " + std::to_string(trace_max_triangles) + " triangles");
                    }
                }
            }
            log_trace_state();
            if (trace_gpu.state() == TraceState::uploading)
                trace_gpu.upload_step();
            if (wants_combine())
                combine_read = gpu.combine_constants(frames);
            for (const std::string &line : trace_gpu.poll(settings.trace).log)
                log(reshade::log::level::info, line);
            if (restart_burst.count != 0 && trace_gpu.passes() >= 1)
            {
                if (restart_burst.count > 1)
                    log(reshade::log::level::info, "the trace's samples hold: a full pass came in after " + std::to_string(restart_burst.count) + " starts over "
                        + std::to_string(frames - restart_burst_frame) + " presents.");
                restart_burst = {};
            }
            bool failed = false;
            std::optional<TraceSave> save;
            try
            {
                save = trace_gpu.read_save(failed);
            }
            catch (const std::exception &e)
            {
                log(reshade::log::level::warning, std::string("the trace's frame could not be kept for its save: ") + e.what());
            }
            if (save)
            {
                fs::path folder = output_root() / ("trace-" + timestamp());
                for (int i = 2; fs::exists(folder); i++)
                    folder = output_root() / ("trace-" + timestamp() + "-" + std::to_string(i));
                log(reshade::log::level::info, "saving the trace's frame " + std::to_string(save->frame) + " after " + std::to_string(save->passes) + " passes"
                    + (save->state == TraceState::stale ? ", its scene stale and its history as it was then," : "") + " into " + folder.u8string() + ".");
                worker.push([folder, save = std::move(*save)] { log_unwritten(write_trace_folder(folder, save)); });
            }
            else if (failed)
                log(reshade::log::level::warning, "the trace's frame could not be read back for its save.");
            if (trace_save_wanted.load() && presents.load() - trace_save_asked.load() > kTraceSaveWait)
            {
                trace_save_wanted = false;
                log(reshade::log::level::warning, "the trace save was given up: the trace pass wrote no frame, nor had a history to save, within " + std::to_string(kTraceSaveWait)
                    + " presents.");
            }
            log_trace_state();
        }

        // The status line as the log last gave it. Each change goes to the log, at most kStatusNotes times a session, so a
        // pass that drops out for a few frames shows why.
        constexpr uint32_t kStatusNotes = 200;
        std::string status_noted;
        uint32_t status_notes = 0;

        void publish(uint32_t remaining)
        {
            Published published;
            EnhancementsStatus &s = published.status;
            s.size = sizeof s;
            s.armed = report.armed;
            s.frames = frames;
            s.settings_applied = settings_seen;
            s.render_width = report.render.width;
            s.render_height = report.render.height;
            s.steps_found = report.found.bits;
            s.steps_wanted = report.wanted.bits;
            s.passes_ran = report.ran.bits;
            s.passes_skipped = report.skipped.bits;
            s.entries_missing = report.missing.bits;
            s.view_shown = static_cast<uint32_t>(report.view_shown());
            s.camera_age = camera_age;
            s.restarts = report.restarts;
            s.error = report.error;
            s.compare_remaining = remaining;
            s.accum_mode = static_cast<uint32_t>(report.accumulate);
            s.accum_samples = accumulator.samples;
            s.accum_generation = accumulator.generation;
            s.scene_face = scene_face;
            s.scene_remaining = scene_remaining.load();
            s.scene_support = scene_gpu.support();
            s.trace_state = static_cast<uint32_t>(trace_gpu.state());
            s.trace_triangles = trace_gpu.triangles();
            s.trace_samples = trace_gpu.passes();
            s.trace_mrays_x100 = static_cast<uint32_t>(std::min(trace_gpu.mrays() * 100, 4e9));
            s.trace_generation = static_cast<uint32_t>(trace_gpu.generation());
            s.trace_target = trace_gpu.target();
            s.trace_converged = trace_gpu.state() == TraceState::ready && trace_wrote && trace_gpu.target() != 0 && trace_gpu.passes() >= trace_gpu.target();
            s.trace_error_x10000 = static_cast<uint32_t>(std::min(trace_gpu.error() * 10000, 4e9));
            s.trace_light = trace_gpu.light() ? trace_gpu.light()->flags() : 0;
            s.trace_restarts = static_cast<uint32_t>(trace_gpu.starts());
            s.trace_restart_reason = static_cast<uint32_t>(trace_gpu.restart());
            s.shot_active = shot_frame ? 1 : 0;
            report.accumulated = accumulator.samples;
            published.line = describe(report);
            if (report.armed && published.line != status_noted && status_notes < kStatusNotes)
            {
                log(reshade::log::level::info, "frame " + std::to_string(frames) + ": " + published.line);
                status_noted = published.line;
                status_notes++;
            }
            status_box.put(published);
        }

        // The frame boundary. Runs before ReShade's effects, so the frame it closes is the game's whole frame.
        void on_present(command_queue *queue, swapchain *chain, const rect *, const rect *, uint32_t, const rect *)
        {
            if (!gpu.ready() && sticky_error == ENHANCEMENTS_ERROR_NONE)
                init(queue);

            frames++;
            presents.store(frames, std::memory_order_relaxed);
            if (gpu.ready())
                read_depth();
            if (report.armed)
                report.finish(tracker, settings, inputs());
            if (report.armed && report.fell_back.has(Step::bloom))
                diagnose_bloom();
            report.error = sticky_error != ENHANCEMENTS_ERROR_NONE ? sticky_error : frame_error;

            std::vector<Variant> variants;
            if (!capture && compare_box.take(variants, compare_seen))
                start_capture(std::move(variants));
            if (capture)
                if (const std::string why = tick_capture(chain); !why.empty())
                    stop_capture(why);
            const uint32_t remaining = compare_remaining.load();
            if (remaining == 0 && !capture)
                compare_count = 0;
            report.compare_count = compare_count;
            report.compare_saved = compare_count - std::min(remaining, compare_count);

            SceneRequest request;
            if (!scene && scene_box.take(request, scene_seen))
                begin_scene_capture(request);
            // After the take, so a capture asked for just before its release stops too.
            if (const uint64_t released = scene_release.exchange(0))
            {
                if (scene && scene_seen < released)
                    end_scene_capture(std::string("the caller released the scene"));
                if (trace_asked < released)
                    trace_gpu.release();
            }
            if (scene)
                tick_scene(chain);
            if (gpu.ready())
                tick_trace();
            const uint32_t scene_left = scene_remaining.load();
            if (scene_left == 0 && !scene)
                scene_count = 0;
            report.scene_count = scene_count;
            report.scene_saved = scene_count - std::min(scene_left, scene_count);
            report.scene_stopped = scene_stopped;
            publish(remaining);

            // A capture owns the settings until it ends; settings applied meanwhile take over after it, and so does a shot.
            if (!capture)
            {
                settings_box.take(applied, settings_seen);
                if (shot_box.take(shot, shot_seen))
                    log(reshade::log::level::info, shot.active ? "a traced shot begins: the trace and the gather run over the applied settings"
                        + (applied.enabled ? std::string() : std::string(", whose master switch is off, so alone")) + ", " + (shot.samples != 0
                        ? std::to_string(shot.samples) : "the applied") + " samples within " + (shot.budget_ms != 0 ? std::to_string(int(shot.budget_ms)) + " ms"
                        : std::string("the applied budget")) + " a frame." : std::string("the traced shot ended; the applied settings run as before."));
                settings = with_shot(applied, shot);
            }
            shot_frame = !capture && shot.active;
            if (camera_box.take(camera, camera_seen))
                camera_age = 0;
            else if (camera_age != UINT32_MAX)
                camera_age++;
            accumulate_box.take(accumulate, accumulate_seen);
            frame_error = ENHANCEMENTS_ERROR_NONE;
            sun_frame = SunCheck::unread;
            if (!wants_sun(settings))
                sun_parse = {};
            bool reading_depth;
            {
                std::lock_guard lock(depth_mutex);
                if (depth_ask.active && frames - depth_ask.last > kDepthForget)
                    depth_ask = {};
                reading_depth = depth_ask.active;
            }

            const bool arm = (settings.enabled || accumulate.mode != AccumulateMode::off || reading_depth || scene) && sticky_error == ENHANCEMENTS_ERROR_NONE
                && gpu.ready();
            scene_frame &= arm;
            scene_gbuffer &= scene_frame;
            scene_light &= scene_frame;
            scene_first &= scene_frame;
            in_gbuffer = false;
            if (scene_frame)
                scene_gpu.begin_frame();
            gpu.end_frame(arm && settings.enabled ? settings.view : View::off);
            tracker.trace(bloom_traces < kBloomTraces && frames < bloom_trace_until);
            tracker.begin_frame();
            report = FrameReport::start(settings);
            report.armed = arm;
            report.accumulate = arm ? accumulate.mode : AccumulateMode::off;
            trace_frame.reset();
            trace_traced = false;
            if (!report.armed || !settings.trace.enabled)
                trace_wrote = false;
            combine_copied = false;
            armed.store(arm, std::memory_order_relaxed);
        }

        // After ReShade's effects: the debug view goes on top of everything, except during a comparison capture.
        void on_reshade_present(effect_runtime *runtime)
        {
            if (!armed.load(std::memory_order_relaxed) || capture)
                return;
            const resource back_buffer = runtime->get_current_back_buffer();
            if (const char *moved = gpu.composite(reinterpret_cast<ID3D11Resource *>(back_buffer.handle), settings.flip))
                trip(moved);
        }

        // Every shader the game creates, on its loading threads: the vertex and domain shaders' bytecode, which a scene
        // capture replays, and the pixel shaders' hashes, which name them.
        void on_init_pipeline(device *, pipeline_layout, uint32_t count, const pipeline_subobject *subobjects, pipeline pipeline)
        {
            for (uint32_t i = 0; i < count; i++)
            {
                ShaderStage stage;
                switch (subobjects[i].type)
                {
                case pipeline_subobject_type::vertex_shader: stage = ShaderStage::vertex; break;
                case pipeline_subobject_type::domain_shader: stage = ShaderStage::domain; break;
                case pipeline_subobject_type::pixel_shader: stage = ShaderStage::pixel; break;
                default: continue;
                }
                const auto *desc = static_cast<const shader_desc *>(subobjects[i].data);
                if (subobjects[i].count == 0 || desc == nullptr || desc->code == nullptr || desc->code_size == 0)
                    continue;
                shaders.add(pipeline.handle, stage, desc->code, desc->code_size);
                return;
            }
        }

        void on_destroy_pipeline(device *, pipeline pipeline)
        {
            shaders.remove(pipeline.handle);
        }

        void on_destroy_device(device *dev)
        {
            if (!gpu.ready() || reinterpret_cast<ID3D11Device *>(dev->get_native()) != gpu.device())
                return;
            armed.store(false, std::memory_order_relaxed);
            gpu = Gpu {};
            immediate = nullptr;
            accumulator = {};
            depth_copy.reset();
            if (capture)
                stop_capture("the D3D11 device went away");
            if (scene)
                end_scene_capture(std::string("the D3D11 device went away"));
            scene_gpu = SceneGpu {};
            trace_gpu = TraceGpu {};
            trace_frame.reset();
            trace_traced = false;
            restart_burst = {};
        }
    }
}

using namespace enhancements;

extern "C" __declspec(dllexport) const char *NAME = "CameraTools Enhancements";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Finds Genshin's G-buffer and lighting buffers each frame, shows them as debug views, relights the world's ambient light, softens the sun's shadows and adds contact shadows to them, adds aerial perspective and sun shafts through a ground mist to the scene, replaces the game's tone map, averages lens samples into depth of field, saves comparison captures of settings variants, captures the scene's geometry and light around the camera, traces ambient occlusion and sun shadows against it, and gathers one bounce of diffuse light from it. Driven by CameraTools.";

extern "C" uint32_t EnhancementsVersion(void)
{
    return ENHANCEMENTS_VERSION;
}

extern "C" uint64_t EnhancementsApply(const EnhancementsSettings *raw)
{
    if (raw == nullptr || raw->size < offsetof(EnhancementsSettings, enabled) + sizeof raw->enabled)
        return settings_box.generation.load();
    // Only the bytes the caller says it has; parse_settings defaults the rest.
    EnhancementsSettings copy {};
    std::memcpy(&copy, raw, std::min<size_t>(raw->size, sizeof copy));
    return settings_box.put(parse_settings(copy));
}

extern "C" uint32_t EnhancementsCompare(const EnhancementsVariant *variants, uint32_t count)
{
    std::optional<std::vector<Variant>> parsed = parse_variants(variants, count);
    uint32_t idle = 0;
    if (!parsed || !compare_remaining.compare_exchange_strong(idle, count))
        return 0;
    compare_box.put(*parsed);
    return 1;
}

extern "C" uint32_t EnhancementsCaptureScene(const EnhancementsSceneRequest *raw)
{
    const std::optional<SceneRequest> request = parse_scene_request(raw);
    if (!request)
        return 0;
    // A release waits for no capture: it stops the one that runs.
    if (request->release)
    {
        scene_release = scene_box.generation.load() + 1;
        return 1;
    }
    // Raised before the render thread takes the request, so a second call is refused until every face is written.
    uint32_t idle = 0;
    if (!scene_remaining.compare_exchange_strong(idle, static_cast<uint32_t>(start_scene(*request).order.size())))
        return 0;
    scene_box.put(*request);
    return 1;
}

extern "C" void EnhancementsSetCamera(const EnhancementsCamera *raw)
{
    if (raw == nullptr)
        return;
    camera_box.put(parse_camera(*raw));
}

extern "C" void EnhancementsSetAccumulate(const EnhancementsAccumulate *raw)
{
    if (raw == nullptr || raw->size < offsetof(EnhancementsAccumulate, mode) + sizeof raw->mode)
        return;
    EnhancementsAccumulate copy {};
    std::memcpy(&copy, raw, std::min<size_t>(raw->size, sizeof copy));
    accumulate_box.put(parse_accumulate(copy));
}

extern "C" void EnhancementsSetShot(const EnhancementsShot *raw)
{
    // Only the bytes the caller says it has, as for the settings.
    EnhancementsShot copy {};
    if (raw != nullptr && raw->size >= offsetof(EnhancementsShot, active) + sizeof raw->active)
        std::memcpy(&copy, raw, std::min<size_t>(raw->size, sizeof copy));
    shot_box.put(parse_shot(&copy));
}

extern "C" float EnhancementsDepthAt(float u, float v)
{
    if (!(u >= 0 && u <= 1 && v >= 0 && v <= 1))
        return -1;
    const uint64_t now = presents.load(std::memory_order_relaxed);
    std::lock_guard lock(depth_mutex);
    if (!depth_ask.active || depth_ask.u != u || depth_ask.v != v)
        depth_ask = { true, u, v, now, now, -1, 0 };
    depth_ask.last = now;
    return depth_ask.answered > depth_ask.asked ? depth_ask.meters : -1;
}

extern "C" uint32_t EnhancementsSaveTrace(void)
{
    EnhancementsStatus status;
    {
        std::lock_guard lock(status_box.mutex);
        status = status_box.value.status;
    }
    // A stale scene keeps its history, which a save takes as it was; a ready one waits for its first full pass.
    const bool ready = status.trace_state == ENHANCEMENTS_TRACE_STATE_READY && status.trace_samples != 0;
    if (!(ready || status.trace_state == ENHANCEMENTS_TRACE_STATE_STALE) || trace_save_wanted.load())
        return 0;
    // The present first, so the render thread never sees the request with an older one.
    trace_save_asked = presents.load(std::memory_order_relaxed);
    trace_save_wanted = true;
    return 1;
}

extern "C" void EnhancementsGetStatus(EnhancementsStatus *status)
{
    if (status == nullptr || status->size < sizeof status->size)
        return;
    const uint32_t size = std::min<uint32_t>(status->size, sizeof *status);
    std::lock_guard lock(status_box.mutex);
    std::memcpy(status, &status_box.value.status, size);
    status->size = size;
}

extern "C" uint32_t EnhancementsDescribe(char *text, uint32_t size)
{
    if (text == nullptr || size == 0)
        return 0;
    std::lock_guard lock(status_box.mutex);
    const std::string &line = status_box.value.line;
    if (line.size() >= size)
    {
        text[0] = '\0';
        return 0;
    }
    line.copy(text, line.size());
    text[line.size()] = '\0';
    return static_cast<uint32_t>(line.size());
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        addon_module = module;
        if (!reshade::register_addon(module))
            return FALSE;
        reshade::register_event<reshade::addon_event::present>(on_present);
        reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);
        reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets);
        reshade::register_event<reshade::addon_event::clear_render_target_view>(on_clear_rtv);
        reshade::register_event<reshade::addon_event::draw>(on_draw);
        reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
        reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_draw_or_dispatch_indirect);
        // Before the game creates its device, so every shader's creation is seen, and so ReShade attaches the destroy
        // callback that destroy_pipeline needs to each of them.
        reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
        reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
        reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
        break;
    case DLL_PROCESS_DETACH:
        reshade::unregister_addon(module);
        break;
    }
    return TRUE;
}
