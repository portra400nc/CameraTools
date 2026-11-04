#include "character_light.h"
#include "../enhancements.h"

namespace enhancements
{
    namespace
    {
        // Unity's worldToCameraMatrix, column-major: element (r, c) at c * 4 + r.
        std::array<double, 3> to_view(const float *m, const double *p, double w)
        {
            std::array<double, 3> v {};
            for (int r = 0; r < 3; r++)
                v[r] = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r] * w;
            return v;
        }
    }

    CharacterLightConstants character_light_constants(const CharacterLightSettings &settings, const Camera &camera)
    {
        CharacterLightConstants c {};
        std::vector<EnhancementsShotLight> sent;
        for (const CharacterLamp &lamp : settings.lights)
        {
            EnhancementsShotLight l {};
            std::copy(lamp.position.begin(), lamp.position.end(), l.position);
            std::copy(lamp.color.begin(), lamp.color.end(), l.color);
            std::copy(lamp.direction.begin(), lamp.direction.end(), l.direction);
            l.range = lamp.range;
            l.cos_outer = lamp.cos_outer;
            l.cos_inner = lamp.cos_inner;
            sent.push_back(l);
        }
        const std::vector<TraceLight> lights = parse_trace_lights(sent.data(), static_cast<uint32_t>(sent.size()), ENHANCEMENTS_MAX_CHARACTER_LIGHTS);
        for (size_t i = 0; i < lights.size(); i++)
        {
            const TraceLight &l = lights[i];
            const double at[3] = { l.position[0], l.position[1], l.position[2] }, axis[3] = { l.direction[0], l.direction[1], l.direction[2] };
            const std::array<double, 3> p = to_view(camera.world_to_view, at, 1), d = to_view(camera.world_to_view, axis, 0);
            for (int k = 0; k < 3; k++)
            {
                c.lights.rows[i][0][k] = static_cast<float>(p[k]);
                c.lights.rows[i][1][k] = l.radiance[k];
                c.lights.rows[i][2][k] = static_cast<float>(d[k]);
            }
            c.lights.rows[i][0][3] = l.range;
            c.lights.rows[i][1][3] = l.cos_outer;
            c.lights.rows[i][2][3] = l.cos_inner;
        }
        c.lights.info[0] = static_cast<uint32_t>(lights.size());
        if (settings.head)
        {
            const double head[3] = { (*settings.head)[0], (*settings.head)[1], (*settings.head)[2] };
            const std::array<double, 3> h = to_view(camera.world_to_view, head, 1);
            for (int k = 0; k < 3; k++)
                c.head[k] = static_cast<float>(h[k]);
            c.head[3] = 1;
        }
        c.params[0] = settings.game_light;
        c.params[1] = settings.rim;
        c.params[2] = settings.soft ? 1.0f : 0.0f;
        return c;
    }
}
