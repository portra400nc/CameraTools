using CameraToolsEnhancements;
using UnityEngine;

namespace CameraTools
{
    // The world while CameraTools records a long exposure. Unity's captureFramerate moves the game's time by exactly
    // 1/FramesPerSecond each frame however long the frame takes to draw, so the frames the Enhancements add-on averages
    // are evenly spaced in game time. Screenshot runs the sequence.
    internal static class NativeExposure
    {
        // Set from Start to End: the game speed, captureFramerate and time of day as they were.
        private static (float Speed, int CaptureFramerate, TimeOfDay.Held Time)? held;

        // Runs the game at speed 1, which also unpauses it, one step a frame, with the time of day held and ReShade's
        // effects off: they draw after the add-on's sum, so the frames need none of them. Returns what it did, for the log.
        public static string Start(int framesPerSecond, uint generation)
        {
            held ??= (Time.timeScale, Time.captureFramerate, TimeOfDay.Hold());
            // Directly rather than through SetPaused, which resumes to the last speed the user set, not to 1.
            Time.timeScale = 1f;
            Time.captureFramerate = framesPerSecond;
            Supersampling.Accumulate(EnhancementsAccumulateMode.Add, generation);
            ReShade.SetEffects(false);
            var (speed, capture, _) = held.Value;
            return $"game speed {speed:0.##} -> 1, captureFramerate {capture} -> {framesPerSecond}";
        }

        // Stops the world at its last frame, so the depth and ReShade's effects see it, and shows the sum's average times
        // gain at the pixels' centres under the effects.
        public static void Freeze(uint generation, float gain)
        {
            if (held is var (_, capture, _))
                Time.captureFramerate = capture;
            Time.timeScale = 0f;
            Supersampling.Centre();
            Supersampling.Accumulate(EnhancementsAccumulateMode.Present, generation, gain);
            ReShade.SetEffects(true);
        }

        // Puts back everything Start changed. Safe to call when nothing was, and more than once.
        public static void End()
        {
            Supersampling.Centre();
            Supersampling.Accumulate(EnhancementsAccumulateMode.Off, 0);
            if (held is var (speed, capture, time))
            {
                Time.captureFramerate = capture;
                Time.timeScale = speed;
                TimeOfDay.Release(time);
            }
            held = null;
        }
    }
}
