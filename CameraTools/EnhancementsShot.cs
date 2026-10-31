using CameraToolsEnhancements;
using MelonLoader;
using UnityEngine;
using static CameraTools.CameraTools;
using Vec3 = System.Numerics.Vector3;

namespace CameraTools
{
    // The traced shot behind the screenshot button: the Enhancements add-on's path tracer lights the picture. After the
    // countdown the shot holds the world still, and once its preset and size have settled it captures the scene around the
    // camera, or takes the last shot's again, turns RTGI off, has the add-on trace over whatever Enhancements settings are
    // applied, and waits for every pixel's samples. Screenshot drives it and puts everything back through End.
    // TracedShot holds its decisions.
    internal static class EnhancementsShot
    {
        // iMMERSE Pro's diffuse RTGI, which the traced bounce light stands in for: the technique of MartysMods_RTGI_DIFFUSE.fx,
        // by the name that file gives it. Its specular RTGI (MartysMods_RTGI_SPECULAR.fx) stays on: the trace has no
        // reflections to put in its place yet.
        private static readonly EffectName[] Rtgi =
        {
            new("MartysMods_RTGI_DIFFUSE.fx", "MartysMods_NEWGI_Diffuse"),
        };

        private static readonly string[] RtgiNames = { "RTGI diffuse" };

        // A shot from the moment it holds the world.
        private sealed class Run
        {
            public ShotPhase Phase = ShotPhase.Settling;
            public float Began;
            public bool PausedHere;
            public TimeOfDay.Held TimeBefore;
            public StillLight Still;
            // The bridge's present count and the time since when every other condition has held.
            public (ulong Presents, float At)? Settled;
            // The shot has begun to trace: the add-on runs its settings, and its scene, the kept one or a new one, is this
            // shot's to keep or release.
            public bool Owns;
            public bool Reused;
            public bool Seen;
            public uint Before;
            public float PhaseBegan;
            public SceneFacts Facts;
            public uint Baseline;
            // The add-on's count of the samples' starts when this trace began.
            public uint Starts;
            public bool Stalled;
            public readonly bool[] RtgiOff = new bool[Rtgi.Length];
            // What the picture was saved with when it was not all its samples.
            public string Partial;
            public bool Saved;
        }

        private static MelonPreferences_Entry<bool> enabled;
        private static MelonPreferences_Entry<bool> saveScene;
        private static MelonPreferences_Entry<float> bounceStrength;
        private static bool alternate = true;
        private static Run run;
        // The scene of the last traced shot, while the next one may use it again.
        private static SceneFacts? kept;

        // The Path traced lighting row.
        public static bool Enabled
        {
            get => enabled.Value;
            set
            {
                enabled.Value = value;
                MelonPreferences.Save();
            }
        }

        public static float BounceStrength
        {
            get => Math.Clamp(float.IsFinite(bounceStrength.Value) ? bounceStrength.Value : TracedShot.DefaultBounceStrength, 0f, TracedShot.MaxBounceStrength);
            set
            {
                if (value == bounceStrength.Value)
                    return;
                bounceStrength.Value = value;
                MelonPreferences.Save();
            }
        }

        // Whether the next screenshot is a traced one.
        public static bool Wanted
        {
            get
            {
                if (!enabled.Value)
                    return false;
                if (saveScene.Value)
                {
                    // Plain shots with RTGI and traced ones alternate, so both come from one pose.
                    alternate = !alternate;
                    if (!alternate)
                    {
                        Log("saving the scene for the Mac, so this shot is a plain one and the next is traced.");
                        return false;
                    }
                }
                if (!Enhancements.Connected)
                    Log("the Enhancements add-on is not loaded, so this is a plain screenshot.");
                return Enhancements.Connected;
            }
        }

        // The camera turns through the scene's faces, or the scene is still being built: the depth of field's autofocus
        // starts over after it.
        public static bool Capturing => run is { Phase: ShotPhase.Requesting or ShotPhase.Capturing };

        public static bool SceneCaptured => run is { Phase: ShotPhase.Tracing or ShotPhase.Done };

        public static bool Converged => run is { Phase: ShotPhase.Done };

        // The game runs again under a shot that paused it.
        public static bool Unpaused => run != null && Time.timeScale != 0f;

        // Test-mode shots wait for every sample: unpausing the game cancels them.
        public static float Timeout => saveScene.Value ? float.PositiveInfinity : TracedShot.Timeout + (run?.Partial != null ? TracedShot.PartialGrace : 0f);

        // What the shot waits for, to complete "timed out waiting for ...", or null while it is the screenshot's own
        // conditions.
        public static string Waiting => run == null ? null : TracedShot.Waiting(run.Phase, Enhancements.Status, run.Stalled);

        public static void Load()
        {
            enabled = MelonPreferences.CreateCategory("CameraToolsEnhancements").CreateEntry("TracedShot", false,
                description: "Light the screenshot with the Enhancements add-on's path tracer: the shot pauses the game, captures the scene around "
                    + "the camera and waits for the traced lighting. Needs CameraToolsEnhancements.addon64.");
            bounceStrength = MelonPreferences.CreateCategory("CameraToolsEnhancements").CreateEntry("TracedShotBounceStrength", TracedShot.DefaultBounceStrength,
                description: $"How much light surfaces bounce onto each other in a traced shot, from 0 to {TracedShot.MaxBounceStrength:0}; 1 is the physical amount.");
            saveScene = MelonPreferences.CreateCategory("CameraToolsEnhancements").CreateEntry("TracedShotSaveScene", false,
                description: "For tests on the Mac: each traced shot captures the scene anew, writes every face's lit HDR picture, depth and albedo "
                    + "beside it, and saves the converged trace.");
            MelonPreferences.Save();
        }

        // When the button is pressed. The bridge answers for a technique only after it was first asked for.
        public static void Start()
        {
            foreach (var technique in Rtgi)
                ReShade.TryGetTechnique(technique, out _);
        }

        // At the end of the countdown: the world holds still from here until End. The game's pause does not stop its time
        // of day, so the hour is locked too.
        public static void Hold()
        {
            float now = Time.unscaledTime;
            bool ran = Time.timeScale != 0f;
            // Before the pause: a scene kept through a game that runs, if only since this frame, is not this world's.
            Watch();
            run = new Run { Began = now, PhaseBegan = now, PausedHere = ran, Still = new StillLight(EnhancementsCamera.Light, now) };
            if (ran)
                SetPaused(true);
            bool locked = TimeOfDay.Locked;
            float speed = TimeOfDay.Speed;
            run.TimeBefore = TimeOfDay.Hold();
            Log($"holding the world: the game {(ran ? "paused" : "was paused already")}; the time of day {TimeOfDay.Clock(TimeOfDay.Hour)} "
                + (!TimeOfDay.Locked ? "could not be locked" : locked ? "was locked already" : "locked") + (speed > 0f ? $", its speed {speed:0.##} stopped" : "") + ".");
        }

        // Once a frame while the screenshot settles. settled: every condition but the shot's own two holds. presents is
        // the bridge's count. Returns why the shot failed, as a notice and the log's version of it, or null.
        public static (string Toast, string Detail)? Step(bool settled, ulong presents)
        {
            if (run == null)
                return null;
            float now = Time.unscaledTime;
            var status = Enhancements.Status;
            switch (run.Phase)
            {
                case ShotPhase.Settling:
                    run.Still = run.Still.Next(EnhancementsCamera.Light, now);
                    if (!settled)
                    {
                        run.Settled = null;
                        return null;
                    }
                    var since = run.Settled ??= (presents, now);
                    bool still = run.Still.Held(now);
                    if (presents - since.Presents < TracedShot.SettledPresents || (!still && now - since.At < TracedShot.StillTimeout))
                        return null;
                    Begin(status, still, now);
                    return null;
                case ShotPhase.Requesting:
                    // A capture that was stopped is still saving for a moment, and the add-on takes none meanwhile.
                    if (Enhancements.CaptureScene(new EnhancementsSceneRequest
                        {
                            Faces = TracedShot.AllFaces, Settle = TracedShot.FaceSettle, Flags = EnhancementsSceneFlags.Trace | EnhancementsSceneFlags.Light | EnhancementsSceneFlags.NoMeshes
                                | (saveScene.Value ? EnhancementsSceneFlags.GBuffer : EnhancementsSceneFlags.None),
                        }))
                    {
                        run.Facts = Facts(status.TraceGeneration);
                        run.Seen = false;
                        Enter(ShotPhase.Capturing, now);
                    }
                    break;
                case ShotPhase.Capturing:
                    switch (TracedShot.Capture(ref run.Seen, run.Before, status))
                    {
                        case CaptureVerdict.Ready:
                            run.Facts = run.Facts with { Generation = status.TraceGeneration };
                            Log($"scene generation {status.TraceGeneration} is ready {now - run.PhaseBegan:0.0} s after it was asked for: {status.TraceTriangles} triangles, "
                                + $"light from the game: {status.TraceLight}.");
                            run.Baseline = run.Starts = status.TraceRestarts;
                            Enter(ShotPhase.Tracing, now);
                            break;
                        case CaptureVerdict.Failed:
                            string line = Enhancements.Describe();
                            return ($"Screenshot failed: the scene capture stopped{(TracedShot.Stopped(line) is { } why ? $": {why}" : "; see ReShade.log")}",
                                $"the Enhancements scene capture failed {now - run.PhaseBegan:0.0} s after it was asked for; the add-on says: {line}");
                    }
                    break;
                case ShotPhase.Tracing:
                    run.Stalled = TracedShot.Stalled(ref run.Baseline, status);
                    switch (TracedShot.Trace(run.Facts.Generation, status))
                    {
                        case TraceVerdict.Converged:
                            Log($"the lighting converged {now - run.PhaseBegan:0.0} s after the scene was ready: {status.TraceSamples} of {status.TraceTarget} samples at "
                                + $"{status.RenderWidth}x{status.RenderHeight}, error {status.TraceErrorX10000 / 100f:0.00}%, after {status.TraceRestarts - run.Starts} starts of "
                                + $"its samples, the last for {status.TraceRestartReason}.");
                            if (saveScene.Value)
                                Log(Enhancements.SaveTrace() ? "saving the converged trace for the Mac." : "the add-on refused to save the trace.");
                            CameraUi.ClearToast();
                            Enter(ShotPhase.Done, now);
                            return null;
                        case TraceVerdict.Lost when run.Reused:
                            // From here the camera sees what the last shot's capture did not, or the world moved after all.
                            Log($"the kept scene is {status.TraceState} now, so the shot captures it again; the add-on says: {Enhancements.Describe()}");
                            run.Reused = false;
                            kept = null;
                            run.Before = status.TraceGeneration;
                            Enter(ShotPhase.Requesting, now);
                            break;
                        case TraceVerdict.Lost:
                            return ($"Screenshot failed: the traced scene went {status.TraceState.ToString().ToLowerInvariant()}; see ReShade.log",
                                $"the traced scene went {status.TraceState} {now - run.PhaseBegan:0.0} s into its trace, at {status.TraceSamples} of {status.TraceTarget} "
                                    + $"samples; the add-on says: {Enhancements.Describe()}");
                    }
                    break;
            }
            if (TracedShot.Progress(run.Phase, status, run.Stalled) is { } progress)
                CameraUi.Toast(progress, whileHidden: true);
            return null;
        }

        // At the timeout: whether the picture is saved as it is, drawn from fewer samples than asked for.
        public static bool SaveAsIs()
        {
            if (run == null)
                return false;
            var status = Enhancements.Status;
            if (saveScene.Value && run.Phase == ShotPhase.Tracing)
                Log(Enhancements.SaveTrace() ? $"saving the unfinished trace for the Mac, {status.TraceSamples} samples." : "the add-on refused to save the trace.");
            if (!TracedShot.SaveAsIs(run.Phase, run.Facts.Generation, status))
                return false;
            run.Partial = $"{status.TraceSamples} of {status.TraceTarget} samples";
            Melon<CameraTools>.Logger.Warning($"Traced shot: the lighting did not converge within {TracedShot.Timeout:0} s; saving the picture as it is, with {run.Partial}"
                + (run.Stalled ? $" ({TracedShot.Changing(status.TraceRestartReason)}, {status.TraceRestarts} starts)" : "") + ".");
            CameraUi.ClearToast();
            Enter(ShotPhase.Done, Time.unscaledTime);
            return true;
        }

        // The picture is on disk: what the notice and the log add about its lighting.
        public static string Saved()
        {
            if (run == null)
                return "";
            run.Saved = true;
            return run.Partial != null ? $", traced with {run.Partial}" : ", traced";
        }

        // Puts back what the shot changed, however it ended: the add-on's trace, RTGI, the time of day and the game's speed.
        // The scene stays for the next shot only after a saved picture, and only while the game stays paused.
        public static void End()
        {
            var ended = run;
            run = null;
            if (ended == null)
                return;
            var notes = new List<string>();
            void Try(string what, Action restore)
            {
                try
                {
                    restore();
                }
                catch (Exception e)
                {
                    Melon<CameraTools>.Logger.Warning($"Traced shot: putting back {what} failed: {e}");
                }
            }
            Try("the add-on's settings", () =>
            {
                if (ended.Owns)
                    Enhancements.SetShot(false);
            });
            Try("RTGI", () =>
            {
                for (int i = 0; i < Rtgi.Length; i++)
                {
                    if (!ended.RtgiOff[i])
                        continue;
                    ReShade.SetTechnique(Rtgi[i], true);
                    notes.Add($"{RtgiNames[i]} on again");
                }
            });
            Try("the time of day", () =>
            {
                TimeOfDay.Release(ended.TimeBefore);
                notes.Add(ended.TimeBefore.Locked ? "the time of day left locked" : "the time of day unlocked");
            });
            Try("the game's speed", () =>
            {
                if (ended.PausedHere && Time.timeScale == 0f)
                    SetPaused(false);
                notes.Add(Time.timeScale == 0f ? "the game left paused" : "the game resumed");
            });
            Try("the scene", () =>
            {
                string release = !ended.Owns ? null
                    : !ended.Saved ? "the shot saved no picture"
                    : TracedShot.Release(Enhancements.Connected, freecamActive, Time.timeScale == 0f);
                if (ended.Owns && release == null)
                {
                    kept = ended.Facts;
                    notes.Add($"scene generation {ended.Facts.Generation} kept for the next shot");
                }
                else if (ended.Owns)
                {
                    kept = null;
                    Enhancements.ReleaseScene();
                    notes.Add($"the scene released: {release}");
                }
            });
            Log($"ended {Time.unscaledTime - ended.Began:0.0} s after the hold; {string.Join("; ", notes)}.");
        }

        // Every frame: the kept scene goes as soon as it may no longer be this world's. A shot that holds the world
        // watches it itself.
        public static void Watch()
        {
            if (kept == null || run != null)
                return;
            if (TracedShot.Release(Enhancements.Connected, freecamActive, Time.timeScale == 0f) is not { } why)
                return;
            Log($"released scene generation {kept.Value.Generation}: {why}.");
            kept = null;
            Enhancements.ReleaseScene();
        }

        // The preset, the size and the light have settled: the add-on traces from here, with RTGI off, the kept scene or a
        // new one.
        private static void Begin(EnhancementsStatus status, bool still, float now)
        {
            run.Owns = true;
            Enhancements.SetShot(true, 0, TracedShot.BudgetMs, BounceStrength);
            var rtgi = new List<string>();
            for (int i = 0; i < Rtgi.Length; i++)
            {
                if (!ReShade.TryGetTechnique(Rtgi[i], out bool on))
                    rtgi.Add($"{RtgiNames[i]} not loaded");
                else if (!on)
                    rtgi.Add($"{RtgiNames[i]} off already");
                else
                {
                    ReShade.SetTechnique(Rtgi[i], false);
                    run.RtgiOff[i] = true;
                    rtgi.Add($"{RtgiNames[i]} turned off");
                }
            }
            var facts = Facts(status.TraceGeneration);
            string unusable = saveScene.Value ? "the scene is saved for the Mac" : kept is { } last ? TracedShot.Unusable(last, facts, status.TraceState) : "no scene is kept";
            Log($"settled {now - run.Began:0.0} s after the hold; the sky's light "
                + (still ? $"has held still {now - run.Still.Since:0.0} s" : $"did not hold still for {TracedShot.StillTimeout:0} s") + $"; {string.Join(", ", rtgi)}; "
                + (unusable == null ? $"tracing the kept scene, generation {facts.Generation}, again." : $"capturing the scene: {unusable}."));
            // A kept scene stays what its capture saw, so shots that each move a little do not walk away from it.
            run.Facts = unusable == null ? kept.Value : facts;
            run.Before = status.TraceGeneration;
            run.Baseline = run.Starts = status.TraceRestarts;
            run.Reused = unusable == null;
            if (!run.Reused)
                kept = null;
            Enter(run.Reused ? ShotPhase.Tracing : ShotPhase.Requesting, now);
        }

        private static void Enter(ShotPhase phase, float now)
        {
            run.Phase = phase;
            run.PhaseBegan = now;
            run.Stalled = false;
        }

        // The world as a scene captured now would hold it.
        private static SceneFacts Facts(uint generation)
        {
            var position = freecam.Pose.Position;
            var origin = WorldShift.Relative(default);
            return new SceneFacts(generation, new Vec3(position.x, position.y, position.z), new Vec3(origin.x, origin.y, origin.z), Weather.Choice, Posing.Revision,
                EnhancementsCamera.Light);
        }

        private static void Log(string message) => Melon<CameraTools>.Logger.Msg($"Traced shot: {message}");
    }
}
