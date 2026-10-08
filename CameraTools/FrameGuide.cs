using MelonLoader;

namespace CameraTools
{
    // The Camera tab's frame guide: where a crop to another aspect ratio will cut the picture. CameraTools' UI draws it, so
    // Hide UI and the screenshot button keep it out of shots.
    internal static class FrameGuide
    {
        // Narrowest first; Custom takes the typed ratio.
        private static readonly (string Label, float Ratio)[] Presets =
        {
            ("1:1", 1f), ("5:4", 5f / 4f), ("4:3", 4f / 3f), ("7:5", 7f / 5f), ("3:2", 3f / 2f), ("16:9", 16f / 9f),
            ("1.85:1", 1.85f), ("2:1", 2f), ("21:9", 21f / 9f), ("2.39:1", 2.39f), ("Custom", 0f),
        };

        private const string DefaultRatio = "7:5";
        private const string DefaultCustom = "2.4:1";
        private const int Square = 0;
        private static readonly int Custom = Presets.Length - 1;

        public static readonly string[] Labels = Presets.Select(preset => preset.Label).ToArray();

        private static MelonPreferences_Entry<bool> shown;
        private static MelonPreferences_Entry<string> ratio;
        private static MelonPreferences_Entry<string> custom;
        private static MelonPreferences_Entry<bool> portrait;
        private static MelonPreferences_Entry<float> shade;
        private static MelonPreferences_Entry<bool> thirds;

        public static bool Shown => shown.Value;
        public static int Choice => Array.IndexOf(Labels, ratio.Value);
        public static string CustomRatio => custom.Value;
        public static bool Portrait => portrait.Value;
        public static float Shade => Math.Clamp(shade.Value, 0f, 1f);
        public static bool Thirds => thirds.Value;

        public static bool CustomChosen => Shown && Choice == Custom;
        public static bool Turnable => Shown && Choice != Square;

        public static string ShadeNote => Shade <= 0f ? "Lines only" : Shade >= 1f ? "Black, as the crop will look" : "Shades what the crop cuts off";

        // The frame's width over its height and its name, on its side in portrait, or null while the guide is off.
        public static (float Ratio, string Label)? Frame
        {
            get
            {
                if (!Shown)
                    return null;
                var (label, value) = Presets[Choice];
                if (Choice == Custom)
                    CropFrame.TryParse(custom.Value, out value, out label);
                return Portrait && value != 1f ? (1f / value, CropFrame.Turned(label)) : (value, label);
            }
        }

        public static void Load()
        {
            var category = MelonPreferences.CreateCategory("CameraToolsFrameGuide");
            shown = category.CreateEntry("Shown", false, description: "Draw the frame guide while the free camera is on.");
            ratio = category.CreateEntry("AspectRatio", DefaultRatio,
                description: $"The crop the frame guide shows: {string.Join(", ", Labels)}. Custom uses CustomRatio.");
            custom = category.CreateEntry("CustomRatio", DefaultCustom,
                description: $"Width:height, such as 7:5, or one number, such as 2.4, from {CropFrame.MinRatio} to {CropFrame.MaxRatio}.");
            portrait = category.CreateEntry("Portrait", false, description: "Turn the frame on its side, so 7:5 becomes 5:7.");
            shade = category.CreateEntry("OutsideTheFrame", 0.6f,
                description: "How dark the guide makes what the crop cuts off, from 0 (lines only) to 1 (black).");
            thirds = category.CreateEntry("RuleOfThirds", false, description: "Draw a rule of thirds grid inside the frame.");
            if (Choice < 0)
            {
                Melon<CameraTools>.Logger.Warning($"CameraToolsFrameGuide AspectRatio = \"{ratio.Value}\" is not one of the choices; using {DefaultRatio}.");
                ratio.Value = DefaultRatio;
            }
            if (!CropFrame.TryParse(custom.Value, out _, out _))
            {
                Melon<CameraTools>.Logger.Warning($"CameraToolsFrameGuide CustomRatio = \"{custom.Value}\" is not a ratio; using {DefaultCustom}.");
                custom.Value = DefaultCustom;
            }
            MelonPreferences.Save();
        }

        public static void SetShown(bool on) => Save(shown, on);

        public static void SetChoice(int index) => Save(ratio, Labels[index]);

        public static void SetCustom(string text)
        {
            if (!CropFrame.TryParse(text, out _, out string label))
            {
                CameraUi.Toast($"Type a ratio like 7:5 or 2.4, from {CropFrame.MinRatio} to {CropFrame.MaxRatio}");
                return;
            }
            Save(custom, label);
            CameraUi.Toast($"Frame guide {label}");
        }

        public static void SetPortrait(bool on) => Save(portrait, on);

        public static void SetShade(float value) => Save(shade, Math.Clamp(value, 0f, 1f));

        public static void SetThirds(bool on) => Save(thirds, on);

        private static void Save<T>(MelonPreferences_Entry<T> entry, T value)
        {
            if (EqualityComparer<T>.Default.Equals(entry.Value, value))
                return;
            entry.Value = value;
            MelonPreferences.Save();
        }
    }
}
