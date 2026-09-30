using Il2CppInterop.Runtime;
using UnityEngine;

namespace CameraTools
{
    public enum LodLevel { MostDetail, LeastDetail }

    // Genshin's LOD system samples its camera mid-frame, while the game's own pose is still on the main camera, so
    // detail follows the gameplay camera. A disabled proxy camera that never renders carries the free camera's pose.
    internal static class Lod
    {
        private const int SampleSize = 3;

        private static Camera proxy;
        private static Transform proxyTransform;
        private static readonly Dictionary<IntPtr, MiHoYoLodLoader> paused = new();
        private static readonly HashSet<LodLevel> sampled = new();
        private static List<MiHoYoLodLoader> sample;
        private static LodLevel sampleLevel;
        private static float nextScan;

        // The level every loader is held at, or null to leave LOD to the game. The World tab's Max detail switch and the
        // Graphics tab's Detail level both set it.
        public static LodLevel? Forced { get; private set; }

        public static bool MaxDetail => Forced == LodLevel.MostDetail;

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

        public static void SetMaxDetail(bool on) => Force(on ? LodLevel.MostDetail : null);

        public static void Force(LodLevel? level)
        {
            if (level == Forced)
                return;
            foreach (var loader in paused.Values)
                if (loader)
                    loader.ResumeLodLoader();
            paused.Clear();
            Forced = level;
            nextScan = 0;
        }

        // Newly streamed-in objects bring their own loaders, so a forced level rescans every two seconds.
        public static void Update()
        {
            if (Forced is not LodLevel level || Time.unscaledTime < nextScan)
                return;
            nextScan = Time.unscaledTime + 2f;

            foreach (var dead in paused.Where(entry => !entry.Value).Select(entry => entry.Key).ToList())
                paused.Remove(dead);
            if (sample != null)
            {
                CameraTools.LogOnce($"{Name(sampleLevel)}: 2 s later: {Describe(sample, false)}.");
                sample = null;
            }

            var loaders = UnityEngine.Object.FindObjectsOfType(Il2CppType.Of<MiHoYoLodLoader>())
                .Select(found => found.TryCast<MiHoYoLodLoader>())
                .Where(loader => loader != null)
                .ToList();
            bool first = !sampled.Contains(level);
            string before = first ? Describe(loaders.Take(SampleSize), true) : null;
            foreach (var loader in loaders)
                if ((level == LodLevel.MostDetail || !loader.name.StartsWith("Avatar_")) && paused.TryAdd(loader.Pointer, loader))
                    loader.PauseLodLoaderWithSpecificLodLevel(level == LodLevel.MostDetail ? 0 : LeastDetailLevel(loader));
            if (first)
            {
                sampled.Add(level);
                sample = loaders.Take(SampleSize).ToList();
                sampleLevel = level;
                CameraTools.LogOnce($"{Name(level)}: paused {paused.Count} of {loaders.Count} LOD loaders at {(level == LodLevel.MostDetail ? "level 0" : "their second-to-last level, characters excepted")}. "
                    + $"Before (level of count): {before}. Right after: {Describe(sample, false)}.");
            }
        }

        // Build 82 held loaders at their last level, and the player's character disappeared: a character's last level draws
        // nothing. The level before it is the least detailed one that draws, and characters keep the game's LOD.
        private static int LeastDetailLevel(MiHoYoLodLoader loader) => Math.Max(loader.GetLodLevelCount() - 2, 0);

        private static string Name(LodLevel level) => level == LodLevel.MostDetail ? "Max detail" : "Min detail";

        private static string Describe(IEnumerable<MiHoYoLodLoader> loaders, bool withCount)
            => string.Join("; ", loaders.Where(loader => loader).Select(loader =>
                $"{loader.name} level {loader.currentLodLevel}{(withCount ? $" of {loader.GetLodLevelCount()}" : "")}"));
    }
}
