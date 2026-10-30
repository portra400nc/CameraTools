using System.Globalization;

namespace CameraTools
{
    // The long exposure's decisions, apart from the game so tests run them.
    internal static class LongExposurePlan
    {
        public static readonly int[] Smoothness = { 60, 120, 240, 480 };
        public static readonly string[] SmoothnessLabels = Smoothness.Select(frames => $"{frames} / s").ToArray();

        // Past this many seconds of recording the estimate warns.
        private const float LongWait = 600f;

        // The game frames a long exposure adds: one per 1/framesPerSecond of game time, at least one.
        public static int Frames(float seconds, int framesPerSecond)
            => Math.Max(1, (int)Math.Round((double)seconds * framesPerSecond, MidpointRounding.AwayFromZero));

        // The Smoothness row's line, such as "960 frames · about 3 min", from the frames the last long exposure added
        // each real second, or the frame count alone before the first. Warn: the wait is past ten minutes.
        public static (string Text, bool Warn) Estimate(int frames, double measuredPerSecond)
        {
            string count = $"{frames.ToString("N0", CultureInfo.InvariantCulture)} frames";
            if (!(measuredPerSecond > 0) || double.IsInfinity(measuredPerSecond))
                return (count, false);
            double seconds = frames / measuredPerSecond;
            string time = seconds < 90 ? $"about {Round(seconds)} s" : $"about {Round(seconds / 60)} min";
            return ($"{count} · {time}", seconds > LongWait);
        }

        public static int NearestSmoothness(int framesPerSecond)
            => Array.IndexOf(Smoothness, Smoothness.MinBy(choice => Math.Abs(choice - framesPerSecond)));

        private static long Round(double value) => (long)Math.Round(value, MidpointRounding.AwayFromZero);
    }
}
