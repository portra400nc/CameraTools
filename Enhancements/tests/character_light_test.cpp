// Studio lighting for characters: parsing EnhancementsCharacterLights, running alone under a master switch that is off,
// and its constants in the camera's view space.
#include "../core/character_light.h"
#include "../enhancements.h"

#include <cmath>
#include <cstddef>
#include <cstdio>

using namespace enhancements;

namespace
{
    int failures = 0;

    void check(const char *name, bool ok)
    {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        failures += ok ? 0 : 1;
    }

    bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }
}

int main()
{
    EnhancementsCharacterLights raw {};
    raw.size = sizeof raw;
    raw.enabled = 1;
    raw.shading = ENHANCEMENTS_CHARACTER_SHADING_SOFT;
    raw.game_light = 2;
    raw.rim = NAN;
    raw.head[0] = 1, raw.head[1] = 2, raw.head[2] = 3;
    raw.head_known = 1;
    raw.count = 9;
    for (EnhancementsShotLight &l : raw.lights)
        l = { { 0, 1, -2 }, 8, { 1, 1, 1 }, -2, { 0, 0, 1 }, -2, {} };
    const CharacterLightSettings parsed = parse_character_lights(&raw);
    check("a soft setup with the game light clamped to 1, the rim's NaN at its default 1.5, the head, and at most 4 lights",
        parsed.enabled && parsed.soft && parsed.game_light == 1 && parsed.rim == 1.5f && parsed.head && (*parsed.head)[2] == 3 && parsed.lights.size() == 4);
    raw.size = offsetof(EnhancementsCharacterLights, head);
    check("a caller that knows no head and no lights still turns it on", parse_character_lights(&raw).enabled && !parse_character_lights(&raw).head
        && parse_character_lights(&raw).lights.empty());
    raw.size = sizeof raw;
    raw.enabled = 0;
    check("off, or null, is off", !parse_character_lights(&raw).enabled && !parse_character_lights(nullptr).enabled);

    Settings applied;
    applied.ambient.enabled = true;
    const Settings alone = with_character_light(applied, parsed);
    check("under a master switch that is off it runs alone", alone.enabled && !alone.ambient.enabled && alone.character_light.enabled
        && with_character_light(applied, CharacterLightSettings {}).enabled == false);
    applied.enabled = true;
    check("under one that is on the other passes keep running", with_character_light(applied, parsed).ambient.enabled);
    check("it wants the combine step, where the G-buffer is copied, and the bloom step, where it draws", wanted_steps(alone).has(Step::combine)
        && wanted_steps(alone).has(Step::bloom));

    // A camera at (0, 0, 5) looking down -z: view space is the world moved back 5.
    Camera camera {};
    for (int i = 0; i < 4; i++)
        camera.world_to_view[i * 5] = 1;
    camera.world_to_view[14] = -5;
    const CharacterLightConstants c = character_light_constants(parsed, camera);
    check("the lights and the head move into view space, a point light's radiance is its color to the power 2.2",
        c.lights.info[0] == 4 && near(c.lights.rows[0][0][1], 1) && near(c.lights.rows[0][0][2], -7) && near(c.lights.rows[0][0][3], 8)
            && near(c.lights.rows[0][1][0], 1) && c.lights.rows[0][1][3] < -1 && near(c.lights.rows[0][2][2], 1) && near(c.head[2], -2) && c.head[3] == 1);
    check("the parameters: game light, rim, soft", c.params[0] == 1 && c.params[1] == 1.5f && c.params[2] == 1);
    std::printf("%s: %d failed\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
