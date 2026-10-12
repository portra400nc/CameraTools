#include "trace_files.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <tuple>

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

        std::string number(double v, int digits = 9)
        {
            char text[40];
            std::snprintf(text, sizeof text, "%.*g", digits, v + 0.0);
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
        row("state", kTraceStateNames[static_cast<size_t>(s.state)]);
        row("why", s.why.empty() ? "-" : s.why);
        static const char *const kCounterNames[] = { "rays", "guard_stops", "stack_overflows", "grid_tested", "grid_off", "grid_128_tested",
            "grid_128_no_hit", "grid_128_nearer", "grid_128_farther", "grid_144_tested", "grid_144_no_hit", "grid_144_nearer", "grid_144_farther",
            "grid_other_tested", "grid_other_no_hit", "grid_other_nearer", "grid_other_farther" };
        static_assert(std::size(kCounterNames) == kTraceCounters);
        for (uint32_t i = 0; i < kTraceCounters; i++)
            row(kCounterNames[i], std::to_string(s.counters[i]));
        row("mrays", number(s.mrays));
        // Every digit of the double, so the Mac reads back the reference the scene's trace-scene.bin holds.
        row("reference", number(s.reference[0], 17) + "," + number(s.reference[1], 17) + "," + number(s.reference[2], 17));
        row("floating_origin", s.floating_origin ? list(s.floating_origin->data(), 3) : "-");
        row("sun_source", s.sun_source);
        row("toward_sun", list(s.constants.sun_cone, 3));
        row("pushed_sun", list(s.camera.toward_sun, 3));
        row("eye", list(s.constants.eye_position, 3));
        row("world_to_view", list(s.camera.world_to_view, 16));
        row("view_to_clip", list(s.camera.view_to_clip, 16));
        row("trace.ao", t.ao ? "1" : "0");
        row("trace.sun", t.sun ? "1" : "0");
        row("trace.characters", t.characters ? "1" : "0");
        row("trace.alpha_cards", t.alpha_cards ? "1" : "0");
        row("trace.whole_cards", t.whole_cards ? "1" : "0");
        row("trace.ao_distance", number(t.ao_distance));
        row("trace.ao_strength", number(t.ao_strength));
        row("trace.sun_size", number(t.sun_size));
        row("trace.sun_strength", number(t.sun_strength));
        row("trace.ao_samples", std::to_string(t.ao_samples));
        row("trace.sun_samples", std::to_string(t.sun_samples));
        row("trace.budget_ms", number(t.budget_ms));
        row("ambient.level", number(s.level));
        // Since T2a: the gather, its counters and what the scene's light came from.
        const GatherSettings &g = s.gather;
        row("gather.enabled", g.enabled ? "1" : "0");
        row("gather.samples", std::to_string(g.samples));
        row("gather.sky", kGatherSkyNames[static_cast<size_t>(g.sky)]);
        row("gather.filter", std::to_string(g.filter));
        row("gather.strength", number(g.strength));
        row("gather.hit_ambient", number(g.hit_ambient));
        row("gathering", s.gathering ? "1" : "0");
        row("trace_target", std::to_string(s.target));
        row("error", number(s.error));
        static const char *const kGatherNames[] = { "gather_rays", "gather_guard_stops", "gather_stack_overflows", "gather_hits_direct", "gather_hits_covered",
            "gather_hits_material", "gather_hits_class", "gather_deviation", "gather_mean" };
        static_assert(std::size(kGatherNames) == kGatherCounters);
        for (uint32_t i = 0; i < kGatherCounters; i++)
            row(kGatherNames[i], std::to_string(s.counters[kTraceCounters + i]));
        // The tone map's exposure stays on the GPU; predict_shot.py takes the census's 0.985.
        row("exposure", "-");
        row("light", s.light != 0 || s.gathering ? s.scene : "-");
        row("light_flags", std::to_string(s.light));
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
        if (s.gathering)
        {
            write_vector(failed, folder, "gather-hit.f32", s.gather_hit);
            write_vector(failed, folder, "gather-sky.f32", s.gather_sky);
            if (!write_bytes(folder / "gather.bin", &s.gather_constants, sizeof s.gather_constants))
                failed.push_back("gather.bin");
        }
        for (const auto &[bytes, name] : { std::pair { &s.albedo, "albedo.rgba8" }, std::pair { &s.hdr, "hdr.r11g11b10" },
                 std::pair { &s.irradiance_out, "irradiance-out.r11g11b10" } })
            if (!bytes->empty())
                write_vector(failed, folder, name, *bytes);
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
        for (size_t i = 0; i < kTraceStateNames.size(); i++)
            if (get("state") == kTraceStateNames[i])
                s.state = static_cast<TraceState>(i);
        s.why = get("why") == "-" ? std::string() : get("why");
        for (const auto &[key, values, count] : { std::tuple { "world_to_view", s.camera.world_to_view, 16 }, std::tuple { "view_to_clip", s.camera.view_to_clip, 16 },
                 std::tuple { "pushed_sun", s.camera.toward_sun, 3 } })
        {
            std::istringstream text(get(key));
            std::string item;
            for (int k = 0; k < count && std::getline(text, item, ','); k++)
                values[k] = std::strtof(item.c_str(), nullptr);
        }
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
        read_vector(folder / "albedo.rgba8", pixels * 4, s.albedo);
        read_vector(folder / "hdr.r11g11b10", pixels * 4, s.hdr);
        read_vector(folder / "irradiance-out.r11g11b10", pixels * 4, s.irradiance_out);
        s.gathering = get("gathering") == "1";
        if (s.gathering)
        {
            std::vector<uint8_t> gather;
            if (!read_vector(folder / "gather-hit.f32", pixels * 4, s.gather_hit) || !read_vector(folder / "gather-sky.f32", pixels * 4, s.gather_sky)
                || !read_vector(folder / "gather.bin", sizeof s.gather_constants, gather))
            {
                why = "the save gathered, but gather-hit.f32, gather-sky.f32 or gather.bin is missing or not its size";
                return std::nullopt;
            }
            std::memcpy(&s.gather_constants, gather.data(), sizeof s.gather_constants);
        }
        s.gather.enabled = get("gather.enabled") == "1";
        for (size_t i = 0; i < kGatherSkyNames.size(); i++)
            if (get("gather.sky") == kGatherSkyNames[i])
                s.gather.sky = static_cast<GatherSky>(i);
        const auto number_of = [&get](const char *key, double fallback) {
            const std::string text = get(key);
            return text.empty() || text == "-" ? fallback : std::strtod(text.c_str(), nullptr);
        };
        s.gather.samples = static_cast<uint32_t>(number_of("gather.samples", s.gather.samples));
        s.gather.filter = static_cast<uint32_t>(number_of("gather.filter", s.gather.filter));
        s.gather.strength = static_cast<float>(number_of("gather.strength", s.gather.strength));
        s.gather.hit_ambient = static_cast<float>(number_of("gather.hit_ambient", s.gather.hit_ambient));
        s.level = static_cast<float>(number_of("ambient.level", s.level));
        s.settings.ao_strength = static_cast<float>(number_of("trace.ao_strength", s.settings.ao_strength));
        s.settings.ao_distance = static_cast<float>(number_of("trace.ao_distance", s.settings.ao_distance));
        s.target = static_cast<uint32_t>(number_of("trace_target", 0));
        s.light = static_cast<uint32_t>(number_of("light_flags", 0));
        return s;
    }
}
