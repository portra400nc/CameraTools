// The volume clouds' type table (core/clouds.h).
#include "../core/clouds.h"

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

    // Where a type's density is highest, as a share of its layer.
    float densest(CloudType type)
    {
        float best = 0, at = 0;
        for (int i = 0; i <= 100; i++)
            if (cloud_profile(type, i / 100.0f).density > best)
            {
                best = cloud_profile(type, i / 100.0f).density;
                at = i / 100.0f;
            }
        return at;
    }

    // The share of its layer where a type's density passes half.
    float reach(CloudType type)
    {
        int count = 0;
        for (int i = 0; i <= 100; i++)
            count += cloud_profile(type, i / 100.0f).density > 0.5f ? 1 : 0;
        return count / 100.0f;
    }
}

int main()
{
    for (CloudType type : { CloudType::cumulus, CloudType::stratocumulus, CloudType::stratus, CloudType::cumulonimbus, CloudType::towering })
        check("every type is empty at its base and at its top, and outside its layer",
            cloud_profile(type, 0).density == 0 && cloud_profile(type, 1).density < 0.01f && cloud_profile(type, -0.1f).density == 0
                && cloud_profile(type, 1.1f).density == 0);
    check("cumulus is densest low in its layer and tapers up through the upper half",
        densest(CloudType::cumulus) < 0.4f && cloud_profile(CloudType::cumulus, 0.7f).density < 0.7f && cloud_profile(CloudType::cumulus, 0.7f).density > 0.2f);
    check("cumulonimbus fills most of its layer, cumulus less, stratocumulus less still and stratus least",
        reach(CloudType::cumulonimbus) > reach(CloudType::cumulus) && reach(CloudType::cumulus) > reach(CloudType::stratocumulus)
            && reach(CloudType::stratocumulus) > reach(CloudType::stratus) && reach(CloudType::stratus) > 0.2f);
    check("a towering cumulus stands full through most of its layer, more than a cumulus, ends sharply at its top and spreads no anvil",
        reach(CloudType::towering) > 0.75f && reach(CloudType::towering) > reach(CloudType::cumulus) && cloud_profile(CloudType::towering, 0.8f).density > 0.9f
            && cloud_profile(CloudType::towering, 0.98f).density < 0.1f && cloud_profile(CloudType::towering, 0.88f).spread == 0);
    check("only cumulonimbus spreads at its top, into an anvil, and not low down",
        cloud_profile(CloudType::cumulonimbus, 0.88f).spread > 0.3f && cloud_profile(CloudType::cumulonimbus, 0.5f).spread == 0
            && cloud_profile(CloudType::cumulus, 0.88f).spread == 0);
    check("stratus is the smoothest and closes its gaps", cloud_profile(CloudType::stratus, 0.3f).erosion < cloud_profile(CloudType::stratocumulus, 0.3f).erosion
        && cloud_profile(CloudType::stratus, 0.3f).spread > 0);
    check("an anvil's top is smoother than its tower", cloud_profile(CloudType::cumulonimbus, 0.95f).erosion < cloud_profile(CloudType::cumulonimbus, 0.5f).erosion);

    const CloudTypeTable table = cloud_type_table();
    const auto texel = [&table](CloudType type, uint32_t i) { return &table[(static_cast<size_t>(type) * kCloudTypeHeights + i) * 4]; };
    const CloudProfile mid = cloud_profile(CloudType::stratus, 20.5f / kCloudTypeHeights);
    check("the table holds a type a row, a texel its height's middle, in bytes",
        texel(CloudType::stratus, 20)[0] == static_cast<uint8_t>(mid.density * 255 + 0.5f) && texel(CloudType::stratus, 20)[2] == 64
            && texel(CloudType::cumulus, 0)[0] < texel(CloudType::cumulus, 8)[0] && texel(CloudType::cumulonimbus, 63)[3] == 255);
    return failures == 0 ? 0 : 1;
}
