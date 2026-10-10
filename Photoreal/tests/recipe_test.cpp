// Replays recorded census frames through the recipe, the way the add-on's events would, and checks the moments and
// entries against the values the census analysis found.
#include "fixture.h"

#include <cstdio>

using namespace photoreal;
using fixture::Moment;

namespace
{
    int failures = 0;

    void check(const char *name, bool ok)
    {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        failures += ok ? 0 : 1;
    }

    uint64_t id(const FrameMap &map, Entry e) { return map[e] ? static_cast<uint64_t>(map[e]->id) : 0; }

    const std::vector<Moment> k214859 = { { 129, Step::gbuffer }, { 886, Step::quarter_shadow }, { 893, Step::shadow_mask },
        { 912, Step::ambient_pair }, { 926, Step::combine }, { 1088, Step::bloom }, { 1118, Step::tonemap } };

    // A second camera's G-buffer: rows 129 to 659 again, with the depth 19 and targets 79 to 84 replaced by 999 and
    // 901 to 906, and seqs moved up by 10000 so their moments stand apart.
    std::vector<fixture::Row> other_camera(const std::vector<fixture::Row> &rows, uint32_t first, uint32_t last)
    {
        std::vector<fixture::Row> copy = fixture::slice(rows, first, last);
        fixture::remap(copy, { { 19, 999 }, { 79, 901 }, { 80, 902 }, { 81, 903 }, { 82, 904 }, { 83, 905 }, { 84, 906 } });
        for (fixture::Row &row : copy)
            row.seq += 10000;
        return copy;
    }
}

int main()
{
    const std::vector<fixture::Row> walk = fixture::load("fixtures/capture-20261009-214859.tsv");
    {
        auto rows = walk;
        FrameTracker tracker;
        check("214859: every step once, in the game's order", fixture::replay(rows, tracker) == k214859);
        const FrameMap &map = tracker.map();
        check("214859: render size is the main depth's, 1152x720", map.render == Size { 1152, 720 });
        check("214859: G-buffer normals 79, albedo 80, specular 81, material id 82, smoothness 83, character 84, depth 19",
            id(map, Entry::normals) == 79 && id(map, Entry::albedo) == 80 && id(map, Entry::specular) == 81 && id(map, Entry::material_id) == 82
                && id(map, Entry::smoothness) == 83 && id(map, Entry::character) == 84 && id(map, Entry::depth) == 19);
        check("214859: ambient specular 390, diffuse 391", id(map, Entry::ambient_specular) == 390 && id(map, Entry::ambient_diffuse) == 391);
        check("214859: shadow mask 387, quarter shadow 385", id(map, Entry::shadow_mask) == 387 && id(map, Entry::quarter_shadow) == 385);
        check("214859: HDR scene 18, bloom 469, tonemap output 474",
            id(map, Entry::hdr_scene) == 18 && id(map, Entry::bloom) == 469 && id(map, Entry::tonemap_out) == 474);
        check("214859: no restart", tracker.restarts() == 0);
    }
    {
        auto rows = fixture::load("fixtures/capture-20261009-201640.tsv");  // FrameCensus 1: inputs without slots
        FrameTracker tracker;
        check("201640 (v1): every step once", fixture::replay(rows, tracker) == std::vector<Moment> {
            { 128, Step::gbuffer }, { 460, Step::quarter_shadow }, { 467, Step::shadow_mask }, { 483, Step::ambient_pair },
            { 497, Step::combine }, { 548, Step::bloom }, { 581, Step::tonemap } });
        const FrameMap &map = tracker.map();
        check("201640 (v1): ambient diffuse 241, HDR scene 33, depth 34", id(map, Entry::ambient_diffuse) == 241 && id(map, Entry::hdr_scene) == 33
            && id(map, Entry::depth) == 34);
        check("201640 (v1): quarter shadow is 237, not a later lone R8", id(map, Entry::quarter_shadow) == 237);
    }
    {
        auto rows = fixture::load("fixtures/capture-20261009-214907.tsv");
        FrameTracker tracker;
        check("214907: every step once", fixture::replay(rows, tracker) == std::vector<Moment> {
            { 129, Step::gbuffer }, { 876, Step::quarter_shadow }, { 883, Step::shadow_mask }, { 902, Step::ambient_pair },
            { 916, Step::combine }, { 1078, Step::bloom }, { 1108, Step::tonemap } });
        check("214907: ambient diffuse 391, HDR scene 18", id(tracker.map(), Entry::ambient_diffuse) == 391 && id(tracker.map(), Entry::hdr_scene) == 18);
    }
    {
        auto rows = walk;
        fixture::rescale(rows, { 1152, 720 }, { 3456, 2160 }, { 864, 540 });
        FrameTracker tracker;
        check("214859 at 3456x2160: the same seven moments", fixture::replay(rows, tracker) == k214859);
        check("214859 at 3456x2160: render size follows", tracker.map().render == Size { 3456, 2160 });
        check("214859 at 3456x2160: quarter shadow 385 at 864x540",
            id(tracker.map(), Entry::quarter_shadow) == 385 && tracker.map()[Entry::quarter_shadow]->size == Size { 864, 540 });
    }
    {
        auto rows = walk;
        fixture::rescale(rows, { 1152, 720 }, { 1153, 721 }, { 289, 181 });
        FrameTracker tracker;
        check("214859 at 1153x721, quarter ceiled to 289x181: the same seven moments", fixture::replay(rows, tracker) == k214859);
        check("214859 at 1153x721: render size and quarter shadow 385",
            tracker.map().render == Size { 1153, 721 } && id(tracker.map(), Entry::quarter_shadow) == 385);
    }
    {
        auto rows = walk;
        fixture::rescale(rows, { 1152, 720 }, { 1153, 721 }, { 288, 180 });
        FrameTracker tracker;
        check("214859 at 1153x721, quarter floored to 288x180: the same seven moments", fixture::replay(rows, tracker) == k214859);
    }
    {
        auto rows = walk;
        fixture::drop(rows, 910, 913);  // the ambient pair's bind with the main depth and both draws
        FrameTracker tracker;
        check("214859 without the ambient pair: the other six moments", fixture::replay(rows, tracker) == std::vector<Moment> {
            { 129, Step::gbuffer }, { 886, Step::quarter_shadow }, { 893, Step::shadow_mask }, { 926, Step::combine },
            { 1088, Step::bloom }, { 1118, Step::tonemap } });
        check("214859 without the ambient pair: no ambient diffuse", !tracker.map()[Entry::ambient_diffuse]);
    }
    {
        auto rows = walk;
        fixture::drop(rows, 892, 892);
        FrameTracker tracker;
        fixture::replay(rows, tracker);
        check("shadow mask without its clear: not found", !tracker.matched().has(Step::shadow_mask) && !tracker.map()[Entry::shadow_mask]);
        check("shadow mask without its clear: the ambient pair and combine still found",
            tracker.matched().has(Step::ambient_pair) && tracker.matched().has(Step::combine));
    }
    {
        auto rows = walk;
        fixture::find(rows, 892).color = { 0, 0, 0, 0 };
        FrameTracker tracker;
        fixture::replay(rows, tracker);
        check("shadow mask cleared to (0,0,0,0) instead of (1,1,1,0): not found", !tracker.map()[Entry::shadow_mask]);
    }
    {
        auto rows = walk;
        fixture::insert_before(rows, 129, other_camera(walk, 129, 659));
        FrameTracker tracker;
        std::vector<Moment> expected = { { 10129, Step::gbuffer } };
        expected.insert(expected.end(), k214859.begin(), k214859.end());
        check("a G-buffer with another depth before combine restarts", fixture::replay(rows, tracker) == expected && tracker.restarts() == 1);
        check("after the restart, the main G-buffer wins: normals 79, depth 19, ambient diffuse 391",
            id(tracker.map(), Entry::normals) == 79 && id(tracker.map(), Entry::depth) == 19 && id(tracker.map(), Entry::ambient_diffuse) == 391);
    }
    {
        auto rows = walk;
        fixture::insert_before(rows, 927, other_camera(walk, 129, 129));
        FrameTracker tracker;
        check("a G-buffer bind after combine is ignored", fixture::replay(rows, tracker) == k214859 && tracker.restarts() == 0
            && id(tracker.map(), Entry::depth) == 19 && id(tracker.map(), Entry::normals) == 79);
    }
    {
        auto rows = walk;
        fixture::insert_before(rows, 890, fixture::slice(walk, 129, 129));
        FrameTracker tracker;
        check("the same G-buffer bound again before combine is not a restart",
            fixture::replay(rows, tracker) == k214859 && tracker.restarts() == 0);
    }
    {
        auto rows = walk;
        FrameTracker tracker;
        fixture::replay(rows, tracker);
        tracker.begin_frame();
        check("begin_frame clears entries and matched steps", tracker.map().found().empty() && tracker.matched().empty()
            && tracker.map().render == Size {});
        check("a second frame through the same tracker matches the same moments", fixture::replay(rows, tracker) == k214859);
    }
    {
        EntrySet filled;
        bool overlap = false;
        for (const StepSpec &spec : kRecipe)
        {
            overlap |= (filled.bits & spec.fills.bits) != 0;
            filled |= spec.fills;
        }
        check("every entry has exactly one writer step", !overlap && filled.bits == (1u << static_cast<uint32_t>(Entry::count)) - 1);
    }
    std::printf("%s: %d failed\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
