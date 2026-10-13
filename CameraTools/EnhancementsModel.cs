using System.Globalization;
using CameraToolsEnhancements;

namespace CameraTools
{
    // Every switch and value of the Graphics tab's Enhancements category, in the rows' order. A name is also its entry in
    // the CameraToolsEnhancements preferences and its key in a saved preset.
    public enum Enhancement
    {
        Enabled,
        AmbientLight, AmbientLevel, AmbientOcclusion, AmbientOcclusionRadius, AmbientFoliageOcclusion,
        ContactShadows, ContactShadowLength, ContactShadowStrength, ContactShadowFoliage,
        SoftSunShadows, SunShadowSoftness, SunShadowStrength,
        Haze, HazeDensity, HazeHeightFalloff, HazeSunGlow, HazeGlowTowardSun,
        LightShafts, LightShaftStrength, MistDensity, MistHeight, LightShaftGlowTowardSun, LightShaftDistance, MistPatchiness, MistPatchSize,
        MistSkyLight, LightShaftQuality,
        WetSurfaces, Wetness, WetDarkening, Puddles,
        ToneMap, ToneMapCurve, ToneMapExposure, ToneMapBloom, ToneMapSaturation, ToneMapContrast,
    }

    public enum EnhancementKind { Switch, Slider, Choice }

    // One row of the category. A switch is 0 or 1, a choice is its option's position, and a slider is a number from Min to
    // Max with the Decimals its row shows. Effect is the switch a row sits under: the master switch for itself and for
    // each effect's switch, and the effect's switch for the effect's own rows.
    public sealed record EnhancementField(Enhancement Id, EnhancementKind Kind, string Label, Enhancement Effect, float Default, float Min = 0f, float Max = 1f,
        float Step = 1f, int Decimals = 0, string Unit = "", string[] Options = null)
    {
        public string Name => Id.ToString();

        // One of the seven effects' switches.
        public bool IsEffect => Kind == EnhancementKind.Switch && Id != Enhancement.Enabled;

        // A slider row's number format, such as 0.00' m'.
        public string Format => "0" + (Decimals > 0 ? "." + new string('0', Decimals) : "") + (Unit.Length > 0 ? $"'{Unit}'" : "");

        // The nearest value the field takes. A number that is none, as a hand-edited preference may hold, is the default.
        public float Clean(float value)
        {
            if (float.IsNaN(value))
                return Default;
            return Kind switch
            {
                EnhancementKind.Switch => value != 0f ? 1f : 0f,
                EnhancementKind.Choice => Math.Clamp(MathF.Round(value), 0f, Options.Length - 1),
                _ => MathF.Round(Math.Clamp(value, Min, Max), Decimals),
            };
        }

        // The position of a choice's option by its label, as the preferences hold it, or null for no option of this field.
        public float? Option(string label)
        {
            int position = Options == null ? -1 : Array.IndexOf(Options, label);
            return position < 0 ? null : position;
        }
    }

    // Every switch and value at one moment. Two are equal when every value is.
    public sealed class EnhancementValues : IEquatable<EnhancementValues>
    {
        // Everything off, and each value as the mockup lists it.
        public static readonly EnhancementValues Defaults = new(EnhancementsModel.Fields.Select(field => field.Default).ToArray());

        private readonly float[] values;

        private EnhancementValues(float[] values)
        {
            this.values = values;
        }

        public float this[Enhancement id] => values[(int)id];

        public bool On(Enhancement id) => values[(int)id] != 0f;

        public EnhancementValues With(Enhancement id, float value)
        {
            float cleaned = EnhancementsModel.Field(id).Clean(value);
            if (cleaned == values[(int)id])
                return this;
            var next = (float[])values.Clone();
            next[(int)id] = cleaned;
            return new EnhancementValues(next);
        }

        // From what is stored for each field, such as its preference: null takes the field's default.
        public static EnhancementValues From(Func<EnhancementField, float?> stored)
            => new(EnhancementsModel.Fields.Select(field => field.Clean(stored(field) ?? field.Default)).ToArray());

        public bool Equals(EnhancementValues other) => other != null && values.AsSpan().SequenceEqual(other.values);

        public override bool Equals(object other) => Equals(other as EnhancementValues);

        public override int GetHashCode()
        {
            var hash = new HashCode();
            foreach (float value in values)
                hash.Add(value);
            return hash.ToHashCode();
        }

        // Name=number for every field, with ; between them: a switch is 1 or 0 and a choice its option's position.
        public override string ToString()
            => string.Join(";", EnhancementsModel.Fields.Select(field => $"{field.Name}={values[(int)field.Id].ToString(CultureInfo.InvariantCulture)}"));
    }

    // The values the add-on runs: the user's own, which the preferences keep, or those of a screenshot's preset until the
    // screenshot puts its settings back.
    public sealed record EnhancementRun(EnhancementValues Own, EnhancementValues Shot = null)
    {
        public EnhancementValues Running => Shot ?? Own;

        // A screenshot's preset holds these values. A preset that holds none, or the ones that run, changes nothing.
        public EnhancementRun Hold(EnhancementValues values) => values == null || values.Equals(Running) ? this : this with { Shot = values };

        public EnhancementRun Release() => Shot == null ? this : this with { Shot = null };
    }

    // The Enhancements category's rules, apart from the game, the preferences and the add-on, so tests run them.
    internal static class EnhancementsModel
    {
        // Light shafts' Quality: Low, Medium and High as samples along each ray.
        private static readonly uint[] ShaftSteps = { 24, 48, 96 };
        // Wet surfaces' Wetness: 0 follows the weather, and 1 is soaked.
        private static readonly float[] WetnessLevels = { 0f, 0.25f, 0.5f, 0.75f, 1f };
        // Haze's Density row counts in multiples of this, the add-on's default.
        private static readonly float HazeDensity = EnhancementsSettings.Defaults.Atmosphere.Density;

        // The mockup's rows: labels, ranges, formats and defaults. Everything is off by default.
        public static readonly EnhancementField[] Fields =
        {
            Switch(Enhancement.Enabled, "Enhancements"),
            Switch(Enhancement.AmbientLight, "Ambient light"),
            Slider(Enhancement.AmbientLevel, Enhancement.AmbientLight, "Level", 0f, 2f, 0.6f, 0.05f, 2),
            Slider(Enhancement.AmbientOcclusion, Enhancement.AmbientLight, "Occlusion", 0f, 1f, 0.5f, 0.05f, 2),
            Slider(Enhancement.AmbientOcclusionRadius, Enhancement.AmbientLight, "Occlusion radius", 0.1f, 5f, 1f, 0.1f, 1, " m"),
            Slider(Enhancement.AmbientFoliageOcclusion, Enhancement.AmbientLight, "Occlusion on grass and leaves", 0f, 1f, 0.5f, 0.05f, 2),
            Switch(Enhancement.ContactShadows, "Contact shadows"),
            Slider(Enhancement.ContactShadowLength, Enhancement.ContactShadows, "Length", 0.05f, 5f, 0.6f, 0.05f, 2, " m"),
            Slider(Enhancement.ContactShadowStrength, Enhancement.ContactShadows, "Strength", 0f, 1f, 1f, 0.05f, 2),
            Slider(Enhancement.ContactShadowFoliage, Enhancement.ContactShadows, "On grass and leaves", 0f, 1f, 0f, 0.05f, 2),
            Switch(Enhancement.SoftSunShadows, "Soft sun shadows"),
            Slider(Enhancement.SunShadowSoftness, Enhancement.SoftSunShadows, "Softness", 0.005f, 0.2f, 0.03f, 0.005f, 3),
            Slider(Enhancement.SunShadowStrength, Enhancement.SoftSunShadows, "Strength", 0f, 1f, 1f, 0.05f, 2),
            Switch(Enhancement.Haze, "Haze"),
            Slider(Enhancement.HazeDensity, Enhancement.Haze, "Density", 0f, 10f, 1f, 0.1f, 1, "×"),
            Slider(Enhancement.HazeHeightFalloff, Enhancement.Haze, "Thins with height", 0f, 0.2f, 0.02f, 0.005f, 3),
            Slider(Enhancement.HazeSunGlow, Enhancement.Haze, "Sun glow", 0f, 10f, 1f, 0.1f, 1),
            Slider(Enhancement.HazeGlowTowardSun, Enhancement.Haze, "Glow toward the sun", 0f, 0.95f, 0.7f, 0.05f, 2),
            Switch(Enhancement.LightShafts, "Light shafts"),
            Slider(Enhancement.LightShaftStrength, Enhancement.LightShafts, "Strength", 0f, 10f, 3f, 0.1f, 1),
            Slider(Enhancement.MistDensity, Enhancement.LightShafts, "Mist density", 0f, 0.05f, 0.006f, 0.001f, 3),
            Slider(Enhancement.MistHeight, Enhancement.LightShafts, "Mist height", 0.5f, 50f, 5f, 0.5f, 1, " m"),
            Slider(Enhancement.LightShaftGlowTowardSun, Enhancement.LightShafts, "Glow toward the sun", 0f, 0.95f, 0.6f, 0.05f, 2),
            Slider(Enhancement.LightShaftDistance, Enhancement.LightShafts, "Distance", 5f, 900f, 150f, 5f, 0, " m"),
            Slider(Enhancement.MistPatchiness, Enhancement.LightShafts, "Patchiness", 0f, 1f, 1f, 0.05f, 2),
            Slider(Enhancement.MistPatchSize, Enhancement.LightShafts, "Patch size", 0.5f, 100f, 6f, 0.5f, 1, " m"),
            Slider(Enhancement.MistSkyLight, Enhancement.LightShafts, "Sky light in the mist", 0f, 4f, 0.3f, 0.05f, 2),
            Choice(Enhancement.LightShaftQuality, Enhancement.LightShafts, "Quality", 1, "Low", "Medium", "High"),
            Switch(Enhancement.WetSurfaces, "Wet surfaces"),
            Choice(Enhancement.Wetness, Enhancement.WetSurfaces, "Wetness", 0, "Follows the weather", "25%", "50%", "75%", "Soaked"),
            Slider(Enhancement.WetDarkening, Enhancement.WetSurfaces, "Darkening", 0f, 1f, 0.35f, 0.05f, 2),
            Slider(Enhancement.Puddles, Enhancement.WetSurfaces, "Puddles", 0f, 1f, 0.3f, 0.05f, 2),
            Switch(Enhancement.ToneMap, "Tone map"),
            Choice(Enhancement.ToneMapCurve, Enhancement.ToneMap, "Curve", 1, "Game", "AgX", "Neutral"),
            Slider(Enhancement.ToneMapExposure, Enhancement.ToneMap, "Exposure", -3f, 3f, 0f, 0.1f, 1, " EV"),
            Slider(Enhancement.ToneMapBloom, Enhancement.ToneMap, "Bloom", 0f, 2f, 1f, 0.05f, 2),
            Slider(Enhancement.ToneMapSaturation, Enhancement.ToneMap, "Saturation", 0f, 2f, 1f, 0.05f, 2),
            Slider(Enhancement.ToneMapContrast, Enhancement.ToneMap, "Contrast", 0.5f, 2f, 1f, 0.05f, 2),
        };

        public static readonly EnhancementField[] Effects = Fields.Where(field => field.IsEffect).ToArray();

        public static EnhancementField Field(Enhancement id) => Fields[(int)id];

        // What the add-on runs for these values. The path tracer has no row, so its trace and gather stay off, and every
        // other field without a row keeps the add-on's default.
        public static EnhancementsSettings Settings(EnhancementValues values)
        {
            uint Bit(Enhancement id) => values.On(id) ? 1u : 0u;
            var settings = EnhancementsSettings.Defaults;
            settings.Enabled = Bit(Enhancement.Enabled);
            settings.Ambient.Enabled = Bit(Enhancement.AmbientLight);
            settings.Ambient.Level = values[Enhancement.AmbientLevel];
            settings.Ambient.AoStrength = values[Enhancement.AmbientOcclusion];
            settings.Ambient.AoRadius = values[Enhancement.AmbientOcclusionRadius];
            settings.Ambient.FoliageAoStrength = values[Enhancement.AmbientFoliageOcclusion];
            settings.ContactShadows.Enabled = Bit(Enhancement.ContactShadows);
            settings.ContactShadows.Length = values[Enhancement.ContactShadowLength];
            settings.ContactShadows.Strength = values[Enhancement.ContactShadowStrength];
            settings.ContactShadows.FoliageStrength = values[Enhancement.ContactShadowFoliage];
            settings.SunShadows.Enabled = Bit(Enhancement.SoftSunShadows);
            settings.SunShadows.LightSize = values[Enhancement.SunShadowSoftness];
            settings.SunShadows.Strength = values[Enhancement.SunShadowStrength];
            settings.Atmosphere.Enabled = Bit(Enhancement.Haze);
            settings.Atmosphere.Density = values[Enhancement.HazeDensity] * HazeDensity;
            settings.Atmosphere.HeightFalloff = values[Enhancement.HazeHeightFalloff];
            settings.Atmosphere.SunScatter = values[Enhancement.HazeSunGlow];
            settings.Atmosphere.Anisotropy = values[Enhancement.HazeGlowTowardSun];
            settings.LightShafts.Enabled = Bit(Enhancement.LightShafts);
            settings.LightShafts.Strength = values[Enhancement.LightShaftStrength];
            settings.LightShafts.Density = values[Enhancement.MistDensity];
            settings.LightShafts.Height = values[Enhancement.MistHeight];
            settings.LightShafts.Anisotropy = values[Enhancement.LightShaftGlowTowardSun];
            settings.LightShafts.Distance = values[Enhancement.LightShaftDistance];
            settings.LightShafts.Patchiness = values[Enhancement.MistPatchiness];
            settings.LightShafts.PatchSize = values[Enhancement.MistPatchSize];
            settings.LightShafts.Ambient = values[Enhancement.MistSkyLight];
            settings.LightShafts.Steps = ShaftSteps[(int)values[Enhancement.LightShaftQuality]];
            settings.Wetness.Enabled = Bit(Enhancement.WetSurfaces);
            settings.Wetness.Wetness = WetnessLevels[(int)values[Enhancement.Wetness]];
            settings.Wetness.Darkening = values[Enhancement.WetDarkening];
            settings.Wetness.Puddles = values[Enhancement.Puddles];
            settings.Tonemap.Enabled = Bit(Enhancement.ToneMap);
            settings.Tonemap.Curve = (EnhancementsCurve)(uint)values[Enhancement.ToneMapCurve];
            settings.Tonemap.ExposureEv = values[Enhancement.ToneMapExposure];
            settings.Tonemap.BloomStrength = values[Enhancement.ToneMapBloom];
            settings.Tonemap.Saturation = values[Enhancement.ToneMapSaturation];
            settings.Tonemap.Contrast = values[Enhancement.ToneMapContrast];
            settings.Trace.Enabled = 0;
            settings.Gather.Enabled = 0;
            return settings;
        }

        // A folded category shows only its heading. Open, every switch shows, and an effect's own rows show while its
        // switch is on, whatever the master switch says.
        public static bool Shown(EnhancementField field, EnhancementValues values, bool folded)
            => !folded && (field.Kind == EnhancementKind.Switch || values.On(field.Effect));

        // While the master switch is off, every row but the master switch is greyed and ignores input.
        public static bool Usable(EnhancementField field, EnhancementValues values)
            => field.Id == Enhancement.Enabled || values.On(Enhancement.Enabled);

        // The right of the heading, folded or not: the effects count only while the master switch is on.
        public static string Summary(EnhancementValues values)
        {
            if (!values.On(Enhancement.Enabled))
                return "Off";
            int effects = Effects.Count(field => values.On(field.Id));
            return $"On · {effects} effect{(effects == 1 ? "" : "s")}";
        }

        // The effects that are switched on, by their rows' labels, for the log.
        public static string Names(EnhancementValues values)
        {
            var names = Effects.Where(field => values.On(field.Id)).Select(field => field.Label).ToArray();
            return names.Length > 0 ? string.Join(", ", names) : "no effect";
        }

        // The values a text of Name=number pairs holds, or null for a text that holds none. A field the text leaves out
        // takes its default. skipped gets each pair that is no field's.
        public static EnhancementValues Parse(string text, ICollection<string> skipped)
        {
            var read = new Dictionary<Enhancement, float>();
            foreach (var (name, value) in Pairs.Split(text, skipped))
            {
                if (!TryRead(name, value, read))
                    skipped.Add($"{name}={value}");
            }
            return From(read);
        }

        // Adds the pair to read when it names a field and holds a number.
        public static bool TryRead(string name, string value, Dictionary<Enhancement, float> read)
        {
            if (!Enum.TryParse(name, out Enhancement id) || !Enum.IsDefined(id) || id.ToString() != name
                || !float.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out float number))
                return false;
            read[id] = number;
            return true;
        }

        public static EnhancementValues From(Dictionary<Enhancement, float> read)
            => read.Count == 0 ? null : EnhancementValues.From(field => read.TryGetValue(field.Id, out float value) ? value : null);

        private static EnhancementField Switch(Enhancement id, string label) => new(id, EnhancementKind.Switch, label, Enhancement.Enabled, 0f);

        private static EnhancementField Slider(Enhancement id, Enhancement effect, string label, float min, float max, float value, float step, int decimals,
            string unit = "")
            => new(id, EnhancementKind.Slider, label, effect, value, min, max, step, decimals, unit);

        private static EnhancementField Choice(Enhancement id, Enhancement effect, string label, int value, params string[] options)
            => new(id, EnhancementKind.Choice, label, effect, value, 0f, options.Length - 1, Options: options);
    }

    // A preference's text of name=value pairs with ; between them.
    internal static class Pairs
    {
        // skipped gets each part that is not a pair.
        public static List<(string Name, string Value)> Split(string text, ICollection<string> skipped)
        {
            var pairs = new List<(string, string)>();
            foreach (var part in (text ?? "").Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
            {
                int at = part.IndexOf('=');
                if (at > 0)
                    pairs.Add((part[..at].Trim(), part[(at + 1)..].Trim()));
                else
                    skipped.Add(part);
            }
            return pairs;
        }
    }
}
