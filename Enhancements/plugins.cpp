#include "plugins.h"

namespace fs = std::filesystem;

namespace enhancements
{
    void Plugins::load(const fs::path &addons, const fs::path &copies, const fs::path &root, const plugin::Host &host, void (*log)(const std::string &line))
    {
        std::error_code error;
        fs::create_directories(copies, error);
        std::vector<Loaded> made;
        for (const fs::directory_entry &entry : fs::directory_iterator(addons, error))
        {
            if (entry.path().extension() != ".plugin64")
                continue;
            Loaded p;
            p.name = entry.path().stem().u8string();
            // A copy of its own, so an install can replace the file while this one runs.
            p.copy = copies / (p.name + "-" + std::to_string(++copies_) + ".plugin64");
            if (!CopyFileW(entry.path().c_str(), p.copy.c_str(), FALSE))
            {
                log("plug-in " + p.name + ": could not copy it, error " + std::to_string(GetLastError()) + ".");
                continue;
            }
            p.module = LoadLibraryExW(p.copy.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            const auto symbol = [&](const char *name) { return p.module ? reinterpret_cast<void *>(GetProcAddress(p.module, name)) : nullptr; };
            const auto version = reinterpret_cast<EnhancementsPluginVersionCall>(symbol("EnhancementsPluginVersion"));
            const auto start = reinterpret_cast<EnhancementsPluginStartCall>(symbol("EnhancementsPluginStart"));
            p.stop = reinterpret_cast<EnhancementsPluginStopCall>(symbol("EnhancementsPluginStop"));
            p.draw = reinterpret_cast<EnhancementsPluginDrawCall>(symbol("EnhancementsPluginDraw"));
            p.gbuffer = reinterpret_cast<EnhancementsPluginGBufferCall>(symbol("EnhancementsPluginGBuffer"));
            p.present = reinterpret_cast<EnhancementsPluginPresentCall>(symbol("EnhancementsPluginPresent"));
            p.function = reinterpret_cast<EnhancementsPluginFunctionCall>(symbol("EnhancementsPluginFunction"));
            if (version == nullptr || start == nullptr || p.stop == nullptr || version() != ENHANCEMENTS_PLUGIN_VERSION)
            {
                log("plug-in " + p.name + ": " + (p.module ? "built for plug-in version " + (version ? std::to_string(version()) : std::string("none"))
                    + ", not " + std::to_string(ENHANCEMENTS_PLUGIN_VERSION) : "could not load it, error " + std::to_string(GetLastError())) + ".");
                if (p.module)
                    FreeLibrary(p.module);
                DeleteFileW(p.copy.c_str());
                continue;
            }
            const fs::path folder = root / p.name;
            fs::create_directories(folder, error);
            p.folder = folder.wstring();
            made.push_back(std::move(p));
        }
        // Started once in place: each Host points at its plug-in's own folder string.
        for (auto it = made.begin(); it != made.end();)
        {
            it->host = host;
            it->host.folder = it->folder.c_str();
            const auto start = reinterpret_cast<EnhancementsPluginStartCall>(GetProcAddress(it->module, "EnhancementsPluginStart"));
            if (!start(&it->host))
            {
                log("plug-in " + it->name + " did not start.");
                FreeLibrary(it->module);
                DeleteFileW(it->copy.c_str());
                it = made.erase(it);
                continue;
            }
            log("plug-in " + it->name + " started.");
            ++it;
        }
        AcquireSRWLockExclusive(&lock_);
        plugins_ = std::move(made);
        ReleaseSRWLockExclusive(&lock_);
    }

    void Plugins::unload()
    {
        AcquireSRWLockExclusive(&lock_);
        std::vector<Loaded> going = std::move(plugins_);
        plugins_.clear();
        ReleaseSRWLockExclusive(&lock_);
        for (Loaded &p : going)
        {
            p.stop();
            FreeLibrary(p.module);
            DeleteFileW(p.copy.c_str());
        }
    }

    bool Plugins::draw(const plugin::Draw &draw) const
    {
        for (const Loaded &p : plugins_)
            if (p.draw && p.draw(&draw))
                return true;
        return false;
    }

    void Plugins::gbuffer(const plugin::GBuffer &gbuffer) const
    {
        for (const Loaded &p : plugins_)
            if (p.gbuffer)
                p.gbuffer(&gbuffer);
    }

    void Plugins::present() const
    {
        for (const Loaded &p : plugins_)
            if (p.present)
                p.present();
    }

    uint32_t Plugins::call(const char *plugin, const char *function, const void *in, uint32_t in_size, void *out, uint32_t out_size) const
    {
        uint32_t result = 0;
        AcquireSRWLockShared(&lock_);
        for (const Loaded &p : plugins_)
            if (p.function && plugin && p.name == plugin)
                result = p.function(function, in, in_size, out, out_size);
        ReleaseSRWLockShared(&lock_);
        return result;
    }
}
