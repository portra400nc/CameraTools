#include "night_lamps.h"
#include "../enhancements.h"

#include <algorithm>
#include <cmath>

namespace enhancements
{
    NightLamps night_lamps(const std::vector<EnhancementsShotLight> &sent, const Camera &camera)
    {
        NightLamps out {};
        const std::vector<TraceLight> lights = parse_trace_lights(sent.data(), static_cast<uint32_t>(sent.size()), ENHANCEMENTS_MAX_GAME_LIGHTS);
        // Unity's worldToCameraMatrix, column-major: element (r, c) at c * 4 + r.
        const float *m = camera.world_to_view;
        const auto to_view = [m](const Vec3d &p, double w) {
            std::array<double, 3> v {};
            for (int r = 0; r < 3; r++)
                v[r] = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r] * w;
            return v;
        };
        struct Near { const TraceLight *light; std::array<double, 3> at; double gap; };
        std::vector<Near> closest;
        for (const TraceLight &l : lights)
        {
            const std::array<double, 3> at = to_view(l.position, 1);
            closest.push_back({ &l, at, std::sqrt(at[0] * at[0] + at[1] * at[1] + at[2] * at[2]) - l.range });
        }
        const size_t count = std::min<size_t>(closest.size(), kNightLamps);
        std::partial_sort(closest.begin(), closest.begin() + static_cast<std::ptrdiff_t>(count), closest.end(), [](const Near &a, const Near &b) { return a.gap < b.gap; });
        for (size_t i = 0; i < count; i++)
        {
            const TraceLight &l = *closest[i].light;
            const std::array<double, 3> axis = to_view({ l.direction[0], l.direction[1], l.direction[2] }, 0);
            for (int k = 0; k < 3; k++)
            {
                out.rows[i][0][k] = static_cast<float>(closest[i].at[size_t(k)]);
                out.rows[i][1][k] = l.radiance[size_t(k)];
                out.rows[i][2][k] = static_cast<float>(axis[size_t(k)]);
            }
            out.rows[i][0][3] = l.range;
            out.rows[i][1][3] = l.cos_outer;
            out.rows[i][2][3] = l.cos_inner;
        }
        out.info[0] = static_cast<uint32_t>(count);
        return out;
    }
}
