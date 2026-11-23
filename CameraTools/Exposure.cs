namespace CameraTools
{
    // A long exposure's shutter time and ISO.
    internal readonly record struct Exposure(float Seconds, int Iso)
    {
        // A camera's half stops from 1/250 s to 1/8 s, then its full stops, as the depth of field's apertures are.
        public static readonly float[] Shutters =
        {
            1f / 250, 1f / 180, 1f / 125, 1f / 90, 1f / 60, 1f / 45, 1f / 30, 1f / 20, 1f / 15, 1f / 10,
            0.125f, 0.25f, 0.5f, 1f, 2f, 4f, 8f, 15f, 30f, 60f, 120f,
        };
        public static readonly string[] ShutterLabels = Shutters.Select(Label).ToArray();
        public static readonly int[] Isos = { 100, 200, 400, 800, 1600 };
        public static readonly string[] IsoLabels = Isos.Select(iso => iso.ToString()).ToArray();

        public string ShutterLabel => Label(Seconds);

        public static int NearestShutter(float seconds)
        {
            int best = 0;
            for (int index = 1; index < Shutters.Length; index++)
                if (MathF.Abs(MathF.Log(Math.Max(seconds, 0.001f) / Shutters[index])) < MathF.Abs(MathF.Log(Math.Max(seconds, 0.001f) / Shutters[best])))
                    best = index;
            return best;
        }

        private static string Label(float seconds) => seconds < 1f ? $"1/{MathF.Round(1f / seconds):0} s" : $"{seconds:0} s";
    }
}
