#include "clouds.h"

#include <algorithm>
#include <cmath>

namespace enhancements
{
    namespace
    {
        float smoothstep(float a, float b, float x)
        {
            const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
            return t * t * (3 - 2 * t);
        }

        // Rises from the base over rise and falls away from fall_start to fall_end.
        float column(float h, float rise, float fall_start, float fall_end)
        {
            return smoothstep(0, rise, h) * (1 - smoothstep(fall_start, fall_end, h));
        }
    }

    CloudProfile cloud_profile(CloudType type, float h)
    {
        if (!(h >= 0 && h <= 1))
            return { 0, 0, 0 };
        switch (type)
        {
        case CloudType::stratocumulus:
            // Rolls in the lower half, rounded on top, lumpy all over.
            return { column(h, 0.1f, 0.3f, 0.7f), 0.55f, 0 };
        case CloudType::stratus:
            // A thin smooth sheet that closes the gaps between its patches.
            return { column(h, 0.15f, 0.35f, 0.6f), 0.25f, 0.25f };
        case CloudType::cumulonimbus:
            // Through the whole layer, its top spreading into a smooth anvil.
            return { column(h, 0.04f, 0.9f, 1.0f), 0.6f - 0.3f * smoothstep(0.75f, 0.9f, h),
                0.7f * smoothstep(0.72f, 0.9f, h) * (1 - smoothstep(0.95f, 1.0f, h)) };
        case CloudType::towering:
            // Full from a flat base nearly to the top, so its tops end crisp in billows rather than thinning into a veil.
            return { column(h, 0.04f, 0.85f, 1.0f), 0.75f, 0 };
        case CloudType::cumulus:
        case CloudType::count:
            break;
        }
        // A flat base and a rounded top that tapers through the upper half, billowing all over.
        return { column(h, 0.07f, 0.35f, 1.0f), 0.75f, 0 };
    }

    CloudTypeTable cloud_type_table()
    {
        CloudTypeTable table {};
        const auto byte = [](float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
        for (size_t t = 0; t < static_cast<size_t>(CloudType::count); t++)
            for (uint32_t i = 0; i < kCloudTypeHeights; i++)
            {
                const CloudProfile p = cloud_profile(static_cast<CloudType>(t), (i + 0.5f) / kCloudTypeHeights);
                uint8_t *texel = &table[(t * kCloudTypeHeights + i) * 4];
                texel[0] = byte(p.density);
                texel[1] = byte(p.erosion);
                texel[2] = byte(p.spread);
                texel[3] = 255;
            }
        return table;
    }
}
