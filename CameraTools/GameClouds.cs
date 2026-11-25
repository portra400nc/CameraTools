using MelonLoader;
using UnityEngine;

namespace CameraTools
{
    // The game's own clouds, the renderers under EnviroSystem whose names hold "Cloud" (the painted layer and the three
    // emitters' sprites), hidden while the Enhancements add-on draws CameraTools' clouds or the physical sky, and back once that stops.
    // So are the clouds the world places as effects, low cloud planes and puffs such as Eff_Stages_Cloud_Soft_02 and
    // Eff_Stages_Dq_Lidao_Cloud_04's CloudBall: lit by the game's sky colors, they stood as pink smoke under the physical
    // sky. The far cloud impostors stay. So is the wind's drifting dust, EffectPool's Eff_Wind_Dust, which stood as a patch
    // of light smoke on the ground.
    internal static class GameClouds
    {
        private const string SkyPath = "/StageRoot/EnviroSystem(Clone)";
        private const string WorldPath = "/BigWorld(Clone)";
        private const string DustPath = "/EffectPool/Eff_Wind_Dust";
        // How often a missing sky is looked for again, as after a scene change.
        private const float LookEvery = 1f;
        // How often the world's streamed-in chunks are looked through for new cloud effects.
        private const float WorldEvery = 2f;

        private static readonly List<Renderer> held = new();
        private static readonly List<Renderer> world = new();
        private static readonly HashSet<IntPtr> seenEffects = new();
        private static float lookAt, worldAt;

        private static bool WorldCloud(string name) =>
            name.StartsWith("Eff_Stages_", StringComparison.Ordinal) && name.Contains("Cloud", StringComparison.Ordinal) && !name.Contains("CloudImpostor", StringComparison.Ordinal);

        // The world's cloud effects, under each chunk's RenderRoot: hidden and kept, those already looked at passed over.
        private static void HideWorldClouds()
        {
            world.RemoveAll(renderer => renderer == null);
            foreach (var renderer in world)
                if (renderer.enabled)
                    renderer.enabled = false;
            if (Time.unscaledTime < worldAt)
                return;
            worldAt = Time.unscaledTime + WorldEvery;
            int before = world.Count;
            var dust = GameObject.Find(DustPath);
            if (dust != null && seenEffects.Add(dust.Pointer))
                foreach (var renderer in dust.GetComponentsInChildren<Renderer>(true))
                {
                    renderer.enabled = false;
                    world.Add(renderer);
                }
            var root = GameObject.Find(WorldPath);
            if (root == null)
                return;
            var chunks = root.transform;
            for (int c = 0; c < chunks.childCount; c++)
            {
                var render = chunks.GetChild(c).Find("RenderRoot");
                if (render == null)
                    continue;
                for (int e = 0; e < render.childCount; e++)
                {
                    var effect = render.GetChild(e);
                    if (!seenEffects.Add(effect.Pointer) || !WorldCloud(effect.name))
                        continue;
                    foreach (var renderer in effect.GetComponentsInChildren<Renderer>(true))
                    {
                        renderer.enabled = false;
                        world.Add(renderer);
                    }
                }
            }
            if (world.Count > before)
                Melon<CameraTools>.Logger.Msg($"Enhancements: hid {world.Count - before} more of the world's cloud effect renderers, {world.Count} in all.");
        }

        private static void ShowWorldClouds()
        {
            foreach (var renderer in world)
                if (renderer != null)
                    renderer.enabled = true;
            world.Clear();
            seenEffects.Clear();
            worldAt = 0f;
        }

        // Once a frame: whether CameraTools' clouds or the physical sky run, and whether the world's cloud effects and the
        // wind's dust go with the game's clouds (Hide game mist).
        public static void Update(bool ours, bool mist)
        {
            if (ours && mist)
                HideWorldClouds();
            else if (world.Count > 0 || seenEffects.Count > 0)
                ShowWorldClouds();
            if (!ours)
            {
                foreach (var renderer in held)
                    if (renderer != null)
                        renderer.enabled = true;
                held.Clear();
                lookAt = 0f;
                return;
            }
            held.RemoveAll(renderer => renderer == null);
            foreach (var renderer in held)
                if (renderer.enabled)
                    renderer.enabled = false;
            if (held.Count > 0 || Time.unscaledTime < lookAt)
                return;
            lookAt = Time.unscaledTime + LookEvery;
            var sky = GameObject.Find(SkyPath);
            if (sky == null)
                return;
            foreach (var renderer in sky.GetComponentsInChildren<Renderer>(true))
                if (renderer.gameObject.name.Contains("Cloud", StringComparison.OrdinalIgnoreCase))
                {
                    held.Add(renderer);
                    renderer.enabled = false;
                }
            Melon<CameraTools>.Logger.Msg($"Enhancements: hid {held.Count} of the game's cloud renderers while CameraTools' clouds or the physical sky run.");
        }
    }
}
