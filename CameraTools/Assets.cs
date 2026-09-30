using Il2CppInterop.Runtime;
using UnityEngine;

namespace CameraTools
{
    // The one game asset CameraTools' UI uses: the game's font, which it loads at login. Arial is loaded then too, and is
    // taken only when the free camera needs the UI before Default_SC exists.
    internal static class Assets
    {
        private const float LookInterval = 1f;
        private const string GameFont = "Default_SC";
        private const string FallbackFont = "Arial";

        private static Font font;
        private static float nextLook;

        public static Font Font(bool needed)
        {
            if (font)
                return font;
            if (Time.unscaledTime < nextLook)
                return null;
            nextLook = Time.unscaledTime + LookInterval;
            Font fallback = null;
            foreach (var item in Resources.FindObjectsOfTypeAll(Il2CppType.Of<Font>()))
            {
                if (item.name == GameFont)
                {
                    font = item.TryCast<Font>();
                    break;
                }
                if (item.name == FallbackFont)
                    fallback = item.TryCast<Font>();
            }
            if (!font && needed && fallback)
                font = fallback;
            if (font)
                CameraTools.LogOnce($"UI: CameraTools writes with the game's {font.name} font.");
            return font;
        }
    }
}
