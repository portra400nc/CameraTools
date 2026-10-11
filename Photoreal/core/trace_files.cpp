#include "trace_files.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>

namespace photoreal
{
    namespace
    {
        namespace fs = std::filesystem;

        bool write_bytes(const fs::path &path, const void *data, size_t size)
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
            return static_cast<bool>(out);
        }

        template <class T>
        void write_vector(std::vector<std::string> &failed, const fs::path &folder, const char *name, const std::vector<T> &v)
        {
            if (!write_bytes(folder / name, v.data(), v.size() * sizeof(T)))
                failed.push_back(name);
        }

        std::string number(double v)
        {
            char text[40];
            std::snprintf(text, sizeof text, "%.9g", v + 0.0);
            return text;
        }

        std::string hex(uint64_t v)
        {
            char text[24];
            std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(v));
            return text;
        }

        template <class T>
        std::string list(const T *values, size_t count)
        {
            std::string out;
            for (size_t i = 0; i < count; i++)
                out += (i == 0 ? "" : ",") + number(values[i]);
            return out;
        }

        template <class T>
        bool read_vector(const fs::path &path, size_t count, std::vector<T> &out)
        {
            std::ifstream in(path, std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (bytes.size() != count * sizeof(T))
                return false;
            out.resize(count);
            std::memcpy(out.data(), bytes.data(), bytes.size());
            return true;
        }
    }

    std::string trace_tsv(const TraceSave &s)
    {
        const TraceSettings &t = s.settings;
        std::string out = "key\tvalue\n";
        const auto row = [&out](const char *key, const std::string &value) { out += std::string(key) + '\t' + value + '\n'; };
        row("width", std::to_string(s.size.width));
        row("height", std::to_string(s.size.height));
        row("frame", std::to_string(s.frame));
        row("scene", s.scene);
        row("generation", std::to_string(s.generation));
        row("bvh_hash", hex(s.bvh_hash));
        row("triangles", std::to_string(s.triangles));
        row("nodes", std::to_string(s.nodes));
        row("passes", std::to_string(s.passes));
        static const char *const kCounterNames[] = { "rays", "guard_stops", "stack_overflows", "grid_tested", "grid_off" };
        static_assert(std::size(kCounterNames) == kTraceCounters);
        for (uint32_t i = 0; i < kTraceCounters; i++)
            row(kCounterNames[i], std::to_string(s.counters[i]));
        row("mrays", number(s.mrays));
        row("reference", list(s.reference.data(), 3));
        row("floating_origin", s.floating_origin ? list(s.floating_origin->data(), 3) : "-");
        row("sun_source", s.sun_source);
        row("toward_sun", list(s.constants.sun_cone, 3));
        row("eye", list(s.constants.eye_position, 3));
        row("world_to_view", list(s.camera.world_to_view, 16));
        row("view_to_clip", list(s.camera.view_to_clip, 16));
        row("trace.ao", t.ao ? "1" : "0");
        row("trace.sun", t.sun ? "1" : "0");
        row("trace.characters", t.characters ? "1" : "0");
        row("trace.alpha_cards", t.alpha_cards ? "1" : "0");
        row("trace.ao_distance", number(t.ao_distance));
        row("trace.ao_strength", number(t.ao_strength));
        row("trace.sun_size", number(t.sun_size));
        row("trace.sun_strength", number(t.sun_strength));
        row("trace.ao_samples", std::to_string(t.ao_samples));
        row("trace.sun_samples", std::to_string(t.sun_samples));
        row("trace.budget_ms", number(t.budget_ms));
        row("ambient.level", number(s.level));
        return out;
    }

    std::vector<std::string> write_trace_folder(const fs::path &folder, const TraceSave &s)
    {
        std::vector<std::string> failed;
        std::error_code error;
        fs::create_directories(folder, error);
        write_vector(failed, folder, "history.f32", s.history);
        write_vector(failed, folder, "depth.r32", s.depth);
        write_vector(failed, folder, "stencil.r8", s.stencil);
        write_vector(failed, folder, "normals.r10g10b10a2", s.normals);
        if (!s.mask.empty())
            write_vector(failed, folder, "mask.r8g8", s.mask);
        if (!s.irradiance.empty())
            write_vector(failed, folder, "irradiance.r11g11b10", s.irradiance);
        if (!write_bytes(folder / "constants.bin", &s.constants, sizeof s.constants))
            failed.push_back("constants.bin");
        if (s.sun && !write_bytes(folder / "sun.bin", s.sun->data(), s.sun->size()))
            failed.push_back("sun.bin");
        const std::string tsv = trace_tsv(s);
        if (!write_bytes(folder / "trace.tsv", tsv.data(), tsv.size()))
            failed.push_back("trace.tsv");
        return failed;
    }

    std::optional<TraceSave> read_trace_folder(const fs::path &folder, std::string &why)
    {
        std::ifstream tsv(folder / "trace.tsv");
        std::map<std::string, std::string> values;
        for (std::string line; std::getline(tsv, line);)
        {
            const size_t tab = line.find('\t');
            if (tab != std::string::npos)
                values[line.substr(0, tab)] = line.substr(tab + 1);
        }
        const auto get = [&values](const char *key) { const auto it = values.find(key); return it == values.end() ? std::string() : it->second; };
        TraceSave s;
        try
        {
            s.size = { static_cast<uint32_t>(std::stoul(get("width"))), static_cast<uint32_t>(std::stoul(get("height"))) };
            s.bvh_hash = std::stoull(get("bvh_hash"), nullptr, 16);
            s.passes = static_cast<uint32_t>(std::stoul(get("passes")));
            s.generation = std::stoull(get("generation"));
            std::istringstream reference(get("reference"));
            char comma = 0;
            reference >> s.reference[0] >> comma >> s.reference[1] >> comma >> s.reference[2];
        }
        catch (const std::exception &)
        {
            why = "trace.tsv is missing or lacks width, height, bvh_hash, passes, generation or reference";
            return std::nullopt;
        }
        s.scene = get("scene");
        const size_t pixels = size_t(s.size.width) * s.size.height;
        std::vector<uint8_t> constants;
        if (!read_vector(folder / "history.f32", pixels * 4, s.history) || !read_vector(folder / "depth.r32", pixels, s.depth)
            || !read_vector(folder / "stencil.r8", pixels, s.stencil) || !read_vector(folder / "normals.r10g10b10a2", pixels, s.normals)
            || !read_vector(folder / "constants.bin", sizeof s.constants, constants))
        {
            why = "history.f32, depth.r32, stencil.r8, normals.r10g10b10a2 or constants.bin is missing or not " + std::to_string(s.size.width) + "x"
                + std::to_string(s.size.height);
            return std::nullopt;
        }
        std::memcpy(&s.constants, constants.data(), sizeof s.constants);
        read_vector(folder / "mask.r8g8", pixels * 2, s.mask);
        read_vector(folder / "irradiance.r11g11b10", pixels * 4, s.irradiance);
        std::vector<uint8_t> sun;
        if (read_vector(folder / "sun.bin", kSunCbufferTotal, sun))
        {
            s.sun.emplace();
            std::memcpy(s.sun->data(), sun.data(), sun.size());
        }
        return s;
    }
}
