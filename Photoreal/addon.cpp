// The shell: ReShade events in, FrameEvents to the tracker, plans to the GPU, reports to the status mailbox.
// Everything that decides lives in core/; everything that draws lives in gpu.cpp.
#include "photoreal.h"
#include "core/compare.h"
#include "core/plan.h"
#include "core/png.h"
#include "core/scene_files.h"
#include "core/shaders.h"
#include "gpu.h"
#include "scene_gpu.h"

#include <reshade.hpp>

#include <algorithm>
#include <atomic>
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
#include <vector>

using namespace reshade::api;
namespace fs = std::filesystem;

namespace photoreal
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
            PhotorealStatus status = [] {
                PhotorealStatus s {};
                s.scene_face = PHOTOREAL_FACE_NONE;
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

        // The point PhotorealDepthAt asks about and its answer, shared by the C# thread and the render thread.
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

        // Variants of the comparison capture not yet saved. PhotorealCompare raises it from 0, and the worker lowers it
        // as it saves each variant's files, or to 0 when a stopped capture's note is written, so while it is not 0 no
        // second capture is accepted.
        std::atomic<uint32_t> compare_remaining { 0 };

        // Faces of the scene capture not yet written, which PhotorealCaptureScene raises from 0 and the worker lowers like
        // compare_remaining, and faces handed to the worker and not yet written, at which the schedule holds its next
        // capture.
        std::atomic<uint32_t> scene_remaining { 0 };
        std::atomic<uint32_t> scene_backlog { 0 };

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
        Settings settings;                  // this frame's, taken at present
        uint64_t settings_seen = 0, camera_seen = 0, compare_seen = 0, accumulate_seen = 0, scene_seen = 0;
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
        SunParse sun_parse;                 // the newest constants read back, from one or two frames before
        SunCheck sun_frame = SunCheck::unread;  // this frame's verdict on them
        uint64_t frames = 0;
        uint32_t sticky_error = PHOTOREAL_ERROR_NONE;   // NOT_D3D11, SHADER or STATE: the add-on stays disarmed
        uint32_t frame_error = PHOTOREAL_ERROR_NONE;    // TEXTURE: this frame only
        HMODULE addon_module = nullptr;

        void log(reshade::log::level level, const std::string &message)
        {
            reshade::log::message(level, ("CameraTools Photoreal: " + message).c_str());
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
            sticky_error = PHOTOREAL_ERROR_STATE;
            armed.store(false, std::memory_order_relaxed);
            log(reshade::log::level::error, std::string("the game's ") + moved + " differed after restoring state. Photoreal is off until the game restarts.");
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
            return base / "Photoreal";
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

        Inputs inputs() { return { camera_known(), sun_frame }; }

        // At the shadow-mask draw, before it runs: parse the newest finished readback of an earlier frame's constants,
        // then copy this draw's. The passes draw with this frame's copies on the GPU, so they stay exact while the camera
        // moves. The readback only decides whether they use them and gives the cascade layout, which changes only with
        // the graphics settings, and with it the atlas's size, which sun_check compares. A frame the scene capture replays
        // copies them too, so the capture can place its geometry with that frame's own camera.
        void gather_sun(const FrameMap &map)
        {
            if (!(wants_sun(settings) || scene_frame) || !map[Entry::sun_atlas])
                return;
            if (const std::optional<SunCbuffers> bytes = gpu.sun_constants(frames))
                sun_parse = parse_sun_shadows(*bytes);
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
                frame_error = PHOTOREAL_ERROR_TEXTURE;
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

        // The work at one moment. Returns the event's return value: true skips the game's call.
        bool on_step(Step step)
        {
            const FrameMap &map = tracker.map();
            if (step == Step::shadow_mask)
                gather_sun(map);
            const Plan p = plan(step, map, settings, inputs());
            report.note(p);
            bool skip = false;
            const char *moved = nullptr;
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
                        call = gpu.run_tonemap(map, settings.tonemap);
                        break;
                    case PassId::trace:     // runs only with a ready traced scene, which nothing builds yet
                    case PassId::count:
                        break;
                    }
                    if (!call)
                    {
                        report.note_failed(d.pass);
                        frame_error = PHOTOREAL_ERROR_TEXTURE;
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
            if (report.note_view(step, map) && !(view.from == PassId::light_shafts ? gpu.snapshot_light_shafts() : gpu.snapshot(*map[view.entry])))
            {
                report.view_found = false;
                frame_error = PHOTOREAL_ERROR_TEXTURE;
            }
            return skip;
        }

        bool feed(command_list *cmd_list, const FrameEvent &event)
        {
            if (cmd_list != immediate)
                return false;
            const StepSet moments = tracker.feed(event);
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
        };

        std::optional<SceneRun> scene;
        uint32_t scene_count = 0;           // the last scene capture's faces, for describe's progress until all are written
        std::string scene_stopped;          // why the last scene capture stopped, for describe until the next one starts
        uint32_t scene_face = PHOTOREAL_FACE_NONE;  // the face the last present published

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
            capture = Capture { std::move(variants), settings, 0, std::nullopt, {}, 0, files };
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
            worker.push([files = c.files, index, name = c.variants[index].name, line = c.lines[index], first = std::move(*first), second = std::move(*second)] {
                const std::vector<uint8_t> a = to_rgb(first), b = to_rgb(second);
                files->rows.push_back({ index, name, line, flicker(a, b), second.size });
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
            if (sticky_error != PHOTOREAL_ERROR_NONE)
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
            const CompareTick tick = compare_tick(c.present++, static_cast<uint32_t>(c.variants.size()));
            if (tick.copy)
            {
                const resource back_buffer = chain->get_current_back_buffer();
                if (!gpu.copy_back_buffer(reinterpret_cast<ID3D11Resource *>(back_buffer.handle), tick.copy->frame))
                    return "the back buffer is not 8-bit RGBA or BGRA, or its copy failed";
                if (tick.copy->frame == 1)
                    c.lines.push_back(describe(report));
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
            scene = std::move(run);
            if (gpu.ready())
                if (const std::optional<std::string> why = scene_gpu.begin())
                    scene->stop = why;
        }

        // Ends the scene capture: what was handed to the worker is still written, then stopped.txt with why, when there
        // is a reason.
        void end_scene_capture(const std::optional<std::string> &why)
        {
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
            scene_gpu.end();
            scene.reset();
            scene_frame = false;
            scene_face = PHOTOREAL_FACE_NONE;
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
            data.depth = std::move(got.depth);
            data.overflowed = got.full;
            data.origin = *run.reference;
            scene_backlog++;
            worker.push([folder = run.folder, request = run.schedule.request, data = std::move(data), picture = std::move(got.picture)]() mutable {
                if (picture)
                {
                    data.rgb = to_rgb(*picture);
                    data.picture = picture->size;
                }
                log_unwritten(write_scene_face(*folder, data, request));
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
            if (sticky_error != PHOTOREAL_ERROR_NONE)
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
            if (tick.copy_at_present)
            {
                // The frame that just ended: its report, its camera, which the mailbox has not replaced yet, and its map.
                run.frame = frames - 1;
                run.status = describe(report);
                run.gbuffer = report.found.has(Step::gbuffer);
                run.pushed = camera_known() ? std::optional<Camera>(camera) : std::nullopt;
                run.sun.reset();
                const resource back_buffer = chain->get_current_back_buffer();
                if (!scene_gpu.copy_at_present(tracker.map()[Entry::depth], reinterpret_cast<ID3D11Resource *>(back_buffer.handle)))
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
                sticky_error = PHOTOREAL_ERROR_NOT_D3D11;
                log(reshade::log::level::warning, "only D3D11 is supported.");
                return;
            }
            immediate = queue->get_immediate_command_list();
            if (!gpu.init(reinterpret_cast<ID3D11Device *>(dev->get_native()), reinterpret_cast<ID3D11DeviceContext *>(immediate->get_native())))
            {
                sticky_error = PHOTOREAL_ERROR_SHADER;
                log(reshade::log::level::error, "could not create its shaders or states. Photoreal is off.");
                return;
            }
            scene_gpu.init(gpu.device(), gpu.context(), gpu.no_write(), gpu.no_depth());
            log(reshade::log::level::info, "ready.");
        }

        void publish(uint32_t remaining)
        {
            Published published;
            PhotorealStatus &s = published.status;
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
            report.accumulated = accumulator.samples;
            published.line = describe(report);
            status_box.put(published);
        }

        // The frame boundary. Runs before ReShade's effects, so the frame it closes is the game's whole frame.
        void on_present(command_queue *queue, swapchain *chain, const rect *, const rect *, uint32_t, const rect *)
        {
            if (!gpu.ready() && sticky_error == PHOTOREAL_ERROR_NONE)
                init(queue);

            frames++;
            presents.store(frames, std::memory_order_relaxed);
            if (gpu.ready())
                read_depth();
            if (report.armed)
                report.finish(tracker, settings, inputs());
            if (report.armed && report.fell_back.has(Step::bloom))
                diagnose_bloom();
            report.error = sticky_error != PHOTOREAL_ERROR_NONE ? sticky_error : frame_error;

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
            if (scene)
                tick_scene(chain);
            const uint32_t scene_left = scene_remaining.load();
            if (scene_left == 0 && !scene)
                scene_count = 0;
            report.scene_count = scene_count;
            report.scene_saved = scene_count - std::min(scene_left, scene_count);
            report.scene_stopped = scene_stopped;
            publish(remaining);

            // A capture owns the settings until it ends; settings applied meanwhile take over after it.
            if (!capture)
                settings_box.take(settings, settings_seen);
            if (camera_box.take(camera, camera_seen))
                camera_age = 0;
            else if (camera_age != UINT32_MAX)
                camera_age++;
            accumulate_box.take(accumulate, accumulate_seen);
            frame_error = PHOTOREAL_ERROR_NONE;
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

            const bool arm = (settings.enabled || accumulate.mode != AccumulateMode::off || reading_depth || scene) && sticky_error == PHOTOREAL_ERROR_NONE
                && gpu.ready();
            scene_frame &= arm;
            in_gbuffer = false;
            if (scene_frame)
                scene_gpu.begin_frame();
            gpu.end_frame(arm && settings.enabled ? settings.view : View::off);
            tracker.trace(bloom_traces < kBloomTraces && frames < bloom_trace_until);
            tracker.begin_frame();
            report = FrameReport::start(settings);
            report.armed = arm;
            report.accumulate = arm ? accumulate.mode : AccumulateMode::off;
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
        }
    }
}

using namespace photoreal;

extern "C" __declspec(dllexport) const char *NAME = "CameraTools Photoreal";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Finds Genshin's G-buffer and lighting buffers each frame, shows them as debug views, relights the world's ambient light, softens the sun's shadows and adds contact shadows to them, adds aerial perspective and sun shafts through a ground mist to the scene, replaces the game's tone map, averages lens samples into depth of field, saves comparison captures of settings variants, and captures the scene's geometry around the camera. Driven by CameraTools.";

extern "C" uint32_t PhotorealVersion(void)
{
    return PHOTOREAL_VERSION;
}

extern "C" uint64_t PhotorealApply(const PhotorealSettings *raw)
{
    if (raw == nullptr || raw->size < offsetof(PhotorealSettings, enabled) + sizeof raw->enabled)
        return settings_box.generation.load();
    // Only the bytes the caller says it has; parse_settings defaults the rest.
    PhotorealSettings copy {};
    std::memcpy(&copy, raw, std::min<size_t>(raw->size, sizeof copy));
    return settings_box.put(parse_settings(copy));
}

extern "C" uint32_t PhotorealCompare(const PhotorealVariant *variants, uint32_t count)
{
    std::optional<std::vector<Variant>> parsed = parse_variants(variants, count);
    uint32_t idle = 0;
    if (!parsed || !compare_remaining.compare_exchange_strong(idle, count))
        return 0;
    compare_box.put(*parsed);
    return 1;
}

extern "C" uint32_t PhotorealCaptureScene(const PhotorealSceneRequest *raw)
{
    const std::optional<SceneRequest> request = parse_scene_request(raw);
    if (!request)
        return 0;
    // Raised before the render thread takes the request, so a second call is refused until every face is written.
    uint32_t idle = 0;
    if (!scene_remaining.compare_exchange_strong(idle, static_cast<uint32_t>(start_scene(*request).order.size())))
        return 0;
    scene_box.put(*request);
    return 1;
}

extern "C" void PhotorealSetCamera(const PhotorealCamera *raw)
{
    if (raw == nullptr)
        return;
    camera_box.put(parse_camera(*raw));
}

extern "C" void PhotorealSetAccumulate(const PhotorealAccumulate *raw)
{
    if (raw == nullptr || raw->size < offsetof(PhotorealAccumulate, mode) + sizeof raw->mode)
        return;
    PhotorealAccumulate copy {};
    std::memcpy(&copy, raw, std::min<size_t>(raw->size, sizeof copy));
    accumulate_box.put(parse_accumulate(copy));
}

extern "C" float PhotorealDepthAt(float u, float v)
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

extern "C" void PhotorealGetStatus(PhotorealStatus *status)
{
    if (status == nullptr || status->size < sizeof status->size)
        return;
    const uint32_t size = std::min<uint32_t>(status->size, sizeof *status);
    std::lock_guard lock(status_box.mutex);
    std::memcpy(status, &status_box.value.status, size);
    status->size = size;
}

extern "C" uint32_t PhotorealDescribe(char *text, uint32_t size)
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
