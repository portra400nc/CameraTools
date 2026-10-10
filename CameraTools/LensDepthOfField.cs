using CameraToolsPhotoreal;
using MelonLoader;
using MoleMole;
using UnityEngine;
using static CameraTools.CameraTools;

namespace CameraTools
{
    // One screenshot's aperture: its radius and focus distance in metres, and how its samples are spread.
    internal readonly record struct LensPlan(float Radius, float Focus, int Count, int Blades, float Rotation);

    // Depth of field from lens samples, for the screenshot: the free camera moves through the aperture over many frames
    // and the Photoreal add-on averages them in HDR. This class holds the settings, the sample on screen, and the camera
    // push that tells the add-on which sample each frame renders. Screenshot runs the sequence.
    internal static class LensDepthOfField
    {
        private const int MaxSamples = 1024;
        private const int MaxBlades = 16;

        private static MelonPreferences_Entry<bool> enabled;
        private static MelonPreferences_Entry<int> samples;
        private static MelonPreferences_Entry<int> blades;
        private static MelonPreferences_Entry<float> bladeRotation;
        private static MelonPreferences_Entry<float> catEye;
        private static LensSample sample;

        // The width of the cat's eye's soft edge, in aperture radii.
        private const float CatEyeFalloff = 0.1f;

        public static bool Enabled => enabled.Value;

        public static float CatEye => Math.Clamp(catEye.Value, 0f, 1f);

        public static void Load()
        {
            var category = MelonPreferences.CreateCategory("CameraToolsReShade");
            enabled = category.CreateEntry("PhotorealDepthOfField", false,
                description: "Take screenshots with depth of field from lens samples averaged by the Photoreal add-on, in place of iMMERSE's depth of field shader. Needs CameraToolsPhotoreal.addon64.");
            samples = category.CreateEntry("LensSamples", 96,
                description: $"How many points of the aperture the screenshot averages, from 1 to {MaxSamples}. Each takes two frames.");
            blades = category.CreateEntry("LensBlades", 0,
                description: $"Aperture blades, from 3 to {MaxBlades}, for polygonal bokeh; 0 is a round aperture.");
            bladeRotation = category.CreateEntry("LensBladeRotation", 0f,
                description: "How far the bladed aperture is turned, in degrees.");
            catEye = category.CreateEntry("CatEye", 0f,
                description: "Cat's-eye bokeh toward the picture's edges, from 0 (round everywhere) to 1. Try 0.4 for a swirl.");
            MelonPreferences.Save();
        }

        // The settings at this moment, for a lens that focuses focus metres ahead at the free camera's field of view.
        public static LensPlan Plan(float focus, float fNumber)
        {
            int count = Math.Clamp(samples.Value, 1, MaxSamples);
            int sides = blades.Value >= 3 ? Math.Min(blades.Value, MaxBlades) : 0;
            return new LensPlan(LensSampler.ApertureRadius(freecam.Pose.Fov, cam.aspect, fNumber), focus, count, sides, bladeRotation.Value);
        }

        // Moves the free camera to the index-th sample of the plan. Mark is set on the one frame of the sample the add-on
        // adds.
        public static void Show(LensPlan plan, int index, bool mark)
        {
            var (x, y) = LensSampler.Sample(index, plan.Count, plan.Blades, plan.Rotation);
            freecam.Shift = new LensShift(x * plan.Radius, y * plan.Radius, plan.Focus);
            sample = new LensSample(x, y, (uint)index, mark);
        }

        public static void Accumulate(PhotorealAccumulateMode mode, uint generation)
            => Photoreal.SetAccumulate(new PhotorealAccumulate { Mode = mode, Generation = generation, CatEye = CatEye, CatEyeFalloff = CatEyeFalloff });

        public static void Centre()
        {
            if (freecam != null)
                freecam.Shift = null;
            sample = default;
        }

        // Camera.onPreCull of the main camera, after the free camera applied its pose, so the add-on gets the camera that
        // renders: its passes rebuild view space with it, and the accumulator learns which lens sample the frame shows.
        public static void Push(Camera camera)
        {
            if (!Photoreal.Connected)
                return;
            // The sun light shines along its forward axis, so the sun lies the other way.
            var sun = EnviroSky.Instance ? EnviroSky.Instance.MainLight : null;
            Vector3 towardSun = sun ? -sun.transform.forward : Vector3.zero;
            Color sunColor = sun ? sun.color.linear * sun.intensity : Color.black;
            Photoreal.SetCamera(camera.worldToCameraMatrix, GL.GetGPUProjectionMatrix(camera.nonJitteredProjectionMatrix, false), towardSun,
                sunColor, RenderSettings.ambientSkyColor.linear, sample);
        }
    }
}
