// Plug-ins of the Enhancements add-on: DLLs named <Name>.plugin64 beside the add-on, which it loads at its start and
// unloads at its stop, so a reload of the add-on reloads them too. A plug-in draws into the game's frame at the moments
// the add-on finds: each draw of the G-buffer pass, and the G-buffer once whole, before the game lights it, which is where
// a change to it reaches the game's lighting and every traced shot alike. Texturizer is one. A plug-in is built against this
// header, and the add-on loads only one built against its own version.
#pragma once

#include "core/shaders.h"

#include <d3d11_1.h>
#include <cstdint>

#define ENHANCEMENTS_PLUGIN_VERSION 1

namespace enhancements::plugin
{
    // A game draw as the game issued it, with all of its state bound: the arguments of DrawInstanced or
    // DrawIndexedInstanced, or of their indirect forms when args is set.
    struct Draw
    {
        bool indexed;
        uint32_t count, instances, first;
        int32_t vertex_offset;
        uint32_t first_instance;
        ID3D11Buffer *args;
        uint32_t args_offset;
    };

    // The G-buffer at a step: the game's own textures, which a plug-in may write over, and their formats as created.
    struct GBuffer
    {
        uint32_t step;              // kGBufferDone or kGBufferLit
        ID3D11Resource *normals, *albedo, *depth;
        DXGI_FORMAT normals_format, albedo_format, depth_format;
        uint32_t width, height;     // the render size
        bool flip;                  // the game renders upside down
        bool enhancements_on;       // the add-on's master switch
    };
    // The G-buffer pass ended and nothing has read it: writes reach the lighting and traced shots.
    constexpr uint32_t kGBufferDone = 0;
    // The lighting began: the G-buffer is whole, for reading back.
    constexpr uint32_t kGBufferLit = 1;

    // The add-on's side, valid from the plug-in's start to its stop.
    struct Host
    {
        uint32_t version;
        ID3D11Device *device;
        ID3D11DeviceContext1 *context;      // the immediate context, which every call below runs on
        const wchar_t *folder;              // ReShade\<Name>, the plug-in's own folder for files it reads and writes
        void (*log)(int level, const char *message);     // a line in ReShade's log at reshade::log::level, named for the plug-in
        bool (*find_shader)(uint64_t handle, ShaderCode *code);
        bool (*find_texture)(uint64_t resource, uint64_t *hash);
        uint64_t (*frame)(void);            // the add-on's frame count
        // Runs work, then puts back every piece of the context's state it changed, as the add-on's own passes do. False
        // when the game's state was not what it should be afterwards: the plug-in should then do nothing more this frame.
        bool (*guard)(void (*work)(void *context), void *context);
    };
}

extern "C"
{
    typedef uint32_t (*EnhancementsPluginVersionCall)(void);
    typedef bool (*EnhancementsPluginStartCall)(const enhancements::plugin::Host *host);
    // On the render thread between frames: releases every GPU resource and thread of its own.
    typedef void (*EnhancementsPluginStopCall)(void);
    // Each draw of the G-buffer pass, on the render thread, with the game's state bound. A plug-in that changes state does
    // it inside host->guard. True when the plug-in drew it, so the game's call is skipped.
    typedef bool (*EnhancementsPluginDrawCall)(const enhancements::plugin::Draw *draw);
    // The G-buffer at a step, on the render thread; state changes go inside host->guard as for a draw.
    typedef void (*EnhancementsPluginGBufferCall)(const enhancements::plugin::GBuffer *gbuffer);
    // Once a frame, between frames.
    typedef void (*EnhancementsPluginPresentCall)(void);
    // EnhancementsPluginCall's function by name, from another thread than the render thread.
    typedef uint32_t (*EnhancementsPluginFunctionCall)(const char *function, const void *in, uint32_t in_size, void *out, uint32_t out_size);
}
