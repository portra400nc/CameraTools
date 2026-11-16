using UnityEngine;

namespace CameraTools
{
    // Experimental features > Weather at run time: the game's weather, and the look the sky blends to as it changes, in three
    // seconds of real time, so a weather picked while the game is paused for a shot still comes in.
    internal static class WeatherFollow
    {
        private const float BlendSeconds = 3f;

        private static WeatherKind target = WeatherKind.Sunny;
        private static WeatherLook from;
        private static float share = 1f;
        private static bool following;

        // The look the sky shows now, or null while Weather is off or the physical sky does not draw.
        public static WeatherLook Current { get; private set; }

        // Whether Current changed this frame, so the add-on is to get the settings again.
        public static bool Changed { get; private set; }

        // The share of the sun's and the moon's direct light the cover lets through.
        public static float Sun => Current?.Sun ?? 1f;

        // The Now line: the game's weather, and how far its look has blended in while it does.
        public static string Now => !following ? "Off" : share >= 1f ? WeatherLooks.Name(target) : $"{WeatherLooks.Name(target)}, {share * 100f:0}% in";

        // Once a frame, before the settings go to the add-on: on is whether Weather runs with the physical sky.
        public static void Update(bool on, EnhancementValues values)
        {
            var before = Current;
            following = on;
            if (!on)
            {
                Current = null;
                from = null;
                share = 1f;
                Changed = before != null;
                return;
            }
            string preset = Weather.RenderedPreset();
            var now = WeatherLooks.OfPreset(preset);
            if (Current == null || now != target)
                MelonLoader.Melon<CameraTools>.Logger.Msg($"Weather: the sky shows {preset ?? "no preset"}, which looks {WeatherLooks.Name(now)}.");
            if (Current == null)
            {
                // Switched on: the weather's look at once.
                target = now;
                from = null;
                share = 1f;
            }
            else if (now != target)
            {
                from = Current;
                target = now;
                share = 0f;
            }
            if (share < 1f)
                share = Math.Min(share + Time.unscaledDeltaTime / BlendSeconds, 1f);
            var look = WeatherLooks.Look(target, values);
            Current = from != null && share < 1f ? WeatherLooks.Between(from, look, share) : look;
            Changed = Current != before;
        }
    }
}
