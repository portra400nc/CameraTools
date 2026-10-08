using System.Globalization;

namespace CameraTools
{
    // The frame guide's arithmetic. It uses nothing of Unity's, so it can be checked outside the game.
    internal static class CropFrame
    {
        public const float MinRatio = 0.2f;
        public const float MaxRatio = 5f;

        // Reads "7:5" or "2.4" as width over height, and gives it back as "7:5" or "2.4:1".
        public static bool TryParse(string text, out float ratio, out string label)
        {
            ratio = 0f;
            label = null;
            var parts = text.Split(':');
            float height = 1f;
            if (parts.Length > 2 || !Number(parts[0], out float width) || parts.Length == 2 && !Number(parts[1], out height) || height <= 0f)
                return false;
            ratio = width / height;
            if (ratio is < MinRatio or > MaxRatio)
                return false;
            label = parts.Length == 2 ? text : text + ":1";
            return true;
        }

        // "7:5" on its side is "5:7".
        public static string Turned(string label)
        {
            var parts = label.Split(':');
            return parts.Length == 2 ? $"{parts[1]}:{parts[0]}" : label;
        }

        // The largest box of the ratio that fits the screen, in its middle; screenAspect is the screen's width over its height.
        public static ScreenBox Box(float ratio, float screenAspect)
        {
            float width = Math.Min(1f, ratio / screenAspect), height = Math.Min(1f, screenAspect / ratio);
            return new ScreenBox((1f - width) / 2f, (1f - height) / 2f, (1f + width) / 2f, (1f + height) / 2f);
        }

        private static bool Number(string text, out float value)
            => float.TryParse(text, NumberStyles.AllowDecimalPoint, CultureInfo.InvariantCulture, out value);
    }
}
