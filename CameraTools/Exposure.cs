namespace CameraTools
{
    // The shader's settings for one shot, and the uniform values that carry them. CameraTools keeps them in its preferences:
    // ReShade saves the uniforms only of effects with a technique on, and the screenshot's resize reloads the effects.
    internal readonly record struct Exposure(float Seconds, int Iso, float Boost)
    {
        // A camera's full stops, as the depth of field's apertures are. The shader takes 0.1 s to 120 s.
        public static readonly float[] Shutters = { 0.125f, 0.25f, 0.5f, 1f, 2f, 4f, 8f, 15f, 30f, 60f, 120f };
        public static readonly string[] ShutterLabels = Shutters.Select(Label).ToArray();
        public static readonly int[] Isos = { 100, 200, 400, 800, 1600 };
        public static readonly string[] IsoLabels = Isos.Select(iso => iso.ToString()).ToArray();
        public const float MinBoost = 0.4f;
        public const float MaxBoost = 4.4f;

        public string ShutterLabel => Label(Seconds);

        // Every uniform the shot sets, with open the shutter's switch. Whole marks a bool or a uint, which the bridge sets as
        // an int. The shader's progress ring and green dot draw on the picture, so they stay off.
        public (string Name, float Value, bool Whole)[] Uniforms(bool open) => new[]
        {
            ("UI_ExposureDuration", Seconds, false),
            ("UI_ISO", (float)Iso, false),
            ("UI_Gamma", Boost, false),
            ("UI_Delay", 0f, true),
            ("UI_ShowProgress", 0f, true),
            ("UI_ShowGreenOnFinish", 0f, true),
            ("UI_StartExposure", open ? 1f : 0f, true),
        };

        public static int NearestShutter(float seconds)
        {
            int best = 0;
            for (int index = 1; index < Shutters.Length; index++)
                if (MathF.Abs(MathF.Log(Math.Max(seconds, 0.01f) / Shutters[index])) < MathF.Abs(MathF.Log(Math.Max(seconds, 0.01f) / Shutters[best])))
                    best = index;
            return best;
        }

        private static string Label(float seconds) => seconds < 1f ? $"1/{MathF.Round(1f / seconds):0} s" : $"{seconds:0} s";
    }
}
