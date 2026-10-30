using MelonLoader;
using Enhancements = CameraToolsEnhancements.Enhancements;

namespace CameraTools
{
    // The ReShade tab's long exposure rows. With the switch on, the screenshot records the shutter time through CameraTools'
    // own method, which the Enhancements add-on averages (NativeExposure).
    internal static class LongExposure
    {
        private static MelonPreferences_Entry<bool> enabled;
        private static MelonPreferences_Entry<float> shutter;
        private static MelonPreferences_Entry<int> iso;
        private static MelonPreferences_Entry<int> smoothness;
        private static MelonPreferences_Entry<double> measured;

        public static readonly Row[] Rows =
        {
            new Section("Long exposure", () => Problem),
            new ToggleRow("Long exposure", () => On, on => Save(enabled, on))
            {
                Shown = () => Installed,
                Detail = () => On ? "Take screenshot records for the shutter time" : "Take screenshot takes a normal shot",
            },
            new ChoiceRow("Shutter speed", Exposure.ShutterLabels, () => Exposure.NearestShutter(shutter.Value),
                position => Save(shutter, Exposure.Shutters[position]), null)
            {
                Shown = () => Active,
                Sub = true,
            },
            new ChoiceRow("ISO", Exposure.IsoLabels, () => Array.IndexOf(Exposure.Isos, iso.Value),
                position => Save(iso, Exposure.Isos[position]), null)
            {
                Shown = () => Active,
                Sub = true,
            },
            new ChoiceRow("Smoothness", LongExposurePlan.SmoothnessLabels, () => LongExposurePlan.NearestSmoothness(smoothness.Value),
                position => Save(smoothness, LongExposurePlan.Smoothness[position]), null)
            {
                Shown = () => Active,
                Sub = true,
                Detail = () => Estimate.Text,
                DetailWarns = () => Estimate.Warn,
            },
        };

        private static bool On => enabled.Value;

        private static bool Installed => ReShade.Connected && Enhancements.Connected;

        public static bool Active => Installed && On;

        public static Exposure Current => new(Exposure.Shutters[Exposure.NearestShutter(shutter.Value)],
            Exposure.Isos.Contains(iso.Value) ? iso.Value : Exposure.Isos[0]);

        // Game frames per second of shutter time.
        public static int FramesPerSecond => LongExposurePlan.Smoothness[LongExposurePlan.NearestSmoothness(smoothness.Value)];

        private static (string Text, bool Warn) Estimate
            => LongExposurePlan.Estimate(LongExposurePlan.Frames(Current.Seconds, FramesPerSecond), measured.Value);

        private static string Problem => !ReShade.Connected ? ReShade.Missing
            : !Enhancements.Connected ? "Install the Enhancements add-on first"
            : null;

        public static void Load()
        {
            var category = MelonPreferences.CreateCategory("CameraToolsReShade");
            enabled = category.CreateEntry("LongExposure", false,
                description: "Take screenshot records for the shutter time, through CameraTools and CameraToolsEnhancements.addon64. The ReShade tab's switch changes it.");
            shutter = category.CreateEntry("LongExposureShutter", 8f,
                description: "The long exposure's shutter time in seconds, rounded to the nearest of 1/8 s to 120 s in full stops.");
            iso = category.CreateEntry("LongExposureIso", 100,
                description: "The long exposure's ISO: 100, 200, 400, 800 or 1600. 100 keeps the game's brightness.");
            smoothness = category.CreateEntry("LongExposureSmoothness", 240,
                description: $"Game frames the long exposure adds for each second of shutter time: {string.Join(", ", LongExposurePlan.Smoothness)}. More make smoother trails and take longer.");
            measured = category.CreateEntry("LongExposureFramesPerSecond", 0.0,
                description: "Frames the last long exposure added each real second, for the time estimate. 0 before the first.");
            MelonPreferences.Save();
        }

        // After a long exposure: the frames the add-on added in seconds of real time, for the next estimate.
        public static void Recorded(uint frames, float seconds)
        {
            if (frames > 0 && seconds > 0f)
                Save(measured, Math.Round(frames / (double)seconds, 2));
        }

        private static void Save<T>(MelonPreferences_Entry<T> entry, T value)
        {
            entry.Value = value;
            MelonPreferences.Save();
        }
    }
}
