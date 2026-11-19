// The add-on ReShade loads, CameraToolsEnhancements.addon64: a stub that stays loaded for the game's life and runs the
// add-on proper, CameraToolsEnhancementsImpl.dll, beside it. It loads that again in the running game when asked, through
// EnhancementsReload or a file named reload in the Enhancements folder, so a rebuilt add-on takes effect without a
// restart. See host.h for what each side keeps.
#include "enhancements.h"
#include "host.h"

#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <type_traits>

using namespace reshade::api;
using reshade::addon_event;
namespace fs = std::filesystem;
using namespace enhancements;

// The add-on proper's C exports, which the stub's own of the same names forward. One more needs a new stub.
#define ENHANCEMENTS_FORWARDED_EXPORTS(X) \
    X(EnhancementsVersion) X(EnhancementsApply) X(EnhancementsSetCamera) X(EnhancementsSetCelestial) X(EnhancementsGetWorldLight) \
    X(EnhancementsCompare) X(EnhancementsCaptureScene) X(EnhancementsGetStatus) X(EnhancementsSaveTrace) X(EnhancementsSaveFrame) \
    X(EnhancementsDevelopDng) X(EnhancementsWriteDng) X(EnhancementsSetAccumulate) X(EnhancementsSetShot) X(EnhancementsSetShotLights) \
    X(EnhancementsSetGameLights) X(EnhancementsSetSurfaceDetail) X(EnhancementsSurfaceWantsLearn) X(EnhancementsSurfaceLastPick) \
    X(EnhancementsSetCharacterLights) X(EnhancementsDepthAt) X(EnhancementsDescribe)

namespace
{
    // ReShade's addon_event::max, which its public header leaves out: one past reshade_overlay_technique.
    constexpr uint32_t kEvents = static_cast<uint32_t>(addon_event::reshade_overlay_technique) + 1;

    enum ExportIndex : size_t
    {
#define ENHANCEMENTS_INDEX(name) k##name,
        ENHANCEMENTS_FORWARDED_EXPORTS(ENHANCEMENTS_INDEX)
#undef ENHANCEMENTS_INDEX
        kExports
    };
    constexpr const char *kExportNames[kExports] = {
#define ENHANCEMENTS_NAME(name) #name,
        ENHANCEMENTS_FORWARDED_EXPORTS(ENHANCEMENTS_NAME)
#undef ENHANCEMENTS_NAME
    };

    constexpr bool forwarded(addon_event event)
    {
#define ENHANCEMENTS_IS(name) if (event == addon_event::name) return true;
        ENHANCEMENTS_FORWARDED_EVENTS(ENHANCEMENTS_IS)
#undef ENHANCEMENTS_IS
        return false;
    }

    // Every shader the game created, from the events of their creation on its loading threads, kept here because a
    // reloaded add-on would never see them created.
    ShaderRegistry shaders;

    // The add-on's entry points. A reload replaces them under the lock taken exclusively. Every other thread holds it shared
    // while it runs the add-on's code, so none is inside the add-on when it unloads. The render thread, where the reload
    // runs between frames, takes no lock: the draws' events would pay for it thousands of times a frame.
    SRWLOCK lock = SRWLOCK_INIT;
    std::atomic<DWORD> render_thread { 0 };
    thread_local uint32_t lock_depth = 0;
    void *listeners[kEvents] = {};
    void *exports[kExports] = {};

    // Shared, taken once however deep a thread's calls nest, so a call back into the stub from the add-on cannot wait behind
    // a reload that waits for it.
    class Hold
    {
    public:
        Hold() : locked_(GetCurrentThreadId() != render_thread.load(std::memory_order_relaxed))
        {
            if (locked_ && lock_depth++ == 0)
                AcquireSRWLockShared(&lock);
        }
        ~Hold()
        {
            if (locked_ && --lock_depth == 0)
                ReleaseSRWLockShared(&lock);
        }
        Hold(const Hold &) = delete;
        Hold &operator=(const Hold &) = delete;

    private:
        bool locked_;
    };

    struct Loaded
    {
        HMODULE module = nullptr;
        fs::path file;
        EnhancementsStartCall start = nullptr;
        EnhancementsStopCall stop = nullptr;
    };

    HMODULE stub_module = nullptr;
    Host host {};
    Loaded loaded;
    uint32_t loads = 0;
    uint32_t copies = 0;
    fs::path impl_file;     // CameraToolsEnhancementsImpl.dll beside the stub, which an install replaces
    fs::path copy_folder;   // where each load's own copy goes
    fs::path trigger_file;  // a file whose appearance asks for a reload
    std::atomic<bool> reload_asked { false };
    ULONGLONG trigger_checked = 0;

    void log(reshade::log::level level, const std::string &message)
    {
        reshade::log::message(level, ("CameraTools Enhancements: " + message).c_str());
    }

    double ms_since(std::chrono::steady_clock::time_point then)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - then).count();
    }

    std::string format_ms(double ms)
    {
        char text[32];
        std::snprintf(text, sizeof text, "%.0f ms", ms);
        return text;
    }

    bool listen(uint32_t event, void *callback)
    {
        if (event >= kEvents || !forwarded(static_cast<addon_event>(event)))
            return false;
        listeners[event] = callback;
        return true;
    }

    void add_shader(uint64_t handle, ShaderStage stage, const void *code, size_t size)
    {
        shaders.add(handle, stage, code, size);
    }

    void remove_shader(uint64_t handle)
    {
        shaders.remove(handle);
    }

    // Runs here, so the shared code's control blocks are the stub's and outlive any copy of the add-on.
    bool find_shader(uint64_t handle, ShaderCode *code)
    {
        std::optional<ShaderCode> found = shaders.find(handle);
        if (!found)
            return false;
        *code = std::move(*found);
        return true;
    }

    template <addon_event ev, typename Decl = typename reshade::addon_event_traits<ev>::decl>
    struct Forward;

    template <addon_event ev, typename R, typename... A>
    struct Forward<ev, R (*)(A...)>
    {
        static R call(A... args)
        {
            Hold hold;
            const auto callback = reinterpret_cast<R (*)(A...)>(listeners[static_cast<uint32_t>(ev)]);
            if (callback != nullptr)
                return callback(args...);
            if constexpr (!std::is_void_v<R>)
                return R {};
        }
    };

    template <typename R, typename... A, typename... B>
    R forward(R (*)(A...), ExportIndex index, B... args)
    {
        Hold hold;
        const auto call = reinterpret_cast<R (*)(A...)>(exports[index]);
        if (call != nullptr)
            return call(args...);
        if constexpr (!std::is_void_v<R>)
            return R {};
    }

    void clear_entry_points()
    {
        std::fill(std::begin(listeners), std::end(listeners), nullptr);
        std::fill(std::begin(exports), std::end(exports), nullptr);
    }

    // Copies CameraToolsEnhancementsImpl.dll to a file of this load's own and loads that, so an install can replace the
    // first while this copy runs, then stops the loaded add-on and starts the new one. A copy that fails to load or start
    // leaves the loaded one running.
    bool load_impl(const char *why)
    {
        const auto began = std::chrono::steady_clock::now();
        std::error_code error;
        fs::create_directories(copy_folder, error);
        const fs::path file = copy_folder / ("CameraToolsEnhancementsImpl-" + std::to_string(++copies) + ".dll");
        if (!CopyFileW(impl_file.c_str(), file.c_str(), FALSE))
        {
            log(reshade::log::level::error, "could not copy " + impl_file.u8string() + " to " + file.u8string() + ": error " + std::to_string(GetLastError())
                + ". The add-on stays as it was.");
            return false;
        }
        const HMODULE module = LoadLibraryExW(file.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (module == nullptr)
        {
            log(reshade::log::level::error, "could not load " + file.u8string() + ": error " + std::to_string(GetLastError()) + ". The add-on stays as it was.");
            DeleteFileW(file.c_str());
            return false;
        }
        const auto version = reinterpret_cast<EnhancementsHostVersionCall>(GetProcAddress(module, "EnhancementsHostVersion"));
        const auto start = reinterpret_cast<EnhancementsStartCall>(GetProcAddress(module, "EnhancementsStart"));
        const auto stop = reinterpret_cast<EnhancementsStopCall>(GetProcAddress(module, "EnhancementsStop"));
        if (version == nullptr || start == nullptr || stop == nullptr || version() != ENHANCEMENTS_HOST_VERSION)
        {
            log(reshade::log::level::error, impl_file.filename().u8string() + " was built for host version " + (version ? std::to_string(version()) : std::string("none"))
                + ", and this stub is version " + std::to_string(ENHANCEMENTS_HOST_VERSION) + ": install both and restart the game. The add-on stays as it was.");
            FreeLibrary(module);
            DeleteFileW(file.c_str());
            return false;
        }
        void *found[kExports];
        for (size_t i = 0; i < kExports; i++)
            found[i] = reinterpret_cast<void *>(GetProcAddress(module, kExportNames[i]));
        const double load_ms = ms_since(began);

        const auto waited = std::chrono::steady_clock::now();
        AcquireSRWLockExclusive(&lock);
        const double wait_ms = ms_since(waited);
        const auto stopping = std::chrono::steady_clock::now();
        const Loaded previous = loaded;
        if (previous.module != nullptr)
            previous.stop();
        const double stop_ms = ms_since(stopping);
        clear_entry_points();
        host.loads = loads + 1;
        const bool started = start(&host);
        if (started)
        {
            std::copy(std::begin(found), std::end(found), std::begin(exports));
            loaded = { module, file, start, stop };
            loads++;
        }
        else
        {
            // The one that ran before carries on: its stop released what it held, and it starts again as on a first present.
            clear_entry_points();
            host.loads = loads;
            if (previous.module != nullptr && previous.start(&host))
                for (size_t i = 0; i < kExports; i++)
                    exports[i] = reinterpret_cast<void *>(GetProcAddress(previous.module, kExportNames[i]));
        }
        ReleaseSRWLockExclusive(&lock);

        const Loaded unload = started ? previous : Loaded { module, file, start, stop };
        if (unload.module != nullptr)
        {
            FreeLibrary(unload.module);
            DeleteFileW(unload.file.c_str());
        }
        if (!started)
        {
            log(reshade::log::level::error, file.filename().u8string() + " did not start: it listens to an event this stub does not forward. Install both and restart the game.");
            return false;
        }
        log(reshade::log::level::info, "loaded " + file.filename().u8string() + " (" + why + ", load " + std::to_string(loads) + ") in " + format_ms(ms_since(began))
            + ": " + format_ms(load_ms) + " to copy and load it, " + format_ms(wait_ms) + " waiting for other threads to leave the add-on, "
            + format_ms(stop_ms) + " for the previous one to stop. It sets itself up at the next present.");
        return true;
    }

    // Between frames, before the add-on sees the present: a reload asked for by the export or the trigger file.
    void on_present(command_queue *queue, swapchain *chain, const rect *source, const rect *dest, uint32_t dirty_count, const rect *dirty)
    {
        render_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
        const ULONGLONG now = GetTickCount64();
        bool triggered = false;
        if (now - trigger_checked >= 500)
        {
            trigger_checked = now;
            triggered = GetFileAttributesW(trigger_file.c_str()) != INVALID_FILE_ATTRIBUTES;
            if (triggered)
                DeleteFileW(trigger_file.c_str());
        }
        if (reload_asked.exchange(false) || triggered)
            load_impl(triggered ? "asked for by the reload file" : "asked for by EnhancementsReload");
        Forward<addon_event::present>::call(queue, chain, source, dest, dirty_count, dirty);
    }

    // Every shader the game creates, on its loading threads: the vertex and domain shaders' bytecode, which a scene
    // capture replays, and the pixel shaders', whose hashes name them.
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

    template <addon_event ev>
    void register_forward()
    {
        if constexpr (ev == addon_event::present)
            reshade::register_event<ev>(on_present);
        else
            reshade::register_event<ev>(&Forward<ev>::call);
    }

    fs::path enhancements_folder()
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
            GetModuleFileNameW(stub_module, module_path, MAX_PATH);
            base = fs::path(module_path).parent_path().parent_path();
        }
        return base / "Enhancements";
    }
}

extern "C" __declspec(dllexport) const char *NAME = "CameraTools Enhancements";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Finds Genshin's G-buffer and lighting buffers each frame, shows them as debug views, relights the world's ambient light, softens the sun's shadows and adds contact shadows to them, adds aerial perspective and sun shafts through a ground mist to the scene, replaces the game's tone map, averages lens samples into depth of field, saves comparison captures of settings variants, captures the scene's geometry and light around the camera, traces ambient occlusion and sun shadows against it, gathers one bounce of diffuse light from it, and develops a raw DNG of a screenshot's frame. Driven by CameraTools. A stub that runs CameraToolsEnhancementsImpl.dll and loads it again while the game runs.";

// ReShade calls it right after loading the stub, outside DllMain, where loading another library is safe; with the device
// being created, so every shader's creation is seen, and ReShade attaches the destroy callback that destroy_pipeline needs
// to each of them.
extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE reshade_module)
{
    stub_module = addon_module;
    if (!reshade::register_addon(addon_module, reshade_module))
        return false;
    reshade::register_event<addon_event::init_pipeline>(on_init_pipeline);
    reshade::register_event<addon_event::destroy_pipeline>(on_destroy_pipeline);
#define ENHANCEMENTS_REGISTER(name) register_forward<addon_event::name>();
    ENHANCEMENTS_FORWARDED_EVENTS(ENHANCEMENTS_REGISTER)
#undef ENHANCEMENTS_REGISTER

    host = Host { ENHANCEMENTS_HOST_VERSION, addon_module, 0, listen, add_shader, remove_shader, find_shader };
    wchar_t module_path[MAX_PATH] = {};
    GetModuleFileNameW(addon_module, module_path, MAX_PATH);
    impl_file = fs::path(module_path).parent_path() / "CameraToolsEnhancementsImpl.dll";
    const fs::path folder = enhancements_folder();
    copy_folder = folder / "loaded";
    trigger_file = folder / "reload";
    // Copies a crash left behind; none is loaded before the first load.
    std::error_code error;
    for (const fs::directory_entry &entry : fs::directory_iterator(copy_folder, error))
        DeleteFileW(entry.path().c_str());
    DeleteFileW(trigger_file.c_str());
    load_impl("the game started");
    return true;
}

// ReShade unloads its add-ons with the last device.
extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE)
{
    AcquireSRWLockExclusive(&lock);
    const Loaded previous = loaded;
    if (previous.module != nullptr)
        previous.stop();
    clear_entry_points();
    loaded = {};
    ReleaseSRWLockExclusive(&lock);
    if (previous.module != nullptr)
    {
        FreeLibrary(previous.module);
        DeleteFileW(previous.file.c_str());
    }
    reshade::unregister_addon(addon_module);
}

extern "C" uint32_t EnhancementsReload(void)
{
    reload_asked = true;
    return 1;
}

extern "C" uint32_t EnhancementsLoads(void)
{
    return loads;
}

extern "C" uint32_t EnhancementsVersion(void) { return forward(EnhancementsVersion, kEnhancementsVersion); }
extern "C" uint64_t EnhancementsApply(const EnhancementsSettings *a) { return forward(EnhancementsApply, kEnhancementsApply, a); }
extern "C" void EnhancementsSetCamera(const EnhancementsCamera *a) { forward(EnhancementsSetCamera, kEnhancementsSetCamera, a); }
extern "C" void EnhancementsSetCelestial(const EnhancementsCelestial *a) { forward(EnhancementsSetCelestial, kEnhancementsSetCelestial, a); }
extern "C" void EnhancementsGetWorldLight(const EnhancementsCelestial *a, float b, EnhancementsWorldLight *c) { forward(EnhancementsGetWorldLight, kEnhancementsGetWorldLight, a, b, c); }
extern "C" uint32_t EnhancementsCompare(const EnhancementsVariant *a, uint32_t b) { return forward(EnhancementsCompare, kEnhancementsCompare, a, b); }
extern "C" uint32_t EnhancementsCaptureScene(const EnhancementsSceneRequest *a) { return forward(EnhancementsCaptureScene, kEnhancementsCaptureScene, a); }
extern "C" void EnhancementsGetStatus(EnhancementsStatus *a) { forward(EnhancementsGetStatus, kEnhancementsGetStatus, a); }
extern "C" uint32_t EnhancementsSaveTrace(void) { return forward(EnhancementsSaveTrace, kEnhancementsSaveTrace); }
extern "C" uint32_t EnhancementsSaveFrame(void) { return forward(EnhancementsSaveFrame, kEnhancementsSaveFrame); }
extern "C" uint32_t EnhancementsDevelopDng(uint32_t a) { return forward(EnhancementsDevelopDng, kEnhancementsDevelopDng, a); }
extern "C" uint32_t EnhancementsWriteDng(uint32_t a, const char *b, const EnhancementsDngInfo *c) { return forward(EnhancementsWriteDng, kEnhancementsWriteDng, a, b, c); }
extern "C" void EnhancementsSetAccumulate(const EnhancementsAccumulate *a) { forward(EnhancementsSetAccumulate, kEnhancementsSetAccumulate, a); }
extern "C" void EnhancementsSetShot(const EnhancementsShot *a) { forward(EnhancementsSetShot, kEnhancementsSetShot, a); }
extern "C" void EnhancementsSetShotLights(const EnhancementsShotLight *a, uint32_t b) { forward(EnhancementsSetShotLights, kEnhancementsSetShotLights, a, b); }
extern "C" void EnhancementsSetGameLights(const EnhancementsShotLight *a, uint32_t b) { forward(EnhancementsSetGameLights, kEnhancementsSetGameLights, a, b); }
extern "C" void EnhancementsSetSurfaceDetail(float a, const uint64_t *b, uint32_t c) { forward(EnhancementsSetSurfaceDetail, kEnhancementsSetSurfaceDetail, a, b, c); }
extern "C" uint32_t EnhancementsSurfaceWantsLearn() { return forward(EnhancementsSurfaceWantsLearn, kEnhancementsSurfaceWantsLearn); }
extern "C" uint64_t EnhancementsSurfaceLastPick() { return forward(EnhancementsSurfaceLastPick, kEnhancementsSurfaceLastPick); }
extern "C" void EnhancementsSetCharacterLights(const EnhancementsCharacterLights *a) { forward(EnhancementsSetCharacterLights, kEnhancementsSetCharacterLights, a); }
extern "C" float EnhancementsDepthAt(float a, float b) { return forward(EnhancementsDepthAt, kEnhancementsDepthAt, a, b); }
extern "C" uint32_t EnhancementsDescribe(char *a, uint32_t b) { return forward(EnhancementsDescribe, kEnhancementsDescribe, a, b); }
