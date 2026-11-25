using CameraToolsEnhancements;
using MelonLoader;
using UnityEngine;

namespace CameraTools
{
    // Anti-aliasing for the screenshot: with the game paused, the camera looks through a different point of each pixel
    // on each sample and the Enhancements add-on averages the samples in HDR, before bloom and the tone map. A
    // lens-sampled screenshot also moves the camera to another point of the aperture each sample, with the projection
    // sheared so the focus plane stays still: the average is the lens's depth of field. This class holds the switches,
    // the sample on screen and the paused world; Screenshot runs the sequence.
    internal static class Supersampling
    {
        private const int MaxSamples = 256;

        private const int MaxLensSamples = 4096;

        private static MelonPreferences_Entry<bool> enabled, lensSampled;
        private static MelonPreferences_Entry<int> samples, lensSamples;
        private static MelonPreferences_Entry<float> edgeSoftness, gapFill;
        private static LensSample sample;
        // Set while the shot holds the world, with whether it paused the game and how the time of day was.
        private static (bool PausedHere, TimeOfDay.Held Time)? held;

        public static bool Enabled => enabled.Value;

        public static int Samples => Lens?.Points.Length ?? Math.Clamp(samples.Value, 1, MaxSamples);

        // Whether a screenshot with the depth of field on takes its blur from lens samples, and how many.
        public static bool LensSampled => lensSampled.Value;

        public static int LensSamples => Math.Clamp(lensSamples.Value, 1, MaxLensSamples);

        public static float EdgeSoftness => Math.Clamp(edgeSoftness.Value, 0f, 1f);

        public static float GapFillScale => Math.Clamp(gapFill.Value, 0f, 4f);

        public static void SetLensSampled(bool on) => Save(lensSampled, on);

        public static void SetLensSamples(int count) => Save(lensSamples, Math.Clamp(count, 1, MaxLensSamples));

        public static void SetGapFill(float scale) => Save(gapFill, Math.Clamp(scale, 0f, 4f));

        public static void SetEdgeSoftness(float softness) => Save(edgeSoftness, Math.Clamp(softness, 0f, 1f));

        private static void Save<T>(MelonPreferences_Entry<T> entry, T value)
        {
            entry.Value = value;
            MelonPreferences.Save();
        }

        // The share of the lens's CoC each sample is blurred by: the gap fill for the samples, times the preference, and twice
        // that under the highlight lift, whose brighter copies of a light show the gaps between them (a lifted lamp's disc
        // was a pattern of dots at 1x and smooth at 2x on the Deck, 2026-11-25).
        public static float Fill => Lens is { } lens ? LensSampler.GapFill(lens.Points.Length) * GapFillScale * (lens.Lift > 0f ? 2f : 1f) : 0f;

        // The lens-sampled screenshot that runs, from its start to its end, or null.
        public static LensShot Lens { get; private set; }

        // Where in the aperture the camera looks from this sample, in meters right and up of its pose, and where its zone
        // of the aperture focuses.
        public static (float X, float Y) CameraOffset { get; private set; }

        private static float sampleFocus;

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
            lensSampled = category.CreateEntry("LensSampledScreenshots", false,
                description: "Depth of field's screenshot method: true renders the scene from points across the lens's aperture and averages them (Lens samples), false blurs the one frame (Single frame). CameraTools' depth of field only; a traced shot's samples reuse the lighting it converged at the lens's center.");
            lensSamples = category.CreateEntry("LensSamples", 256,
                description: $"How many aperture points a lens-sampled screenshot averages, from 1 to {MaxLensSamples}. Each takes two frames.");
            gapFill = category.CreateEntry("LensGapFill", 1f,
                description: "How much each lens sample is blurred so a point's copies merge into one disc, as a multiple of the automatic fill for the sample count, from 0 (off) to 4.");
            edgeSoftness = category.CreateEntry("LensEdgeSoftness", 0f,
                description: "Bokeh edge softness of a lens-sampled screenshot, from 0 to 1: fewer aperture points toward the rim, for softer disc edges.");
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

        // Starts a lens-sampled screenshot's samples; End stops them.
        public static void BeginLens(LensShot shot) => Lens = shot;

        public static void Show(int index, bool mark)
        {
            Offset = SupersamplingPattern.Offset(index);
            if (Lens is { } lens && index < lens.Points.Length)
            {
                var (x, y) = lens.Points[index];
                CameraOffset = (x * lens.Radius, y * lens.Radius);
                sampleFocus = lens.FocusAt(index);
                var (sx, sy) = lens.Sample(index);
                sample = new LensSample(sx, sy, (uint)index, mark);
                return;
            }
            sample = new LensSample(0f, 0f, (uint)index, mark);
        }

        // A lens-sampled screenshot's accumulator also cuts each pixel's aperture points by the cat's eye and swirl, and
        // reads red and blue along the point. The add-on's points lie in its unit disc, so the shifts are per unit of it.
        public static void Accumulate(EnhancementsAccumulateMode mode, uint generation, float gain = 1f)
            => Enhancements.SetAccumulate(new EnhancementsAccumulate { Mode = mode, Generation = generation, Gain = gain, CatEye = Lens?.CatEye ?? 0f,
                CatEyeFalloff = 0.1f, Swirl = Lens?.Swirl ?? 0f, ChromaticRed = (Lens?.Chromatic.Red ?? 0f) * (Lens?.Reach ?? 1f),
                ChromaticBlue = (Lens?.Chromatic.Blue ?? 0f) * (Lens?.Reach ?? 1f),
                Lift = Lens?.Lift ?? 0f, LiftThreshold = Lens?.LiftThreshold ?? 2f, LiftCocPerDiopter = Lens?.CocPerDiopter ?? 0f,
                LiftFocusDiopters = Lens is { Focus: > 0f } shot ? 1f / shot.Focus : 0f });

        public static void Centre()
        {
            Offset = null;
            CameraOffset = (0f, 0f);
            sample = default;
        }

        // Puts back everything the shot changed. Safe to call when nothing was.
        public static void End()
        {
            Centre();
            Accumulate(EnhancementsAccumulateMode.Off, 0);
            Lens = null;
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
            if (Lens is { } lens && CameraOffset != (0f, 0f))
            {
                var (m02, m12) = LensSampler.Shear(projection.m00, projection.m11, CameraOffset.X, CameraOffset.Y, sampleFocus > 0f ? sampleFocus : lens.Focus);
                projection.m02 += m02;
                projection.m12 += m12;
            }
            return projection;
        }
    }
}
