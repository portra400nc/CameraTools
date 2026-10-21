using CameraToolsEnhancements;
using MelonLoader;
using UnityEngine;

namespace CameraTools
{
    // Anti-aliasing for the screenshot: with the game paused, the camera looks through a different point of each pixel
    // on each sample and the Enhancements add-on averages the samples in HDR, before bloom and the tone map. This class
    // holds the switch, the sample on screen and the paused world; Screenshot runs the sequence.
    internal static class Supersampling
    {
        private const int MaxSamples = 256;

        private static MelonPreferences_Entry<bool> enabled;
        private static MelonPreferences_Entry<int> samples;
        private static LensSample sample;
        // Set while the shot holds the world, with whether it paused the game and how the time of day was.
        private static (bool PausedHere, TimeOfDay.Held Time)? held;

        public static bool Enabled => enabled.Value;

        public static int Samples => Math.Clamp(samples.Value, 1, MaxSamples);

        // The offset within the pixel the camera looks through, in pixels, or null for the pixel's centre.
        public static (float X, float Y)? Offset { get; private set; }

        // What the add-on gets with the camera: the sample's index, and whether it adds this frame.
        public static LensSample Sample => sample;

        public static void Load()
        {
            var category = MelonPreferences.CreateCategory("CameraToolsReShade");
            enabled = category.CreateEntry("ScreenshotSupersampling", false,
                description: "Smooth the screenshot's edges by averaging many frames, each looking through another point of every pixel. Pauses the game for the shot and needs CameraToolsEnhancements.addon64.");
            samples = category.CreateEntry("SupersamplingSamples", 32,
                description: $"How many frames a supersampled screenshot averages, from 1 to {MaxSamples}. Each takes two frames.");
            MelonPreferences.Save();
        }

        public static void SetEnabled(bool on)
        {
            enabled.Value = on;
            MelonPreferences.Save();
        }

        // Pauses the game and stops the time of day, since the game's pause does not, and anything that moves between
        // samples would smear.
        public static string Hold()
        {
            bool ran = Time.timeScale != 0f;
            if (ran)
                CameraTools.SetPaused(true);
            held = (ran, TimeOfDay.Hold());
            return ran ? "paused the game" : "the game was paused already";
        }

        public static void Show(int index, bool mark)
        {
            Offset = SupersamplingPattern.Offset(index);
            sample = new LensSample(0f, 0f, (uint)index, mark);
        }

        public static void Accumulate(EnhancementsAccumulateMode mode, uint generation)
            => Enhancements.SetAccumulate(new EnhancementsAccumulate { Mode = mode, Generation = generation });

        public static void Centre()
        {
            Offset = null;
            sample = default;
        }

        // Puts back everything the shot changed. Safe to call when nothing was.
        public static void End()
        {
            Centre();
            Accumulate(EnhancementsAccumulateMode.Off, 0);
            if (held is var (pausedHere, time))
            {
                TimeOfDay.Release(time);
                if (pausedHere && Time.timeScale == 0f)
                    CameraTools.SetPaused(false);
            }
            held = null;
        }

        // The projection of a camera at fov that looks through Offset of each pixel of a width by height picture.
        public static Matrix4x4 Projection(float fov, float aspect, float near, float far, int width, int height)
        {
            var projection = Matrix4x4.Perspective(fov, aspect, near, far);
            if (Offset is var (x, y) && width > 0 && height > 0)
            {
                projection.m02 += 2f * x / width;
                projection.m12 += 2f * y / height;
            }
            return projection;
        }
    }
}
