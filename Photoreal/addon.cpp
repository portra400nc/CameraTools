// The shell: ReShade events in, FrameEvents to the tracker, plans to the GPU, reports to the status mailbox.
// Everything that decides lives in core/; everything that draws lives in gpu.cpp.
#include "photoreal.h"
#include "core/compare.h"
#include "core/plan.h"
#include "core/png.h"
#include "gpu.h"

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
            PhotorealStatus status {};
            std::string line = "off";
        };

        Mailbox<Settings> settings_box;
        Mailbox<Camera> camera_box;
        Mailbox<Published> status_box;
        Mailbox<std::vector<Variant>> compare_box;

        // Variants of the comparison capture not yet saved. PhotorealCompare raises it from 0, and the worker lowers it
        // as it saves each variant's files, or to 0 when a stopped capture's note is written, so while it is not 0 no
        // second capture is accepted.
        std::atomic<uint32_t> compare_remaining { 0 };

        // The hot path's only check. Written by the render thread at present, so tracking never starts mid-frame.
        std::atomic<bool> armed { false };

        // Render-thread state.
        command_list *immediate = nullptr;
        Gpu gpu;
        FrameTracker tracker;
        FrameReport report;
        Settings settings;                  // this frame's, taken at present
        uint64_t settings_seen = 0, camera_seen = 0, compare_seen = 0;
        Camera camera {};
        uint32_t camera_age = UINT32_MAX;
        uint64_t frames = 0;
        uint32_t sticky_error = PHOTOREAL_ERROR_NONE;   // NOT_D3D11, SHADER or STATE: the add-on stays disarmed
        uint32_t frame_error = PHOTOREAL_ERROR_NONE;    // TEXTURE: this frame only
        HMODULE addon_module = nullptr;

        void log(reshade::log::level level, const std::string &message)
        {
            reshade::log::message(level, ("CameraTools Photoreal: " + message).c_str());
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

        // The work at one moment. Returns the event's return value: true skips the game's call.
        bool on_step(Step step)
        {
            const FrameMap &map = tracker.map();
            const Plan p = plan(step, map, settings, camera_known());
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
                    case PassId::contact_shadows:
                        call = gpu.run_contact_shadows(map, settings.contact_shadows, camera, settings.flip);
                        break;
                    case PassId::atmosphere:
                        call = gpu.run_atmosphere(map, settings.atmosphere, camera, settings.flip);
                        break;
                    case PassId::tonemap:
                        call = gpu.run_tonemap(map, settings.tonemap);
                        break;
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
            if (report.note_view(step, map) && !gpu.snapshot(*map[kViews[static_cast<size_t>(report.view)].entry]))
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
            const std::optional<Step> step = tracker.feed(event);
            return step && on_step(*step);
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
            feed(cmd_list, bind);
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

        bool on_draw(command_list *cmd_list, uint32_t, uint32_t, uint32_t, uint32_t) { return on_draw_any(cmd_list); }
        bool on_draw_indexed(command_list *cmd_list, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) { return on_draw_any(cmd_list); }

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
            published.line = describe(report);
            status_box.put(published);
        }

        // The frame boundary. Runs before ReShade's effects, so the frame it closes is the game's whole frame.
        void on_present(command_queue *queue, swapchain *chain, const rect *, const rect *, uint32_t, const rect *)
        {
            if (!gpu.ready() && sticky_error == PHOTOREAL_ERROR_NONE)
                init(queue);

            frames++;
            if (report.armed)
                report.finish(tracker, settings, camera_known());
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
            publish(remaining);

            // A capture owns the settings until it ends; settings applied meanwhile take over after it.
            if (!capture)
                settings_box.take(settings, settings_seen);
            if (camera_box.take(camera, camera_seen))
                camera_age = 0;
            else if (camera_age != UINT32_MAX)
                camera_age++;
            frame_error = PHOTOREAL_ERROR_NONE;

            const bool arm = settings.enabled && sticky_error == PHOTOREAL_ERROR_NONE && gpu.ready();
            gpu.end_frame(arm ? settings.view : View::off);
            tracker.begin_frame();
            report = FrameReport::start(settings);
            report.armed = arm;
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

        void on_destroy_device(device *dev)
        {
            if (!gpu.ready() || reinterpret_cast<ID3D11Device *>(dev->get_native()) != gpu.device())
                return;
            armed.store(false, std::memory_order_relaxed);
            gpu = Gpu {};
            immediate = nullptr;
            if (capture)
                stop_capture("the D3D11 device went away");
        }
    }
}

using namespace photoreal;

extern "C" __declspec(dllexport) const char *NAME = "CameraTools Photoreal";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Finds Genshin's G-buffer and lighting buffers each frame, shows them as debug views, relights the world's ambient light, adds contact shadows to the sun's and aerial perspective to the scene, replaces the game's tone map, and saves comparison captures of settings variants. Driven by CameraTools.";

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

extern "C" void PhotorealSetCamera(const PhotorealCamera *raw)
{
    if (raw == nullptr)
        return;
    camera_box.put(parse_camera(*raw));
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
        reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
        break;
    case DLL_PROCESS_DETACH:
        reshade::unregister_addon(module);
        break;
    }
    return TRUE;
}
