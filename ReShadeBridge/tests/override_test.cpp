#include "bridge_tables.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

struct Handle { uint64_t handle; };
struct Tech { std::string effect, name; bool on; };
struct FakeRuntime
{
    std::vector<Tech> techs;
    std::vector<bool> preset;   // what a reload or the preset file holds
    std::vector<bool> saved;
    template <class F> void enumerate_techniques(const char *, F f) { for (size_t i = 0; i < techs.size(); i++) f(this, Handle { i + 1 }); }
    Handle find_technique(const char *effect, const char *name)
    {
        for (size_t i = 0; i < techs.size(); i++) if (techs[i].effect == effect && techs[i].name == name) return { i + 1 };
        return { 0 };
    }
    void get_technique_effect_name(Handle h, char *out, size_t *size) { std::snprintf(out, *size, "%s", techs[h.handle - 1].effect.c_str()); }
    void get_technique_name(Handle h, char *out, size_t *size) { std::snprintf(out, *size, "%s", techs[h.handle - 1].name.c_str()); }
    bool get_technique_state(Handle h) { return techs[h.handle - 1].on; }
    void set_technique_state(Handle h, bool on) { techs[h.handle - 1].on = on; }
    Handle find_uniform_variable(const char *, const char *) { return { 0 }; }
    void get_uniform_value_float(Handle, float *, size_t) {}
    void get_uniform_value_int(Handle, int32_t *, size_t) {}
    void set_uniform_value_float(Handle, const float *, size_t) {}
    void set_uniform_value_int(Handle, const int32_t *, size_t) {}
    bool get_preprocessor_definition_for_effect(const char *, const char *, char *, size_t *) { return false; }
    void save_current_preset() { saved.clear(); for (auto &t : techs) saved.push_back(t.on); }
    void reload() { for (size_t i = 0; i < techs.size(); i++) techs[i].on = preset[i]; }
    std::string states() { std::string s; for (auto &t : techs) s += t.on ? '1' : '0'; return s; }
};

int failures = 0;
void check(const char *name, bool ok) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", name); if (!ok) failures++; }

int main()
{
    FakeRuntime rt;
    rt.techs = { { "MartysMods_DEPTHOFFIELD.fx", "MartysMods_DOF", true }, { "MartysMods_CLARITY.fx", "MartysMods_Clarity", true },
        { "MartysMods_RELIGHT.fx", "MartysMods_RELIGHT", false }, { "MartysMods_LAUNCHPAD.fx", "MartysMods_Launchpad", false } };
    rt.preset = { true, true, false, false };
    bridge::Tables tables;
    bridge::OverrideState state;
    std::set<bridge::Key> warned;
    auto present = [&] { auto work = tables.check_out(); bridge::sync(&rt, work, warned, state, [](auto &) {}); tables.check_in(std::move(work)); };
    auto request = [&](bool solo, bool forced) {
        bridge::Override o; o.solo = solo;
        if (forced) o.forced = { "MartysMods_RELIGHT.fx", "MartysMods_LAUNCHPAD.fx" };
        tables.override_request = o;
    };

    request(true, true); present();
    check("solo: only ReLight and Launchpad on (0011)", rt.states() == "0011");
    rt.reload(); present();
    check("a reload is overridden again on the next present", rt.states() == "0011");
    request(false, true); present();
    check("screenshot: the user's shaders and ReLight on (1111)", rt.states() == "1111");
    request(true, true); present();
    tables.technique_sets[bridge::Key("MartysMods_DEPTHOFFIELD.fx", "MartysMods_DOF")] = false; present();
    check("a technique set during solo stays off", rt.states() == "0011");
    tables.watched[{ bridge::Kind::technique, bridge::Key("MartysMods_CLARITY.fx", "MartysMods_Clarity") }];
    present();
    auto &clarity = tables.watched[{ bridge::Kind::technique, bridge::Key("MartysMods_CLARITY.fx", "MartysMods_Clarity") }];
    check("a held technique reads as the user's choice (on) while solo keeps it off", clarity && std::get<bool>(*clarity) && rt.states()[1] == '0');
    tables.save_preset = true; present();
    check("a preset save writes the user's states (0100)", rt.saved == std::vector<bool>({ false, true, false, false }));
    check("and solo holds after the save", rt.states() == "0011");
    request(false, false); present();
    check("release puts back the user's states, depth of field off as set (0100)", rt.states() == "0100");
    check("release forgets the originals", state.original.empty());
    return failures;
}
