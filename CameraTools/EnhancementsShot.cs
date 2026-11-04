using Il2CppInterop.Runtime;
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
            public bool Saved;
            // A capture or trace failed once and the shot captured the scene again.
            public bool Retried;
            // ReShade's effects are off from the capture until the trace converges: they cost each frame between the rays and
            // add nothing to them.
            public bool EffectsHeld;
            public ulong RestoredAt;
        }

        private static MelonPreferences_Entry<bool> enabled;
        private static MelonPreferences_Entry<bool> saveScene;
        private static MelonPreferences_Entry<bool> tracedReflections;
        private static MelonPreferences_Entry<float> surfaceGloss, reflectedLight;
        private const float GameLightReach = 100f;
        private static MelonPreferences_Entry<float> bounceStrength;
        private static MelonPreferences_Entry<int> bounces;
        private static MelonPreferences_Entry<bool> tracedSunShadows;
        private static bool alternate = true;
        private static Run run;
        // The scene of the last traced shot, while the next one may use it again.
        private static SceneFacts? kept;

        private static MelonPreferences_Entry<bool> shown;
        private static readonly PadButtons[] Sequence =
        {
            PadButtons.DpadUp, PadButtons.DpadUp, PadButtons.DpadDown, PadButtons.DpadDown,
            PadButtons.DpadLeft, PadButtons.DpadRight, PadButtons.DpadLeft, PadButtons.DpadRight,
        };
        private static readonly (KeyCode Key, PadButtons Pad)[] Directions =
        {
            (KeyCode.UpArrow, PadButtons.DpadUp), (KeyCode.DownArrow, PadButtons.DpadDown),
            (KeyCode.LeftArrow, PadButtons.DpadLeft), (KeyCode.RightArrow, PadButtons.DpadRight),
        };
        // Each press with the game speed from before it: the arrow keys and the d-pad also change the speed.
        private static readonly List<(PadButtons Pad, float Speed, float LastSpeed)> entered = new();

        // Whether the Path traced lighting row is in the ReShade tab. Off, the traced shot is off too.
        public static bool Shown => shown.Value;

        // The Path traced lighting row.
        public static bool Enabled
        {
            get => shown.Value && enabled.Value;
            set
            {
                enabled.Value = value;
                MelonPreferences.Save();
            }
        }

        public static bool TracedReflections
        {
            get => tracedReflections.Value;
            set
            {
                tracedReflections.Value = value;
                MelonPreferences.Save();
            }
        }

        public static float SurfaceGloss
        {
            get => Math.Clamp(float.IsFinite(surfaceGloss.Value) ? surfaceGloss.Value : TracedShot.DefaultSurfaceGloss, 0f, 1f);
            set
            {
                surfaceGloss.Value = value;
                MelonPreferences.Save();
            }
        }

        public static float ReflectedLight
        {
            get => Math.Clamp(float.IsFinite(reflectedLight.Value) ? reflectedLight.Value : TracedShot.DefaultReflectedLight, 1f, TracedShot.MaxReflectedLight);
            set
            {
                reflectedLight.Value = value;
                MelonPreferences.Save();
            }
        }

        public static int Bounces
        {
            get => Math.Clamp(bounces.Value, 1, TracedShot.MaxBounces);
            set
            {
                bounces.Value = value;
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

        // The Sun shadows row: the tracer's, or the game's own.
        public static bool TracedSunShadows
        {
            get => tracedSunShadows.Value;
            set
            {
                tracedSunShadows.Value = value;
                MelonPreferences.Save();
            }
        }

        // Whether the next screenshot is a traced one.
        public static bool Wanted
        {
            get
            {
                if (!Enabled)
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

        public static bool SceneCaptured => run is { Phase: ShotPhase.Tracing or ShotPhase.Finishing or ShotPhase.Done };

        public static bool HoldsEffectsOff => run is { EffectsHeld: true };

        public static bool Converged => run is { Phase: ShotPhase.Done };

        // The game runs again under a shot that paused it.
        public static bool Unpaused => run != null && Time.timeScale != 0f;

        // When the phase began: the hold, the scene's request, the trace or the converged light.
        public static float PhaseBegan => run?.PhaseBegan ?? 0f;

        // The shot captures or traces: no time limit holds meanwhile.
        public static bool Busy => run is { Phase: ShotPhase.Requesting or ShotPhase.Capturing or ShotPhase.Tracing or ShotPhase.Finishing };

        // Over the plain free camera, as the screenshot hotkey reads: the arrow keys or the d-pad, in order. speed and
        // lastSpeed are the game speed before this frame's hotkeys, which the sequence puts back once it is complete.
        public static void ReadSequence(float speed, float lastSpeed)
        {
            if (shown.Value)
                return;
            if (!freecamActive || Controls.TextCapture || CameraUi.PanelOpen || Lights.Moving || Posing.Editing || Posing.Moving
                || !PathPlayback.Idle || !ReplayPlayer.Idle || !ReplayRecorder.Idle)
            {
                entered.Clear();
                return;
            }
            var pressed = Gamepad.Current.Buttons & ~Gamepad.Previous.Buttons;
            foreach (var (key, pad) in Directions)
            {
                if (!Input.GetKeyDown(key) && (pressed & pad) == 0)
                    continue;
                entered.Add((pad, speed, lastSpeed));
                if (entered.Count > Sequence.Length)
                    entered.RemoveAt(0);
                if (!entered.Select(press => press.Pad).SequenceEqual(Sequence))
                    continue;
                Time.timeScale = entered[0].Speed;
                lastTimeScale = entered[0].LastSpeed;
                entered.Clear();
                shown.Value = true;
                MelonPreferences.Save();
                CameraUi.Toast("Path tracer unlocked.", whileHidden: true);
                return;
            }
        }

        public static void Load()
        {
            shown = MelonPreferences.CreateCategory("CameraToolsEnhancements").CreateEntry("TracedShotShown", false, dont_save_default: true);
            enabled = MelonPreferences.CreateCategory("CameraToolsEnhancements").CreateEntry("TracedShot", false,
                description: "Light the screenshot with the Enhancements add-on's path tracer: the shot pauses the game, captures the scene around "
                    + "the camera and waits for the traced lighting. Needs CameraToolsEnhancements.addon64.");
            bounceStrength = MelonPreferences.CreateCategory("CameraToolsEnhancements").CreateEntry("TracedShotBounceStrength", TracedShot.DefaultBounceStrength,
                description: $"How much light surfaces bounce onto each other in a traced shot, from 0 to {TracedShot.MaxBounceStrength:0}; 1 is the physical amount, and above 1 surfaces reflect that many times more, up to 90%.");
            bounces = MelonPreferences.CreateCategory("CameraToolsEnhancements").CreateEntry("TracedShotBounces", TracedShot.DefaultBounces,
                description: $"How many times light may bounce between surfaces in a traced shot, from 1 to {TracedShot.MaxBounces}.");
            tracedSunShadows = MelonPreferences.CreateCategory("CameraToolsEnhancements").CreateEntry("TracedShotSunShadows", true,
                description: "Sun shadows in a traced shot: true for the tracer's sharp ones from the traced scene, false for the game's own soft ones.");
            tracedReflections = MelonPreferences.CreateCategory("CameraToolsEnhancements").CreateEntry("TracedShotReflections", true,
                description: "Reflections in a traced shot: true traces them from the scene, false keeps the game's own soft ones.");
            surfaceGloss = MelonPreferences.CreateCategory("CameraToolsEnhancements").CreateEntry("TracedShotSurfaceGloss", TracedShot.DefaultSurfaceGloss,
                description: "How much smoother surfaces are than the game stores them in a traced shot, from 0 to 1.");
            reflectedLight = MelonPreferences.CreateCategory("CameraToolsEnhancements").CreateEntry("TracedShotReflectedLight", TracedShot.DefaultReflectedLight,
                description: $"How much brighter windows, lanterns and other light sources appear in reflections, from 1 to {TracedShot.MaxReflectedLight:0}.");
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
                            if (Retry(status, now, $"the scene capture failed {now - run.PhaseBegan:0.0} s after it was asked for; the add-on says: {line}"))
                                break;
                            return ($"Screenshot failed: the scene capture stopped{(TracedShot.Stopped(line) is { } why ? $": {why}" : "; see ReShade.log")}",
                                $"the Enhancements scene capture failed {now - run.PhaseBegan:0.0} s after it was asked for; the add-on says: {line}");
                    }
                    break;
                case ShotPhase.Tracing:
                    run.Stalled = TracedShot.Stalled(ref run.Baseline, status);
                    if (status.TraceSamples == 0 && now - run.PhaseBegan > TracedShot.NoSampleTimeout)
                    {
                        string none = $"no sample {now - run.PhaseBegan:0.0} s after the scene was ready; the add-on says: {Enhancements.Describe()}";
                        if (Retry(status, now, none))
                            return null;
                        return ("Screenshot failed: the trace did not start; see ReShade.log", none);
                    }
                    switch (TracedShot.Trace(run.Facts.Generation, status))
                    {
                        case TraceVerdict.Converged:
                            Log($"the lighting converged {now - run.PhaseBegan:0.0} s after the scene was ready: {status.TraceSamples} of {status.TraceTarget} samples at "
                                + $"{status.RenderWidth}x{status.RenderHeight}, error {status.TraceErrorX10000 / 100f:0.00}%, after {status.TraceRestarts - run.Starts} starts of "
                                + $"its samples, the last for {status.TraceRestartReason}.");
                            if (saveScene.Value)
                            {
                                Log(Enhancements.SaveTrace() ? "saving the converged trace for the Mac." : "the add-on refused to save the trace.");
                                TracedLights.Save();
                            }
                            ReShade.SetEffects(true);
                            run.EffectsHeld = false;
                            run.RestoredAt = presents;
                            Enter(ShotPhase.Finishing, now);
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
                            if (Retry(status, now, $"the traced scene went {status.TraceState} {now - run.PhaseBegan:0.0} s into its trace, at "
                                + $"{status.TraceSamples} of {status.TraceTarget} samples; the add-on says: {Enhancements.Describe()}"))
                                break;
                            return ($"Screenshot failed: the traced scene went {status.TraceState.ToString().ToLowerInvariant()}; see ReShade.log",
                                $"the traced scene went {status.TraceState} {now - run.PhaseBegan:0.0} s into its trace, at {status.TraceSamples} of {status.TraceTarget} "
                                    + $"samples; the add-on says: {Enhancements.Describe()}");
                    }
                    break;
            }
            if (run.Phase == ShotPhase.Finishing && presents - run.RestoredAt >= TracedShot.SettledPresents && now - run.PhaseBegan >= TracedShot.EffectsSettle)
            {
                CameraUi.ClearToast();
                Enter(ShotPhase.Done, now);
                return null;
            }
            if (TracedShot.Progress(run.Phase, status, run.Stalled, now - run.PhaseBegan) is { } progress)
                CameraUi.Toast(progress.Title, whileHidden: true, progress.Detail, progress.Fraction);
            return null;
        }

        // The first failure of a capture or a trace captures the scene again, so one bad capture costs time, not the picture.
        // A scene that failed for its size comes back smaller, as the add-on halves the next capture's triangles.
        private static bool Retry(EnhancementsStatus status, float now, string why)
        {
            if (run.Retried)
                return false;
            run.Retried = true;
            Log($"{why}; capturing the scene again.");
            kept = null;
            run.Reused = false;
            run.Before = status.TraceGeneration;
            Enter(ShotPhase.Requesting, now);
            return true;
        }

        // The picture is on disk: what the notice and the log add about its lighting.
        public static string Saved()
        {
            if (run == null)
                return "";
            run.Saved = true;
            return ", traced";
        }

        // Puts back what the shot changed, however it ended: the add-on's trace, RTGI, the time of day and the game's speed.
        // The scene stays for the next shot only after a saved picture, and only while the game stays paused.
        public static void End()
        {
            var ended = run;
            run = null;
            if (ended == null)
                return;
            Lights.WorldLightsHeld = false;
            if (ended.EffectsHeld)
                ReShade.SetEffects(true);
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
                {
                    Enhancements.SetShot(false);
                    Enhancements.SetShotLights(null);
                }
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

        // CameraTools' lights that light the world, the brightest at the camera first, as the add-on takes them.
        private static EnhancementsShotLight[] ShotLights()
        {
            var eye = CameraTools.maincam ? CameraTools.maincam.transform.position : Vector3.zero;
            return Lights.WorldLights().Select(light => ToShotLight(light, 0)).OrderByDescending(l => (l.R + l.G + l.B) / Math.Max((new Vector3(l.X, l.Y, l.Z) - eye).sqrMagnitude, 0.01f))
                .Take(EnhancementsShotLight.Max).ToArray();
        }

        // The game's own lights near the camera, nearest first, as they are at the capture: the add-on bakes them for the
        // gather's hits that no face shows, and the game keeps drawing them. Lamp scripts move their intensities with the
        // time of day, so this reads them after the time is locked.
        private static EnhancementsShotLight[] GameLights()
        {
            var camera = CameraTools.maincam;
            if (!camera)
                return Array.Empty<EnhancementsShotLight>();
            var eye = camera.transform.position;
            var lights = UnityEngine.Object.FindObjectsOfType(Il2CppType.Of<Light>()).Select(o => o.Cast<Light>())
                .Where(light => light.isActiveAndEnabled && !Lights.IsOurs(light) && light.intensity > 0 && light.type is LightType.Point or LightType.Spot)
                .Select(light => (light, distance: Vector3.Distance(light.transform.position, eye)))
                .Where(l => l.distance <= GameLightReach).OrderBy(l => l.distance).Take(EnhancementsShotLight.MaxGame)
                .Select(l => ToShotLight(l.light, l.light.shadows != LightShadows.None ? EnhancementsShotLight.Shadows : 0)).ToArray();
            Log($"baking {lights.Length} of the game's lights within {GameLightReach:0} m for the bounce.");
            return lights;
        }

        internal static EnhancementsShotLight ToShotLight(Light light, uint flags)
        {
            var t = light.transform;
            var p = t.position;
            var f = t.forward;
            var c = light.color;
            bool spot = light.type == LightType.Spot;
            return new EnhancementsShotLight
            {
                X = p.x, Y = p.y, Z = p.z, Range = light.range,
                R = c.r * light.intensity, G = c.g * light.intensity, B = c.b * light.intensity,
                CosOuter = spot ? MathF.Cos(light.spotAngle * MathF.PI / 360f) : -2f,
                DirectionX = f.x, DirectionY = f.y, DirectionZ = f.z,
                CosInner = spot ? MathF.Cos(light.spotInnerAngle * MathF.PI / 360f) : -2f,
                Reserved0 = flags,
            };
        }

        // The preset, the size and the light have settled: the add-on traces from here, with RTGI off, the kept scene or a
        // new one.
        private static void Begin(EnhancementsStatus status, bool still, float now)
        {
            run.Owns = true;
            var lights = ShotLights();
            Enhancements.SetShotLights(lights);
            Enhancements.SetShot(true, 0, TracedShot.BudgetMs, BounceStrength,
                TracedSunShadows ? EnhancementsTraceOutputs.Ao | EnhancementsTraceOutputs.Sun : EnhancementsTraceOutputs.Ao,
                TracedReflections, SurfaceGloss, ReflectedLight, (uint)Bounces);
            if (lights.Length > 0)
                Log($"tracing {lights.Length} of CameraTools' lights: " + string.Join("; ", lights.Select(l =>
                    $"({l.X:0.0}, {l.Y:0.0}, {l.Z:0.0}) range {l.Range:0.#}{(l.CosOuter >= -1f ? " spot" : "")}")) + ".");
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
            {
                kept = null;
                Enhancements.SetGameLights(GameLights());
            }
            ReShade.SetEffects(false);
            run.EffectsHeld = true;
            Enter(run.Reused ? ShotPhase.Tracing : ShotPhase.Requesting, now);
        }

        private static void Enter(ShotPhase phase, float now)
        {
            // The scene's faces take the lights as the game draws them; from here the shot draws them itself.
            Lights.WorldLightsHeld = phase is ShotPhase.Tracing or ShotPhase.Finishing or ShotPhase.Done;
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
                EnhancementsCamera.Light, LightsHash());
        }

        // Which lights the scene's faces saw: other lights make its pictures out of date.
        private static int LightsHash()
        {
            var hash = new HashCode();
            foreach (var l in ShotLights())
            {
                hash.Add(l.X);
                hash.Add(l.Y);
                hash.Add(l.Z);
                hash.Add(l.Range);
                hash.Add(l.R);
                hash.Add(l.G);
                hash.Add(l.B);
                hash.Add(l.CosOuter);
                hash.Add(l.CosInner);
                hash.Add(l.DirectionX);
                hash.Add(l.DirectionY);
                hash.Add(l.DirectionZ);
            }
            return hash.ToHashCode();
        }

        private static void Log(string message) => Melon<CameraTools>.Logger.Msg($"Traced shot: {message}");
    }
}
