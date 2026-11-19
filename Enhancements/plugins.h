// The add-on's plug-ins (plugin.h): found beside the stub, copied and loaded when the GPU side is ready, and unloaded with
// it, at a reload or when the game's device goes. The render thread loads, unloads and calls them; EnhancementsPluginCall
// reaches them from other threads under the lock.
#pragma once

#include "plugin.h"

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace enhancements
{
    class Plugins
    {
    public:
        // Loads every <Name>.plugin64 in addons, each through a copy in copies, and starts it. Logs each.
        void load(const std::filesystem::path &addons, const std::filesystem::path &copies, const std::filesystem::path &root, const plugin::Host &host,
            void (*log)(const std::string &line));
        void unload();
        bool loaded() const { return !plugins_.empty(); }
        bool draw(const plugin::Draw &draw) const;
        void gbuffer(const plugin::GBuffer &gbuffer) const;
        void present() const;
        uint32_t call(const char *plugin, const char *function, const void *in, uint32_t in_size, void *out, uint32_t out_size) const;

    private:
        struct Loaded
        {
            std::string name;
            HMODULE module = nullptr;
            std::filesystem::path copy;
            std::wstring folder;
            plugin::Host host {};
            EnhancementsPluginStopCall stop = nullptr;
            EnhancementsPluginDrawCall draw = nullptr;
            EnhancementsPluginGBufferCall gbuffer = nullptr;
            EnhancementsPluginPresentCall present = nullptr;
            EnhancementsPluginFunctionCall function = nullptr;
        };
        std::vector<Loaded> plugins_;
        mutable SRWLOCK lock_ = SRWLOCK_INIT;
        uint32_t copies_ = 0;
    };
}
