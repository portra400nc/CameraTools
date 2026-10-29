using CameraToolsEnhancements;

namespace CameraTools
{
    // What a screenshot is, decided when the button is pressed. Traced: the Enhancements add-on's path tracer lights it.
    // Supersampled: the add-on averages the samples into it. Dng: the add-on also develops a DNG of the frame ReShade saves.
    internal readonly record struct ShotKinds(bool Traced, bool Supersampled, bool Dng)
    {
        // The traced lighting pauses the game, which leaves a long exposure nothing to record, so a long exposure goes
        // without. A long exposure averages over time and a traced shot over its own samples, so neither is supersampled.
        // Neither has a DNG: a long exposure's average is ReShade's, made after the add-on draws, and the traced lighting
        // is shelved.
        public static ShotKinds Of(bool longExposure, bool tracedWanted, bool supersampling, bool dng, bool enhancements)
        {
            bool traced = tracedWanted && !longExposure;
            bool plain = !traced && !longExposure;
            return new ShotKinds(traced, supersampling && enhancements && plain, dng && enhancements && plain);
        }
    }

    // The camera a DNG's EXIF names, as the panel shows it when the capture is asked for: the full-frame focal length, and
    // while CameraTools' own depth of field blurs the DNG, its f-number and focus distance.
    internal sealed record DngCamera(float FocalLength, float? FNumber, float? FocusMeters, DateTime Taken)
    {
        public override string ToString() => $"{FocalLength:0} mm" + (FNumber is float f ? $", f/{f:0.##}" : "")
            + (FocusMeters is float m ? $", focus {m:0.##} m" : "");
    }

    // The DNG of a shot: the id the add-on develops it under, and its camera.
    internal sealed record DngTake(uint Id, DngCamera Camera);

    // What a shot's DNG does next, once ReShade saved the PNG.
    internal enum DngStep
    {
        Wait,       // the add-on is developing or writing it, or its status has not caught up
        Write,      // it is ready: ask the add-on to write it
        Written,
        Failed,     // the status's DngFailure says why
    }

    internal static class ScreenshotDng
    {
        // Beside the PNG, with its name.
        public static string PathFor(string png) => Path.ChangeExtension(png, ".dng");

        // From the add-on's status for the DNG of id. Writing: the write was asked for, so a status still reading Ready is a
        // present behind. A status of another id is a present behind the shot's develop.
        public static DngStep Next(uint id, bool writing, uint statusId, EnhancementsDngState state)
        {
            if (statusId != id)
                return DngStep.Wait;
            return state switch
            {
                EnhancementsDngState.Ready => writing ? DngStep.Wait : DngStep.Write,
                EnhancementsDngState.Written => DngStep.Written,
                EnhancementsDngState.Failed => DngStep.Failed,
                _ => DngStep.Wait,
            };
        }

        // Completes "no DNG: ...".
        public static string Why(EnhancementsDngFailure failure) => failure switch
        {
            EnhancementsDngFailure.NoTonemap => "no tone map draw developed it",
            EnhancementsDngFailure.Develop => "the Enhancements add-on could not develop it",
            EnhancementsDngFailure.Readback => "its picture did not come back from the GPU",
            EnhancementsDngFailure.Write => "its file could not be written",
            EnhancementsDngFailure.Dropped => "the Enhancements add-on dropped it before it was written",
            _ => "the Enhancements add-on gave no reason",
        };

        // The toast of a saved shot, such as "Saved GenshinImpact 2026-10-28 21-14-05.png and .dng · supersampled". kind is
        // the shot's own suffix.
        public static string Saved(string png, bool dng, string kind)
            => (png.Length > 0 ? $"Saved {Path.GetFileName(png)}{(dng ? " and .dng" : "")}" : "Screenshot saved") + kind;

        // The toast of a shot whose PNG was saved without its DNG.
        public static string SavedWithout(string png, string why, string kind) => $"{Saved(png, false, kind)}, but no DNG: {why}";
    }
}
