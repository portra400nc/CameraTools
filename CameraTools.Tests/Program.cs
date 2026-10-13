// The traced screenshot's decisions (CameraTools/TracedShot.cs) and the Photoreal binding's layout against
// Photoreal/photoreal.h's literal sizes. No game, no ReShade, no add-on: run with dotnet run.
using System.Runtime.InteropServices;
using CameraTools;
using CameraToolsPhotoreal;
using Vec3 = System.Numerics.Vector3;

int failures = 0;

void Check(string name, bool ok)
{
    Console.WriteLine($"{(ok ? "PASS" : "FAIL")} {name}");
    failures += ok ? 0 : 1;
}

int Offset<T>(string field) => (int)Marshal.OffsetOf<T>(field);

// The binding: what photoreal.h's static_asserts say of version 10.
Check("the binding's status is photoreal.h's 144 bytes, with the restarts at 132, their reason at 136 and the shot at 140",
    Marshal.SizeOf<PhotorealStatus>() == 144 && Offset<PhotorealStatus>(nameof(PhotorealStatus.TraceTarget)) == 116
    && Offset<PhotorealStatus>(nameof(PhotorealStatus.TraceLight)) == 128 && Offset<PhotorealStatus>(nameof(PhotorealStatus.TraceRestarts)) == 132
    && Offset<PhotorealStatus>(nameof(PhotorealStatus.TraceRestartReason)) == 136 && Offset<PhotorealStatus>(nameof(PhotorealStatus.ShotActive)) == 140);
Check("the binding's shot is 16 bytes, its settings 236 and its scene request 20",
    Marshal.SizeOf<PhotorealShot>() == 16 && Offset<PhotorealShot>(nameof(PhotorealShot.BudgetMs)) == 12 && Marshal.SizeOf<PhotorealSettings>() == 236
    && Marshal.SizeOf<PhotorealSceneRequest>() == 20);
Check("the restart reasons and the scene states count as photoreal.h's", (uint)PhotorealTraceRestart.Scene == 1 && (uint)PhotorealTraceRestart.Camera == 4
    && (uint)PhotorealTraceRestart.Light == 6 && (uint)PhotorealTraceState.Ready == 3 && (uint)PhotorealTraceState.Failed == 5
    && (uint)PhotorealSceneFlags.Release == 32 && (uint)PhotorealSceneFlags.Light == 64);
Check("the shot's capture names all seven faces, the view among them, since a traced capture of no face is the self-test scene",
    TracedShot.AllFaces == 0x7F && (TracedShot.AllFaces & PhotorealFaces.Bit(PhotorealFace.View)) != 0);

// The sky's light.
var dawn = new SkyLight(new Vec3(0.239f, 0.574f, -0.784f), new Vec3(0.303f, 0.808f, 1.331f), new Vec3(0.24f, 0.53f, 0.81f), 0f);
var later = dawn with { SunColor = new Vec3(0.301f, 0.805f, 1.327f) };
Check("a light is near itself, and build 210's dawn a moment later is not still but may use a scene again",
    dawn.Near(dawn, TracedShot.StillTolerance) && !dawn.Near(later, TracedShot.StillTolerance) && dawn.Near(later, TracedShot.ReuseTolerance));
Check("a sun a degree off, a tenth more wetness, or an ambient light 5% dimmer is another light",
    !dawn.Near(dawn with { TowardSun = new Vec3(0.239f, 0.574f + 0.03f, -0.784f) }, TracedShot.ReuseTolerance)
    && !dawn.Near(dawn with { Wetness = 0.1f }, TracedShot.ReuseTolerance) && !dawn.Near(dawn with { Ambient = dawn.Ambient * 0.95f }, TracedShot.ReuseTolerance));
var bright = dawn with { SunColor = new Vec3(4f, 3.9f, 1.6f) };
Check("a bright color counts by its share: 1% of 4 is near at 2%, 5% is not",
    bright.Near(bright with { SunColor = new Vec3(4.04f, 3.9f, 1.6f) }, TracedShot.ReuseTolerance)
    && !bright.Near(bright with { SunColor = new Vec3(4.2f, 3.9f, 1.6f) }, TracedShot.ReuseTolerance));
{
    // A sun that moves 0.00004 a frame for 10 frames, then stops: no frame is far from the last, the third is from the first.
    var still = new StillLight(dawn, 10f);
    int restarts = 0;
    float since = still.Since;
    for (int frame = 1; frame <= 10; frame++)
    {
        var light = dawn with { TowardSun = dawn.TowardSun + new Vec3(0.00004f * frame, 0, 0) };
        still = still.Next(light, 10f + frame / 30f);
        restarts += still.Since != since ? 1 : 0;
        since = still.Since;
    }
    Check("a slow drift starts the still wait again each time it has gone past the tolerance from where the wait began", restarts == 3 && !still.Held(10.4f));
    var stopped = still.Reference;
    still = still.Next(stopped, 10.5f).Next(stopped, 10.8f);
    Check("a light that stopped has held still half a second after its last start", !still.Held(still.Since + 0.49f) && still.Held(still.Since + 0.5f));
}

// A kept scene.
var kept = new SceneFacts(4, new Vec3(10, 2, -5), new Vec3(-1024, 0, 2048), 0, 7, dawn);
string Unusable(SceneFacts now, PhotorealTraceState state = PhotorealTraceState.Ready) => TracedShot.Unusable(kept, now, state);
Check("a kept scene serves a shot from the same spot, and one 0.25 m away under a slightly other light",
    Unusable(kept) == null && Unusable(kept with { Camera = new Vec3(10.25f, 2, -5), Light = later }) == null);
Check("not a camera that moved further", Unusable(kept with { Camera = new Vec3(10.3f, 2.1f, -5) }) == "the camera moved 0.32 m");
Check("not a scene the add-on calls stale, failed or gone", Unusable(kept, PhotorealTraceState.Stale) == "the add-on's scene is stale"
    && Unusable(kept, PhotorealTraceState.Failed) == "the add-on's scene is failed" && Unusable(kept, PhotorealTraceState.None) == "the add-on's scene is none");
Check("not another caller's scene", Unusable(kept with { Generation = 5 }) == "the add-on holds another scene");
Check("not after a world shift, though the camera's scene position is the same", Unusable(kept with { Origin = new Vec3(0, 0, 2048) }) == "the world's origin shifted");
Check("not after the weather, the pose or the light changed", Unusable(kept with { Weather = 3 }) == "the weather was changed"
    && Unusable(kept with { Pose = 8 }) == "the pose was changed" && Unusable(kept with { Light = dawn with { SunColor = default } }) == "the sky's light changed");
Check("a camera at no position is no camera within reach", Unusable(kept with { Camera = new Vec3(float.NaN, 2, -5) }) != null);
Check("a kept scene is released when the game runs, the free camera goes off or the add-on unloads, and kept while paused in the free camera",
    TracedShot.Release(true, true, true) == null && TracedShot.Release(true, true, false) == "the game was unpaused"
    && TracedShot.Release(true, false, true) == "the free camera is off" && TracedShot.Release(false, true, true) == "the Photoreal add-on was unloaded");

// A capture, as the add-on's status shows it present after present.
PhotorealStatus Status(PhotorealTraceState state, uint generation, uint remaining = 0, uint samples = 0, uint target = 64, uint converged = 0, uint shot = 1,
    uint restarts = 1, PhotorealTraceRestart reason = PhotorealTraceRestart.Scene, uint passes = 0x80)
    => new()
    {
        TraceState = state, TraceGeneration = generation, SceneRemaining = remaining, TraceSamples = samples, TraceTarget = target, TraceConverged = converged,
        ShotActive = shot, TraceRestarts = restarts, TraceRestartReason = reason, PassesRan = passes,
    };
{
    bool seen = false;
    var verdicts = new List<CaptureVerdict>
    {
        // The status still shows the failed scene of an earlier capture, then this capture's faces, build and upload.
        TracedShot.Capture(ref seen, 4, Status(PhotorealTraceState.Failed, 4)),
        TracedShot.Capture(ref seen, 4, Status(PhotorealTraceState.Building, 4, remaining: 7)),
        TracedShot.Capture(ref seen, 4, Status(PhotorealTraceState.Building, 4)),
        TracedShot.Capture(ref seen, 4, Status(PhotorealTraceState.Uploading, 5)),
        TracedShot.Capture(ref seen, 4, Status(PhotorealTraceState.Ready, 5)),
    };
    Check("a capture is ready at a ready scene of a new generation, and an old failure before the add-on took the request is not its own",
        verdicts.SequenceEqual(new[] { CaptureVerdict.Waiting, CaptureVerdict.Waiting, CaptureVerdict.Waiting, CaptureVerdict.Waiting, CaptureVerdict.Ready }));
    seen = false;
    Check("the ready scene from before the capture is not the capture's", TracedShot.Capture(ref seen, 4, Status(PhotorealTraceState.Ready, 4)) == CaptureVerdict.Waiting && !seen);
    Check("a capture that stopped, or whose scene was released, has failed once the status had shown it",
        TracedShot.Capture(ref seen, 4, Status(PhotorealTraceState.Building, 4, remaining: 5)) == CaptureVerdict.Waiting
        && TracedShot.Capture(ref seen, 4, Status(PhotorealTraceState.Failed, 4, remaining: 5)) == CaptureVerdict.Failed
        && TracedShot.Capture(ref seen, 4, Status(PhotorealTraceState.None, 4)) == CaptureVerdict.Failed);
}

// The trace.
Check("the lighting has converged only on a frame that ran with the shot",
    TracedShot.Trace(5, Status(PhotorealTraceState.Ready, 5, samples: 64, converged: 1)) == TraceVerdict.Converged
    && TracedShot.Trace(5, Status(PhotorealTraceState.Ready, 5, samples: 64, converged: 1, shot: 0)) == TraceVerdict.Waiting
    && TracedShot.Trace(5, Status(PhotorealTraceState.Ready, 5, samples: 40)) == TraceVerdict.Waiting);
Check("a scene that went stale or failed, or another scene, is lost", TracedShot.Trace(5, Status(PhotorealTraceState.Stale, 5)) == TraceVerdict.Lost
    && TracedShot.Trace(5, Status(PhotorealTraceState.Failed, 5)) == TraceVerdict.Lost && TracedShot.Trace(5, Status(PhotorealTraceState.Ready, 6)) == TraceVerdict.Lost);
{
    uint baseline = 3;
    bool early = TracedShot.Stalled(ref baseline, Status(PhotorealTraceState.Ready, 5, restarts: 10));
    bool stalled = TracedShot.Stalled(ref baseline, Status(PhotorealTraceState.Ready, 5, restarts: 11));
    bool sampled = TracedShot.Stalled(ref baseline, Status(PhotorealTraceState.Ready, 5, samples: 2, restarts: 400));
    bool again = TracedShot.Stalled(ref baseline, Status(PhotorealTraceState.Ready, 5, restarts: 407));
    Check("no sample accumulates once the samples started 8 times without one, counted from the last start that kept a sample",
        !early && stalled && !sampled && baseline == 400 && !again && TracedShot.Stalled(ref baseline, Status(PhotorealTraceState.Ready, 5, restarts: 408)));
}

// What the screen and a timeout say.
var tracing = Status(PhotorealTraceState.Ready, 5, samples: 12);
var changing = Status(PhotorealTraceState.Ready, 5, restarts: 2910, reason: PhotorealTraceRestart.Light);
Check("the notices: the capture's direction, its build and upload, the samples, and what keeps changing in place of 0 samples",
    TracedShot.Progress(ShotPhase.Capturing, Status(PhotorealTraceState.Building, 4, remaining: 5), false) == "Capturing the scene 3/7"
    && TracedShot.Progress(ShotPhase.Capturing, Status(PhotorealTraceState.Building, 4, remaining: 7), false) == "Capturing the scene 1/7"
    && TracedShot.Progress(ShotPhase.Capturing, Status(PhotorealTraceState.Building, 4), false) == "Building the scene"
    && TracedShot.Progress(ShotPhase.Capturing, Status(PhotorealTraceState.Uploading, 5), false) == "Sending the scene to the GPU"
    && TracedShot.Progress(ShotPhase.Requesting, tracing, false) == "Capturing the scene" && TracedShot.Progress(ShotPhase.Tracing, tracing, false) == "Lighting 12/64"
    && TracedShot.Progress(ShotPhase.Tracing, changing, true) == "Lighting: the light keeps changing"
    && TracedShot.Progress(ShotPhase.Settling, tracing, false) == null && TracedShot.Progress(ShotPhase.Done, tracing, false) == null);
Check("a timeout names what the shot waited for, with why no sample came in",
    TracedShot.Waiting(ShotPhase.Settling, tracing, false) == null && TracedShot.Waiting(ShotPhase.Done, tracing, false) == null
    && TracedShot.Waiting(ShotPhase.Requesting, tracing, false) == "the Photoreal add-on to take a scene capture"
    && TracedShot.Waiting(ShotPhase.Capturing, Status(PhotorealTraceState.Building, 4, remaining: 5), false) == "the Photoreal scene to be captured (5 of 7 directions left)"
    && TracedShot.Waiting(ShotPhase.Capturing, Status(PhotorealTraceState.Building, 4), false) == "the Photoreal scene to be captured (building)"
    && TracedShot.Waiting(ShotPhase.Tracing, tracing, false) == "the lighting to converge (12 of 64 samples)"
    && TracedShot.Waiting(ShotPhase.Tracing, changing, true) == "the lighting to converge (0 of 64 samples; the light keeps changing, 2910 starts)");
Check("a failed capture's reason comes from the add-on's status line",
    TracedShot.Stopped("1920x1200 found gbuffer combine; ran trace@combine; scene stopped: the camera moved; error texture") == "the camera moved"
    && TracedShot.Stopped("found nothing; scene stopped: pos-x was not saved within 900 presents") == "pos-x was not saved within 900 presents"
    && TracedShot.Stopped("1920x1200 found gbuffer; scene 3/7") == null);
Check("each restart reason has its words", TracedShot.Changing(PhotorealTraceRestart.Camera) == "the camera keeps moving"
    && TracedShot.Changing(PhotorealTraceRestart.SunDirection) == "the sun keeps moving" && TracedShot.Changing(PhotorealTraceRestart.RenderSize) == "the render size keeps changing"
    && TracedShot.Changing(PhotorealTraceRestart.Settings) == "the Photoreal settings keep changing" && TracedShot.Changing(PhotorealTraceRestart.Scene) == "the scene keeps changing"
    && TracedShot.Changing(PhotorealTraceRestart.None) == "the samples keep starting again");
Check("at the timeout a frame traced from 16 samples or more is saved as it is; one with fewer, one the trace pass does not draw, or a lost scene is not",
    TracedShot.SaveAsIs(ShotPhase.Tracing, 5, Status(PhotorealTraceState.Ready, 5, samples: 16)) && !TracedShot.SaveAsIs(ShotPhase.Tracing, 5, Status(PhotorealTraceState.Ready, 5, samples: 15))
    && !TracedShot.SaveAsIs(ShotPhase.Tracing, 5, Status(PhotorealTraceState.Ready, 5, samples: 40, passes: 0))
    && !TracedShot.SaveAsIs(ShotPhase.Tracing, 5, Status(PhotorealTraceState.Ready, 5, samples: 40, shot: 0))
    && !TracedShot.SaveAsIs(ShotPhase.Tracing, 5, Status(PhotorealTraceState.Stale, 5, samples: 40))
    && !TracedShot.SaveAsIs(ShotPhase.Capturing, 5, Status(PhotorealTraceState.Ready, 5, samples: 40))
    && !TracedShot.SaveAsIs(ShotPhase.Done, 5, Status(PhotorealTraceState.Ready, 5, samples: 40)));

Console.WriteLine($"CameraTools tests: {failures} failed");
return failures == 0 ? 0 : 1;
