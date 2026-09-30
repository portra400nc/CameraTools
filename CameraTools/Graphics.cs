using MelonLoader;
using UnityEngine;
using UnityEngine.Rendering.PostProcessing;
using SettingKey = DNMAIIHOLOL;

namespace CameraTools
{
    public enum Pick { Keep, Lowest, Highest }

    // One of the game's settings: its row label, the PC menu's option labels, and what each preset picks.
    public sealed record SettingRow(SettingKey Key, string Label, string[] Options, Pick Screenshot, Pick Performance);

    // Values past the game's own range. Null leaves a value to the game. They are not saved, so a restart resets them.
    public sealed record Overrides(
        float? RenderScale = null,
        ShadowQuality? Shadows = null,
        float? ShadowDistance = null,
        bool? DistantShadows = null,
        bool? Fog = null,
        int? Particles = null)
    {
        public static readonly Overrides None = new();
    }

    // Lod is held with the World tab's Max detail switch rather than in Overrides.
    public sealed record Preset(string Name, Func<SettingRow, Pick> Pick, Overrides Overrides, LodLevel? Lod);

    // One setting's option list: how many options it has, their text keys, and the option each position of the settings
    // menu's dropdown shows. Menu is null when the game's mapping failed.
    public sealed record OptionList(int Count, string[] Texts, int[] Menu)
    {
        // Some options may not be offered on PC, so a row steps through the options the menu reaches, in menu order.
        public int[] Reachable => Menu?.Where(index => index >= 0 && index < Count).Distinct().ToArray() is { Length: > 0 } reached
            ? reached
            : Enumerable.Range(0, Count).ToArray();

        public int Lowest => Reachable.DefaultIfEmpty().Min();
        public int Highest => Reachable.DefaultIfEmpty().Max();
    }

    // A resolution slot's size, parsed from its "WxH" preference.
    public readonly record struct ScreenSize(int Width, int Height)
    {
        public const int MinWidth = 320;
        public const int MinHeight = 200;
        public const int MaxSide = 16384;

        public static bool TryParse(string text, out ScreenSize size)
        {
            size = default;
            var parts = (text ?? "").Split('x', 'X');
            if (parts.Length != 2 || !int.TryParse(parts[0].Trim(), out int width) || !int.TryParse(parts[1].Trim(), out int height)
                || width < MinWidth || height < MinHeight || width > MaxSide || height > MaxSide)
                return false;
            size = new ScreenSize(width, height);
            return true;
        }

        public string Display => $"{Width} × {Height}";

        public override string ToString() => $"{Width}x{Height}";
    }

    // The Graphics tab. Game settings are written through the game's own settings store, so its menu shows them and they
    // are saved; values past the game's range are held as overrides. The user's settings are saved once, before
    // CameraTools first changes one, for Restore.
    internal static class Graphics
    {
        private const string MainCameraPath = "/EntityRoot/MainCamera(Clone)";
        private const float LookInterval = 1f;
        private const float LogDelay = 2f;

        private static readonly string[] OffOn = { "Off", "On" };
        private static readonly string[] Quality = { "Lowest", "Low", "Medium", "High" };

        // Index 0 is assumed to be each list's low end; the log shows each list's text keys to confirm it.
        private static readonly SettingRow[] Table =
        {
            new(SettingKey.TargetFrameRate, "Frame rate", new[] { "30", "45", "60" }, Pick.Keep, Pick.Highest),
            new(SettingKey.VSync, "V-Sync", OffOn, Pick.Keep, Pick.Lowest),
            new(SettingKey.RenderResolution, "Render resolution", new[] { "0.6", "0.8", "0.9", "1.0", "1.1", "1.2", "1.3", "1.4", "1.5" },
                Pick.Highest, Pick.Lowest),
            new(SettingKey.AntiAliasing, "Anti-aliasing", new[] { "Off", "FSR 2", "SMAA", "TAA" }, Pick.Highest, Pick.Lowest),
            new(SettingKey.ShadowQuality, "Shadow quality", Quality, Pick.Highest, Pick.Lowest),
            new(SettingKey.PostprocessEffect, "Visual effects", Quality, Pick.Highest, Pick.Lowest),
            new(SettingKey.ParticleEffect, "SFX quality", Quality, Pick.Highest, Pick.Lowest),
            new(SettingKey.ComprehensiveQuality, "Environment detail", new[] { "Lowest", "Low", "Medium", "High", "Very high" },
                Pick.Highest, Pick.Lowest),
            new(SettingKey.VolumetricFog, "Volumetric fog", OffOn, Pick.Highest, Pick.Lowest),
            new(SettingKey.Reflection, "Reflections", OffOn, Pick.Highest, Pick.Lowest),
            new(SettingKey.MotionBlur, "Motion blur", new[] { "Off", "Low", "High", "Extreme" }, Pick.Lowest, Pick.Lowest),
            new(SettingKey.Bloom, "Bloom", OffOn, Pick.Highest, Pick.Lowest),
            new(SettingKey.CrowdDensity, "Crowd density", new[] { "Low", "High" }, Pick.Highest, Pick.Lowest),
            new(SettingKey.ScreenSubsurfaceScattering, "Subsurface scattering", new[] { "Off", "Medium", "High" }, Pick.Highest, Pick.Lowest),
            new(SettingKey.OnlineEffect, "Co-op teammate effects", new[] { "Off", "Partial", "On" }, Pick.Keep, Pick.Lowest),
            new(SettingKey.AnisotropicFiltering, "Anisotropic filtering", new[] { "1x", "2x", "4x", "8x", "16x" }, Pick.Highest, Pick.Lowest),
            new(SettingKey.GlobalIllumination, "Global illumination", OffOn, Pick.Highest, Pick.Lowest),
        };

        // The game's own render resolution goes to its highest, so Max leaves render scale to the game.
        public static readonly Preset Screenshot = new("Max quality", row => row.Screenshot,
            new Overrides(ShadowDistance: 300f), LodLevel.MostDetail);

        public static readonly Preset Performance = new("Min for performance", row => row.Performance,
            new Overrides(RenderScale: 0.5f, Shadows: ShadowQuality.Disable, ShadowDistance: 20f, DistantShadows: false, Fog: false, Particles: 0),
            LodLevel.LeastDetail);

        private static readonly ScreenSize[] CommonSizes =
        {
            new(1280, 800), new(1440, 900), new(1680, 1050), new(1920, 1200), new(2560, 1600), new(3840, 2400),
        };

        private static readonly (string Name, ScreenSize Size)[] SlotDefaults =
        {
            ("ResolutionSlot1", new(1280, 800)),
            ("ResolutionSlot2", new(2560, 1600)),
        };

        // The interop has only a setter for shadowDistance, so it is written again every second instead of compared. Each
        // knob names the game setting whose apply writes the same engine value, which hands the value back to the game.
        private static readonly IKnob[] Knobs =
        {
            new Knob<float>("innerResolutionScale", SettingKey.RenderResolution, wanted => wanted.RenderScale, ReadScale, WriteScale),
            new Knob<ShadowQuality>("shadows", SettingKey.ShadowQuality, wanted => wanted.Shadows, () => QualitySettings.shadows,
                value => QualitySettings.shadows = value),
            new Knob<float>("shadowDistance", SettingKey.ShadowQuality, wanted => wanted.ShadowDistance, null, QualitySettings.set_shadowDistance),
            new Knob<bool>("enableDistantShadow", SettingKey.ShadowQuality, wanted => wanted.DistantShadows, () => QualitySettings.enableDistantShadow,
                value => QualitySettings.enableDistantShadow = value),
            new Knob<bool>("volumetricFogEnabled", SettingKey.VolumetricFog, wanted => wanted.Fog, () => QualitySettings.volumetricFogEnabled,
                value => QualitySettings.volumetricFogEnabled = value),
            new Knob<bool>("godRayEnabled", SettingKey.VolumetricFog, wanted => wanted.Fog, () => QualitySettings.godRayEnabled,
                value => QualitySettings.godRayEnabled = value),
            new Knob<int>("particleEmitLevel", SettingKey.ParticleEffect, wanted => wanted.Particles, () => QualitySettings.particleEmitLevel,
                value => QualitySettings.particleEmitLevel = value),
        };

        private static MelonPreferences_Entry<string> saved;
        private static MelonPreferences_Entry<string>[] slotEntries;
        private static ScreenSize[] slots;
        private static Overrides active = Overrides.None;
        private static PostProcessLayer layer;
        private static float nextLayerLook;
        private static float nextWorldLook;
        private static bool worldLogged;
        private static (float At, string When)? pendingLog;

        public static void Load()
        {
            var category = MelonPreferences.CreateCategory("CameraToolsGraphics");
            saved = category.CreateEntry("SavedGameSettings", "",
                description: "Your own game settings, saved by CameraTools before it first changes one. Restore puts them back and empties this.");
            slotEntries = new MelonPreferences_Entry<string>[SlotDefaults.Length];
            slots = new ScreenSize[SlotDefaults.Length];
            for (int i = 0; i < SlotDefaults.Length; i++)
            {
                var (name, fallback) = SlotDefaults[i];
                slotEntries[i] = category.CreateEntry(name, fallback.ToString(), description: "A resolution as WxH, for example 1920x1200.");
                if (ScreenSize.TryParse(slotEntries[i].Value, out slots[i]))
                    continue;
                Melon<CameraTools>.Logger.Warning($"CameraToolsGraphics {name} = \"{slotEntries[i].Value}\" is not a resolution; using {fallback}.");
                slots[i] = fallback;
            }
            MelonPreferences.Save();
        }

        public static void Update()
        {
            try
            {
                float now = Time.unscaledTime;
                if (!worldLogged && now >= nextWorldLook)
                {
                    nextWorldLook = now + LookInterval;
                    if (CameraTools.maincam || GameObject.Find(MainCameraPath))
                    {
                        worldLogged = true;
                        LogState("when the world loaded", true);
                    }
                }
                foreach (var knob in Knobs)
                {
                    try
                    {
                        knob.Update(active, now);
                    }
                    catch (Exception e)
                    {
                        CameraTools.LogOnce($"Graphics: holding {knob.Name} failed: {e.Message}");
                    }
                }
                if (pendingLog is var (at, when) && now >= at)
                {
                    pendingLog = null;
                    LogState(when, false);
                }
            }
            catch (Exception e)
            {
                CameraTools.LogOnce($"Graphics: update failed: {e}");
            }
        }

        public static bool HasSettings => GameSettings.OptionLists().Count > 0;

        // A row for each setting whose menu reaches more than one option on this machine.
        public static IEnumerable<Row> SettingRows()
        {
            var lists = GameSettings.OptionLists();
            if (lists.Count == 0)
                CameraTools.LogOnce("Graphics: the game's option lists were not readable when the UI was built; the Graphics tab has no game settings rows.");
            foreach (var row in Table)
            {
                if (!lists.TryGetValue(row.Key, out var list))
                {
                    CameraTools.LogOnce($"Graphics: no row for {row.Key}; the game has no option list for it.");
                    continue;
                }
                int[] reachable = list.Reachable;
                if (reachable.Length < 2)
                {
                    CameraTools.LogOnce($"Graphics: no row for {row.Key}; its menu reaches {reachable.Length} of {list.Count} options.");
                    continue;
                }
                var labels = OptionLabels(row, list);
                string Note(Pick pick) => pick switch
                {
                    Pick.Lowest => labels[list.Lowest],
                    Pick.Highest => labels[list.Highest],
                    _ => "Keep",
                };
                yield return new ChoiceRow(row.Label, reachable.Select(index => labels[index]).ToArray(),
                    () => GameSettings.Get(row.Key) is int now ? Array.IndexOf(reachable, now) : -1,
                    position => Change(row.Key, reachable[position]),
                    new PresetNote(Note(row.Screenshot), Note(row.Performance), row.Key == SettingKey.ComprehensiveQuality));
            }
        }

        public static IEnumerable<Row> BeyondRows()
        {
            string Scale(Preset preset) => preset.Overrides.RenderScale?.ToString("0.00") ?? "Game";
            yield return new SliderRow("Render scale", 0.25f, 2f, 0.05f, "0.00", () => RenderScale, SetRenderScale,
                new PresetNote(Scale(Screenshot), Scale(Performance), false));
            yield return Beyond("Shadows", () => active.Shadows, value => Override(active with { Shadows = value }, "Shadows"),
                preset => preset.Overrides.Shadows, ("Game", null), ("Off", ShadowQuality.Disable));
            yield return Beyond("Shadow distance", () => active.ShadowDistance, value => Override(active with { ShadowDistance = value }, "Shadow distance"),
                preset => preset.Overrides.ShadowDistance, ("Game", null), ("20 m", 20f), ("50 m", 50f), ("150 m", 150f), ("300 m", 300f));
            yield return Beyond("Distant shadows", () => active.DistantShadows, value => Override(active with { DistantShadows = value }, "Distant shadows"),
                preset => preset.Overrides.DistantShadows, ("Game", null), ("Off", false));
            yield return Beyond("Fog and god rays", () => active.Fog, value => Override(active with { Fog = value }, "Fog and god rays"),
                preset => preset.Overrides.Fog, ("Game", null), ("Off", false));
            yield return Beyond("Particles", () => active.Particles, value => Override(active with { Particles = value }, "Particles"),
                preset => preset.Overrides.Particles, ("Game", null), ("Fewest", 0));
            yield return Beyond("Detail level", () => Lod.Forced, Lod.Force,
                preset => preset.Lod, ("Game", null), ("Highest", LodLevel.MostDetail), ("Lowest", LodLevel.LeastDetail));
        }

        public static void Apply(Preset preset)
        {
            var lists = GameSettings.OptionLists();
            if (lists.Count == 0)
            {
                CameraUi.Toast("Graphics presets are unavailable; see the log");
                return;
            }
            SaveSettings(lists);
            var changes = new List<string>();
            bool environment = false;
            foreach (var row in Table)
            {
                if (!lists.TryGetValue(row.Key, out var list) || list.Reachable.Length < 2 || GameSettings.Get(row.Key) is not int now)
                    continue;
                int? target = preset.Pick(row) switch
                {
                    Pick.Lowest => list.Lowest,
                    Pick.Highest => list.Highest,
                    _ => null,
                };
                if (target is not int index || index == now || !GameSettings.Set(row.Key, index))
                    continue;
                changes.Add($"{row.Key} {now}->{index}");
                environment |= row.Key == SettingKey.ComprehensiveQuality;
            }
            GameSettings.Save();
            Override(preset.Overrides, preset.Name);
            Lod.Force(preset.Lod);
            Melon<CameraTools>.Logger.Msg($"Graphics: {preset.Name} changed {changes.Count} settings ({string.Join(", ", changes)}); "
                + $"{active}; LOD {preset.Lod}.");
            CameraUi.Toast($"{preset.Name}: {changes.Count} game settings changed" + (environment ? ". Environment detail applies after a restart" : ""));
        }

        public static void Restore()
        {
            var pairs = Parse(saved.Value);
            bool overridden = active != Overrides.None || Lod.Forced != null;
            foreach (var knob in Knobs)
                knob.Release();
            active = Overrides.None;
            Lod.Force(null);
            if (pairs.Count == 0)
            {
                CameraUi.Toast(overridden ? "Overrides cleared" : "Nothing to restore");
                return;
            }
            // Every saved setting is applied again, even an unchanged one, so the game rewrites the engine values the
            // overrides held.
            bool environment = false;
            bool failed = false;
            foreach (var (key, index) in pairs)
            {
                environment |= key == SettingKey.ComprehensiveQuality && GameSettings.Get(key) is int now && now != index;
                failed |= !GameSettings.Set(key, index);
            }
            failed |= !GameSettings.Save();
            if (failed)
            {
                CameraUi.Toast("Restoring your settings failed; see the log");
                return;
            }
            Melon<CameraTools>.Logger.Msg($"Graphics: restored your game settings: {saved.Value}.");
            saved.Value = "";
            MelonPreferences.Save();
            pendingLog = (Time.unscaledTime + LogDelay, $"{LogDelay:0} s after Restore");
            CameraUi.Toast("Restored your settings" + (environment ? ". Environment detail applies after a restart" : ""));
        }

        public static float RenderScale => active.RenderScale ?? ReadScale() ?? 1f;

        public static void SetRenderScale(float scale) => Override(active with { RenderScale = MathF.Round(scale * 20f) / 20f }, "render scale");

        public static ScreenSize Slot(int slot) => slots[slot - 1];

        public static void SetSlot(int slot, ScreenSize size)
        {
            slots[slot - 1] = size;
            slotEntries[slot - 1].Value = size.ToString();
            MelonPreferences.Save();
        }

        public static void CycleSlot(int slot, int direction)
        {
            var size = Slot(slot);
            int area = size.Width * size.Height;
            var next = direction > 0
                ? CommonSizes.FirstOrDefault(common => common.Width * common.Height > area)
                : CommonSizes.LastOrDefault(common => common.Width * common.Height < area);
            if (next != default)
                SetSlot(slot, next);
        }

        public static void ApplySlot(int slot)
        {
            var size = Slot(slot);
            Screen.SetResolution(size.Width, size.Height, Screen.fullScreen);
            CameraUi.Toast($"Resolution {size}");
        }

        // A "Beyond the game's limits" row. Its first option is "Game", which holds nothing.
        private static ChoiceRow Beyond<T>(string label, Func<T?> get, Action<T?> set, Func<Preset, T?> preset, params (string Text, T? Value)[] options)
            where T : struct
        {
            int Position(T? value) => Array.FindIndex(options, option => Nullable.Equals(option.Value, value));
            var texts = options.Select(option => option.Text).ToArray();
            return new ChoiceRow(label, texts, () => Position(get()), position => set(options[position].Value),
                new PresetNote(texts[Position(preset(Screenshot))], texts[Position(preset(Performance))], false));
        }

        // The game's own words when its text map resolves every option, else the PC menu's, else numbers.
        private static string[] OptionLabels(SettingRow row, OptionList list)
        {
            var mapped = list.Texts.Select(GameSettings.Text).ToArray();
            if (mapped.Length == list.Count && mapped.All(text => text != null))
            {
                CameraTools.LogOnce($"Graphics: {row.Key} options from the game's text map: {string.Join(", ", mapped)}.");
                return mapped;
            }
            if (row.Options.Length == list.Count)
            {
                CameraTools.LogOnce($"Graphics: {row.Key} options from CameraTools' table; the text map resolved {mapped.Count(text => text != null)} of {list.Count}.");
                return row.Options;
            }
            CameraTools.LogOnce($"Graphics: {row.Key} has {list.Count} options where CameraTools' table has {row.Options.Length}; numbering them.");
            return Enumerable.Range(1, list.Count).Select(number => number.ToString()).ToArray();
        }

        private static void Change(SettingKey key, int index)
        {
            SaveSettings(GameSettings.OptionLists());
            int? before = GameSettings.Get(key);
            if (!GameSettings.Set(key, index) || !GameSettings.Save())
            {
                CameraUi.Toast("Changing the setting failed; see the log");
                return;
            }
            Melon<CameraTools>.Logger.Msg($"Graphics: {key} {before}->{index}.");
            if (key == SettingKey.ComprehensiveQuality)
                CameraUi.Toast("Environment detail applies after a restart");
        }

        // Only while the entry is empty, so Max, then Min, then Restore still returns to the user's own settings.
        private static void SaveSettings(Dictionary<SettingKey, OptionList> lists)
        {
            if (saved.Value.Length > 0)
                return;
            saved.Value = string.Join(";", Table.Where(row => lists.ContainsKey(row.Key))
                .Select(row => (row.Key, Index: GameSettings.Get(row.Key)))
                .Where(pair => pair.Index != null)
                .Select(pair => $"{pair.Key}={pair.Index}"));
            MelonPreferences.Save();
            Melon<CameraTools>.Logger.Msg($"Graphics: saved your game settings for Restore: {saved.Value}.");
        }

        private static void Override(Overrides overrides, string what)
        {
            active = overrides;
            pendingLog = (Time.unscaledTime + LogDelay, $"{LogDelay:0} s after {what}");
        }

        private static PostProcessLayer Layer()
        {
            if (layer)
                return layer;
            if (Time.unscaledTime < nextLayerLook)
                return null;
            nextLayerLook = Time.unscaledTime + LookInterval;
            var camera = CameraTools.maincam ? CameraTools.maincam.gameObject : GameObject.Find(MainCameraPath);
            layer = camera ? camera.GetComponent<PostProcessLayer>() : null;
            if (camera)
                CameraTools.LogOnce(layer ? "Graphics: found the main camera's PostProcessLayer." : "Graphics: the main camera has no PostProcessLayer.");
            return layer ? layer : null;
        }

        private static float? ReadScale()
        {
            try
            {
                return Layer() is { } found ? found.innerResolutionScale : null;
            }
            catch (Exception e)
            {
                CameraTools.LogOnce($"Graphics: reading innerResolutionScale failed: {e.Message}");
                return null;
            }
        }

        private static void WriteScale(float scale)
        {
            if (Layer() is not { } found)
                return;
            found.innerResolutionScale = scale;
            found.RefreshInnerResolution();
        }

        private static void LogState(string when, bool options)
        {
            var log = Melon<CameraTools>.Logger;
            var lists = GameSettings.OptionLists();
            var indices = new List<string>();
            foreach (var key in Enum.GetValues<SettingKey>())
            {
                if (key == SettingKey.Invalid || !lists.TryGetValue(key, out var list))
                    continue;
                string index = GameSettings.Get(key) is int now ? now.ToString() : "?";
                indices.Add($"{key}={index}/{list.Count}");
                if (options)
                    log.Msg($"Graphics {when}: {key} index {index} of {list.Count}; text keys [{string.Join(", ", list.Texts)}]; "
                        + $"menu position->index {(list.Menu == null ? "failed" : string.Join(" ", list.Menu.Select((index, position) => $"{position}->{index}")))}; "
                        + $"lowest {list.Lowest}, highest {list.Highest}.");
            }
            if (options)
                log.Msg($"Graphics {when}: settings without an option list: "
                    + string.Join(", ", Enum.GetValues<SettingKey>().Where(key => key != SettingKey.Invalid && !lists.ContainsKey(key))) + ".");
            else
                log.Msg($"Graphics {when}: settings (index/count) {string.Join(", ", indices)}.");
            log.Msg($"Graphics {when}: {string.Join(", ", Knobs.Select(knob => knob.Describe()))}; LOD {Lod.Forced?.ToString() ?? "game"}.");
        }

        private static List<(SettingKey Key, int Index)> Parse(string text)
        {
            var pairs = new List<(SettingKey, int)>();
            foreach (var part in text.Split(';', StringSplitOptions.RemoveEmptyEntries))
            {
                var halves = part.Split('=');
                if (halves.Length == 2 && Enum.TryParse(halves[0].Trim(), out SettingKey key) && int.TryParse(halves[1].Trim(), out int index))
                    pairs.Add((key, index));
                else
                    CameraTools.LogOnce($"Graphics: SavedGameSettings has \"{part}\", which is not setting=index; skipping it.");
            }
            return pairs;
        }

        private interface IKnob
        {
            string Name { get; }
            void Update(Overrides overrides, float time);
            void Release();
            string Describe();
        }

        // One engine value an override holds. The game may write it back, on a scene load or when it applies its own
        // settings, so it is written again when it differs; a repeat write waits a second, so a value the game rewrites
        // every frame is not fought every frame.
        private sealed class Knob<T> : IKnob where T : struct
        {
            private const float ReassertInterval = 1f;

            private readonly SettingKey owner;
            private readonly Func<Overrides, T?> wanted;
            // Null when the interop has only the setter.
            private readonly Func<T?> read;
            private readonly Action<T> write;
            private T? written;
            private T? original;
            private float next;

            public Knob(string name, SettingKey owner, Func<Overrides, T?> wanted, Func<T?> read, Action<T> write)
            {
                Name = name;
                this.owner = owner;
                this.wanted = wanted;
                this.read = read;
                this.write = write;
            }

            public string Name { get; }

            public void Update(Overrides overrides, float time)
            {
                if (wanted(overrides) is not T target)
                {
                    Release();
                    return;
                }
                bool fresh = !Same(written, target);
                if (read == null)
                {
                    if (fresh || time >= next)
                        Write(target, time);
                    return;
                }
                if (read() is not T now || Same(now, target) || !fresh && time < next)
                    return;
                if (!fresh)
                    CameraTools.LogOnce($"Graphics: the game set {Name} back to {now}; CameraTools writes {target} again.");
                original ??= now;
                Write(target, time);
            }

            // Puts back the value from before the override where the interop can read it, then has the game apply its own
            // setting again, which covers a value that cannot be read and one the game changed in the meantime.
            public void Release()
            {
                if (written == null)
                    return;
                if (original is T value && read?.Invoke() != null)
                    write(value);
                GameSettings.Reapply(owner);
                written = null;
                original = null;
            }

            public string Describe()
            {
                try
                {
                    return $"{Name}={(read == null ? "write-only" : read() is T now ? now.ToString() : "unavailable")}";
                }
                catch (Exception e)
                {
                    return $"{Name}=failed ({e.Message})";
                }
            }

            private void Write(T value, float time)
            {
                write(value);
                written = value;
                next = time + ReassertInterval;
            }

            private static bool Same(T? a, T b) => a is T value && EqualityComparer<T>.Default.Equals(value, b);
        }
    }

    // The only class that names the game's obfuscated members, which are the September 2026 build's. Each call is caught
    // and logged once, so a game update makes the presets fail visibly and leaves the rest of CameraTools running.
    internal static class GameSettings
    {
        // The Graphics tab reads every shown setting each frame, so this builds no closure or message unless it fails.
        public static int? Get(SettingKey key)
        {
            try
            {
                return FMMBOLGMJGM.MPDLNNKGPCP(key);
            }
            catch (Exception e)
            {
                CameraTools.LogOnce($"Graphics: reading {key} (FMMBOLGMJGM.MPDLNNKGPCP) failed: {e.Message}");
                return null;
            }
        }

        // The settings menu's dropdown handler: the index in the current store, the 1-based grade in the older store, then
        // apply.
        public static bool Set(SettingKey key, int index) => Call($"setting {key} to {index}", () =>
        {
            FMMBOLGMJGM.ENHADOCFABO(key, index, true, true);
            DFNEKCNFEDF.FDHDFCLKFBJ(key, index + 1, true, false);
            DFNEKCNFEDF.PENEIPFNKHB(key);
            CameraTools.LogOnce("Graphics: settings are set with FMMBOLGMJGM.ENHADOCFABO(key, index, true, true), "
                + "DFNEKCNFEDF.FDHDFCLKFBJ(key, index + 1, true, false) and DFNEKCNFEDF.PENEIPFNKHB(key).");
            return true;
        }, false);

        public static bool Reapply(SettingKey key) => Call($"re-applying {key} (DFNEKCNFEDF.PENEIPFNKHB)", () =>
        {
            DFNEKCNFEDF.PENEIPFNKHB(key);
            return true;
        }, false);

        public static bool Save() => Call("saving the settings", () =>
        {
            FMMBOLGMJGM.DJMHMKEJJFA(true);
            DFNEKCNFEDF.DJMHMKEJJFA();
            CameraTools.LogOnce("Graphics: settings are saved with FMMBOLGMJGM.DJMHMKEJJFA(true) and DFNEKCNFEDF.DJMHMKEJJFA().");
            return true;
        }, false);

        public static Dictionary<SettingKey, OptionList> OptionLists() => Call("reading the option lists (DFNEKCNFEDF.KKJCDGBNBOJ)", () =>
        {
            var lists = new Dictionary<SettingKey, OptionList>();
            var entries = DFNEKCNFEDF.KKJCDGBNBOJ()?.HDPKADAMLAO;
            if (entries == null)
                return lists;
            foreach (var entry in entries)
            {
                if (entry == null)
                    continue;
                var key = entry.DKOKOOGIBML;
                int count = entry.AHALONJFNEJ?.Length ?? 0;
                lists[key] = new OptionList(count, entry.OAOJLNPHFPI?.ToArray() ?? Array.Empty<string>(), Menu(key, count));
            }
            return lists;
        }, new Dictionary<SettingKey, OptionList>());

        // MonoLocalizedText resolves its text IDs with FBPIAOMICJL(id, false). Null when the key does not resolve.
        public static string Text(string key)
        {
            string text = Call("looking up option text in the text map (AEPDNIAIMBA.FBPIAOMICJL)", () => AEPDNIAIMBA.FBPIAOMICJL(key, false), null);
            return string.IsNullOrWhiteSpace(text) || text == key ? null : text;
        }

        private static int[] Menu(SettingKey key, int count) => Call($"mapping {key}'s menu positions (LLJPCNPPHIO.CEJHBLKFKCH.CHONAFGBIKI)",
            () => Enumerable.Range(0, count).Select(position => LLJPCNPPHIO.CEJHBLKFKCH.CHONAFGBIKI(position, key)).ToArray(), null);

        private static T Call<T>(string what, Func<T> call, T failed)
        {
            try
            {
                return call();
            }
            catch (Exception e)
            {
                CameraTools.LogOnce($"Graphics: {what} failed: {e.Message}");
                return failed;
            }
        }
    }
}
