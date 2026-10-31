using CameraToolsEnhancements;
using Vec3 = System.Numerics.Vector3;

namespace CameraTools
{
    // The light the sky gives the scene, as EnhancementsCamera reads it each frame: the direction toward the sun, the sun
    // light's color times its intensity, the sky's ambient color, and the weather's wetness.
    internal readonly record struct SkyLight(Vec3 TowardSun, Vec3 SunColor, Vec3 Ambient, float Wetness)
    {
        // Whether other is this light within tolerance: the direction and the wetness by their difference, a color by its
        // difference over its brightest channel, or over 1 for a dim one.
        public bool Near(SkyLight other, float tolerance)
            => Apart(TowardSun, other.TowardSun) <= tolerance && MathF.Abs(Wetness - other.Wetness) <= tolerance
                && Apart(SunColor, other.SunColor) <= tolerance * Brightest(SunColor, other.SunColor)
                && Apart(Ambient, other.Ambient) <= tolerance * Brightest(Ambient, other.Ambient);

        private static float Apart(Vec3 a, Vec3 b) => MathF.Max(MathF.Abs(a.X - b.X), MathF.Max(MathF.Abs(a.Y - b.Y), MathF.Abs(a.Z - b.Z)));

        private static float Brightest(Vec3 a, Vec3 b) => MathF.Max(1f, MathF.Max(Apart(a, default), Apart(b, default)));
    }

    // Since when the sky's light has held still. Reference is the light the wait began with, not the frame before's, so a
    // slow drift starts the wait again once it has gone past the tolerance.
    internal readonly record struct StillLight(SkyLight Reference, float Since)
    {
        public StillLight Next(SkyLight light, float now) => light.Near(Reference, TracedShot.StillTolerance) ? this : new StillLight(light, now);

        public bool Held(float now) => now - Since >= TracedShot.StillSeconds;
    }

    // What a traced scene was captured with, which says whether a later shot may trace it again: the add-on's generation
    // of it, the free camera's position and the world's origin in scene coordinates, the World tab's weather, the pose's
    // revision, and the sky's light.
    internal readonly record struct SceneFacts(uint Generation, Vec3 Camera, Vec3 Origin, int Weather, int Pose, SkyLight Light);

    internal enum ShotPhase
    {
        Settling,   // the shot's preset and size settle, then the sky's light
        Requesting, // the add-on is asked for a scene capture until it takes one
        Capturing,  // the camera turns through the faces, then the scene is built and sent to the GPU
        Tracing,    // the samples come in
        Done,       // the frame is drawn from all its samples, or at the timeout from enough of them
    }

    internal enum CaptureVerdict { Waiting, Ready, Failed }

    internal enum TraceVerdict { Waiting, Converged, Lost }

    // The traced screenshot's decisions, apart from the game, ReShade and the add-on, so tests run them.
    internal static class TracedShot
    {
        // A kept scene serves a camera this near where it was captured: it holds every direction, so turning and zooming
        // are free.
        public const float ReuseMeters = 0.25f;
        // And a sky whose light is within this of the capture's.
        public const float ReuseTolerance = 0.02f;
        // The light holds still while it stays within this for this long. The time of day moves the sun 0.004 a second.
        public const float StillTolerance = 1e-4f;
        public const float StillSeconds = 0.5f;
        // After this long without a still light the shot goes on, and the add-on's own restarts say what moves.
        public const float StillTimeout = 10f;
        // Presents every other condition holds before the scene is captured, so the render size has settled.
        public const ulong SettledPresents = 10;
        public const float Timeout = 300f;
        // After a timeout that saves the picture as it is, the wait left for the frames without the UI.
        public const float PartialGrace = 30f;
        // From this many samples a pixel shows its own average, not its neighbors' blur.
        public const uint PartialSamples = 16;
        // GPU time a frame for the shot's rays: nothing else needs the frames while it waits, and each 4K frame on the Deck
        // also costs the game's own, about 575 ms, which took most of a shot at 100 ms a frame.
        public const float BudgetMs = 1500f;
        // The Bounce strength row: 1 is the physical amount for the game's colors, and the default makes up for Genshin's
        // painted colors being darker than real materials.
        public const float DefaultBounceStrength = 3f, MaxBounceStrength = 4f;
        // Starts without a sample after which the wait says what keeps changing.
        public const uint StalledStarts = 8;
        public const uint Faces = 7;
        // Presents each turned face is held before its capture, against the add-on's 45. The camera only turns, so nothing
        // streams in: at 6, a Deck capture matched one at 45 in depth and in the lit pictures of every surface, and only the
        // sun's halo, which the default sky mix does not read, was still fading in.
        public const uint FaceSettle = 8;
        // Every face by name, EnhancementsSceneRequest.Faces' bits: with none named, a traced capture is the add-on's self-test
        // scene.
        public const uint AllFaces = (1u << (int)Faces) - 1;
        private const uint TracePass = 1u << 7;

        // Why the kept scene cannot serve this shot, or null when it can. The game has stayed paused since its capture,
        // or it would have been released.
        public static string Unusable(SceneFacts kept, SceneFacts now, EnhancementsTraceState state)
        {
            if (state != EnhancementsTraceState.Ready)
                return $"the add-on's scene is {state.ToString().ToLowerInvariant()}";
            if (now.Generation != kept.Generation)
                return "the add-on holds another scene";
            if (now.Origin != kept.Origin)
                return "the world's origin shifted";
            float moved = Vec3.Distance(now.Camera, kept.Camera);
            if (!(moved <= ReuseMeters))
                return $"the camera moved {moved:0.00} m";
            if (now.Weather != kept.Weather)
                return "the weather was changed";
            if (now.Pose != kept.Pose)
                return "the pose was changed";
            if (!now.Light.Near(kept.Light, ReuseTolerance))
                return "the sky's light changed";
            return null;
        }

        // Why a kept scene is released while no shot holds the world, or null to keep it.
        public static string Release(bool connected, bool freecam, bool paused)
            => !connected ? "the Enhancements add-on was unloaded"
                : !freecam ? "the free camera is off"
                : !paused ? "the game was unpaused"
                : null;

        // A capture asked for while the add-on's scene had the generation before. The status of the frames before the
        // add-on took the request still shows the scene from before, so a failed or missing scene is this capture's only
        // once the status has shown the capture: seen.
        public static CaptureVerdict Capture(ref bool seen, uint before, EnhancementsStatus status)
        {
            bool fresh = status.TraceGeneration != before;
            seen |= fresh || status.SceneRemaining > 0 || status.TraceState is EnhancementsTraceState.Building or EnhancementsTraceState.Uploading;
            if (fresh && status.TraceState == EnhancementsTraceState.Ready)
                return CaptureVerdict.Ready;
            return seen && status.TraceState is EnhancementsTraceState.None or EnhancementsTraceState.Stale or EnhancementsTraceState.Failed
                ? CaptureVerdict.Failed
                : CaptureVerdict.Waiting;
        }

        // The trace of the scene of that generation: converged only on a frame that ran with the shot, since a status
        // from before the add-on took the shot may show another caller's converged trace.
        public static TraceVerdict Trace(uint generation, EnhancementsStatus status)
            => status.TraceState != EnhancementsTraceState.Ready || status.TraceGeneration != generation ? TraceVerdict.Lost
                : status.ShotActive != 0 && status.TraceConverged != 0 ? TraceVerdict.Converged
                : TraceVerdict.Waiting;

        // Whether no sample accumulates: the samples have started StalledStarts times since baseline, the starts when a
        // pixel last had a sample, which follows the status while it has one.
        public static bool Stalled(ref uint baseline, EnhancementsStatus status)
        {
            if (status.TraceSamples > 0)
                baseline = status.TraceRestarts;
            return status.TraceSamples == 0 && status.TraceRestarts - baseline >= StalledStarts;
        }

        // What keeps starting the samples again, for a notice.
        public static string Changing(EnhancementsTraceRestart reason) => reason switch
        {
            EnhancementsTraceRestart.Camera => "the camera keeps moving",
            EnhancementsTraceRestart.SunDirection => "the sun keeps moving",
            EnhancementsTraceRestart.Light => "the light keeps changing",
            EnhancementsTraceRestart.RenderSize => "the render size keeps changing",
            EnhancementsTraceRestart.Settings => "the Enhancements settings keep changing",
            EnhancementsTraceRestart.Scene => "the scene keeps changing",
            _ => "the samples keep starting again",
        };

        // The notice shown while a phase waits, or null for none.
        public static string Progress(ShotPhase phase, EnhancementsStatus status, bool stalled) => phase switch
        {
            ShotPhase.Requesting => "Capturing the scene",
            ShotPhase.Capturing => status.SceneRemaining > 0 ? $"Capturing the scene {Math.Min(Faces - Math.Min(status.SceneRemaining, Faces) + 1, Faces)}/{Faces}"
                : status.TraceState == EnhancementsTraceState.Uploading ? "Sending the scene to the GPU"
                : "Building the scene",
            ShotPhase.Tracing => stalled ? $"Lighting: {Changing(status.TraceRestartReason)}"
                : $"Lighting {Math.Min(status.TraceSamples, status.TraceTarget)}/{status.TraceTarget}",
            _ => null,
        };

        // What a traced shot waits for, to complete "timed out waiting for ...", or null while it waits for the
        // screenshot's own conditions.
        public static string Waiting(ShotPhase phase, EnhancementsStatus status, bool stalled) => phase switch
        {
            ShotPhase.Requesting => "the Enhancements add-on to take a scene capture",
            ShotPhase.Capturing => "the Enhancements scene to be captured (" + (status.SceneRemaining > 0 ? $"{status.SceneRemaining} of {Faces} directions left"
                : status.TraceState.ToString().ToLowerInvariant()) + ")",
            ShotPhase.Tracing => $"the lighting to converge ({status.TraceSamples} of {status.TraceTarget} samples"
                + (stalled ? $"; {Changing(status.TraceRestartReason)}, {status.TraceRestarts} starts" : "") + ")",
            _ => null,
        };

        // Why the add-on's last scene capture stopped, from its status line, or null when the line does not say.
        public static string Stopped(string line)
        {
            const string mark = "; scene stopped: ";
            int at = line.IndexOf(mark, StringComparison.Ordinal);
            if (at < 0)
                return null;
            string why = line[(at + mark.Length)..];
            int end = why.IndexOf(';');
            return end < 0 ? why : why[..end];
        }

        // At the timeout: a shot whose frame the trace pass draws from at least PartialSamples samples a pixel is saved as
        // it is. Any other has no traced lighting to show and fails.
        public static bool SaveAsIs(ShotPhase phase, uint generation, EnhancementsStatus status)
            => phase == ShotPhase.Tracing && status.TraceState == EnhancementsTraceState.Ready && status.TraceGeneration == generation && status.ShotActive != 0
                && (status.PassesRan & TracePass) != 0 && status.TraceSamples >= PartialSamples;
    }
}
