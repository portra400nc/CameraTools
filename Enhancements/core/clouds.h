// The volume clouds' types (Hillaire, "Physically Based Sky, Atmosphere and Cloud Rendering in Frostbite", 2016, 5.3): each
// type says, up its layer, how dense its clouds are, how much the fine noise erodes them, and how far their cover spreads.
// The march reads the table as a texture, a row a type and the layer's height along it.
#pragma once

#include "plan.h"

#include <array>
#include <cstdint>

namespace enhancements
{
    constexpr uint32_t kCloudTypeHeights = 64;

    // At one height of a type's layer, each 0 to 1.
    struct CloudProfile
    {
        float density;      // multiplies the shape noise before the coverage cuts it: 0 no cloud can stand here
        float erosion;      // how deep the fine noise eats into the edges: 0 smooth, 1 torn into wisps
        float spread;       // added to the cover: the anvil a cumulonimbus spreads at its top, the sheet stratus closes
    };

    // height runs 0 at the layer's base to 1 at its top; outside that, no cloud.
    CloudProfile cloud_profile(CloudType type, float height);

    // The table the march samples: RGBA8, kCloudTypeHeights texels a row and a row a CloudType, r the density, g the
    // erosion, b the spread, a 255.
    using CloudTypeTable = std::array<uint8_t, kCloudTypeHeights * static_cast<size_t>(CloudType::count) * 4>;
    CloudTypeTable cloud_type_table();
}
