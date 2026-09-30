using Il2CppInterop.Runtime;
using UnityEngine;

namespace CameraTools
{
    // The game's sprites and fonts by name, for the photo mode layout and the controller glyphs. They are found by reading
    // the name of every loaded sprite, which takes about 200 ms on the Deck, so a scan runs only while something wanted is
    // missing: at most every 10 s, plus once when the free camera turns on and once when the game's photo mode page appears.
    // After login most photo mode sprites are not loaded yet; photo mode, the map, and teleporting load them. A resolution
    // change destroyed the tab icons' sprites on the Deck, so cached assets are checked every second, and a lost one is
    // looked up again one second after the change and then with the other missing assets.
    internal static class Assets
    {
        private const float ScanInterval = 10f;
        private const float PageLookInterval = 1f;
        private const float HealthInterval = 1f;
        private const float ResolutionSettle = 1f;
        private const string GlyphPrefix = "UI_KeyXbox_";

        // All but Start are confirmed on the Deck; Start follows the naming of Back, LB and RB.
        private static readonly (PadButtons Button, string Sprite)[] Glyphs =
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

        private static readonly Dictionary<string, Sprite> sprites = new(StringComparer.Ordinal);
        private static readonly Dictionary<string, Font> fonts = new(StringComparer.Ordinal);
        // Glyphs a scan that saw Xbox sprites did not find. The game loads each glyph when some prompt first shows it (Start
        // loaded later than the rest), so they are looked for again on the next ScanSoon, not every 10 s.
        private static readonly HashSet<PadButtons> absent = new();
        private static float nextHealth;
        private static (int Width, int Height) screen;
        private static HashSet<string> wanted;
        private static string[] missing;
        private static float nextScan;
        private static float nextPageLook;
        private static bool pageSeen;
        private static bool pageScanDue;
        private static bool scanLogged;

        // Changes whenever a sprite is found or lost, so a view can tell when to show glyphs again.
        public static int Version { get; private set; }

        // Every sprite and font the layout needs was found and has not been seen unloaded since.
        public static bool LayoutReady => missing is { Length: 0 };

        public static Sprite Glyph(PadButtons button)
        {
            int index = Array.FindIndex(Glyphs, glyph => glyph.Button == button);
            return index < 0 ? null : Sprite(Glyphs[index].Sprite);
        }

        public static Sprite Sprite(string name)
        {
            if (!sprites.TryGetValue(name, out var sprite))
                return null;
            if (sprite)
                return sprite;
            sprites.Remove(name);
            Lost(name);
            return null;
        }

        public static Font Font(string name)
        {
            if (!fonts.TryGetValue(name, out var font))
                return null;
            if (font)
                return font;
            fonts.Remove(name);
            Lost(name);
            return null;
        }

        public static void ScanSoon()
        {
            nextScan = 0f;
            absent.Clear();
        }

        // layout: the free camera is on. glyphs: controller hints are showing.
        public static void Update(bool layout, bool glyphs)
        {
            float time = Time.unscaledTime;
            if (time >= nextHealth)
            {
                nextHealth = time + HealthInterval;
                CheckHealth();
            }
            var size = (Screen.width, Screen.height);
            if (size != screen)
            {
                if (screen != default)
                {
                    nextScan = time + ResolutionSettle;
                    absent.Clear();
                }
                screen = size;
            }
            bool layoutMissing = !LayoutReady;
            bool glyphsMissing = Glyphs.Any(glyph => !sprites.ContainsKey(glyph.Sprite) && !absent.Contains(glyph.Button));
            if (!layoutMissing && !glyphsMissing)
                return;
            float now = Time.unscaledTime;
            if (now >= nextPageLook)
            {
                nextPageLook = now + PageLookInterval;
                // Transform.Find, unlike GameObject.Find, also finds the page while photo mode is closed.
                var pages = GameObject.Find("/Canvas/Pages");
                bool page = pages && pages.transform.Find("InLevelPhotographContext");
                pageScanDue |= page && !pageSeen;
                pageSeen = page;
            }
            if (pageScanDue || now >= nextScan && (layout && layoutMissing || glyphs && glyphsMissing))
                Scan();
        }

        private static void CheckHealth()
        {
            foreach (var name in sprites.Where(entry => !entry.Value).Select(entry => entry.Key).ToList())
                Sprite(name);
            foreach (var name in fonts.Where(entry => !entry.Value).Select(entry => entry.Key).ToList())
                Font(name);
        }

        private static void Lost(string name)
        {
            CameraTools.LogOnce($"UI: the game unloaded {name} at {Time.unscaledTime:0} s (screen {Screen.width}x{Screen.height}); "
                + "CameraTools looks for it again.");
            Version++;
            if (name.StartsWith(GlyphPrefix, StringComparison.Ordinal))
                absent.Clear();
            missing = Missing();
        }

        private static void Scan()
        {
            pageScanDue = false;
            nextScan = Time.unscaledTime + ScanInterval;
            var layout = PhotoLayout.Current;
            wanted ??= layout.RequiredSprites.Concat(layout.OptionalSprites).Concat(Glyphs.Select(glyph => glyph.Sprite))
                .ToHashSet(StringComparer.Ordinal);
            float started = Time.realtimeSinceStartup;
            var found = Resources.FindObjectsOfTypeAll(Il2CppType.Of<Sprite>());
            var xbox = new SortedSet<string>(StringComparer.Ordinal);
            foreach (var item in found)
            {
                string name = item.name;
                if (name.StartsWith(GlyphPrefix, StringComparison.Ordinal))
                    xbox.Add(name);
                if (wanted.Contains(name) && !Sprite(name))
                {
                    sprites[name] = item.TryCast<Sprite>();
                    Version++;
                }
            }
            var loadedFonts = Resources.FindObjectsOfTypeAll(Il2CppType.Of<Font>());
            foreach (var item in loadedFonts)
            {
                string name = item.name;
                if (layout.Fonts.Contains(name) && !Font(name))
                    fonts[name] = item.TryCast<Font>();
            }
            if (!scanLogged)
            {
                scanLogged = true;
                CameraTools.LogOnce($"UI: an asset scan read {found.Length} sprites and {loadedFonts.Length} fonts in "
                    + $"{(Time.realtimeSinceStartup - started) * 1000f:0} ms.");
            }
            if (xbox.Count > 0)
            {
                CameraTools.LogOnce($"UI: loaded Xbox glyph sprites: {string.Join(", ", xbox)}.");
                foreach (var (button, sprite) in Glyphs)
                    if (!sprites.ContainsKey(sprite))
                        absent.Add(button);
            }
            missing = Missing();
            int needed = layout.RequiredSprites.Length + layout.Fonts.Length;
            CameraTools.LogOnce($"UI: {needed - missing.Length} of the {needed} sprites and fonts the layout needs are loaded"
                + (missing.Length > 0 ? $"; missing {string.Join(", ", missing)}." : "."));
        }

        private static string[] Missing()
        {
            var layout = PhotoLayout.Current;
            return layout.RequiredSprites.Where(name => !sprites.ContainsKey(name))
                .Concat(layout.Fonts.Where(name => !fonts.ContainsKey(name))).ToArray();
        }
    }
}
