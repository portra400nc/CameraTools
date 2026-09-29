using Il2CppInterop.Runtime;
using UnityEngine;

namespace CameraTools
{
    // Genshin loads its Xbox button sprites once it shows controller prompts, so they are looked up by name while any is
    // missing. All but Start are confirmed on the Deck; Start follows the naming of Back, LB and RB.
    internal static class Glyphs
    {
        private const float LookInterval = 2f;

        private static readonly (PadButtons Button, string Sprite)[] Names =
        {
            (PadButtons.A, "UI_KeyXbox_0"),
            (PadButtons.B, "UI_KeyXbox_1"),
            (PadButtons.X, "UI_KeyXbox_2"),
            (PadButtons.Y, "UI_KeyXbox_3"),
            (PadButtons.LB, "UI_KeyXbox_LB"),
            (PadButtons.RB, "UI_KeyXbox_RB"),
            (PadButtons.Back, "UI_KeyXbox_Back"),
            (PadButtons.Start, "UI_KeyXbox_Start"),
            (PadButtons.L3, "UI_KeyXbox_10"),
            (PadButtons.R3, "UI_KeyXbox_11"),
            (PadButtons.LT, "UI_KeyXbox_LT"),
            (PadButtons.RT, "UI_KeyXbox_RT"),
        };

        private static readonly Dictionary<PadButtons, Sprite> found = new();
        // Names a scan that saw Xbox sprites did not find. The sprites load together, so these do not exist and are not
        // looked for again until a found sprite is unloaded.
        private static readonly HashSet<PadButtons> absent = new();
        private static float nextLook;
        private static bool scanLogged;

        // Changes whenever a glyph is found or lost, so a view can tell when to show glyphs again.
        public static int Count => found.Count;

        public static Sprite Get(PadButtons button)
        {
            if (!found.TryGetValue(button, out var sprite))
                return null;
            if (sprite)
                return sprite;
            found.Remove(button);
            absent.Clear();
            return null;
        }

        public static void Update()
        {
            if (found.Count + absent.Count == Names.Length || Time.unscaledTime < nextLook)
                return;
            nextLook = Time.unscaledTime + LookInterval;
            float started = Time.realtimeSinceStartup;
            var sprites = Resources.FindObjectsOfTypeAll(Il2CppType.Of<Sprite>());
            var xbox = new SortedSet<string>(StringComparer.Ordinal);
            foreach (var sprite in sprites)
            {
                string name = sprite.name;
                if (!name.StartsWith("UI_KeyXbox_", StringComparison.Ordinal))
                    continue;
                xbox.Add(name);
                foreach (var (button, wanted) in Names)
                    if (wanted == name && !Get(button))
                        found[button] = sprite.TryCast<Sprite>();
            }
            if (!scanLogged)
            {
                scanLogged = true;
                CameraTools.LogOnce($"UI: looking up glyphs read {sprites.Length} sprites in {(Time.realtimeSinceStartup - started) * 1000f:0} ms.");
            }
            if (xbox.Count == 0)
                return;
            CameraTools.LogOnce($"UI: loaded Xbox glyph sprites: {string.Join(", ", xbox)}.");
            foreach (var (button, _) in Names)
                if (!found.ContainsKey(button))
                    absent.Add(button);
        }
    }
}
