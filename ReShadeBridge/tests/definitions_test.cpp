#include "bridge_tables.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <map>

struct Handle { uint64_t handle; };
struct FakeRuntime
{
    std::map<std::string, std::string> defs;
    bool loading = false;
    template <class F> void enumerate_techniques(const char *, F f) { if (!loading) f(this, Handle { 1 }); }
    Handle find_uniform_variable(const char *, const char *) { return { 0 }; }
    Handle find_technique(const char *, const char *) { return { 0 }; }
    void get_technique_effect_name(Handle, char *, size_t *) {}
    void get_technique_name(Handle, char *, size_t *) {}
    bool get_technique_state(Handle) { return false; }
    void set_technique_state(Handle, bool) {}
    void get_uniform_value_float(Handle, float *, size_t) {}
    void get_uniform_value_int(Handle, int32_t *, size_t) {}
    void set_uniform_value_float(Handle, const float *, size_t) {}
    void set_uniform_value_int(Handle, const int32_t *, size_t) {}
    void save_current_preset() {}
    bool get_preprocessor_definition_for_effect(const char *effect, const char *name, char *value, size_t *size)
    {
        auto it = defs.find(std::string(effect) + "/" + name);
        if (it == defs.end()) return false;
        std::snprintf(value, *size, "%s", it->second.c_str());
        *size = it->second.size() + 1;
        return true;
    }
};

int main()
{
    FakeRuntime rt;
    rt.defs["MartysMods_RELIGHT.fx/RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN"] = "1";
    bridge::Tables tables;
    bridge::OverrideState ostate;
    std::set<bridge::Key> warned;
    const bridge::Key far("MartysMods_RELIGHT.fx", "RESHADE_DEPTH_LINEARIZATION_FAR_PLANE"), flip("MartysMods_RELIGHT.fx", "RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN");
    tables.watched[{ bridge::Kind::definition, far }];
    tables.watched[{ bridge::Kind::definition, flip }];
    auto work = tables.check_out();
    bridge::sync(&rt, work, warned, ostate, [](auto &) {});
    tables.check_in(std::move(work));
    auto &a = tables.watched[{ bridge::Kind::definition, flip }], &b = tables.watched[{ bridge::Kind::definition, far }];
    assert(a && std::get<std::string>(*a) == "1");
    assert(b && std::get<std::string>(*b).empty());
    rt.loading = true;
    work = tables.check_out();
    bridge::sync(&rt, work, warned, ostate, [](auto &) {});
    tables.check_in(std::move(work));
    auto &c = tables.watched[{ bridge::Kind::definition, flip }];
    assert(!c.has_value());
    std::puts("definition reads: defined \"1\", undefined \"\", none while loading");
}
