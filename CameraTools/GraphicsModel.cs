using System.Globalization;

namespace CameraTools
{
    public enum PresetSlot { Highest, Lowest }

    // What a preset does to one game setting: leaves it, picks the first or the last option of its menu, or picks the
    // option a saved preset holds.
    public enum Pick { Keep, Lowest, Highest, Saved }

    public enum LodLevel { MostDetail, LeastDetail }

    // Values past the game's own range. Null leaves a value to the game, and a false switches the game's off. They are not
    // saved, so a restart resets them.
    public sealed record Overrides(
        float? RenderScale = null,
        bool? Shadows = null,
        float? ShadowDistance = null,
        bool? DistantShadows = null,
        bool? Fog = null,
        int? Particles = null)
    {
        public static readonly Overrides None = new();
    }

    // One setting's options in the settings menu's order, lowest first. Menu[position] is the option index the game stores
    // for that menu position; the two differ where the game appended an option later, such as 45 FPS.
    public sealed record OptionList(int[] Menu)
    {
        public int Lowest => Menu[0];
        public int Highest => Menu[^1];

        // The Graphics tab has a row for a setting only when there is something to choose, and a preset changes only
        // settings that have a row.
        public bool HasRow => Menu.Length > 1;

        // An option index's place in the menu, or -1 for an index the game does not offer.
        public int Position(int index) => Array.IndexOf(Menu, index);
    }

    // Index is the option index of a Saved pick.
    public readonly record struct Target(Pick Pick, int Index = 0)
    {
        // The option index this picks from a setting's options, or null to leave the setting alone. A saved index the game
        // no longer offers picks nothing.
        public int? In(OptionList list) => Pick switch
        {
            Pick.Lowest => list.Lowest,
            Pick.Highest => list.Highest,
            Pick.Saved when list.Position(Index) >= 0 => Index,
            _ => null,
        };
    }

    // What Highest or Lowest sets: a target for each game setting by its number, the values past the game's range, the
    // forced detail level, and every enhancement switch and value, or null to leave the enhancements as they are.
    public sealed record Preset(PresetSlot Slot, IReadOnlyDictionary<int, Target> Settings, Overrides Overrides, LodLevel? Lod, EnhancementValues Enhancements)
    {
        public string Name => Slot.ToString();

        public Target For(int setting) => Settings.TryGetValue(setting, out var target) ? target : default;
    }

    public readonly record struct SettingChange(int Setting, int From, int To);

    // What the Highest and Lowest presets set a row to, shown on a second line under its label.
    public readonly record struct PresetNote(string Highest, string Lowest, bool Restart = false);

    // The presets' decisions, apart from the game and the preferences, so tests run them.
    internal static class Presets
    {
        private const string RenderScale = "RenderScale", Shadows = "Shadows", ShadowDistance = "ShadowDistance", DistantShadows = "DistantShadows",
            Fog = "Fog", Particles = "Particles", Detail = "Detail", Off = "Off";

        // A built-in preset: picks holds what it picks of each game setting, by the setting's number. The game's own render
        // resolution goes to its highest, so Highest leaves render scale to the game. The enhancements stay as they are.
        public static Preset BuiltIn(PresetSlot slot, IReadOnlyDictionary<int, Pick> picks)
        {
            var targets = picks.ToDictionary(pair => pair.Key, pair => new Target(pair.Value));
            return slot == PresetSlot.Highest
                ? new Preset(slot, targets, new Overrides(ShadowDistance: 300f), LodLevel.MostDetail, null)
                : new Preset(slot, targets, new Overrides(RenderScale: 0.5f, Shadows: false, ShadowDistance: 20f, DistantShadows: false, Fog: false, Particles: 0),
                    LodLevel.LeastDetail, null);
        }

        // What the rows are set to now, as a preset: settings holds the option index of each game setting that has a row.
        public static Preset Saved(PresetSlot slot, IReadOnlyDictionary<int, int> settings, Overrides overrides, LodLevel? lod, EnhancementValues enhancements)
            => new(slot, settings.ToDictionary(pair => pair.Key, pair => new Target(Pick.Saved, pair.Value)), overrides, lod, enhancements);

        // The game settings a preset changes, in the order of now: each setting that has a row and is not yet at what the
        // preset picks for it. lists holds the options of each setting the game offers, and now what each is set to.
        public static List<SettingChange> Changes(Preset preset, IReadOnlyDictionary<int, OptionList> lists, IReadOnlyDictionary<int, int> now)
        {
            var changes = new List<SettingChange>();
            foreach (var (setting, index) in now)
            {
                if (lists.TryGetValue(setting, out var list) && list.HasRow && preset.For(setting).In(list) is int target && target != index)
                    changes.Add(new SettingChange(setting, index, target));
            }
            return changes;
        }

        // The game settings to set so that they are as before again, which a screenshot does once it is taken.
        public static List<SettingChange> Back(IReadOnlyDictionary<int, int> before, IReadOnlyDictionary<int, int> now)
            => before.Where(pair => now.TryGetValue(pair.Key, out int index) && index != pair.Value)
                .Select(pair => new SettingChange(pair.Key, now[pair.Key], pair.Value))
                .ToList();

        // The enhancements after a preset is applied, and what Restore returns to: the values from before the first preset
        // changed them. A preset that holds no enhancements, or the ones already set, changes nothing.
        public static (EnhancementValues Next, EnhancementValues Remembered) Enhance(Preset preset, EnhancementValues now, EnhancementValues remembered)
            => preset.Enhancements == null || preset.Enhancements.Equals(now) ? (now, remembered) : (preset.Enhancements, remembered ?? now);

        // What a preset sets a game setting to, for the line under the setting's row: the option's label, or Keep.
        public static string SettingNote(Preset preset, int setting, OptionList list, string[] labels)
            => preset.For(setting).In(list) is int index ? labels[list.Position(index)] : "Keep";

        // What a preset sets the master switch or an effect's switch to, for the line under the switch's label.
        public static string SwitchNote(Preset preset, Enhancement id) => preset.Enhancements == null ? "Keep" : preset.Enhancements.On(id) ? "On" : "Off";

        public static string ScaleNote(Preset preset) => preset.Overrides.RenderScale?.ToString("0.00", CultureInfo.InvariantCulture) ?? "Game";

        // A saved preset as its preference holds it: setting number=option index for each game setting, then each value
        // past the game's range that is not the game's, then the detail level, then every enhancement by its name.
        public static string Text(Preset preset)
        {
            var parts = preset.Settings.Where(pair => pair.Value.Pick == Pick.Saved).Select(pair => $"{pair.Key}={pair.Value.Index}").ToList();
            var held = preset.Overrides;
            if (held.RenderScale is float scale)
                parts.Add($"{RenderScale}={Number(scale)}");
            if (held.Shadows == false)
                parts.Add($"{Shadows}={Off}");
            if (held.ShadowDistance is float distance)
                parts.Add($"{ShadowDistance}={Number(distance)}");
            if (held.DistantShadows == false)
                parts.Add($"{DistantShadows}={Off}");
            if (held.Fog == false)
                parts.Add($"{Fog}={Off}");
            if (held.Particles is int particles)
                parts.Add($"{Particles}={particles}");
            if (preset.Lod is LodLevel lod)
                parts.Add($"{Detail}={lod}");
            if (preset.Enhancements != null)
                parts.Add(preset.Enhancements.ToString());
            return string.Join(";", parts);
        }

        // The preset a preference's text holds, or null for an empty text, which stands for the built-in preset. skipped
        // gets each part that is no setting, value or enhancement, and the preset is read without it.
        public static Preset Parse(PresetSlot slot, string text, ICollection<string> skipped)
        {
            var pairs = Pairs.Split(text, skipped);
            if (pairs.Count == 0)
                return null;
            var settings = new Dictionary<int, Target>();
            var held = Overrides.None;
            LodLevel? lod = null;
            var enhancements = new Dictionary<Enhancement, float>();
            foreach (var (name, value) in pairs)
            {
                bool read = true;
                if (int.TryParse(name, NumberStyles.None, CultureInfo.InvariantCulture, out int setting))
                {
                    read = int.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out int index);
                    if (read)
                        settings[setting] = new Target(Pick.Saved, index);
                }
                else if (name == RenderScale && TryNumber(value, out float scale))
                    held = held with { RenderScale = scale };
                else if (name == Shadows && value == Off)
                    held = held with { Shadows = false };
                else if (name == ShadowDistance && TryNumber(value, out float distance))
                    held = held with { ShadowDistance = distance };
                else if (name == DistantShadows && value == Off)
                    held = held with { DistantShadows = false };
                else if (name == Fog && value == Off)
                    held = held with { Fog = false };
                else if (name == Particles && int.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out int particles))
                    held = held with { Particles = particles };
                else if (name == Detail && Enum.TryParse(value, out LodLevel level) && level.ToString() == value)
                    lod = level;
                else
                    read = EnhancementsModel.TryRead(name, value, enhancements);
                if (!read)
                    skipped.Add($"{name}={value}");
            }
            return new Preset(slot, settings, held, lod, EnhancementsModel.From(enhancements));
        }

        private static string Number(float value) => value.ToString(CultureInfo.InvariantCulture);

        private static bool TryNumber(string text, out float value)
            => float.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out value) && !float.IsNaN(value);
    }
}
