// Between the stub that ReShade loads as CameraToolsEnhancements.addon64 (stub.cpp) and the add-on proper,
// CameraToolsEnhancementsImpl.dll (addon.cpp), which the stub loads and can load again while the game runs, so a rebuilt
// add-on takes effect without the game's 2 to 3 minute restart. The stub never unloads, so it keeps what only the game's
// start shows: every shader's bytecode from its creation. It forwards ReShade's events and the C exports to whichever copy
// of the add-on is loaded. Both sides are built from this header: a change to it, or to the events or exports the stub
// forwards, needs a new stub and so a game restart.
#pragma once

#include "core/shaders.h"

#include <cstddef>
#include <cstdint>

// Raised with every change to Host or to the lists below. The stub loads only an add-on built against its own.
#define ENHANCEMENTS_HOST_VERSION 2

// The ReShade events the stub listens to for the add-on, by reshade::addon_event's names.
#define ENHANCEMENTS_FORWARDED_EVENTS(X) \
    X(present) X(reshade_present) X(reshade_begin_effects) X(reshade_render_technique) X(reshade_finish_effects) \
    X(bind_render_targets_and_depth_stencil) X(clear_render_target_view) X(draw) X(draw_indexed) X(draw_or_dispatch_indirect) \
    X(destroy_device)

namespace enhancements
{
    // The stub's side, valid while the add-on is loaded.
    struct Host
    {
        uint32_t version;
        void *addon_module;     // the stub, which ReShade knows as the add-on: the add-on's log lines carry its name
        uint32_t loads;         // 1 for the add-on the game started with, then one more for each reload
        // Sends a ReShade event (reshade::addon_event) to callback. False for one the stub does not forward.
        bool (*listen)(uint32_t event, void *callback);
        // The game's shaders, as ShaderRegistry keeps them. The add-on adds its own grass port's too, and removes them at
        // its stop.
        void (*add_shader)(uint64_t handle, ShaderStage stage, const void *code, size_t size);
        void (*remove_shader)(uint64_t handle);
        bool (*find_shader)(uint64_t handle, ShaderCode *code);
        // A texture's fingerprint: a hash of the pixels of its mip of at most 64 pixels a side, as the game uploaded them, so
        // the same texture has the same one each launch and at every resolution its streaming loads, as 3DMigoto's texture
        // hashes. False for a texture the game drew into or never uploaded.
        bool (*find_texture)(uint64_t resource, uint64_t *hash);
    };
}

// The add-on proper's side.
extern "C"
{
    // ENHANCEMENTS_HOST_VERSION as the add-on was built.
    typedef uint32_t (*EnhancementsHostVersionCall)(void);
    // Listens to its events through host. The add-on starts its GPU work at the next present, as on the game's first.
    typedef bool (*EnhancementsStartCall)(const enhancements::Host *host);
    // On the render thread between frames, with no other thread inside the add-on: releases every GPU resource and ends
    // every thread of its own, so the stub can unload it.
    typedef void (*EnhancementsStopCall)(void);
}
