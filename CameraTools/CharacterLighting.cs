using CameraToolsEnhancements;
using Il2CppInterop.Runtime;
using MelonLoader;
using UnityEngine;

namespace CameraTools
{
    // Studio lighting: CameraTools' lights shape the characters like real lamps through the Enhancements add-on's character
    // light pass, and the game's own light on them dims to Game light. The face shades as a rounded head around the middle of
    // Bip001 Head and Bip001 HeadNub.
    internal static class CharacterLighting
    {
        public const float DefaultGameLight = 0.1f, DefaultRim = 1.5f, MaxRim = 3f;
        public static readonly string[] ShadingNames = { "Toon", "Soft" };

        private static MelonPreferences_Entry<bool> enabled, soft;
        private static MelonPreferences_Entry<float> gameLight, rim;
        private static bool sent;
        private static IntPtr headOf;
        private static Transform head, headNub;

        public static void Load()
        {
            var category = MelonPreferences.CreateCategory("CameraToolsLights");
            enabled = category.CreateEntry("StudioLighting", false,
                description: "Light characters with CameraTools' lights like real lamps, through CameraToolsEnhancements.addon64.");
            soft = category.CreateEntry("StudioLightingSoft", false, description: "Studio lighting's shading: false for toon edges, true for a lamp's soft falloff.");
            gameLight = category.CreateEntry("StudioLightingGameLight", DefaultGameLight,
                description: "The share of the game's own light studio lighting keeps on characters, from 0 to 1.");
            rim = category.CreateEntry("StudioLightingRim", DefaultRim, description: $"How strongly a light behind a character glows on its edges, from 0 to {MaxRim:0}.");
        }

        public static bool Enabled
        {
            get => enabled.Value;
            set => Save(enabled, value);
        }

        public static bool Soft
        {
            get => soft.Value;
            set => Save(soft, value);
        }

        public static float GameLight
        {
            get => Math.Clamp(float.IsFinite(gameLight.Value) ? gameLight.Value : DefaultGameLight, 0f, 1f);
            set => Save(gameLight, value);
        }

        public static float Rim
        {
            get => Math.Clamp(float.IsFinite(rim.Value) ? rim.Value : DefaultRim, 0f, MaxRim);
            set => Save(rim, value);
        }

        public static bool Active => Enabled && Enhancements.Connected;

        // After the lights are placed.
        public static void Update()
        {
            if (!Active)
            {
                if (sent)
                    Enhancements.SetCharacterLights(false, false, 0, 0, null, null);
                sent = false;
                return;
            }
            var lights = Lights.CharacterLights().Take(EnhancementsShotLight.Max).Select(light => EnhancementsShot.ToShotLight(light, 0)).ToList();
            Enhancements.SetCharacterLights(true, Soft, GameLight, Rim, HeadCenter(), lights);
            sent = true;
        }

        private static Vector3? HeadCenter()
        {
            var avatar = Character.Active();
            if (!avatar)
                return null;
            if (headOf != avatar.Pointer || !head || !headNub)
            {
                headOf = avatar.Pointer;
                head = headNub = null;
                foreach (var found in avatar.GetComponentsInChildren(Il2CppType.Of<Transform>(), true))
                {
                    var t = found.Cast<Transform>();
                    if (t.name == "Bip001 Head")
                        head = t;
                    else if (t.name == "Bip001 HeadNub")
                        headNub = t;
                }
            }
            return head && headNub ? (head.position + headNub.position) * 0.5f : null;
        }

        private static void Save<T>(MelonPreferences_Entry<T> entry, T value)
        {
            entry.Value = value;
            MelonPreferences.Save();
        }
    }
}
