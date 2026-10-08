using System.Reflection;
using Il2CppInterop.Runtime;
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
        int? Particles = null,
        bool? Outlines = null)
    {
        public static readonly Overrides None = new();
    }

    // Lod is held with the World tab's Max detail switch rather than in Overrides.
    public sealed record Preset(string Name, Func<SettingRow, Pick> Pick, Overrides Overrides, LodLevel? Lod);

    // One setting's options in the settings menu's order, lowest first. Menu[position] is the option index the game stores
    // for that menu position; the two differ where the game appended an option later, such as 45 FPS.
    public sealed record OptionList(int[] Menu)
    {
        public int Lowest => Menu[0];
        public int Highest => Menu[^1];
    }

    // The game settings, overrides and forced LOD at one moment, which PutBack returns to.
    public sealed record GraphicsState(IReadOnlyList<(SettingRow Row, int Index)> Settings, Overrides Overrides, LodLevel? Lod);

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

        // Labels are in menu order, which the log confirms against each setting's option count.
        private static readonly SettingRow[] Table =
        {
            new(SettingKey.TargetFrameRate, "Frame rate", new[] { "30", "45", "60" }, Pick.Keep, Pick.Highest),
            new(SettingKey.VSync, "V-Sync", OffOn, Pick.Keep, Pick.Lowest),
            new(SettingKey.RenderResolution, "Render resolution", new[] { "0.6", "0.8", "0.9", "1.0", "1.1", "1.2", "1.3", "1.4", "1.5" },
                Pick.Highest, Pick.Lowest),
            new(SettingKey.AntiAliasing, "Anti-aliasing", new[] { "Off", "FSR 2", "SMAA" }, Pick.Highest, Pick.Lowest),
            new(SettingKey.ShadowQuality, "Shadow quality", Quality, Pick.Highest, Pick.Lowest),
            new(SettingKey.PostprocessEffect, "Visual effects", Quality, Pick.Highest, Pick.Lowest),
            new(SettingKey.ParticleEffect, "SFX quality", Quality, Pick.Highest, Pick.Lowest),
            new(SettingKey.ComprehensiveQuality, "Environment detail", new[] { "Lowest", "Low", "Medium", "High", "Highest" },
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
        // knob names the game setting whose apply writes the same engine value, which hands the value back to the game, or
        // none where no setting writes it.
        private static readonly IKnob[] Knobs =
        {
            new Knob<float>("innerResolutionScale", SettingKey.RenderResolution, wanted => wanted.RenderScale, ReadScale, WriteScale),
            new Knob<ShadowQuality>("shadows", SettingKey.ShadowQuality, wanted => wanted.Shadows ?? LightShadows(), () => QualitySettings.shadows,
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
            OutlineKnob("outlineCorrectionWidth", found => found.outlineCorrectionWidth, (found, value) => found.outlineCorrectionWidth = value),
            OutlineKnob("resolutionOutlineCorrectionWidth", found => found.resolutionOutlineCorrectionWidth,
                (found, value) => found.resolutionOutlineCorrectionWidth = value),
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
        private static (float At, ScreenSize Wanted, bool Refreshed, bool Quiet)? pendingSize;

        // The unscaled time CameraTools last changed a game setting or the window size, or applied the render resolution
        // again after a size change.
        public static float Changed { get; private set; }

        // A new window size has not had its render resolution applied again yet.
        public static bool SizePending => pendingSize is { Refreshed: false };

        public static void Load()
        {
            var category = MelonPreferences.CreateCategory("CameraToolsGraphics");
            // Build 82 saved each setting's last option index here instead of the setting, so Restore must not use it.
            category.DeleteEntry("SavedGameSettings");
            saved = category.CreateEntry("SavedSettings", "",
                description: "Your own game settings as setting number=option index, saved by CameraTools before it first changes one. Restore puts them back and empties this.");
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
                if (pendingSize is var (sizeAt, wanted, refreshed, quiet) && now >= sizeAt)
                {
                    pendingSize = null;
                    CheckSize(wanted, refreshed, quiet);
                }
            }
            catch (Exception e)
            {
                CameraTools.LogOnce($"Graphics: update failed: {e}");
            }
        }

        public static bool HasSettings => OptionLists().Count > 0;

        // A row for each setting the game offers more than one option for on this machine.
        public static IEnumerable<Row> SettingRows()
        {
            var lists = OptionLists();
            if (lists.Count == 0)
                CameraTools.LogOnce("Graphics: the game's options were not readable when the UI was built; the Graphics tab has no game settings rows.");
            foreach (var row in Table)
            {
                if (!lists.TryGetValue(row.Key, out var list) || list.Menu.Length < 2)
                {
                    CameraTools.LogOnce($"Graphics: no row for {row.Label}; the game offers {(list == null ? 0 : list.Menu.Length)} options for it.");
                    continue;
                }
                var labels = OptionLabels(row, list);
                string Note(Pick pick) => pick switch
                {
                    Pick.Lowest => labels[0],
                    Pick.Highest => labels[^1],
                    _ => "Keep",
                };
                yield return new ChoiceRow(row.Label, labels,
                    () => GameSettings.Get(row.Key) is int now ? Array.IndexOf(list.Menu, now) : -1,
                    position => Change(row, list.Menu[position]),
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
            yield return Beyond("Outlines", () => active.Outlines, value => Override(active with { Outlines = value }, "Outlines"),
                preset => preset.Overrides.Outlines, ("Game", null), ("Off", false));
            yield return Beyond("Particles", () => active.Particles, value => Override(active with { Particles = value }, "Particles"),
                preset => preset.Overrides.Particles, ("Game", null), ("Fewest", 0));
            yield return Beyond("Detail level", () => Lod.Forced, Lod.Force,
                preset => preset.Lod, ("Game", null), ("Highest", LodLevel.MostDetail), ("Lowest", LodLevel.LeastDetail));
        }

        public static void Apply(Preset preset) => CameraUi.Toast(ApplyQuietly(preset));

        // Returns the notice Apply shows. The screenshot button applies a preset while nothing may be on screen.
        public static string ApplyQuietly(Preset preset)
        {
            var lists = OptionLists();
            if (lists.Count == 0)
                return "Graphics presets are unavailable; see the log";
            SaveSettings(lists);
            var changes = new List<string>();
            bool environment = false;
            foreach (var row in Table)
            {
                if (!lists.TryGetValue(row.Key, out var list) || list.Menu.Length < 2 || GameSettings.Get(row.Key) is not int now)
                    continue;
                int? target = preset.Pick(row) switch
                {
                    Pick.Lowest => list.Lowest,
                    Pick.Highest => list.Highest,
                    _ => null,
                };
                if (target is not int index || index == now || !GameSettings.Set(row.Key, index))
                    continue;
                changes.Add($"{row.Label} {now}->{index}");
                environment |= row.Key == SettingKey.ComprehensiveQuality;
            }
            GameSettings.Save();
            Override(preset.Overrides, preset.Name);
            Lod.Force(preset.Lod);
            Changed = Time.unscaledTime;
            Melon<CameraTools>.Logger.Msg($"Graphics: {preset.Name} changed {changes.Count} settings ({string.Join(", ", changes)}); "
                + $"{active}; LOD {preset.Lod}.");
            return $"{preset.Name}: {changes.Count} game settings changed" + (environment ? ". Environment detail applies after a restart" : "");
        }

        public static GraphicsState Capture() => new(Indices(OptionLists()), active, Lod.Forced);

        // Unlike Restore, this returns to the captured moment, not to the user's own settings, and it leaves SavedSettings
        // as it is. A knob whose override ends here hands its engine value back to the game on the next Update.
        public static void PutBack(GraphicsState state)
        {
            var changes = new List<string>();
            foreach (var (row, index) in state.Settings)
                if (GameSettings.Get(row.Key) is int now && now != index && GameSettings.Set(row.Key, index))
                    changes.Add($"{row.Label} {now}->{index}");
            if (changes.Count > 0)
                GameSettings.Save();
            Override(state.Overrides, "putting the settings back");
            Lod.Force(state.Lod);
            Changed = Time.unscaledTime;
            Melon<CameraTools>.Logger.Msg($"Graphics: put back {changes.Count} settings ({string.Join(", ", changes)}); "
                + $"{active}; LOD {state.Lod?.ToString() ?? "game"}.");
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
            Melon<CameraTools>.Logger.Msg($"Graphics: restored your game settings (setting=index): {saved.Value}.");
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

        public static void ApplySlot(int slot) => Resize(Slot(slot));

        // quiet: no notice now or when the size is checked, for the screenshot button.
        public static void Resize(ScreenSize size, bool quiet = false)
        {
            Screen.SetResolution(size.Width, size.Height, Screen.fullScreen);
            Changed = Time.unscaledTime;
            pendingSize = (Changed + 1f, size, false, quiet);
            if (!quiet)
                CameraUi.Toast($"Resolution {size}");
        }

        // Wine only offers the display modes of the screen it runs on, so a size past them is cut down. The game sizes its
        // render targets when it applies its render resolution setting, not when the window changes, so CameraTools
        // applies that setting again once the window has its new size.
        private static void CheckSize(ScreenSize wanted, bool refreshed, bool quiet)
        {
            var got = new ScreenSize(Screen.width, Screen.height);
            var camera = CameraTools.maincam ? CameraTools.maincam : GameObject.Find(MainCameraPath)?.GetComponent<Camera>();
            string rendered = camera ? $"{camera.pixelWidth}x{camera.pixelHeight}" : "no camera";
            if (refreshed)
            {
                Melon<CameraTools>.Logger.Msg($"Graphics: after re-applying render resolution, the window is {got} and the camera renders {rendered}, "
                    + $"innerResolutionScale {ReadScale()?.ToString("0.00") ?? "unreadable"}.");
                return;
            }
            string modes;
            try
            {
                modes = $"current {Screen.currentResolution.width}x{Screen.currentResolution.height}; offered "
                    + string.Join(", ", Screen.resolutions.Select(mode => $"{mode.width}x{mode.height}").Distinct());
            }
            catch (Exception e)
            {
                modes = $"unreadable ({e.Message})";
            }
            Melon<CameraTools>.Logger.Msg($"Graphics: asked for {wanted}, the window is {got}, fullscreen {Screen.fullScreen}, the camera renders {rendered}; "
                + $"display modes: {modes}.");
            if (got != wanted && !quiet)
                CameraUi.Toast($"The display allows {got.Display}, not {wanted.Display}");
            GameSettings.Reapply(SettingKey.RenderResolution);
            if (Layer() is { } found)
                found.RefreshInnerResolution();
            Changed = Time.unscaledTime;
            pendingSize = (Changed + 1f, wanted, true, quiet);
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

        private static Dictionary<SettingKey, OptionList> OptionLists() => GameSettings.OptionLists(Table.Select(row => row.Key));

        // The PC menu's labels, in menu order, when the game has as many options; else numbers.
        private static string[] OptionLabels(SettingRow row, OptionList list)
        {
            if (row.Options.Length == list.Menu.Length)
                return row.Options;
            CameraTools.LogOnce($"Graphics: {row.Label} has {list.Menu.Length} options where CameraTools' table has {row.Options.Length}; numbering them.");
            return Enumerable.Range(1, list.Menu.Length).Select(number => number.ToString()).ToArray();
        }

        private static void Change(SettingRow row, int index)
        {
            SaveSettings(OptionLists());
            int? before = GameSettings.Get(row.Key);
            if (!GameSettings.Set(row.Key, index) || !GameSettings.Save())
            {
                CameraUi.Toast("Changing the setting failed; see the log");
                return;
            }
            Melon<CameraTools>.Logger.Msg($"Graphics: {row.Label} {before}->{index}.");
            if (row.Key == SettingKey.ComprehensiveQuality)
                CameraUi.Toast("Environment detail applies after a restart");
        }

        // Only while the entry is empty, so Max, then Min, then Restore still returns to the user's own settings.
        private static void SaveSettings(Dictionary<SettingKey, OptionList> lists)
        {
            if (saved.Value.Length > 0)
                return;
            saved.Value = string.Join(";", Indices(lists).Select(pair => $"{(int)pair.Row.Key}={pair.Index}"));
            MelonPreferences.Save();
            Melon<CameraTools>.Logger.Msg($"Graphics: saved your game settings for Restore (setting=index): {saved.Value}.");
        }

        private static List<(SettingRow Row, int Index)> Indices(Dictionary<SettingKey, OptionList> lists)
            => Table.Where(row => lists.ContainsKey(row.Key))
                .Select(row => (Row: row, Index: GameSettings.Get(row.Key)))
                .Where(pair => pair.Index != null)
                .Select(pair => (pair.Row, pair.Index.Value))
                .ToList();

        private static void Override(Overrides overrides, string what)
        {
            active = overrides;
            pendingLog = (Time.unscaledTime + LogDelay, $"{LogDelay:0} s after {what}");
        }

        // The game's Lowest shadow quality switches Unity's shadows off, and with them every shadow of a point or a spot,
        // so while a Lights tab light casts shadows they are on. The game's own setting decides, not the engine value, so
        // a screenshot's Max quality hands the shadows back to the game's better ones. The Shadows row's own choice wins.
        private static ShadowQuality? LightShadows()
            => Lights.NeedsShadows && GameSettings.Get(SettingKey.ShadowQuality) == 0 ? ShadowQuality.All : null;

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

        private static float? ReadScale() => ReadLayer("innerResolutionScale", found => found.innerResolutionScale);

        private static float? ReadLayer(string name, Func<PostProcessLayer, float> read)
        {
            try
            {
                return Layer() is { } found ? read(found) : null;
            }
            catch (Exception e)
            {
                CameraTools.LogOnce($"Graphics: reading {name} failed: {e.Message}");
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

        // PostProcessLayer.CorrectOutlineWidth publishes _OutlineCorrectionWidth each frame from two multipliers on the layer,
        // and the character shader scales its outline by it, so both at 0 hide the outlines. No game setting writes them.
        private static Knob<float> OutlineKnob(string name, Func<PostProcessLayer, float> read, Action<PostProcessLayer, float> write)
            => new(name, null, wanted => wanted.Outlines == false ? 0f : null, () => ReadLayer(name, read), value =>
            {
                if (Layer() is not { } found)
                    return;
                write(found, value);
                LogOutlineOffsets();
            });

        // CorrectOutlineWidth reads its multipliers at 0x34 and 0x3c; the fields are matched to them by name only.
        private static void LogOutlineOffsets()
        {
            string Offset(string field)
            {
                try
                {
                    var info = typeof(PostProcessLayer).GetField($"NativeFieldInfoPtr_{field}", BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic);
                    return info == null ? "no field pointer" : $"0x{IL2CPP.il2cpp_field_get_offset((IntPtr)info.GetValue(null)):X}";
                }
                catch (Exception e)
                {
                    return $"failed ({e.Message})";
                }
            }
            CameraTools.LogOnce($"Graphics: PostProcessLayer offsets outlineCorrectionWidth {Offset("outlineCorrectionWidth")}, "
                + $"resolutionOutlineCorrectionWidth {Offset("resolutionOutlineCorrectionWidth")}; CorrectOutlineWidth reads 0x34 and 0x3c.");
        }

        // The interop's enum names do not survive at runtime (PostprocessEffect printed as Reflection), so settings are logged
        // by label and number.
        private static void LogState(string when, bool options)
        {
            var log = Melon<CameraTools>.Logger;
            var lists = OptionLists();
            var indices = new List<string>();
            foreach (var row in Table)
            {
                if (!lists.TryGetValue(row.Key, out var list))
                {
                    indices.Add($"{row.Label}=none");
                    continue;
                }
                string index = GameSettings.Get(row.Key) is int now ? now.ToString() : "?";
                indices.Add($"{row.Label}={index}");
                if (options)
                    log.Msg($"Graphics {when}: {row.Label} ({(int)row.Key}) {GameSettings.Describe(row.Key)}; {list.Menu.Length} options, "
                        + $"option index by menu position: {string.Join(" ", list.Menu)}.");
            }
            log.Msg($"Graphics {when}: settings (option index) {string.Join(", ", indices)}.");
            string outline;
            try
            {
                outline = Shader.GetGlobalFloat("_OutlineCorrectionWidth").ToString();
            }
            catch (Exception e)
            {
                outline = $"failed ({e.Message})";
            }
            log.Msg($"Graphics {when}: {string.Join(", ", Knobs.Select(knob => knob.Describe()))}, _OutlineCorrectionWidth={outline}; "
                + $"LOD {Lod.Forced?.ToString() ?? "game"}.");
        }

        private static List<(SettingKey Key, int Index)> Parse(string text)
        {
            var pairs = new List<(SettingKey, int)>();
            foreach (var part in text.Split(';', StringSplitOptions.RemoveEmptyEntries))
            {
                var halves = part.Split('=');
                if (halves.Length == 2 && int.TryParse(halves[0].Trim(), out int key) && int.TryParse(halves[1].Trim(), out int index))
                    pairs.Add(((SettingKey)key, index));
                else
                    CameraTools.LogOnce($"Graphics: SavedSettings has \"{part}\", which is not setting=index; skipping it.");
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

            // Null when no game setting writes the value.
            private readonly SettingKey? owner;
            private readonly Func<Overrides, T?> wanted;
            // Null when the interop has only the setter.
            private readonly Func<T?> read;
            private readonly Action<T> write;
            private T? written;
            private T? original;
            private float next;

            public Knob(string name, SettingKey? owner, Func<Overrides, T?> wanted, Func<T?> read, Action<T> write)
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
                if (owner is SettingKey key)
                    GameSettings.Reapply(key);
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
        // The saved option index, or the game's default for a setting that was never changed. The Graphics tab reads every
        // shown setting each frame, so this builds no closure or message unless it fails.
        public static int? Get(SettingKey key)
        {
            try
            {
                var saved = FMMBOLGMJGM.MGCADENEBCP(key);
                return saved != null ? saved.index : FMMBOLGMJGM.ADNAKKLFMHG(key);
            }
            catch (Exception e)
            {
                CameraTools.LogOnce($"Graphics: reading setting {(int)key} (FMMBOLGMJGM.MGCADENEBCP, ADNAKKLFMHG) failed: {e.Message}");
                return null;
            }
        }

        public static string Describe(SettingKey key) => Call($"describing setting {(int)key}", () =>
        {
            var saved = FMMBOLGMJGM.MGCADENEBCP(key);
            return $"saved {(saved != null ? saved.index.ToString() : "none")}, default {FMMBOLGMJGM.ADNAKKLFMHG(key)}";
        }, "unreadable");

        // The settings menu's dropdown handler: the index in the current store, the 1-based grade in the older store, then
        // apply.
        public static bool Set(SettingKey key, int index) => Call($"setting {(int)key} to {index}", () =>
        {
            FMMBOLGMJGM.ENHADOCFABO(key, index, true, true);
            DFNEKCNFEDF.FDHDFCLKFBJ(key, index + 1, true, false);
            DFNEKCNFEDF.PENEIPFNKHB(key);
            CameraTools.LogOnce("Graphics: settings are set with FMMBOLGMJGM.ENHADOCFABO(key, index, true, true), "
                + "DFNEKCNFEDF.FDHDFCLKFBJ(key, index + 1, true, false) and DFNEKCNFEDF.PENEIPFNKHB(key).");
            return true;
        }, false);

        public static bool Reapply(SettingKey key) => Call($"re-applying setting {(int)key} (DFNEKCNFEDF.PENEIPFNKHB)", () =>
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

        // MPDLNNKGPCP returns a setting's last option index, or -1 for a setting the game has no options for. The options are
        // stored in the order they were added to the game; CHONAFGBIKI maps an option index to its position in the menu, and
        // returns the index itself for a setting whose menu shows the stored order.
        public static Dictionary<SettingKey, OptionList> OptionLists(IEnumerable<SettingKey> keys) => Call(
            "reading the options (FMMBOLGMJGM.MPDLNNKGPCP, LLJPCNPPHIO.CEJHBLKFKCH.CHONAFGBIKI)", () =>
        {
            var lists = new Dictionary<SettingKey, OptionList>();
            foreach (var key in keys)
            {
                int count = FMMBOLGMJGM.MPDLNNKGPCP(key) + 1;
                if (count > 0)
                    lists[key] = new OptionList(Menu(key, count));
            }
            return lists;
        }, new Dictionary<SettingKey, OptionList>());

        private static int[] Menu(SettingKey key, int count)
        {
            var menu = new int[count];
            Array.Fill(menu, -1);
            for (int index = 0; index < count; index++)
            {
                int position = LLJPCNPPHIO.CEJHBLKFKCH.CHONAFGBIKI(index, key);
                if (position < 0 || position >= count || menu[position] >= 0)
                {
                    CameraTools.LogOnce($"Graphics: setting {(int)key}'s menu positions are not one per option (index {index} -> {position}); using the stored order.");
                    return Enumerable.Range(0, count).ToArray();
                }
                menu[position] = index;
            }
            return menu;
        }

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
