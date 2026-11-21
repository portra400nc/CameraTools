using System.Globalization;
using CameraToolsEnhancements;

namespace CameraTools
{
    // Every switch and value of the Graphics tab's Enhancements category, in the rows' order, and then those of CameraTools'
    // own depth of field, whose rows are in the ReShade tab. A name is also its entry in the CameraToolsEnhancements
    // preferences and its key in a saved preset.
    public enum Enhancement
    {
        Enabled,
        AmbientLight, AmbientLevel, AmbientColor, AmbientOcclusion, AmbientOcclusionRadius, AmbientFoliageOcclusion,
        ContactShadows, ContactShadowLength, ContactShadowStrength, ContactShadowFoliage,
        LightThroughLeaves, LightThroughLeavesStrength,
        LeafShading, LeafShadingStrength,
        SkillLight, SkillLightStrength,
        GrassDepth, GrassRootShade, GrassDensity, GrassCustomTexture, GrassCustomHeight,
        SoftSunShadows, SunShadowSoftness, SunShadowStrength,
        Haze, HazeDensity, HazeHeightFalloff, HazeSunGlow, HazeGlowTowardSun,
        LightShafts, LightShaftStrength, MistDensity, MistHeight, LightShaftGlowTowardSun, LightShaftDistance, MistPatchiness, MistPatchSize,
        MistSkyLight, LightShaftQuality,
        WetSurfaces, Wetness, WetDarkening, Puddles,
        ToneMap, ToneMapCurve, ToneMapExposure, ToneMapAutoExposure, ToneMapBloom, ToneMapSaturation, ToneMapContrast, ToneMapShadows, ToneMapHighlights,
        LensFlare, LensFlareSource, LensFlareX, LensFlareY, LensFlareStrength, LensFlareArcs, LensFlareRainbow, LensFlareFilledArcs, LensFlareGhosts, LensFlareStreaks,
        LensFlareGlare, LensFlareSize, LensFlareWarmth, LensFlareRedFringe, LensFlareTexture,
        PhysicalSky, SkyHaziness, SunsetColor, DistanceHaze, NightBrightness,
        PhysicalWater, WaterClarity, WaterWaves, WaterSize,
        Clouds, CloudType, CloudCoverage, CloudHeight, CloudThickness, CloudSize, CloudDensity, CloudEdgeDetail, CloudSunlight, CloudSkyLight,
        CloudWind, CloudWindDirection, CloudQuality, CloudKind,
        Weather, WeatherMist, WeatherRain,
        DepthOfField, DepthOfFieldFocusMode, DepthOfFieldFocusSpeed, DepthOfFieldFocusDistance, DepthOfFieldAperture, DepthOfFieldFocusX, DepthOfFieldFocusY, DepthOfFieldMaxBlur,
        BokehShape, BladeRoundness, BladeRotation, SoapBubble, BokehChromaticAberration, CatsEye, Swirl, AnamorphicRatio,
        HighlightDiscs, HighlightBoost, HighlightThreshold, BokehContrast,
        LensSoftness,
    }

    public enum EnhancementKind { Switch, Slider, Choice }

    // One row of the category. A switch is 0 or 1, a choice is its option's position, and a slider is a number from Min to
    // Max with the Decimals its row shows. Effect is the switch a row sits under: the master switch for itself and for
    // each effect's switch, and the effect's switch for the effect's own rows. The depth of field's switch sits under
    // itself, since it runs whatever the master switch says.
    public sealed record EnhancementField(Enhancement Id, EnhancementKind Kind, string Label, Enhancement Effect, float Default, float Min = 0f, float Max = 1f,
        float Step = 1f, int Decimals = 0, string Unit = "", string[] Options = null)
    {
        public string Name => Id.ToString();

        // One of the ten effects' switches.
        public bool IsEffect => Kind == EnhancementKind.Switch && Effect == Enhancement.Enabled && Id != Enhancement.Enabled;

        // One of the depth of field's, whose rows are in the ReShade tab.
        public bool Lens => Effect == Enhancement.DepthOfField;

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

        // These values with the depth of field's taken from other.
        public EnhancementValues WithLensOf(EnhancementValues other)
        {
            var next = (float[])values.Clone();
            foreach (var field in EnhancementsModel.Fields)
                if (field.Lens)
                    next[(int)field.Id] = other.values[(int)field.Id];
            return next.AsSpan().SequenceEqual(values) ? this : new EnhancementValues(next);
        }

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
        public override string ToString() => Text(EnhancementsModel.Fields);

        // The same without the depth of field's, as a preset holds them.
        public string WithoutLens() => Text(EnhancementsModel.Fields.Where(field => !field.Lens));

        public string TextOf(Func<Enhancement, bool> pick) => Text(EnhancementsModel.Fields.Where(field => pick(field.Id)));

        private string Text(IEnumerable<EnhancementField> fields)
            => string.Join(";", fields.Select(field => $"{field.Name}={values[(int)field.Id].ToString(CultureInfo.InvariantCulture)}"));
    }

    // The values the add-on runs: the user's own, which the preferences keep, or those of a screenshot's preset until the
    // screenshot puts its settings back. The depth of field is always the user's own: it is part of the picture the user
    // framed, where the preset is about quality.
    public sealed record EnhancementRun(EnhancementValues Own, EnhancementValues Shot = null)
    {
        public EnhancementValues Running => Shot?.WithLensOf(Own) ?? Own;

        // A screenshot's preset holds these values. A preset that holds none, or the ones that run, changes nothing.
        public EnhancementRun Hold(EnhancementValues values)
            => values == null || values.WithLensOf(Own).Equals(Running) ? this : this with { Shot = values };

        public EnhancementRun Release() => Shot == null ? this : this with { Shot = null };
    }

    // Where a screenshot is: none, counting down with its toast on screen, or past that, capturing with ReShade's effects on.
    public enum ShotStage { None, Countdown, Capture }

    // What the depth of field's block needs beside the values: whether the free camera is on, which it runs in only; the
    // picture's width over its height, which gives the sensor; the miniature scale's N, which makes the sensor N times
    // taller; the screenshot's stage: a screenshot gathers more samples, and while it captures, the add-on blurs after
    // the ReShade effects that read the depth buffer, so RTGI and the like light the sharp frame and the shot has the blur;
    // the iris that Eyes focuses on, null while the focus point stands in for it; and the game's speed, which the clouds'
    // wind follows, so they stand still while the game is paused.
    internal readonly record struct LensView(bool FreeCamera, float Aspect, int Miniature, ShotStage Shot, EyeSight? Eye = null, float GameSpeed = 1f);

    // One of Clouds' Types: its row's name and line, the shape rows it sets, and the stretch, volume cloud type and painted
    // pattern it gives the add-on, which have no rows. Custom keeps those of the last type picked.
    internal readonly record struct CloudKind(string Name, string Detail, float Coverage, float Height, float Thickness, float Size, float Density,
        float EdgeDetail, float Stretch, EnhancementsCloudType Type, EnhancementsCloudPatternKind Pattern);

    // The Enhancements category's rules, apart from the game, the preferences and the add-on, so tests run them.
    internal static class EnhancementsModel
    {
        // The depth of field's Focus mode: the autofocus at the focus point, the Focus distance, or the active character's
        // nearer eye.
        public const int FocusPoint = 0, ManualFocus = 1, EyeFocus = 2;
        // Focus distance's range in meters.
        public const float NearFocus = 0.3f, FarFocus = 50f;
        // Gather samples a pixel while composing, and in a screenshot.
        public const uint LiveSamples = 49, ShotSamples = 289;
        // The depth of field's Aperture: the third stops the add-on takes, from f/1 to f/22.
        public static readonly float[] Stops = Lens.Stops.Where(stop => stop >= 1f).ToArray();
        private static readonly string[] Shapes = { "Round", "5 blades", "6 blades", "7 blades", "8 blades", "9 blades" };
        // Aperture shape's options as blades: 0 is round.
        private const uint FewestBlades = 5;
        // Light shafts' Quality: Low, Medium and High as samples along each ray.
        private static readonly uint[] ShaftSteps = { 24, 48, 96 };
        // Clouds' Types, in the row's order before Custom.
        public static readonly CloudKind[] CloudKinds =
        {
            new("Altocumulus", "Small puffs in high rows", 55f, 3000f, 600f, 250f, 0.04f, 0.8f, 1.5f, EnhancementsCloudType.Stratocumulus, EnhancementsCloudPatternKind.Puffs),
            new("Stratocumulus", "A low sheet with blue gaps", 75f, 1800f, 700f, 500f, 0.04f, 0.8f, 1.3f, EnhancementsCloudType.Stratocumulus, EnhancementsCloudPatternKind.Broken),
            new("Stratus", "A flat grey layer over the whole sky", 88f, 1000f, 400f, 1000f, 0.08f, 0.5f, 1.3f, EnhancementsCloudType.Stratus, EnhancementsCloudPatternKind.Overcast),
            new("Cumulus", "Tall heaps on a clear day", 50f, 1500f, 1500f, 400f, 0.04f, 0.8f, 1f, EnhancementsCloudType.Cumulus, EnhancementsCloudPatternKind.Heaps),
            new("High patches", "Thin patches far up", 45f, 4500f, 400f, 200f, 0.03f, 0.9f, 2.5f, EnhancementsCloudType.Stratocumulus, EnhancementsCloudPatternKind.Patches),
            new("Cirrus", "Thin streaks of ice very high up", 55f, 8000f, 300f, 800f, 0.002f, 1f, 3f, EnhancementsCloudType.Stratus, EnhancementsCloudPatternKind.Streaks),
        };
        // Type's last option, the user's own shape.
        public static readonly int CustomClouds = CloudKinds.Length;
        // The rows a Type sets, and that make it Custom when moved.
        private static readonly Enhancement[] CloudShapeRows =
        {
            Enhancement.CloudCoverage, Enhancement.CloudHeight, Enhancement.CloudThickness, Enhancement.CloudSize, Enhancement.CloudDensity, Enhancement.CloudEdgeDetail,
        };
        // Clouds' Quality: Low, Medium and High as march samples along each ray.
        private static readonly uint[] CloudSteps = { 32, 48, 64 };
        private static readonly string[] CloudTypeOptions = CloudKinds.Select(kind => kind.Name).Append("Custom").ToArray();
        // Wet surfaces' Wetness: 0 follows the weather, and 1 is soaked.
        private static readonly float[] WetnessLevels = { 0f, 0.25f, 0.5f, 0.75f, 1f };
        // The lens flare's Source: the sun disc the sky draws, or a light at Position X and Y.
        public const int FlareSun = 0, FlareManual = 1;

        // Ambient light's Sky: the game's own sky light, or the physical sky's.
        // Haze's Density row counts in multiples of this, the add-on's default.
        private static readonly float HazeDensity = EnhancementsSettings.Defaults.Atmosphere.Density;

        // The mockup's rows: labels, ranges, formats and defaults. Everything is off by default.
        public static readonly EnhancementField[] Fields =
        {
            Switch(Enhancement.Enabled, "Enhancements"),
            Switch(Enhancement.AmbientLight, "Ambient light"),
            Slider(Enhancement.AmbientLevel, Enhancement.AmbientLight, "Level", 0f, 2f, 0.6f, 0.05f, 2),
            Slider(Enhancement.AmbientColor, Enhancement.AmbientLight, "Color", 0f, 1f, 1f, 0.05f, 2),
            Slider(Enhancement.AmbientOcclusion, Enhancement.AmbientLight, "Occlusion", 0f, 1f, 0.5f, 0.05f, 2),
            Slider(Enhancement.AmbientOcclusionRadius, Enhancement.AmbientLight, "Occlusion radius", 0.1f, 5f, 1f, 0.1f, 1, " m"),
            Slider(Enhancement.AmbientFoliageOcclusion, Enhancement.AmbientLight, "Occlusion on grass and leaves", 0f, 1f, 0.5f, 0.05f, 2),
            Switch(Enhancement.ContactShadows, "Contact shadows"),
            Slider(Enhancement.ContactShadowLength, Enhancement.ContactShadows, "Length", 0.05f, 5f, 0.6f, 0.05f, 2, " m"),
            Slider(Enhancement.ContactShadowStrength, Enhancement.ContactShadows, "Strength", 0f, 1f, 1f, 0.05f, 2),
            Slider(Enhancement.ContactShadowFoliage, Enhancement.ContactShadows, "On grass and leaves", 0f, 1f, 0f, 0.05f, 2),
            Switch(Enhancement.LightThroughLeaves, "Light through leaves"),
            Slider(Enhancement.LightThroughLeavesStrength, Enhancement.LightThroughLeaves, "Strength", 0f, 200f, 100f, 5f, 0, "%"),
            Switch(Enhancement.LeafShading, "Leaf shading"),
            Slider(Enhancement.LeafShadingStrength, Enhancement.LeafShading, "Strength", 0f, 100f, 100f, 5f, 0, "%"),
            Switch(Enhancement.SkillLight, "Skill light"),
            Slider(Enhancement.SkillLightStrength, Enhancement.SkillLight, "Strength", 0f, 400f, 100f, 5f, 0, "%"),
            Switch(Enhancement.GrassDepth, "Grass"),
            Slider(Enhancement.GrassRootShade, Enhancement.GrassDepth, "Root shade", 0f, 100f, 100f, 5f, 0, "%"),
            Slider(Enhancement.GrassDensity, Enhancement.GrassDepth, "Density", 100f, 800f, 300f, 100f, 0, "%"),
            new(Enhancement.GrassCustomTexture, EnhancementKind.Switch, "Load custom texture", Enhancement.GrassDepth, 0f),
            Slider(Enhancement.GrassCustomHeight, Enhancement.GrassCustomTexture, "Height", 100f, 300f, 200f, 10f, 0, "%"),
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
            Slider(Enhancement.ToneMapAutoExposure, Enhancement.ToneMap, "Auto exposure", 0f, 100f, 0f, 10f, 0, "%"),
            Slider(Enhancement.ToneMapBloom, Enhancement.ToneMap, "Bloom", 0f, 2f, 1f, 0.05f, 2),
            Slider(Enhancement.ToneMapSaturation, Enhancement.ToneMap, "Saturation", 0f, 2f, 1f, 0.05f, 2),
            Slider(Enhancement.ToneMapContrast, Enhancement.ToneMap, "Contrast", 0.5f, 2f, 1f, 0.05f, 2),
            Slider(Enhancement.ToneMapShadows, Enhancement.ToneMap, "Shadows", -1f, 1f, 0f, 0.05f, 2),
            Slider(Enhancement.ToneMapHighlights, Enhancement.ToneMap, "Highlights", -1f, 1f, 0f, 0.05f, 2),
            Switch(Enhancement.LensFlare, "Lens flare"),
            Choice(Enhancement.LensFlareSource, Enhancement.LensFlare, "Source", FlareSun, "Sun", "Manual"),
            Slider(Enhancement.LensFlareX, Enhancement.LensFlare, "Position X", -2f, 2f, 0.6f, 0.05f, 2),
            Slider(Enhancement.LensFlareY, Enhancement.LensFlare, "Position Y", -2f, 2f, -0.76f, 0.05f, 2),
            Slider(Enhancement.LensFlareStrength, Enhancement.LensFlare, "Strength", 0f, 4f, 1f, 0.05f, 2),
            Slider(Enhancement.LensFlareArcs, Enhancement.LensFlare, "Arcs", 0f, 4f, 1f, 0.05f, 2),
            Slider(Enhancement.LensFlareRainbow, Enhancement.LensFlare, "Rainbow", 0f, 4f, 1f, 0.05f, 2),
            Slider(Enhancement.LensFlareFilledArcs, Enhancement.LensFlare, "Filled arcs", 0f, 4f, 0.5f, 0.05f, 2),
            Slider(Enhancement.LensFlareGhosts, Enhancement.LensFlare, "Ghosts", 0f, 4f, 1f, 0.05f, 2),
            Slider(Enhancement.LensFlareStreaks, Enhancement.LensFlare, "Streaks", 0f, 4f, 1f, 0.05f, 2),
            Slider(Enhancement.LensFlareGlare, Enhancement.LensFlare, "Glare", 0f, 4f, 0.5f, 0.05f, 2),
            Slider(Enhancement.LensFlareSize, Enhancement.LensFlare, "Size", 0.25f, 4f, 1f, 0.05f, 2),
            Slider(Enhancement.LensFlareWarmth, Enhancement.LensFlare, "Warmth", 0f, 1f, 0.5f, 0.05f, 2),
            Slider(Enhancement.LensFlareRedFringe, Enhancement.LensFlare, "Red fringe", 0f, 4f, 1f, 0.05f, 2),
            Slider(Enhancement.LensFlareTexture, Enhancement.LensFlare, "Texture", 0f, 2f, 1f, 0.05f, 2),
            Switch(Enhancement.PhysicalSky, "Physical sky"),
            Slider(Enhancement.SkyHaziness, Enhancement.PhysicalSky, "Haziness", 0f, 0.5f, 0.1f, 0.05f, 2),
            Slider(Enhancement.SunsetColor, Enhancement.PhysicalSky, "Sunset color", 0f, 100f, 0f, 5f, 0, "%"),
            Slider(Enhancement.DistanceHaze, Enhancement.PhysicalSky, "Distance haze", 0f, 32f, 8f, 1f, 0, "×"),
            Slider(Enhancement.NightBrightness, Enhancement.PhysicalSky, "Night brightness", 0f, 200f, 85f, 5f, 0, "%"),
            Switch(Enhancement.PhysicalWater, "Physical water"),
            Slider(Enhancement.WaterClarity, Enhancement.PhysicalWater, "Clarity", 0.5f, 20f, 6f, 0.5f, 1, " m"),
            Slider(Enhancement.WaterWaves, Enhancement.PhysicalWater, "Wave height", 0f, 300f, 100f, 10f, 0, "%"),
            Slider(Enhancement.WaterSize, Enhancement.PhysicalWater, "Wave size", 25f, 400f, 100f, 25f, 0, "%"),
            Switch(Enhancement.Clouds, "Clouds"),
            Choice(Enhancement.CloudType, Enhancement.Clouds, "Type", 0, CloudTypeOptions),
            Slider(Enhancement.CloudCoverage, Enhancement.Clouds, "Coverage", 0f, 100f, CloudKinds[0].Coverage, 5f, 0, "%"),
            Slider(Enhancement.CloudHeight, Enhancement.Clouds, "Height", 500f, 10000f, CloudKinds[0].Height, 100f, 0, " m"),
            Slider(Enhancement.CloudThickness, Enhancement.Clouds, "Thickness", 100f, 3000f, CloudKinds[0].Thickness, 50f, 0, " m"),
            Slider(Enhancement.CloudSize, Enhancement.Clouds, "Cloud size", 100f, 1000f, CloudKinds[0].Size, 10f, 0, " m"),
            Slider(Enhancement.CloudDensity, Enhancement.Clouds, "Density", 0.002f, 0.15f, CloudKinds[0].Density, 0.002f, 3),
            Slider(Enhancement.CloudEdgeDetail, Enhancement.Clouds, "Edge detail", 0f, 1f, CloudKinds[0].EdgeDetail, 0.05f, 2),
            Slider(Enhancement.CloudSunlight, Enhancement.Clouds, "Sunlight", 0f, 8f, 4f, 0.05f, 2),
            Slider(Enhancement.CloudSkyLight, Enhancement.Clouds, "Sky light", 0f, 2f, 1f, 0.05f, 2),
            Slider(Enhancement.CloudWind, Enhancement.Clouds, "Wind", 0f, 30f, 8f, 1f, 0, " m/s"),
            Slider(Enhancement.CloudWindDirection, Enhancement.Clouds, "Wind direction", 0f, 355f, 30f, 5f, 0, "°"),
            Choice(Enhancement.CloudQuality, Enhancement.Clouds, "Quality", 1, "Low", "Medium", "High"),
            Choice(Enhancement.CloudKind, Enhancement.Clouds, "Last type", 0, CloudKinds.Select(kind => kind.Name).ToArray()),
            Switch(Enhancement.Weather, "Weather"),
            Slider(Enhancement.WeatherMist, Enhancement.Weather, "Mist", 0f, 200f, 100f, 5f, 0, "%"),
            new(Enhancement.WeatherRain, EnhancementKind.Switch, "Rain", Enhancement.Weather, 1f),
            new(Enhancement.DepthOfField, EnhancementKind.Switch, "Depth of field", Enhancement.DepthOfField, 0f),
            Choice(Enhancement.DepthOfFieldFocusMode, Enhancement.DepthOfField, "Focus mode", FocusPoint, "Focus point", "Manual distance", "Eyes"),
            Slider(Enhancement.DepthOfFieldFocusSpeed, Enhancement.DepthOfField, "Focus speed", 0f, 3f, 0.5f, 0.1f, 1, " s"),
            Slider(Enhancement.DepthOfFieldFocusDistance, Enhancement.DepthOfField, "Focus distance", NearFocus, FarFocus, 2f, 0.01f, 2, " m"),
            Choice(Enhancement.DepthOfFieldAperture, Enhancement.DepthOfField, "Aperture", Array.IndexOf(Stops, 2.8f), Stops.Select(stop => $"f/{stop:0.##}").ToArray()),
            Slider(Enhancement.DepthOfFieldFocusX, Enhancement.DepthOfField, "Focus point X", -1f, 1f, 0f, 0.02f, 3),
            Slider(Enhancement.DepthOfFieldFocusY, Enhancement.DepthOfField, "Focus point Y", -1f, 1f, 0f, 0.02f, 3),
            Slider(Enhancement.DepthOfFieldMaxBlur, Enhancement.DepthOfField, "Max blur", 1f, 8f, 3f, 0.5f, 1),
            Choice(Enhancement.BokehShape, Enhancement.DepthOfField, "Aperture shape", 0, Shapes),
            Slider(Enhancement.BladeRoundness, Enhancement.DepthOfField, "Blade roundness", 0f, 1f, 0.5f, 0.05f, 2),
            Slider(Enhancement.BladeRotation, Enhancement.DepthOfField, "Blade rotation", 0f, 90f, 0f, 1f, 0, "°"),
            Slider(Enhancement.SoapBubble, Enhancement.DepthOfField, "Soap bubble", -1f, 1f, 0.6f, 0.05f, 2),
            Slider(Enhancement.BokehChromaticAberration, Enhancement.DepthOfField, "Chromatic aberration", 0f, 5f, 0f, 0.05f, 2),
            Slider(Enhancement.CatsEye, Enhancement.DepthOfField, "Cat's eye", 0f, 1f, 0f, 0.05f, 2),
            Slider(Enhancement.Swirl, Enhancement.DepthOfField, "Swirl", 0f, 1f, 0f, 0.05f, 2),
            Slider(Enhancement.AnamorphicRatio, Enhancement.DepthOfField, "Anamorphic ratio", 1f, 2f, 1f, 0.05f, 2, "x"),
            new(Enhancement.HighlightDiscs, EnhancementKind.Switch, "Highlight discs", Enhancement.DepthOfField, 0f),
            Slider(Enhancement.HighlightBoost, Enhancement.DepthOfField, "Highlight boost", 0f, 4f, 2f, 0.1f, 1),
            Slider(Enhancement.HighlightThreshold, Enhancement.DepthOfField, "Highlight threshold", 1.5f, 4f, 2f, 0.1f, 1, "x average"),
            Slider(Enhancement.BokehContrast, Enhancement.DepthOfField, "Bokeh contrast", 0f, 1f, 0f, 0.05f, 2),
            Slider(Enhancement.LensSoftness, Enhancement.DepthOfField, "Lens softness", 0f, 1.5f, 0f, 0.1f, 1, " px"),
        };

        public static readonly EnhancementField[] Effects = Fields.Where(field => field.IsEffect).ToArray();

        public static EnhancementField Field(Enhancement id) => Fields[(int)id];

        // What the add-on runs for these values. The path tracer has no row, so its trace and gather stay off, and every
        // other field without a row keeps the add-on's default. The depth of field runs also while the master switch is
        // off, and then alone.
        public static EnhancementsSettings Settings(EnhancementValues values, LensView lens, WeatherLook weather = null)
        {
            uint Bit(Enhancement id) => values.On(id) ? 1u : 0u;
            var settings = EnhancementsSettings.Defaults;
            settings.Enabled = Bit(Enhancement.Enabled);
            // The physical sky darkens the game's ambient light at night through the ambient pass, which then runs also with
            // Ambient light off, at the game's level and without occlusion.
            bool sky = SkyDrawn(values), forced = sky && !values.On(Enhancement.AmbientLight);
            settings.Ambient.Enabled = forced ? 1u : Bit(Enhancement.AmbientLight);
            settings.Ambient.Level = forced ? 1f : values[Enhancement.AmbientLevel];
            settings.AmbientColor.Neutral = forced ? 0f : 1f - values[Enhancement.AmbientColor];
            settings.Sky.Physical = sky ? 1u : 0u;
            settings.Sky.Haze = values[Enhancement.SkyHaziness];
            settings.Aerial.Distance = sky ? values[Enhancement.DistanceHaze] : 0f;
            settings.Sunset.Strength = sky ? values[Enhancement.SunsetColor] / 100f : 0f;
            settings.View = ProbeView;
            settings.Water.Enabled = sky && values.On(Enhancement.PhysicalWater) ? 1u : 0u;
            settings.Water.Clarity = values[Enhancement.WaterClarity];
            settings.Water.Waves = values[Enhancement.WaterWaves] / 100f;
            settings.Water.Size = values[Enhancement.WaterSize] / 100f;
            settings.Ambient.AoStrength = forced ? 0f : values[Enhancement.AmbientOcclusion];
            settings.Ambient.AoRadius = values[Enhancement.AmbientOcclusionRadius];
            settings.Ambient.FoliageAoStrength = forced ? 0f : values[Enhancement.AmbientFoliageOcclusion];
            settings.Transmission.Strength = values.On(Enhancement.LightThroughLeaves) ? values[Enhancement.LightThroughLeavesStrength] / 100f : 0f;
            settings.Grass.RootShade = values.On(Enhancement.GrassDepth) ? values[Enhancement.GrassRootShade] / 100f : 0f;
            // Grass: the game's grass drawn Density / 100 times, or the custom clump in its place.
            bool grass = values.On(Enhancement.GrassDepth), custom = grass && values.On(Enhancement.GrassCustomTexture);
            settings.Grass.Copies = grass ? values[Enhancement.GrassDensity] / 100f : 1f;
            settings.Grass.Port = custom ? 2f : grass && settings.Grass.Copies > 1f ? 1f : 0f;
            settings.Grass.Height = values[Enhancement.GrassCustomHeight] / 100f;
            settings.Leaves.Shading = values.On(Enhancement.LeafShading) ? values[Enhancement.LeafShadingStrength] / 100f : 0f;
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
            settings.Clouds = Clouds(values, lens.GameSpeed);
            settings.CloudPattern.Pattern = PatternOf(CloudKinds[(int)values[Enhancement.CloudKind]]);
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
            settings.AutoExposure.Strength = values[Enhancement.ToneMapAutoExposure] / 100f;
            settings.Tonemap.BloomStrength = values[Enhancement.ToneMapBloom];
            settings.Tonemap.Saturation = values[Enhancement.ToneMapSaturation];
            settings.Tonemap.Contrast = values[Enhancement.ToneMapContrast];
            settings.Tones.Shadows = values[Enhancement.ToneMapShadows];
            settings.Tones.Highlights = values[Enhancement.ToneMapHighlights];
            settings.LensFlare = LensFlare(values, lens.Shot);
            settings.Trace.Enabled = 0;
            settings.Gather.Enabled = 0;
            settings.DepthOfField = DepthOfField(values, lens);
            // The lens softness is the lens's, so it holds with the depth of field switched off, while the free camera is on.
            settings.LensSoftness.Radius = lens.FreeCamera ? values[Enhancement.LensSoftness] : 0f;
            // Weather's look in place of the user's clouds, haziness and Haze effect, as the game's weather has it now.
            if (weather != null && WeatherRuns(values))
            {
                WeatherLooks.Apply(ref settings, weather, values[Enhancement.WeatherMist] / 100f, HazeDensity);
                // Rain off leaves the game's own rain and lightning, which GameRain then shows again.
                if (!values.On(Enhancement.WeatherRain))
                {
                    settings.Rain.Amount = 0f;
                    settings.Rain.Lightning = 0f;
                }
            }
            if (settings.Enabled == 0 && (settings.DepthOfField.Enabled != 0 || settings.LensSoftness.Radius > 0f))
            {
                settings.Enabled = 1;
                settings.Ambient.Enabled = 0;
                settings.Sky.Physical = 0;
                settings.ContactShadows.Enabled = 0;
                settings.SunShadows.Enabled = 0;
                settings.Atmosphere.Enabled = 0;
                settings.Clouds.Enabled = 0;
                settings.LightShafts.Enabled = 0;
                settings.Wetness.Enabled = 0;
                settings.Tonemap.Enabled = 0;
                settings.LensFlare.Enabled = 0;
            }
            return settings;
        }

        // The clouds' block. Coverage is a percentage, and the wind blows at the game's speed.
        public static EnhancementsClouds Clouds(EnhancementValues values, float gameSpeed)
        {
            var kind = CloudKinds[(int)values[Enhancement.CloudKind]];
            var clouds = EnhancementsSettings.Defaults.Clouds;
            clouds.Enabled = values.On(Enhancement.Clouds) ? 1u : 0u;
            clouds.Coverage = values[Enhancement.CloudCoverage] / 100f;
            clouds.Base = values[Enhancement.CloudHeight];
            clouds.Thickness = values[Enhancement.CloudThickness];
            clouds.Size = values[Enhancement.CloudSize];
            clouds.Stretch = kind.Stretch;
            clouds.Density = values[Enhancement.CloudDensity];
            clouds.Detail = values[Enhancement.CloudEdgeDetail];
            clouds.WindSpeed = values[Enhancement.CloudWind] * Math.Max(gameSpeed, 0f);
            clouds.WindDirection = values[Enhancement.CloudWindDirection];
            clouds.Sun = values[Enhancement.CloudSunlight];
            clouds.Ambient = values[Enhancement.CloudSkyLight];
            clouds.Steps = CloudSteps[(int)values[Enhancement.CloudQuality]];
            clouds.Type = kind.Type;
            return clouds;
        }

        // Whether the add-on draws the clouds for these values, and the game's own clouds are to stay hidden.
        public static bool CloudsRun(EnhancementValues values) => values.On(Enhancement.Enabled) && values.On(Enhancement.Clouds);

        // The values after a row's change. A Type but Custom sets the shape rows, and moving a shape row makes Type Custom.
        public static EnhancementValues Edit(EnhancementValues values, Enhancement id, float value)
        {
            var next = values.With(id, value);
            if (ReferenceEquals(next, values))
                return values;
            if (id == Enhancement.CloudType && (int)next[id] < CustomClouds)
            {
                var kind = CloudKinds[(int)next[id]];
                return next.With(Enhancement.CloudKind, next[id]).With(Enhancement.CloudCoverage, kind.Coverage).With(Enhancement.CloudHeight, kind.Height)
                    .With(Enhancement.CloudThickness, kind.Thickness).With(Enhancement.CloudSize, kind.Size).With(Enhancement.CloudDensity, kind.Density)
                    .With(Enhancement.CloudEdgeDetail, kind.EdgeDetail);
            }
            return Array.IndexOf(CloudShapeRows, id) >= 0 ? next.With(Enhancement.CloudType, CustomClouds) : next;
        }

        // The lens flare's block. Position X and Y run from -1 at the left and top edges to 1 at the right and bottom, as the
        // focus point's do, and the add-on's from 0 at the top left; past the edges they place the light off the picture.
        // Live and through the countdown it is drawn before the game's HUD and the panel, which stay clear of it; while a
        // screenshot captures, after ReShade's effects and the depth of field, which then neither blur nor relight it.
        public static EnhancementsLensFlare LensFlare(EnhancementValues values, ShotStage shot)
        {
            var flare = EnhancementsSettings.Defaults.LensFlare;
            flare.Enabled = values.On(Enhancement.LensFlare) ? 1u : 0u;
            flare.Source = (int)values[Enhancement.LensFlareSource] == FlareManual ? EnhancementsFlareSource.Manual : EnhancementsFlareSource.Sun;
            flare.X = (values[Enhancement.LensFlareX] + 1f) * 0.5f;
            flare.Y = (values[Enhancement.LensFlareY] + 1f) * 0.5f;
            flare.Strength = values[Enhancement.LensFlareStrength];
            flare.Arcs = values[Enhancement.LensFlareArcs];
            flare.Rainbow = values[Enhancement.LensFlareRainbow];
            flare.FilledArcs = values[Enhancement.LensFlareFilledArcs];
            flare.Ghosts = values[Enhancement.LensFlareGhosts];
            flare.Streaks = values[Enhancement.LensFlareStreaks];
            flare.Glare = values[Enhancement.LensFlareGlare];
            flare.Size = values[Enhancement.LensFlareSize];
            flare.Warmth = values[Enhancement.LensFlareWarmth];
            flare.Chromatic = values[Enhancement.LensFlareRedFringe];
            flare.Texture = values[Enhancement.LensFlareTexture];
            flare.Place = shot == ShotStage.Capture ? EnhancementsFlarePlace.Display : EnhancementsFlarePlace.Scene;
            return flare;
        }

        // Whether the add-on draws the lens flare for these values, and the game's own sun flare is to stay off.
        public static bool FlareRuns(EnhancementValues values) => values.On(Enhancement.Enabled) && values.On(Enhancement.LensFlare);

        // A debug view of the add-on's, for the probe only: the panel has no row for it.
        public static EnhancementsView ProbeView { get; set; }

        // For the probe only while the volume clouds are rebuilt: they stand in for the painted patterns, but cirrus, which
        // they cannot draw.
        public static bool ProbeVolumeClouds { get; set; }

        // The add-on's pattern for a cloud type: its painted one, or the volume clouds' noise while the probe asks for them.
        public static EnhancementsCloudPatternKind PatternOf(CloudKind kind) =>
            ProbeVolumeClouds && kind.Pattern != EnhancementsCloudPatternKind.Streaks ? EnhancementsCloudPatternKind.Noise : kind.Pattern;

        // Whether Grass's custom texture files are there, null while that is not known: CustomGrass.Found in the game.
        public static Func<bool?> CustomGrassFound { get; set; } = () => null;



        // Whether the physical sky runs, which draws the sky, the sun, the moon and the stars and turns the main light to them.
        // Skill light's share of its full strength, 0 while it or the enhancements are off. It needs no add-on.
        public static float SkillLightStrength(EnhancementValues values)
            => values.On(Enhancement.Enabled) && values.On(Enhancement.SkillLight) ? values[Enhancement.SkillLightStrength] / 100f : 0f;

        public static bool SkyDrawn(EnhancementValues values) => values.On(Enhancement.Enabled) && values.On(Enhancement.PhysicalSky);

        // The Experimental features group's rows: the physical sky's and the clouds'.
        public static bool Experimental(EnhancementField field) => field.Id >= Enhancement.PhysicalSky && field.Id <= Enhancement.WeatherRain;

        // The depth of field's block. The sensor is the one the focal length readout assumes, a full-frame diagonal in the
        // picture's shape, so the readout's millimeters and the blur describe the same lens; N times taller, the whole
        // lens grows with it and images as a 1:N miniature. The focus point's rows run from -1 at the left and the top to
        // 1, the add-on's from 0 at the top left. Eyes autofocuses at the iris's point, from the distance reckoned to it.
        // Focus speed is the seconds the autofocus, and the focus on an eye, take to cover 99% of the way; a manual distance
        // jumps. Max blur is the largest disc's radius in hundredths of the picture's height.
        public static EnhancementsDepthOfField DepthOfField(EnhancementValues values, LensView lens)
        {
            var dof = EnhancementsSettings.Defaults.DepthOfField;
            dof.Enabled = values.On(Enhancement.DepthOfField) && lens.FreeCamera ? 1u : 0u;
            dof.FNumber = Stops[(int)values[Enhancement.DepthOfFieldAperture]];
            dof.SensorHeightMm = Lens.SensorHeight(lens.Aspect) * Math.Max(lens.Miniature, 1);
            int mode = (int)values[Enhancement.DepthOfFieldFocusMode];
            var eye = mode == EyeFocus ? lens.Eye : null;
            dof.FocusDistance = mode == ManualFocus ? values[Enhancement.DepthOfFieldFocusDistance] : 0f;
            dof.FocusX = ((eye?.X ?? values[Enhancement.DepthOfFieldFocusX]) + 1f) * 0.5f;
            dof.FocusY = ((eye?.Y ?? values[Enhancement.DepthOfFieldFocusY]) + 1f) * 0.5f;
            dof.EyeDistance = eye?.Meters ?? 0f;
            dof.FocusSeconds = mode == ManualFocus ? 0f : values[Enhancement.DepthOfFieldFocusSpeed];
            dof.MaxBlur = values[Enhancement.DepthOfFieldMaxBlur] / 100f;
            int shape = (int)values[Enhancement.BokehShape];
            dof.Blades = shape == 0 ? 0u : FewestBlades + (uint)shape - 1u;
            dof.BladeRoundness = values[Enhancement.BladeRoundness];
            dof.BladeRotation = values[Enhancement.BladeRotation];
            dof.SphericalAberration = values[Enhancement.SoapBubble];
            dof.Chromatic = values[Enhancement.BokehChromaticAberration];
            dof.CatEye = values[Enhancement.CatsEye];
            dof.Swirl = values[Enhancement.Swirl];
            dof.Anamorphic = values[Enhancement.AnamorphicRatio];
            dof.Scatter = values.On(Enhancement.HighlightDiscs) ? 1u : 0u;
            dof.HighlightGain = values[Enhancement.HighlightBoost];
            dof.HighlightThreshold = values[Enhancement.HighlightThreshold];
            dof.BokehContrast = values[Enhancement.BokehContrast];
            dof.Samples = lens.Shot != ShotStage.None ? ShotSamples : LiveSamples;
            // Live play and the countdown blur in the scene, so the HUD, the panel and the countdown's toast stay sharp.
            dof.Place = lens.Shot == ShotStage.Capture ? EnhancementsDofPlace.Display : EnhancementsDofPlace.Scene;
            return dof;
        }

        // A folded category shows only its heading, Grass's custom texture switch and its height only while Grass is on, and the folded Experimental features group only its own heading. Open,
        // every switch shows, and an effect's own rows show while its switch is on, whatever the master switch says; the
        // lens flare's position only for its manual light, and Ambient light's Color only for the game's sky, whose color
        // it sets.
        public static bool Shown(EnhancementField field, EnhancementValues values, bool folded, bool experimentalFolded)
            => !folded && !(experimentalFolded && Experimental(field)) && (field.Kind == EnhancementKind.Switch || values.On(field.Effect))
            && (field.Id is not (Enhancement.LensFlareX or Enhancement.LensFlareY) || (int)values[Enhancement.LensFlareSource] == FlareManual)
            && field.Id != Enhancement.CloudKind
            && (field.Id != Enhancement.AmbientColor || !values.On(Enhancement.PhysicalSky))
            && (field.Id is not (Enhancement.GrassCustomTexture or Enhancement.GrassCustomHeight) || values.On(Enhancement.GrassDepth));

        // A slider's value as its row shows it, or null for the field's format: Distance haze's 0 is Off, Sunset color's
        // Natural.
        public static Func<float, string> ValueText(EnhancementField field) => field.Id switch
        {
            Enhancement.DistanceHaze => value => value == 0f ? "Off" : value.ToString(field.Format, CultureInfo.InvariantCulture),
            Enhancement.SunsetColor => value => value == 0f ? "Natural" : value.ToString(field.Format, CultureInfo.InvariantCulture),
            _ => null,
        };

        // The line under a row's label, or null for none: the physical sky says what it does, and the clouds' Type what
        // they look like.
        public static string Detail(EnhancementField field, EnhancementValues values) => field.Id switch
        {
            Enhancement.PhysicalSky => "Real sky, sun, moon and stars that light the world",
            Enhancement.PhysicalWater => "Real reflections and depth for rivers, lakes and the sea",
            Enhancement.Weather => "Clouds and mist that follow the game's weather",
            Enhancement.WeatherRain => values.On(Enhancement.WeatherRain) ? "Real raindrops in place of the game's" : "The game's own rain",
            Enhancement.GrassCustomTexture => CustomGrassFound() switch
            {
                true => "From ReShade's Enhancements\\grass-port folder",
                false => "None in ReShade's Enhancements\\grass-port folder",
                null => null,
            },
            Enhancement.CloudType => (int)values[Enhancement.CloudType] < CustomClouds ? CloudKinds[(int)values[Enhancement.CloudType]].Detail : "Your own shape",
            _ => null,
        };

        // While the master switch is off, every row but the master switch is greyed and ignores input, and Physical water's
        // while Physical sky is off, which it draws with.
        public static bool Usable(EnhancementField field, EnhancementValues values)
            => field.Id == Enhancement.Enabled
            || values.On(Enhancement.Enabled) && (field.Id is not (Enhancement.PhysicalWater or Enhancement.WaterClarity or Enhancement.WaterWaves or Enhancement.WaterSize
                or Enhancement.Weather or Enhancement.WeatherMist or Enhancement.WeatherRain) || values.On(Enhancement.PhysicalSky));

        // Whether Weather's looks run: it draws with the physical sky.
        public static bool WeatherRuns(EnhancementValues values) => SkyDrawn(values) && values.On(Enhancement.Weather);

        // The right of the heading, folded or not: the effects count only while the master switch is on.
        public static string Summary(EnhancementValues values)
        {
            if (!values.On(Enhancement.Enabled))
                return "Off";
            int effects = Effects.Count(field => values.On(field.Id));
            return $"On · {effects} effect{(effects == 1 ? "" : "s")}";
        }

        // The right of the Experimental features heading, as the mockup has it: nothing while it is open, and how many
        // sections it holds while it is folded.
        public static string ExperimentalSummary(bool folded)
        {
            if (!folded)
                return "";
            int sections = Effects.Count(Experimental);
            return $"{sections} section{(sections == 1 ? "" : "s")}";
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

        // The names of fields that moved, as presets saved before then hold them: Ambient light's Sky choice, 1 for the
        // physical sky, and its Haze.
        public static readonly IReadOnlyDictionary<string, Enhancement> Renamed = new Dictionary<string, Enhancement>
        {
            ["AmbientSky"] = Enhancement.PhysicalSky, ["AmbientHaze"] = Enhancement.SkyHaziness,
        };

        // Adds the pair to read when it names a field, or a field's old name, and holds a number.
        public static bool TryRead(string name, string value, Dictionary<Enhancement, float> read)
        {
            if (Renamed.TryGetValue(name, out Enhancement renamed))
                name = renamed.ToString();
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
