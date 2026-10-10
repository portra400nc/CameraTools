using CameraToolsPhotoreal;
using MoleMole;
using UnityEngine;

namespace CameraTools
{
    // Tells the Photoreal add-on which camera renders, since its passes rebuild view space from it.
    internal static class PhotorealCamera
    {
        // Real seconds the weather's wetness takes to go from dry to soaked or back.
        private const float WetnessSeconds = 5;

        private static float wetness;

        // Camera.onPreCull of the main camera, after the free camera applied its pose. The lens sample is zero: the frame
        // renders from the camera's own position.
        public static void Push(Camera camera)
        {
            if (!Photoreal.Connected)
                return;
            var sky = EnviroSky.Instance;
            bool raining = Weather.Raining ?? (sky && sky.isWet);
            wetness = Mathf.MoveTowards(wetness, raining ? 1 : 0, Time.unscaledDeltaTime / WetnessSeconds);
            // The sun light shines along its forward axis, so the sun lies the other way.
            var sun = sky ? sky.MainLight : null;
            Vector3 towardSun = sun ? -sun.transform.forward : Vector3.zero;
            Color sunColor = sun ? sun.color.linear * sun.intensity : Color.black;
            Photoreal.SetCamera(camera.worldToCameraMatrix, GL.GetGPUProjectionMatrix(camera.nonJitteredProjectionMatrix, false), towardSun,
                sunColor, RenderSettings.ambientSkyColor.linear, default, wetness);
        }
    }
}
