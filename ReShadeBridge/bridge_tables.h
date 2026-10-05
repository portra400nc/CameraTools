// The queued sets and watched gets of the bridge, and the one function that plays them against a ReShade runtime.
// It names no ReShade type, so it also compiles against a fake runtime off Windows.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace bridge
{
    // An effect file name and the name of a uniform or technique in it.
    using Key = std::pair<std::string, std::string>;
    using Floats = std::array<float, 4>;
    using Ints = std::array<int32_t, 4>;

    struct UniformSet
    {
        std::variant<Floats, Ints> values;
        uint32_t count;
    };

    enum class Kind
    {
        float_uniform,
        int_uniform,
        technique,
        definition,
    };

    // The alternative always matches the entry's Kind. A definition the effect does not have reads as an empty string.
    using Reading = std::variant<Floats, Ints, bool, std::string>;

    // Techniques a mod keeps on or off whatever the preset says: every technique of a forced effect file is on, and
    // with solo every other technique is off. An empty request with solo off releases the override.
    struct Override
    {
        std::set<std::string> forced;
        bool solo = false;

        bool active() const { return solo || !forced.empty(); }
    };

    // Lives on the render thread for as long as the add-on. Original holds each technique's state from before the
    // override first changed it, which is what a release puts back; a set through the bridge while the override holds
    // updates it, because that is the mod's or the user's own choice.
    struct OverrideState
    {
        Override wanted;
        std::map<Key, bool> original;
    };

    struct Tables
    {
        std::map<Key, UniformSet> uniform_sets;
        std::map<Key, bool> technique_sets;
        std::map<std::pair<Kind, Key>, std::optional<Reading>> watched;
        bool save_preset = false;
        std::optional<Override> override_request;

        // Moves the requests out for the render thread to work on without the lock. The watched entries are copied,
        // so that a get made meanwhile still reads the previous present's values.
        Tables check_out()
        {
            Tables work;
            work.uniform_sets.swap(uniform_sets);
            work.technique_sets.swap(technique_sets);
            work.watched = watched;
            std::swap(work.save_preset, save_preset);
            work.override_request.swap(override_request);
            return work;
        }

        // Takes back what the render thread could not apply, and its readings. A set made meanwhile wins over an
        // older one of the same key, and an entry watched meanwhile stays without a value.
        void check_in(Tables &&work)
        {
            uniform_sets.merge(work.uniform_sets);
            technique_sets.merge(work.technique_sets);
            work.watched.merge(watched);
            watched.swap(work.watched);
            save_preset = save_preset || work.save_preset;
            if (!override_request.has_value())
                override_request.swap(work.override_request);
        }

        void clear_readings()
        {
            for (auto &entry : watched)
                entry.second.reset();
        }
    };

    // ReShade exposes no "is loading" query, but enumerate_techniques returns nothing while effects load or compile.
    // reshade_reloaded_effects cannot serve here: it also fires when a reload starts, right after the old effects
    // are destroyed, and it does not fire at the end of a reload that enables no technique.
    template <class Runtime>
    bool effects_loaded(Runtime *runtime)
    {
        bool loaded = false;
        runtime->enumerate_techniques(nullptr, [&loaded](auto *, auto) { loaded = true; });
        return loaded;
    }

    // Applies and removes each set whose name the runtime finds, and removes with one warning each set whose name
    // the loaded effects do not have. It stops and keeps the rest once the runtime is loading again, which enabling
    // a technique of an effect that has not been created yet causes: every find fails from then on.
    template <class Runtime, class Sets, class Find, class Apply, class Warn>
    void apply_sets(Runtime *runtime, Sets &sets, std::set<Key> &warned, Find find, Apply apply, Warn warn)
    {
        for (auto it = sets.begin(); it != sets.end(); it = sets.erase(it))
        {
            const Key &key = it->first;
            if (const auto handle = find(key.first.c_str(), key.second.c_str()); handle.handle != 0)
                apply(handle, it->second);
            else if (!effects_loaded(runtime))
                return;
            else if (warned.insert(key).second)
                warn(key);
        }
    }

    template <class Runtime>
    using TechniqueHandle = decltype(std::declval<Runtime &>().find_technique("", ""));

    template <class Runtime>
    std::vector<std::pair<Key, TechniqueHandle<Runtime>>> list_techniques(Runtime *runtime)
    {
        std::vector<TechniqueHandle<Runtime>> handles;
        runtime->enumerate_techniques(nullptr, [&handles](auto *, auto technique) { handles.push_back(technique); });
        std::vector<std::pair<Key, TechniqueHandle<Runtime>>> listed;
        for (const auto handle : handles)
        {
            char effect[256] = {}, name[256] = {};
            size_t effect_size = sizeof(effect), name_size = sizeof(name);
            runtime->get_technique_effect_name(handle, effect, &effect_size);
            runtime->get_technique_name(handle, name, &name_size);
            listed.emplace_back(Key(effect, name), handle);
        }
        return listed;
    }

    // Puts every technique the override changed back to its original state and forgets them.
    template <class Runtime>
    void restore_override(Runtime *runtime, OverrideState &state)
    {
        for (const auto &[key, handle] : list_techniques(runtime))
            if (const auto it = state.original.find(key); it != state.original.end() && runtime->get_technique_state(handle) != it->second)
                runtime->set_technique_state(handle, it->second);
        state.original.clear();
    }

    // Holds the wanted states every present, because a reload, such as the one a resize causes, loads the preset's.
    template <class Runtime>
    void enforce_override(Runtime *runtime, OverrideState &state)
    {
        for (const auto &[key, handle] : list_techniques(runtime))
        {
            const bool current = runtime->get_technique_state(handle);
            const auto original = state.original.find(key);
            const bool desired = state.wanted.forced.count(key.first) != 0 ? true
                : state.wanted.solo ? false
                : original != state.original.end() ? original->second : current;
            if (desired == current)
                continue;
            state.original.try_emplace(key, current);
            runtime->set_technique_state(handle, desired);
        }
    }

    // Runs on the render thread, on tables from check_out. Handles are looked up again every time, because ReShade
    // destroys them on every reload.
    template <class Runtime, class Warn>
    void sync(Runtime *runtime, Tables &tables, std::set<Key> &warned, OverrideState &override_state, Warn warn)
    {
        if (tables.override_request.has_value())
        {
            override_state.wanted = *tables.override_request;
            tables.override_request.reset();
        }
        // A technique set while the override holds becomes the state to put back, and solo keeps it off meanwhile.
        if (override_state.wanted.active())
            for (auto it = tables.technique_sets.begin(); it != tables.technique_sets.end();)
            {
                if (override_state.wanted.forced.count(it->first.first) != 0)
                {
                    it = tables.technique_sets.erase(it);
                    continue;
                }
                override_state.original.insert_or_assign(it->first, it->second);
                it = override_state.wanted.solo ? tables.technique_sets.erase(it) : std::next(it);
            }

        const bool loaded = effects_loaded(runtime);
        if (loaded)
        {
            apply_sets(runtime, tables.uniform_sets, warned,
                [runtime](const char *effect, const char *name) { return runtime->find_uniform_variable(effect, name); },
                [runtime](auto handle, const UniformSet &set) {
                    if (const Floats *floats = std::get_if<Floats>(&set.values))
                        runtime->set_uniform_value_float(handle, floats->data(), set.count);
                    else
                        runtime->set_uniform_value_int(handle, std::get<Ints>(set.values).data(), set.count);
                },
                warn);
            // The uniforms go first, because a technique set can put the runtime back into loading.
            if (tables.uniform_sets.empty())
                apply_sets(runtime, tables.technique_sets, warned,
                    [runtime](const char *effect, const char *name) { return runtime->find_technique(effect, name); },
                    [runtime](auto handle, bool enabled) { runtime->set_technique_state(handle, enabled); },
                    warn);
            if (override_state.wanted.active())
                enforce_override(runtime, override_state);
            else if (!override_state.original.empty())
                restore_override(runtime, override_state);
        }

        const bool readable = loaded && effects_loaded(runtime);
        for (auto &[watch, reading] : tables.watched)
        {
            reading.reset();
            if (!readable)
                continue;
            const char *const effect = watch.second.first.c_str(), *const name = watch.second.second.c_str();
            if (watch.first == Kind::definition)
            {
                char value[256] = {};
                size_t size = sizeof(value);
                reading = runtime->get_preprocessor_definition_for_effect(effect, name, value, &size) ? std::string(value) : std::string();
            }
            else if (watch.first == Kind::technique)
            {
                // A technique the override holds reads as the user's own choice, which the release will put back.
                const auto held = override_state.original.find(watch.second);
                if (const auto handle = runtime->find_technique(effect, name); handle.handle != 0)
                    reading = held != override_state.original.end() ? held->second : runtime->get_technique_state(handle);
            }
            else if (const auto handle = runtime->find_uniform_variable(effect, name); handle.handle != 0)
            {
                if (watch.first == Kind::float_uniform)
                {
                    Floats floats = {};
                    runtime->get_uniform_value_float(handle, floats.data(), floats.size());
                    reading = floats;
                }
                else
                {
                    Ints ints = {};
                    runtime->get_uniform_value_int(handle, ints.data(), ints.size());
                    reading = ints;
                }
            }
        }

        // save_current_preset writes the technique list as it is, so a save during a reload would write a preset
        // with no or only some techniques.
        // The preset gets the user's own technique states, never the override's.
        if (tables.save_preset && loaded && tables.uniform_sets.empty() && tables.technique_sets.empty())
        {
            const bool overridden = !override_state.original.empty();
            if (overridden)
                for (const auto &[key, handle] : list_techniques(runtime))
                    if (const auto it = override_state.original.find(key); it != override_state.original.end())
                        runtime->set_technique_state(handle, it->second);
            runtime->save_current_preset();
            if (overridden)
                enforce_override(runtime, override_state);
            tables.save_preset = false;
        }
    }
}
