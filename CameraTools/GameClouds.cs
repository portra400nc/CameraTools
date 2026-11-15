using MelonLoader;
using UnityEngine;

namespace CameraTools
{
    // The game's own clouds, the renderers under EnviroSystem whose names hold "Cloud" (the painted layer and the three
    // emitters' sprites), hidden while the Enhancements add-on draws CameraTools' clouds or the physical sky, and back once that stops.
    internal static class GameClouds
    {
        private const string SkyPath = "/StageRoot/EnviroSystem(Clone)";
        // How often a missing sky is looked for again, as after a scene change.
        private const float LookEvery = 1f;

        private static readonly List<Renderer> held = new();
        private static float lookAt;

        // Once a frame: whether CameraTools' clouds or the physical sky run.
        public static void Update(bool ours)
        {
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
