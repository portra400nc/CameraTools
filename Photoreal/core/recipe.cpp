#include "recipe.h"
#include "../photoreal.h"

#include <algorithm>

namespace photoreal
{
    namespace
    {
        bool is(const Texture &t, Format format, Size size) { return t.format == format && t.size == size; }

        bool main_depth_bound(const Targets &bound, const FrameMap &map)
        {
            return bound.depth && map[Entry::depth] && bound.depth->id == map[Entry::depth]->id;
        }

        const Texture *single_target(const Targets &bound) { return bound.count == 1 ? &bound.color[0] : nullptr; }

        template <class Pred>
        bool samples(const Draw &draw, Pred pred)
        {
            uint32_t count = 0;
            const Texture *inputs = draw.queries.ps_inputs(&count);
            for (uint32_t i = 0; i < count; i++)
                if (pred(inputs[i]))
                    return true;
            return false;
        }

        // The sun's shadow atlas, D16: 6144x4096 at low settings, 4096x2048 at the user's normal ones.
        bool is_sun_atlas(const Texture &t)
        {
            return (t.format == Format::r16_typeless || t.format == Format::d16_unorm || t.format == Format::r16_unorm) && t.size.width >= 4096;
        }

        bool is_gbuffer(const Targets &t)
        {
            static constexpr Format layout[] = { Format::r10g10b10a2_unorm, Format::r8g8b8a8_unorm_srgb, Format::r8g8b8a8_unorm_srgb,
                Format::r8_unorm, Format::r8_unorm, Format::r8_unorm };
            if (t.count != 6 || !t.depth || t.depth->format != Format::d32_float_s8x24_uint)
                return false;
            for (int i = 0; i < 6; i++)
                if (!is(t.color[i], layout[i], t.depth->size))
                    return false;
            return true;
        }

        // Each matcher checks the cheap facts (targets) before the costly one (what the draw samples).

        bool match_gbuffer(const Seen &, const FrameEvent &event, FrameMap &map)
        {
            const auto *bind = std::get_if<BindTargets>(&event);
            if (!bind || !is_gbuffer(bind->targets))
                return false;
            const Targets &t = bind->targets;
            map.render = t.depth->size;
            static constexpr Entry order[] = { Entry::normals, Entry::albedo, Entry::specular, Entry::material_id, Entry::smoothness, Entry::character };
            for (int i = 0; i < 6; i++)
                map[order[i]] = t.color[i];
            map[Entry::depth] = *t.depth;
            return true;
        }

        bool match_quarter_shadow(const Seen &seen, const FrameEvent &event, FrameMap &map)
        {
            const auto *draw = std::get_if<Draw>(&event);
            const Texture *rt = single_target(seen.targets);
            if (!draw || !rt || rt->format != Format::r8_unorm || !rt->size.is_quarter_of(map.render))
                return false;
            if (!samples(*draw, is_sun_atlas))
                return false;
            map[Entry::quarter_shadow] = *rt;
            return true;
        }

        // Every census capture's shadow-mask draw samples the sun's atlas at t2. When the slot holds anything else, the
        // step still matches and sun-atlas stays empty.
        constexpr uint32_t kShadowMaskAtlasSlot = 2;

        // The game clears the shadow mask to (1,1,1,0) earlier in the frame; the clear tells it apart from any other
        // render-size R8G8 target drawn with the main depth. In the 1152x720 captures the clear follows the bind with
        // the main depth; at 1920x1200 it comes in a bind of its own, without depth, almost 400 events before the draw.
        bool match_shadow_mask(const Seen &seen, const FrameEvent &event, FrameMap &map)
        {
            const Texture *rt = single_target(seen.targets);
            if (!std::holds_alternative<Draw>(event) || !rt || !is(*rt, Format::r8g8_unorm, map.render) || !main_depth_bound(seen.targets, map))
                return false;
            if (!seen.cleared(rt->id, Color { 1, 1, 1, 0 }))
                return false;
            map[Entry::shadow_mask] = *rt;
            uint32_t count = 0;
            const Texture *inputs = std::get<Draw>(event).queries.ps_inputs(&count);
            if (count > kShadowMaskAtlasSlot && is_sun_atlas(inputs[kShadowMaskAtlasSlot]))
                map[Entry::sun_atlas] = inputs[kShadowMaskAtlasSlot];
            return true;
        }

        bool match_ambient_pair(const Seen &seen, const FrameEvent &event, FrameMap &map)
        {
            const Targets &t = seen.targets;
            if (!std::holds_alternative<Draw>(event) || t.count != 2 || !main_depth_bound(t, map))
                return false;
            if (!is(t.color[0], Format::r11g11b10_float, map.render) || !is(t.color[1], Format::r11g11b10_float, map.render))
                return false;
            map[Entry::ambient_specular] = t.color[0];
            map[Entry::ambient_diffuse] = t.color[1];
            return true;
        }

        // The game rebinds the HDR scene several times without drawing, so the anchor is its clear, not a bind.
        bool match_combine(const Seen &seen, const FrameEvent &event, FrameMap &map)
        {
            const auto *clear = std::get_if<ClearTarget>(&event);
            if (!clear || !is(clear->cleared, Format::r11g11b10_float, map.render) || !main_depth_bound(seen.targets, map))
                return false;
            if (seen.targets.count == 0 || seen.targets.color[0].id != clear->cleared.id)
                return false;
            map[Entry::hdr_scene] = clear->cleared;
            return true;
        }

        bool samples_hdr(const Draw &draw, const FrameMap &map)
        {
            const ResourceId hdr = map[Entry::hdr_scene]->id;
            return samples(draw, [hdr](const Texture &t) { return t.id == hdr; });
        }

        bool match_bloom(const Seen &seen, const FrameEvent &event, FrameMap &map)
        {
            const auto *draw = std::get_if<Draw>(&event);
            const Texture *rt = single_target(seen.targets);
            if (!draw || !rt || rt->format != Format::r11g11b10_float || !rt->size.is_quarter_of(map.render) || !samples_hdr(*draw, map))
                return false;
            map[Entry::bloom] = *rt;
            return true;
        }

        // Every census capture's tonemap draw reads the bloom chain's last result at t1. When the slot holds anything but a
        // quarter-size R11G11B10 texture, the tonemap step still matches and bloom-final stays empty.
        constexpr uint32_t kTonemapBloomSlot = 1;

        bool match_tonemap(const Seen &seen, const FrameEvent &event, FrameMap &map)
        {
            const auto *draw = std::get_if<Draw>(&event);
            const Texture *rt = single_target(seen.targets);
            if (!draw || !rt || !is(*rt, Format::r8g8b8a8_unorm, map.render) || !samples_hdr(*draw, map))
                return false;
            map[Entry::tonemap_out] = *rt;
            uint32_t count = 0;
            const Texture *inputs = draw->queries.ps_inputs(&count);
            if (count > kTonemapBloomSlot && inputs[kTonemapBloomSlot].format == Format::r11g11b10_float
                && inputs[kTonemapBloomSlot].size.is_quarter_of(map.render))
                map[Entry::bloom_final] = inputs[kTonemapBloomSlot];
            return true;
        }

        // The game's deferred draws after the combine clear all sample the G-buffer normals: world, characters and vegetation,
        // and in 111824 two more surface classes into the HDR scene and a second target. Its forward draws (sky,
        // transparents, fog) do not, so the first draw into the HDR scene that does not is the moment the deferred
        // lighting is whole and nothing has been drawn over it yet.
        bool match_forward(const Seen &seen, const FrameEvent &event, FrameMap &map)
        {
            const auto *draw = std::get_if<Draw>(&event);
            if (!draw || seen.targets.count == 0 || seen.targets.color[0].id != map[Entry::hdr_scene]->id)
                return false;
            const ResourceId normals = map[Entry::normals]->id;
            return !samples(*draw, [normals](const Texture &t) { return t.id == normals; });
        }

        // The game writes the G-buffer in three binds: the main draws, then decals into rt0 to rt4 and into rt0, rt1, rt2
        // and rt4. In every capture the first draw after them that samples the smoothness (rt4) is a half-size pass
        // before the ambient pair, which reads it too (348 in 111824), so at that draw the G-buffer is whole and nothing
        // has read it yet. A half-size draw that samples only the normals comes before the decals, so it would not do.
        // A draw with the normals still bound as a target belongs to the G-buffer and is passed over without asking what
        // it samples.
        bool match_gbuffer_done(const Seen &seen, const FrameEvent &event, FrameMap &map)
        {
            const auto *draw = std::get_if<Draw>(&event);
            const ResourceId normals = map[Entry::normals]->id;
            if (!draw || std::any_of(seen.targets.color.begin(), seen.targets.color.begin() + seen.targets.count,
                    [normals](const Texture &t) { return t.id == normals; }))
                return false;
            const ResourceId smoothness = map[Entry::smoothness]->id;
            return samples(*draw, [smoothness](const Texture &t) { return t.id == smoothness; });
        }

        constexpr uint32_t bit(Step s) { return 1u << static_cast<uint32_t>(s); }
        constexpr uint32_t bit(Entry e) { return 1u << static_cast<uint32_t>(e); }

        static_assert(PHOTOREAL_STEP_GBUFFER == bit(Step::gbuffer) && PHOTOREAL_STEP_QUARTER_SHADOW == bit(Step::quarter_shadow)
            && PHOTOREAL_STEP_SHADOW_MASK == bit(Step::shadow_mask) && PHOTOREAL_STEP_AMBIENT_PAIR == bit(Step::ambient_pair)
            && PHOTOREAL_STEP_COMBINE == bit(Step::combine) && PHOTOREAL_STEP_BLOOM == bit(Step::bloom)
            && PHOTOREAL_STEP_TONEMAP == bit(Step::tonemap) && PHOTOREAL_STEP_FORWARD == bit(Step::forward)
            && PHOTOREAL_STEP_GBUFFER_DONE == bit(Step::gbuffer_done) && static_cast<int>(Step::count) == 9);
        static_assert(PHOTOREAL_ENTRY_NORMALS == bit(Entry::normals) && PHOTOREAL_ENTRY_ALBEDO == bit(Entry::albedo)
            && PHOTOREAL_ENTRY_SPECULAR == bit(Entry::specular) && PHOTOREAL_ENTRY_MATERIAL_ID == bit(Entry::material_id)
            && PHOTOREAL_ENTRY_SMOOTHNESS == bit(Entry::smoothness) && PHOTOREAL_ENTRY_CHARACTER == bit(Entry::character)
            && PHOTOREAL_ENTRY_DEPTH == bit(Entry::depth) && PHOTOREAL_ENTRY_QUARTER_SHADOW == bit(Entry::quarter_shadow)
            && PHOTOREAL_ENTRY_SHADOW_MASK == bit(Entry::shadow_mask) && PHOTOREAL_ENTRY_AMBIENT_SPECULAR == bit(Entry::ambient_specular)
            && PHOTOREAL_ENTRY_AMBIENT_DIFFUSE == bit(Entry::ambient_diffuse) && PHOTOREAL_ENTRY_HDR_SCENE == bit(Entry::hdr_scene)
            && PHOTOREAL_ENTRY_BLOOM == bit(Entry::bloom) && PHOTOREAL_ENTRY_TONEMAP_OUT == bit(Entry::tonemap_out)
            && PHOTOREAL_ENTRY_BLOOM_FINAL == bit(Entry::bloom_final) && PHOTOREAL_ENTRY_SUN_ATLAS == bit(Entry::sun_atlas)
            && static_cast<int>(Entry::count) == 16);
    }

    const std::array<const char *, static_cast<size_t>(Entry::count)> kEntryNames = {
        "normals", "albedo", "specular", "material-id", "smoothness", "character", "depth",
        "quarter-shadow", "shadow-mask", "ambient-specular", "ambient-diffuse", "hdr-scene", "bloom", "tonemap-out", "bloom-final", "sun-atlas",
    };

    const std::array<StepSpec, static_cast<size_t>(Step::count)> kRecipe = { {
        { Step::gbuffer, "gbuffer", {},
          { Entry::normals, Entry::albedo, Entry::specular, Entry::material_id, Entry::smoothness, Entry::character, Entry::depth }, match_gbuffer },
        { Step::quarter_shadow, "quarter-shadow", { Step::gbuffer }, { Entry::quarter_shadow }, match_quarter_shadow },
        { Step::shadow_mask, "shadow-mask", { Step::gbuffer }, { Entry::shadow_mask, Entry::sun_atlas }, match_shadow_mask },
        { Step::ambient_pair, "ambient-pair", { Step::gbuffer }, { Entry::ambient_specular, Entry::ambient_diffuse }, match_ambient_pair },
        { Step::combine, "combine", { Step::gbuffer }, { Entry::hdr_scene }, match_combine },
        { Step::bloom, "bloom", { Step::combine }, { Entry::bloom }, match_bloom },
        { Step::tonemap, "tonemap", { Step::combine }, { Entry::tonemap_out, Entry::bloom_final }, match_tonemap },
        { Step::forward, "forward", { Step::combine }, {}, match_forward },
        { Step::gbuffer_done, "gbuffer-done", { Step::gbuffer }, {}, match_gbuffer_done },
    } };

    bool Seen::cleared(ResourceId id, const Color &color) const
    {
        for (const Clear &clear : clears)
            if (clear.id == id && clear.color == color)
                return true;
        return false;
    }

    EntrySet FrameMap::found() const
    {
        EntrySet set;
        for (size_t i = 0; i < entries.size(); i++)
            if (entries[i])
                set.add(static_cast<Entry>(i));
        return set;
    }

    void FrameTracker::begin_frame()
    {
        // The targets survive: those bound at the end of one frame are still bound at the start of the next.
        seen_.clears.clear();
        map_ = {};
        matched_ = {};
        restarts_ = 0;
    }

    std::optional<Step> FrameTracker::feed(const FrameEvent &event)
    {
        const auto *bind = std::get_if<BindTargets>(&event);
        if (bind && matched_.has(Step::gbuffer) && !matched_.has(Step::combine) && is_gbuffer(bind->targets)
            && bind->targets.depth->id != map_[Entry::depth]->id)
        {
            restarts_++;
            map_ = {};
            matched_ = {};
        }

        // At most one step per event: no event in the captures fits two.
        std::optional<Step> result;
        for (const StepSpec &spec : kRecipe)
        {
            if (matched_.has(spec.step) || !matched_.contains(spec.after))
                continue;
            if (spec.match(seen_, event, map_))
            {
                matched_.add(spec.step);
                result = spec.step;
                break;
            }
        }

        // After matching, so a match sees the targets bound and the clears done before this event.
        if (bind)
            seen_.targets = bind->targets;
        else if (const auto *clear = std::get_if<ClearTarget>(&event))
            seen_.clears.push_back({ clear->cleared.id, clear->color });
        return result;
    }
}
