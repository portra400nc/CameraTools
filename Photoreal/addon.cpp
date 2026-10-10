// The shell: ReShade events in, FrameEvents to the tracker, plans to the GPU, reports to the status mailbox.
// Everything that decides lives in core/; everything that draws lives in gpu.cpp.
#include "photoreal.h"
#include "core/plan.h"
#include "gpu.h"

#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>

using namespace reshade::api;

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

        // The hot path's only check. Written by the render thread at present, so tracking never starts mid-frame.
        std::atomic<bool> armed { false };

        // Render-thread state.
        command_list *immediate = nullptr;
        Gpu gpu;
        FrameTracker tracker;
        FrameReport report;
        Settings settings;                  // this frame's, taken at present
        uint64_t settings_seen = 0, camera_seen = 0;
        Camera camera {};
        uint32_t camera_age = UINT32_MAX;
        uint64_t frames = 0;
        uint32_t sticky_error = PHOTOREAL_ERROR_NONE;   // NOT_D3D11, SHADER or STATE: the add-on stays disarmed
        uint32_t frame_error = PHOTOREAL_ERROR_NONE;    // TEXTURE: this frame only

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
                    for (ID3D11ShaderResourceView *srv : srvs)
                    {
                        if (srv == nullptr)
                            continue;
                        ID3D11Resource *resource = nullptr;
                        srv->GetResource(&resource);
                        Texture &t = draw_inputs[count_++];
                        t = { static_cast<ResourceId>(reinterpret_cast<uintptr_t>(resource)), Format::unknown, {} };
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

        void publish()
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
            published.line = describe(report);
            status_box.put(published);
        }

        // The frame boundary. Runs before ReShade's effects, so the frame it closes is the game's whole frame.
        void on_present(command_queue *queue, swapchain *, const rect *, const rect *, uint32_t, const rect *)
        {
            if (!gpu.ready() && sticky_error == PHOTOREAL_ERROR_NONE)
                init(queue);

            frames++;
            if (report.armed)
                report.finish(tracker, settings, camera_known());
            report.error = sticky_error != PHOTOREAL_ERROR_NONE ? sticky_error : frame_error;
            publish();

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

        // After ReShade's effects: the debug view goes on top of everything.
        void on_reshade_present(effect_runtime *runtime)
        {
            if (!armed.load(std::memory_order_relaxed))
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
        }
    }
}

using namespace photoreal;

extern "C" __declspec(dllexport) const char *NAME = "CameraTools Photoreal";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Finds Genshin's G-buffer and lighting buffers each frame, shows them as debug views, and relights the world's ambient light. Driven by CameraTools.";

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

extern "C" void PhotorealSetCamera(const PhotorealCamera *raw)
{
    if (raw == nullptr)
        return;
    Camera c;
    std::memcpy(c.world_to_view, raw->world_to_view, sizeof c.world_to_view);
    std::memcpy(c.view_to_clip, raw->view_to_clip, sizeof c.view_to_clip);
    camera_box.put(c);
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
