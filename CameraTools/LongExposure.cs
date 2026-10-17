using MelonLoader;
using static CameraTools.CameraTools;

namespace CameraTools
{
    // The ReShade tab's long exposure rows and SirCobra's RealLongExposure shader, which no other class names. With the
    // switch on, the screenshot switches the shader on, opens its shutter once the picture has settled, and saves after
    // the shutter time; ReShade then shows the shader's average of every frame in that time.
    internal static class LongExposure
    {
        public const string File = "RealLongExposure.fx";
        // Presents with ReShade's effects loaded and the technique still unreadable before the shader counts as missing.
        // The first get only asks the bridge to watch the name, and its copy arrives with a later present.
        private const ulong MissingPresents = 10;

        private enum Shader { Unknown, Missing, Present }

        private static readonly EffectName technique = new(File, "TECH_RealLongExposure");
        private static readonly EffectName start = new(File, "UI_StartExposure");
        private static readonly Dictionary<string, EffectName> uniforms = new Exposure().Uniforms(false)
            .ToDictionary(uniform => uniform.Name, uniform => new EffectName(File, uniform.Name));

        private static MelonPreferences_Entry<bool> enabled;
        private static MelonPreferences_Entry<float> shutter;
        private static MelonPreferences_Entry<int> iso;
        private static MelonPreferences_Entry<float> boost;
        private static Shader shader;
        private static bool loaded;
        private static ulong loadedAt;

        public static readonly Row[] Rows =
        {
            new Section("Long exposure", () => Problem),
            new ToggleRow("Long exposure", () => On, on => Save(enabled, on))
            {
                Shown = () => Installed,
                Detail = () => On ? "Take screenshot records for the shutter time" : "Take screenshot takes a normal shot",
            },
            new ChoiceRow("Shutter", Exposure.ShutterLabels, () => Exposure.NearestShutter(shutter.Value),
                position => Save(shutter, Exposure.Shutters[position]), null)
            {
                Shown = () => Active,
                Detail = () => "How long the picture records; moving things blur and lights draw trails",
            },
            new ChoiceRow("ISO", Exposure.IsoLabels, () => Array.IndexOf(Exposure.Isos, iso.Value),
                position => Save(iso, Exposure.Isos[position]), null)
            {
                Shown = () => Active,
                Detail = () => "100 keeps the game's brightness; each step brightens the picture",
            },
            new SliderRow("Highlight boost", Exposure.MinBoost, Exposure.MaxBoost, 0.05f, "0.00", () => boost.Value,
                value =>
                {
                    if (value != boost.Value)
                        Save(boost, value);
                })
            {
                Shown = () => Active,
                Detail = () => "Higher keeps bright lights strong as they move; 1 is plain averaging",
            },
        };

        private static bool On => enabled.Value;

        private static bool Installed => ReShade.Connected && shader == Shader.Present;

        // The next screenshot is a long exposure.
        public static bool Active => On && Installed;

        public static Exposure Current => new(Exposure.Shutters[Exposure.NearestShutter(shutter.Value)],
            Exposure.Isos.Contains(iso.Value) ? iso.Value : Exposure.Isos[0], Math.Clamp(boost.Value, Exposure.MinBoost, Exposure.MaxBoost));

        // Whether the technique is on in ReShade now, for the screenshot to put back.
        public static bool TechniqueOn => ReShade.TryGetTechnique(technique, out bool on) && on;

        // The shutter has opened: the shader has the start, so it froze its start time at that present.
        public static bool Open => ReShade.TryGetInt(start, out int value) && value != 0;

        private static string Problem => !ReShade.Connected ? ReShade.Missing
            : shader == Shader.Missing ? $"Install {File} first"
            : shader == Shader.Unknown ? "ReShade is loading its effects"
            : null;

        public static void Load()
        {
            var category = MelonPreferences.CreateCategory("CameraToolsReShade");
            enabled = category.CreateEntry("LongExposure", false,
                description: $"Take screenshot records for the shutter time through {File}. The ReShade tab's switch changes it.");
            shutter = category.CreateEntry("LongExposureShutter", 8f,
                description: "The long exposure's shutter time in seconds, rounded to the nearest of 1/8 s to 120 s in full stops.");
            iso = category.CreateEntry("LongExposureIso", 100,
                description: "The long exposure's ISO: 100, 200, 400, 800 or 1600. 100 keeps the game's brightness.");
            boost = category.CreateEntry("LongExposureHighlightBoost", 1f,
                description: $"How strongly bright lights stay as they move, from {Exposure.MinBoost} to {Exposure.MaxBoost}. 1 is plain averaging.");
            MelonPreferences.Save();
        }

        // Each frame, while the free camera is on. The shader counts as installed once the bridge reads its technique, and
        // as missing once ReShade has had its effects loaded for a while without it. Between loads it keeps what it was.
        public static void Update()
        {
            if (!freecamActive || !ReShade.Connected)
                return;
            var status = ReShade.Status;
            bool ready = status.Runtime == 1 && status.EffectsReady == 1;
            if (ready && !loaded)
                loadedAt = status.Presents;
            loaded = ready;
            if (!ready)
                return;
            // Once read in this load, a failed read is the shader being created after the shot switched it on.
            if (ReShade.TryGetTechnique(technique, out _))
                shader = Shader.Present;
            else if (shader != Shader.Present && status.Presents >= loadedAt + MissingPresents)
                shader = Shader.Missing;
        }

        // Each frame of a long exposure shot: switch the technique on and set every uniform the bridge does not read back
        // as wanted, which also repeats them after the resize reloads the effects. True once all of them read back.
        public static bool Converge(Exposure exposure, bool open)
        {
            bool ready = ReShade.TryGetTechnique(technique, out bool on) && on;
            if (!ready)
                ReShade.SetTechnique(technique, true);
            foreach (var (name, value, whole) in exposure.Uniforms(open))
            {
                var uniform = uniforms[name];
                bool read = whole
                    ? ReShade.TryGetInt(uniform, out int number) && number == (int)value
                    : ReShade.TryGetFloat(uniform, out float x, out _) && x == value;
                if (read)
                    continue;
                ready = false;
                if (whole)
                    ReShade.SetInt(uniform, (int)value);
                else
                    ReShade.SetFloat(uniform, value);
            }
            return ready;
        }

        // After the shot: the shutter closed and the technique as it was, so the shader passes the picture through.
        public static void End(bool techniqueWasOn)
        {
            ReShade.SetInt(start, 0);
            ReShade.SetTechnique(technique, techniqueWasOn);
        }

        private static void Save<T>(MelonPreferences_Entry<T> entry, T value)
        {
            entry.Value = value;
            MelonPreferences.Save();
        }
    }
}
