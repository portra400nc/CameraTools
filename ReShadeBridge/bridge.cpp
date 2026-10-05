#include "bridge.h"
#include "bridge_tables.h"

#include <reshade.hpp>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>

#define BRIDGE_STRINGIFY_VALUE(value) #value
#define BRIDGE_STRINGIFY(value) BRIDGE_STRINGIFY_VALUE(value)

using reshade::api::effect_runtime;

namespace
{
    struct State
    {
        std::mutex mutex;
        effect_runtime *runtime = nullptr;
        BridgeStatus status = {};
        std::optional<bool> set_effects;
        bool screenshot_requested = false;
        std::string last_screenshot;
        bridge::Tables tables;
    };

    State state;

    // Only the render thread touches these, so they need no lock.
    std::set<bridge::Key> warned_sets;
    bridge::OverrideState override_state;

    void warn_dropped_set(const bridge::Key &key)
    {
        const std::string message = "CameraTools ReShade Bridge dropped a set of \"" + key.second + "\" in \"" + key.first + "\", which the loaded effects do not have.";
        reshade::log::message(reshade::log::level::warning, message.c_str());
    }

    template <class Values, class Value>
    void queue_uniform_set(const char *effect, const char *name, const Value *values, uint32_t count)
    {
        if (effect == nullptr || name == nullptr || values == nullptr || count == 0)
            return;
        bridge::UniformSet set = { Values {}, std::min<uint32_t>(count, 4) };
        std::copy_n(values, set.count, std::get<Values>(set.values).begin());
        std::lock_guard lock(state.mutex);
        state.tables.uniform_sets.insert_or_assign(bridge::Key(effect, name), set);
    }

    // Starts watching the entry on the first call, and returns its reading when the render thread has made one.
    template <class Values>
    const Values *watch(bridge::Kind kind, const char *effect, const char *name)
    {
        const std::optional<bridge::Reading> &reading = state.tables.watched.try_emplace({ kind, bridge::Key(effect, name) }).first->second;
        return reading.has_value() ? std::get_if<Values>(&*reading) : nullptr;
    }

    template <class Values, class Value>
    uint32_t read_uniform(bridge::Kind kind, const char *effect, const char *name, Value *values, uint32_t count)
    {
        if (effect == nullptr || name == nullptr || values == nullptr || count == 0)
            return 0;
        std::lock_guard lock(state.mutex);
        const Values *const reading = watch<Values>(kind, effect, name);
        if (reading == nullptr)
            return 0;
        count = std::min<uint32_t>(count, 4);
        std::copy_n(reading->begin(), count, values);
        return count;
    }

    void on_init_effect_runtime(effect_runtime *runtime)
    {
        std::lock_guard lock(state.mutex);
        if (state.runtime != nullptr)
            return;
        state.runtime = runtime;
        state.status.runtime = 1;
        state.status.effects_ready = 0;
        runtime->get_screenshot_width_and_height(&state.status.width, &state.status.height);
    }

    void on_destroy_effect_runtime(effect_runtime *runtime)
    {
        std::lock_guard lock(state.mutex);
        if (state.runtime != runtime)
            return;
        state.runtime = nullptr;
        state.status.runtime = 0;
        state.status.effects_ready = 0;
        state.tables.clear_readings();
    }

    // Returns whether this call may take the screenshot. The lock is released before save_screenshot runs, because
    // that call can take a while and BridgeGetStatus must not wait for it.
    bool claim_screenshot(effect_runtime *runtime)
    {
        std::lock_guard lock(state.mutex);
        // A pending effects change applies at present, so the shot waits for the frame after it.
        if (state.runtime != runtime || !state.screenshot_requested || state.set_effects.has_value())
            return false;
        state.screenshot_requested = false;
        return true;
    }

    void on_finish_effects(effect_runtime *runtime, reshade::api::command_list *, reshade::api::resource_view rtv, reshade::api::resource_view)
    {
        reshade::api::device *const device = runtime->get_device();
        // save_screenshot expects the back buffer in the present state, and ReShade renders effects with it in the
        // render target state. Only the APIs without resource states do not care.
        const reshade::api::device_api api = device->get_api();
        if (api == reshade::api::device_api::d3d12 || api == reshade::api::device_api::vulkan)
            return;
        // Other add-ons can render effects into their own targets, any number of times per frame.
        if (device->get_resource_from_view(rtv) != runtime->get_current_back_buffer())
            return;
        if (claim_screenshot(runtime))
            runtime->save_screenshot(nullptr);
    }

    void on_present(effect_runtime *runtime)
    {
        std::optional<bool> set_effects;
        bridge::Tables tables;
        {
            std::lock_guard lock(state.mutex);
            if (state.runtime != runtime)
                return;
            set_effects.swap(state.set_effects);
            tables = state.tables.check_out();
        }

        if (set_effects.has_value())
            runtime->set_effects_state(*set_effects);
        else if (claim_screenshot(runtime))
            runtime->save_screenshot(nullptr);

        bridge::sync(runtime, tables, warned_sets, override_state, warn_dropped_set);

        const bool enabled = runtime->get_effects_state();
        const bool ready = bridge::effects_loaded(runtime);
        uint32_t width = 0, height = 0;
        runtime->get_screenshot_width_and_height(&width, &height);

        std::lock_guard lock(state.mutex);
        state.tables.check_in(std::move(tables));
        if (state.runtime != runtime)
            return;
        state.status.effects_enabled = enabled;
        state.status.effects_ready = ready;
        state.status.width = width;
        state.status.height = height;
        state.status.presents++;
    }

    void on_screenshot(effect_runtime *, const char *path)
    {
        std::lock_guard lock(state.mutex);
        state.last_screenshot = path;
        state.status.screenshots++;
    }
}

extern "C" __declspec(dllexport) const char *NAME = "CameraTools ReShade Bridge";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Lets CameraTools toggle effects, set uniforms and techniques, read preprocessor definitions, and save screenshots through C exports.";

extern "C" uint32_t BridgeVersion(void)
{
    return BRIDGE_VERSION;
}

extern "C" void BridgeGetStatus(BridgeStatus *status)
{
    std::lock_guard lock(state.mutex);
    *status = state.status;
}

extern "C" void BridgeSetEffects(uint32_t enabled)
{
    std::lock_guard lock(state.mutex);
    state.set_effects = enabled != 0;
}

extern "C" void BridgeSaveScreenshot(void)
{
    std::lock_guard lock(state.mutex);
    state.screenshot_requested = true;
}

extern "C" uint32_t BridgeGetLastScreenshot(char *path, uint32_t size)
{
    if (size == 0)
        return 0;
    std::lock_guard lock(state.mutex);
    if (state.last_screenshot.size() >= size)
    {
        path[0] = '\0';
        return 0;
    }
    state.last_screenshot.copy(path, state.last_screenshot.size());
    path[state.last_screenshot.size()] = '\0';
    return static_cast<uint32_t>(state.last_screenshot.size());
}

extern "C" void BridgeSetFloat(const char *effect, const char *name, const float *values, uint32_t count)
{
    queue_uniform_set<bridge::Floats>(effect, name, values, count);
}

extern "C" void BridgeSetInt(const char *effect, const char *name, const int32_t *values, uint32_t count)
{
    queue_uniform_set<bridge::Ints>(effect, name, values, count);
}

extern "C" void BridgeSetTechnique(const char *effect, const char *technique, uint32_t enabled)
{
    if (effect == nullptr || technique == nullptr)
        return;
    std::lock_guard lock(state.mutex);
    state.tables.technique_sets.insert_or_assign(bridge::Key(effect, technique), enabled != 0);
}

extern "C" uint32_t BridgeGetFloat(const char *effect, const char *name, float *values, uint32_t count)
{
    return read_uniform<bridge::Floats>(bridge::Kind::float_uniform, effect, name, values, count);
}

extern "C" uint32_t BridgeGetInt(const char *effect, const char *name, int32_t *values, uint32_t count)
{
    return read_uniform<bridge::Ints>(bridge::Kind::int_uniform, effect, name, values, count);
}

extern "C" uint32_t BridgeGetTechnique(const char *effect, const char *technique, uint32_t *enabled)
{
    if (effect == nullptr || technique == nullptr || enabled == nullptr)
        return 0;
    std::lock_guard lock(state.mutex);
    const bool *const reading = watch<bool>(bridge::Kind::technique, effect, technique);
    if (reading == nullptr)
        return 0;
    *enabled = *reading;
    return 1;
}

extern "C" uint32_t BridgeGetDefinition(const char *effect, const char *name, char *value, uint32_t size)
{
    if (effect == nullptr || name == nullptr || value == nullptr || size == 0)
        return 0;
    std::lock_guard lock(state.mutex);
    const std::string *const reading = watch<std::string>(bridge::Kind::definition, effect, name);
    if (reading == nullptr)
        return 0;
    const uint32_t length = std::min<uint32_t>(static_cast<uint32_t>(reading->size()), size - 1);
    std::copy_n(reading->begin(), length, value);
    value[length] = '\0';
    return length + 1;
}

extern "C" void BridgeSetOverride(const char *forced_effects, uint32_t solo)
{
    bridge::Override wanted;
    wanted.solo = solo != 0;
    for (const char *line = forced_effects; line != nullptr && *line != '\0';)
    {
        const char *const end = std::strchr(line, '\n');
        const std::string effect = end != nullptr ? std::string(line, end) : std::string(line);
        if (!effect.empty())
            wanted.forced.insert(effect);
        line = end != nullptr ? end + 1 : nullptr;
    }
    std::lock_guard lock(state.mutex);
    state.tables.override_request = std::move(wanted);
}

extern "C" void BridgeSavePreset(void)
{
    std::lock_guard lock(state.mutex);
    state.tables.save_preset = true;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(module))
            return FALSE;
        reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
        reshade::register_event<reshade::addon_event::reshade_finish_effects>(on_finish_effects);
        reshade::register_event<reshade::addon_event::reshade_present>(on_present);
        reshade::register_event<reshade::addon_event::reshade_screenshot>(on_screenshot);
        reshade::log::message(reshade::log::level::info, "CameraTools ReShade Bridge registered, bridge version " BRIDGE_STRINGIFY(BRIDGE_VERSION) ".");
        break;
    case DLL_PROCESS_DETACH:
        reshade::unregister_addon(module);
        break;
    }
    return TRUE;
}
