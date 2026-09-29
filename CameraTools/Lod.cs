using Il2CppInterop.Runtime;
using UnityEngine;

namespace CameraTools
{
    // Genshin's LOD system samples its camera mid-frame, while the game's own pose is still on the main camera, so
    // detail follows the gameplay camera. A disabled proxy camera that never renders carries the free camera's pose.
    internal static class Lod
    {
        private const int SampleSize = 3;

        private static Camera proxy;
        private static Transform proxyTransform;
        private static readonly Dictionary<IntPtr, MiHoYoLodLoader> paused = new();
        private static List<MiHoYoLodLoader> sample;
        private static bool sampled;
        private static float nextScan;

        public static bool MaxDetail { get; private set; }

        public static void Attach(Camera main)
        {
            if (!proxy)
            {
                var owner = new GameObject("CameraTools LOD");
                UnityEngine.Object.DontDestroyOnLoad(owner);
                proxy = owner.AddComponent<Camera>();
                proxy.enabled = false;
                proxyTransform = owner.transform;
            }
            proxy.nearClipPlane = main.nearClipPlane;
            proxy.farClipPlane = main.farClipPlane;
            proxy.fieldOfView = main.fieldOfView;
            proxy.aspect = main.aspect;

            var unityMain = Camera.main;
            CameraTools.LogOnce(unityMain == main
                ? "LOD: Camera.main is the main camera CameraTools found."
                : $"LOD: Camera.main is {(unityMain ? unityMain.name : "null")}, not the main camera CameraTools found.");
        }

        public static void Sync(Vector3 position, Quaternion rotation, float fieldOfView)
        {
            proxyTransform.SetPositionAndRotation(position, rotation);
            proxy.fieldOfView = fieldOfView;
        }

        public static void Follow(bool freecam)
        {
            if (freecam)
            {
                try
                {
                    MiHoYoLodLoader.SetLodUseCustomMainCamera(proxy);
                    CameraTools.LogOnce("LOD: SetLodUseCustomMainCamera(proxy) succeeded; LOD follows the free camera.");
                }
                catch (Exception e)
                {
                    CameraTools.LogOnce($"LOD: SetLodUseCustomMainCamera(proxy) failed: {e.Message}");
                }
                return;
            }
            try
            {
                MiHoYoLodLoader.SetLodUseCustomMainCamera(null);
                CameraTools.LogOnce("LOD: SetLodUseCustomMainCamera(null) succeeded; LOD follows the game camera.");
            }
            catch (Exception e)
            {
                CameraTools.LogOnce($"LOD: SetLodUseCustomMainCamera(null) failed ({e.Message}); passing the main camera instead.");
                try
                {
                    MiHoYoLodLoader.SetLodUseCustomMainCamera(CameraTools.maincam);
                }
                catch (Exception fallback)
                {
                    CameraTools.LogOnce($"LOD: SetLodUseCustomMainCamera(main camera) failed: {fallback.Message}");
                }
            }
        }

        public static void SetMaxDetail(bool on)
        {
            MaxDetail = on;
            if (on)
            {
                nextScan = 0;
                return;
            }
            foreach (var loader in paused.Values)
                if (loader)
                    loader.ResumeLodLoader();
            paused.Clear();
        }

        // Newly streamed-in objects bring their own loaders, so max detail rescans every two seconds.
        public static void Update()
        {
            if (!MaxDetail || Time.unscaledTime < nextScan)
                return;
            nextScan = Time.unscaledTime + 2f;

            foreach (var dead in paused.Where(entry => !entry.Value).Select(entry => entry.Key).ToList())
                paused.Remove(dead);
            if (sample != null)
            {
                CameraTools.LogOnce($"Max detail: 2 s later: {Describe(sample, false)}.");
                sample = null;
            }

            var loaders = UnityEngine.Object.FindObjectsOfType(Il2CppType.Of<MiHoYoLodLoader>())
                .Select(found => found.TryCast<MiHoYoLodLoader>())
                .Where(loader => loader != null)
                .ToList();
            bool first = !sampled;
            string before = first ? Describe(loaders.Take(SampleSize), true) : null;
            foreach (var loader in loaders)
                if (paused.TryAdd(loader.Pointer, loader))
                    loader.PauseLodLoaderWithSpecificLodLevel(0);
            if (first)
            {
                sampled = true;
                sample = loaders.Take(SampleSize).ToList();
                CameraTools.LogOnce($"Max detail: paused {loaders.Count} LOD loaders at level 0. Before (level of count): {before}. "
                    + $"Right after: {Describe(sample, false)}.");
            }
        }

        private static string Describe(IEnumerable<MiHoYoLodLoader> loaders, bool withCount)
            => string.Join("; ", loaders.Where(loader => loader).Select(loader =>
                $"{loader.name} level {loader.currentLodLevel}{(withCount ? $" of {loader.GetLodLevelCount()}" : "")}"));
    }
}
