using MoleMole;
using UnityEngine;

namespace CameraTools
{
    // Genshin shades its characters with their own copy of the sun: EnviroSky turns AvatarLight (AMJHLALKIKH) by
    // Euler(PAEKODDDLFI), a fixed 35 degree pitch with the sun's yaw, and the toon shader reads the direction toward it from the
    // global CharacterLight.lightDirShaderId and its colour from characterMainLightColorId. While a light has Alternative
    // shading, CameraTools aims all of them from that light at the character, so the face shadow and the shadow edge follow it.
    internal static class CharacterSun
    {
        // Where the light aims on the character, above the feet.
        private const float AimHeight = 1.1f;

        private static EnviroSky held;
        // The angles EnviroSky had before CameraTools took over; with the flag off again the game rewrites only the yaw.
        private static Vector3 heldAngles;
        private static Vector3? toward;
        private static Color tint;

        // After the lights are placed: from is the light's position this frame, or null to give the shading back to the game.
        public static void Update(Vector3? from, Color color)
        {
            var sky = EnviroSky.Instance;
            var avatar = Character.Active();
            if (from is not { } at || !sky || !avatar)
            {
                Release();
                return;
            }
            var shine = avatar.position + Vector3.up * AimHeight - at;
            if (shine.sqrMagnitude < 1e-4f)
                return;
            if (held == null || held.Pointer != sky.Pointer)
            {
                Release();
                held = sky;
                heldAngles = sky.PAEKODDDLFI;
            }
            var rotation = Quaternion.LookRotation(shine);
            var euler = rotation.eulerAngles;
            sky.rewriteUpdateCharacterLightDir = true;
            sky.PAEKODDDLFI = new Vector3(euler.x, euler.y, 0f);
            if (sky.AMJHLALKIKH)
                sky.AMJHLALKIKH.rotation = rotation;
            toward = -shine.normalized;
            tint = color;
        }

        // Camera.onPreCull of the main camera: the game writes the globals every frame, so they are written again just before drawing.
        public static void PreCull()
        {
            if (toward is not { } direction || held == null || !held)
                return;
            try
            {
                int id = CharacterLight.lightDirShaderId;
                float w = Shader.GetGlobalVector(id).w;
                Shader.SetGlobalVector(id, new Vector4(direction.x, direction.y, direction.z, w));
                Shader.SetGlobalColor(held.characterMainLightColorId, tint);
            }
            catch (Exception e)
            {
                CameraTools.LogOnce($"Lights: shading characters from a light failed: {e}");
            }
        }

        private static void Release()
        {
            if (held != null && held)
            {
                held.rewriteUpdateCharacterLightDir = false;
                held.PAEKODDDLFI = heldAngles;
            }
            held = null;
            toward = null;
        }
    }
}
