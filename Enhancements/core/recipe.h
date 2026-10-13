// The game's frame as a recipe of recognizable steps. Pure C++17: no ReShade, no D3D11, so tests compile it on macOS.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace enhancements
{
    // ReShade's resource handle (the ID3D11Resource pointer) in the add-on; the census resource id in tests.
    enum class ResourceId : uint64_t {};

    // DXGI_FORMAT values. ReShade's api::format uses the same numbers, so the shell casts.
    enum class Format : uint32_t
    {
        unknown = 0,
        r32g8x24_typeless = 19,
        d32_float_s8x24_uint = 20,
        r10g10b10a2_unorm = 24,
        r11g11b10_float = 26,
        r8g8b8a8_typeless = 27,
        r8g8b8a8_unorm = 28,
        r8g8b8a8_unorm_srgb = 29,
        r8g8_unorm = 49,
        r16_typeless = 53,
        d16_unorm = 55,
        r16_unorm = 56,
        r8_typeless = 60,
        r8_unorm = 61,
    };

    struct Size
    {
        uint32_t width = 0, height = 0;
        friend bool operator==(Size a, Size b) { return a.width == b.width && a.height == b.height; }
        friend bool operator!=(Size a, Size b) { return !(a == b); }

        // The game's quarter-size targets are render/4 rounded either way, so a render size that does not divide by 4
        // (a screenshot slot) still matches.
        bool is_quarter_of(Size render) const
        {
            return (width == render.width / 4 || width == (render.width + 3) / 4) && (height == render.height / 4 || height == (render.height + 3) / 4);
        }
    };

    // A texture as a step sees it: the resource, the format of the view the game used (resource format for inputs), size.
    struct Texture
    {
        ResourceId id {};
        Format format = Format::unknown;
        Size size;
    };

    using Color = std::array<float, 4>;

    constexpr int kMaxTargets = 8;

    struct Targets
    {
        std::array<Texture, kMaxTargets> color {};
        uint8_t count = 0;
        std::optional<Texture> depth;
    };

    // What the shell knows about a draw only when asked, because reading it from the native context costs.
    class DrawQueries
    {
    public:
        // The pixel shader resources indexed by slot, from t0 to the last bound slot, each with its resource format and
        // size. A slot with nothing bound holds an empty Texture. Memoized by the implementation.
        virtual const Texture *ps_inputs(uint32_t *count) = 0;

    protected:
        ~DrawQueries() = default;
    };

    struct BindTargets { Targets targets; };
    struct ClearTarget { Texture cleared; Color color; };
    struct Draw { DrawQueries &queries; };

    using FrameEvent = std::variant<BindTargets, ClearTarget, Draw>;

    // What a step's match sees besides the event: the targets bound now, and every clear earlier in the frame, whatever
    // was bound since. The game clears some targets in one bind and draws into them in a later one.
    struct Seen
    {
        struct Clear { ResourceId id; Color color; };

        Targets targets;
        std::vector<Clear> clears;  // emptied, not freed, at each frame, so the hot path stops allocating after a frame

        bool cleared(ResourceId id, const Color &color) const;
    };

    enum class Entry : uint8_t
    {
        normals, albedo, specular, material_id, smoothness, character, depth,
        quarter_shadow, shadow_mask, ambient_specular, ambient_diffuse, hdr_scene, bloom, tonemap_out, bloom_final, sun_atlas,
        count,
    };

    // Each entry's name in status lines.
    extern const std::array<const char *, static_cast<size_t>(Entry::count)> kEntryNames;

    enum class Step : uint8_t { gbuffer, quarter_shadow, shadow_mask, ambient_pair, combine, bloom, tonemap, forward, gbuffer_done, count };

    // A bitset over an enum whose bits are the ABI bits in enhancements.h.
    template <class E>
    struct Set
    {
        uint32_t bits = 0;
        constexpr Set() = default;
        constexpr Set(std::initializer_list<E> items) { for (E e : items) bits |= 1u << static_cast<uint32_t>(e); }
        constexpr bool has(E e) const { return bits >> static_cast<uint32_t>(e) & 1u; }
        constexpr void add(E e) { bits |= 1u << static_cast<uint32_t>(e); }
        constexpr void remove(E e) { bits &= ~(1u << static_cast<uint32_t>(e)); }
        constexpr bool contains(Set other) const { return (bits & other.bits) == other.bits; }
        constexpr Set minus(Set other) const { Set s; s.bits = bits & ~other.bits; return s; }
        constexpr Set &operator|=(Set other) { bits |= other.bits; return *this; }
        constexpr bool empty() const { return bits == 0; }
    };
    using EntrySet = Set<Entry>;
    using StepSet = Set<Step>;

    struct FrameMap
    {
        Size render;  // the main depth's size, known once gbuffer matched
        std::array<std::optional<Texture>, static_cast<size_t>(Entry::count)> entries;

        const std::optional<Texture> &operator[](Entry e) const { return entries[static_cast<size_t>(e)]; }
        std::optional<Texture> &operator[](Entry e) { return entries[static_cast<size_t>(e)]; }
        EntrySet found() const;
    };

    // One row of the recipe. match sees the targets bound when the event happens, the frame's clears and the map so
    // far; it returns whether the event completes the step, and only then writes the step's entries into map.
    struct StepSpec
    {
        Step step;
        const char *name;   // "quarter-shadow"; the single source of the name in status lines
        StepSet after;      // prerequisites matched earlier this frame
        EntrySet fills;     // the entries this step is the only writer of
        bool (*match)(const Seen &seen, const FrameEvent &event, FrameMap &map);
        // A later row whose event is this step's moment too when this step has not matched by then, because the game's
        // call there still reads what this step's passes write; count for none.
        Step fallback = Step::count;
    };

    extern const std::array<StepSpec, static_cast<size_t>(Step::count)> kRecipe;

    // A draw as the tracker's trace holds it: the targets bound and the pixel shader inputs by slot.
    struct TracedDraw
    {
        Targets targets;
        std::vector<Texture> inputs;
    };

    // Feeds one frame's events through kRecipe. Rules:
    //  - a step matches at most once per frame, and only after its prerequisites;
    //  - a step whose prerequisites matched but which has not when its fallback matches falls back: the fallback's event
    //    is its moment too, and it matches no more that frame, so its passes never run twice;
    //  - a gbuffer bind with a different depth before combine restarts the frame (another camera drew first);
    //  - after combine the frame is committed and gbuffer binds are ignored;
    //  - clears are facts of the frame: a restart keeps them, begin_frame forgets them.
    class FrameTracker
    {
    public:
        void begin_frame();

        // The steps whose moment this event is: the step it completed, if any, and the steps that fell back to it. Passes
        // subscribed to them run now, in kRecipe order, so a step that fell back runs before its fallback.
        StepSet feed(const FrameEvent &event);

        const FrameMap &map() const { return map_; }
        StepSet matched() const { return matched_; }
        StepSet fell_back() const { return fell_back_; }
        uint32_t restarts() const { return restarts_; }

        // While on, every draw from the moment a step with a fallback could match until it or its fallback matched, the
        // draw that did included, is traced with its inputs. Off by default, because asking every draw for its inputs
        // costs. begin_frame empties the trace and keeps the switch.
        void trace(bool on) { tracing_ = on; }
        bool tracing() const { return tracing_; }
        const std::vector<TracedDraw> &traced() const { return traced_; }

    private:
        bool awaiting_fallback() const;

        Seen seen_;
        FrameMap map_;
        StepSet matched_;
        StepSet fell_back_;
        uint32_t restarts_ = 0;
        bool tracing_ = false;
        std::vector<TracedDraw> traced_;
    };

    // The tracker's trace, one line per run of alike draws: "draws 4-6: rt #2 26:480x300; t0 hdr-scene t2 #3 60:1920x1200
    // +9 more". rt and ds are the render targets and the depth target, t<slot> the pixel shader inputs from t0 to t7 and
    // any frame-map entry past them, then how many other slots hold something. An entry goes by its name; any other
    // texture by a number of its own in this trace, its DXGI_FORMAT number and its size.
    std::vector<std::string> describe_trace(const FrameTracker &tracker);
}
