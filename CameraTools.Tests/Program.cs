// The traced screenshot's decisions (CameraTools/TracedShot.cs), the Enhancements category's rules and its settings for
// the add-on (CameraTools/EnhancementsModel.cs), the presets' decisions (CameraTools/GraphicsModel.cs), and the
// Enhancements binding's layout against Enhancements/enhancements.h's literal sizes, and the camera path's motion
// (CameraTools/PathSampler.cs), the long exposure's settings (CameraTools/Exposure.cs), and a replay's timeline and
// words (CameraTools/ReplayModel.cs), and where a supersampled screenshot's samples fall (CameraTools/SupersamplingModel.cs), and the default bindings and
// their migration (CameraTools/ControlsModel.cs). No game, no ReShade, no add-on: run with dotnet run.
using System.Runtime.InteropServices;
using CameraTools;
using CameraToolsEnhancements;
using Quat = System.Numerics.Quaternion;
using Vec3 = System.Numerics.Vector3;

int failures = 0;

void Check(string name, bool ok)
{
    Console.WriteLine($"{(ok ? "PASS" : "FAIL")} {name}");
    failures += ok ? 0 : 1;
}

int Offset<T>(string field) => (int)Marshal.OffsetOf<T>(field);

bool Fails(Action read)
{
    try
    {
        read();
        return false;
    }
    catch (InvalidDataException)
    {
        return true;
    }
}

// The binding: what enhancements.h's static_asserts say of version 14.
Check("the binding's status is enhancements.h's 152 bytes, with the restarts at 132, their reason at 136, the shot at 140, the saved frames at 144 and the focus at 148",
    Marshal.SizeOf<EnhancementsStatus>() == 152 && Offset<EnhancementsStatus>(nameof(EnhancementsStatus.TraceTarget)) == 116
    && Offset<EnhancementsStatus>(nameof(EnhancementsStatus.TraceLight)) == 128 && Offset<EnhancementsStatus>(nameof(EnhancementsStatus.TraceRestarts)) == 132
    && Offset<EnhancementsStatus>(nameof(EnhancementsStatus.TraceRestartReason)) == 136 && Offset<EnhancementsStatus>(nameof(EnhancementsStatus.ShotActive)) == 140
    && Offset<EnhancementsStatus>(nameof(EnhancementsStatus.FramesSaved)) == 144 && Offset<EnhancementsStatus>(nameof(EnhancementsStatus.DofFocusMeters)) == 148);
Check("the binding's shot is 16 bytes, its settings 320 with the depth of field at 236 and its highlights at 68, scatter at 76 and bokeh contrast at 80 in it, and its scene request 20",
    Marshal.SizeOf<EnhancementsShot>() == 16 && Offset<EnhancementsShot>(nameof(EnhancementsShot.BudgetMs)) == 12 && Marshal.SizeOf<EnhancementsSettings>() == 320
    && Offset<EnhancementsSettings>(nameof(EnhancementsSettings.DepthOfField)) == 236 && Marshal.SizeOf<EnhancementsDepthOfField>() == 84
    && Offset<EnhancementsDepthOfField>(nameof(EnhancementsDepthOfField.HighlightThreshold)) == 68
    && Offset<EnhancementsDepthOfField>(nameof(EnhancementsDepthOfField.Scatter)) == 76
    && Offset<EnhancementsDepthOfField>(nameof(EnhancementsDepthOfField.BokehContrast)) == 80
    && Marshal.SizeOf<EnhancementsSceneRequest>() == 20);
Check("the restart reasons and the scene states count as enhancements.h's", (uint)EnhancementsTraceRestart.Scene == 1 && (uint)EnhancementsTraceRestart.Camera == 4
    && (uint)EnhancementsTraceRestart.Light == 6 && (uint)EnhancementsTraceState.Ready == 3 && (uint)EnhancementsTraceState.Failed == 5
    && (uint)EnhancementsSceneFlags.Release == 32 && (uint)EnhancementsSceneFlags.Light == 64);
Check("the shot's capture names all seven faces, the view among them, since a traced capture of no face is the self-test scene",
    TracedShot.AllFaces == 0x7F && (TracedShot.AllFaces & EnhancementsFaces.Bit(EnhancementsFace.View)) != 0);

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
string Unusable(SceneFacts now, EnhancementsTraceState state = EnhancementsTraceState.Ready) => TracedShot.Unusable(kept, now, state);
Check("a kept scene serves a shot from the same spot, and one 0.25 m away under a slightly other light",
    Unusable(kept) == null && Unusable(kept with { Camera = new Vec3(10.25f, 2, -5), Light = later }) == null);
Check("not a camera that moved further", Unusable(kept with { Camera = new Vec3(10.3f, 2.1f, -5) }) == "the camera moved 0.32 m");
Check("not a scene the add-on calls stale, failed or gone", Unusable(kept, EnhancementsTraceState.Stale) == "the add-on's scene is stale"
    && Unusable(kept, EnhancementsTraceState.Failed) == "the add-on's scene is failed" && Unusable(kept, EnhancementsTraceState.None) == "the add-on's scene is none");
Check("not another caller's scene", Unusable(kept with { Generation = 5 }) == "the add-on holds another scene");
Check("not after a world shift, though the camera's scene position is the same", Unusable(kept with { Origin = new Vec3(0, 0, 2048) }) == "the world's origin shifted");
Check("not after the weather, the pose or the light changed", Unusable(kept with { Weather = 3 }) == "the weather was changed"
    && Unusable(kept with { Pose = 8 }) == "the pose was changed" && Unusable(kept with { Light = dawn with { SunColor = default } }) == "the sky's light changed");
Check("a camera at no position is no camera within reach", Unusable(kept with { Camera = new Vec3(float.NaN, 2, -5) }) != null);
Check("a kept scene is released when the game runs, the free camera goes off or the add-on unloads, and kept while paused in the free camera",
    TracedShot.Release(true, true, true) == null && TracedShot.Release(true, true, false) == "the game was unpaused"
    && TracedShot.Release(true, false, true) == "the free camera is off" && TracedShot.Release(false, true, true) == "the Enhancements add-on was unloaded");

// A capture, as the add-on's status shows it present after present.
EnhancementsStatus Status(EnhancementsTraceState state, uint generation, uint remaining = 0, uint samples = 0, uint target = 64, uint converged = 0, uint shot = 1,
    uint restarts = 1, EnhancementsTraceRestart reason = EnhancementsTraceRestart.Scene, uint passes = 0x80)
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
        TracedShot.Capture(ref seen, 4, Status(EnhancementsTraceState.Failed, 4)),
        TracedShot.Capture(ref seen, 4, Status(EnhancementsTraceState.Building, 4, remaining: 7)),
        TracedShot.Capture(ref seen, 4, Status(EnhancementsTraceState.Building, 4)),
        TracedShot.Capture(ref seen, 4, Status(EnhancementsTraceState.Uploading, 5)),
        TracedShot.Capture(ref seen, 4, Status(EnhancementsTraceState.Ready, 5)),
    };
    Check("a capture is ready at a ready scene of a new generation, and an old failure before the add-on took the request is not its own",
        verdicts.SequenceEqual(new[] { CaptureVerdict.Waiting, CaptureVerdict.Waiting, CaptureVerdict.Waiting, CaptureVerdict.Waiting, CaptureVerdict.Ready }));
    seen = false;
    Check("the ready scene from before the capture is not the capture's", TracedShot.Capture(ref seen, 4, Status(EnhancementsTraceState.Ready, 4)) == CaptureVerdict.Waiting && !seen);
    Check("a capture that stopped, or whose scene was released, has failed once the status had shown it",
        TracedShot.Capture(ref seen, 4, Status(EnhancementsTraceState.Building, 4, remaining: 5)) == CaptureVerdict.Waiting
        && TracedShot.Capture(ref seen, 4, Status(EnhancementsTraceState.Failed, 4, remaining: 5)) == CaptureVerdict.Failed
        && TracedShot.Capture(ref seen, 4, Status(EnhancementsTraceState.None, 4)) == CaptureVerdict.Failed);
}

// The trace.
Check("the lighting has converged only on a frame that ran with the shot",
    TracedShot.Trace(5, Status(EnhancementsTraceState.Ready, 5, samples: 64, converged: 1)) == TraceVerdict.Converged
    && TracedShot.Trace(5, Status(EnhancementsTraceState.Ready, 5, samples: 64, converged: 1, shot: 0)) == TraceVerdict.Waiting
    && TracedShot.Trace(5, Status(EnhancementsTraceState.Ready, 5, samples: 40)) == TraceVerdict.Waiting);
Check("a scene that went stale or failed, or another scene, is lost", TracedShot.Trace(5, Status(EnhancementsTraceState.Stale, 5)) == TraceVerdict.Lost
    && TracedShot.Trace(5, Status(EnhancementsTraceState.Failed, 5)) == TraceVerdict.Lost && TracedShot.Trace(5, Status(EnhancementsTraceState.Ready, 6)) == TraceVerdict.Lost);
{
    uint baseline = 3;
    bool early = TracedShot.Stalled(ref baseline, Status(EnhancementsTraceState.Ready, 5, restarts: 10));
    bool stalled = TracedShot.Stalled(ref baseline, Status(EnhancementsTraceState.Ready, 5, restarts: 11));
    bool sampled = TracedShot.Stalled(ref baseline, Status(EnhancementsTraceState.Ready, 5, samples: 2, restarts: 400));
    bool again = TracedShot.Stalled(ref baseline, Status(EnhancementsTraceState.Ready, 5, restarts: 407));
    Check("no sample accumulates once the samples started 8 times without one, counted from the last start that kept a sample",
        !early && stalled && !sampled && baseline == 400 && !again && TracedShot.Stalled(ref baseline, Status(EnhancementsTraceState.Ready, 5, restarts: 408)));
}

// What the screen and a timeout say.
var tracing = Status(EnhancementsTraceState.Ready, 5, samples: 12);
var changing = Status(EnhancementsTraceState.Ready, 5, restarts: 2910, reason: EnhancementsTraceRestart.Light);
Check("the notices: the capture's direction, its build and upload, the samples, and what keeps changing in place of 0 samples",
    TracedShot.Progress(ShotPhase.Capturing, Status(EnhancementsTraceState.Building, 4, remaining: 5), false) == "Capturing the scene 3/7"
    && TracedShot.Progress(ShotPhase.Capturing, Status(EnhancementsTraceState.Building, 4, remaining: 7), false) == "Capturing the scene 1/7"
    && TracedShot.Progress(ShotPhase.Capturing, Status(EnhancementsTraceState.Building, 4), false) == "Building the scene"
    && TracedShot.Progress(ShotPhase.Capturing, Status(EnhancementsTraceState.Uploading, 5), false) == "Sending the scene to the GPU"
    && TracedShot.Progress(ShotPhase.Requesting, tracing, false) == "Capturing the scene" && TracedShot.Progress(ShotPhase.Tracing, tracing, false) == "Lighting 12/64"
    && TracedShot.Progress(ShotPhase.Tracing, changing, true) == "Lighting: the light keeps changing"
    && TracedShot.Progress(ShotPhase.Settling, tracing, false) == null && TracedShot.Progress(ShotPhase.Done, tracing, false) == null);
Check("a timeout names what the shot waited for, with why no sample came in",
    TracedShot.Waiting(ShotPhase.Settling, tracing, false) == null && TracedShot.Waiting(ShotPhase.Done, tracing, false) == null
    && TracedShot.Waiting(ShotPhase.Requesting, tracing, false) == "the Enhancements add-on to take a scene capture"
    && TracedShot.Waiting(ShotPhase.Capturing, Status(EnhancementsTraceState.Building, 4, remaining: 5), false) == "the Enhancements scene to be captured (5 of 7 directions left)"
    && TracedShot.Waiting(ShotPhase.Capturing, Status(EnhancementsTraceState.Building, 4), false) == "the Enhancements scene to be captured (building)"
    && TracedShot.Waiting(ShotPhase.Tracing, tracing, false) == "the lighting to converge (12 of 64 samples)"
    && TracedShot.Waiting(ShotPhase.Tracing, changing, true) == "the lighting to converge (0 of 64 samples; the light keeps changing, 2910 starts)");
Check("a failed capture's reason comes from the add-on's status line",
    TracedShot.Stopped("1920x1200 found gbuffer combine; ran trace@combine; scene stopped: the camera moved; error texture") == "the camera moved"
    && TracedShot.Stopped("found nothing; scene stopped: pos-x was not saved within 900 presents") == "pos-x was not saved within 900 presents"
    && TracedShot.Stopped("1920x1200 found gbuffer; scene 3/7") == null);
Check("each restart reason has its words", TracedShot.Changing(EnhancementsTraceRestart.Camera) == "the camera keeps moving"
    && TracedShot.Changing(EnhancementsTraceRestart.SunDirection) == "the sun keeps moving" && TracedShot.Changing(EnhancementsTraceRestart.RenderSize) == "the render size keeps changing"
    && TracedShot.Changing(EnhancementsTraceRestart.Settings) == "the Enhancements settings keep changing" && TracedShot.Changing(EnhancementsTraceRestart.Scene) == "the scene keeps changing"
    && TracedShot.Changing(EnhancementsTraceRestart.None) == "the samples keep starting again");
Check("at the timeout a frame traced from 16 samples or more is saved as it is; one with fewer, one the trace pass does not draw, or a lost scene is not",
    TracedShot.SaveAsIs(ShotPhase.Tracing, 5, Status(EnhancementsTraceState.Ready, 5, samples: 16)) && !TracedShot.SaveAsIs(ShotPhase.Tracing, 5, Status(EnhancementsTraceState.Ready, 5, samples: 15))
    && !TracedShot.SaveAsIs(ShotPhase.Tracing, 5, Status(EnhancementsTraceState.Ready, 5, samples: 40, passes: 0))
    && !TracedShot.SaveAsIs(ShotPhase.Tracing, 5, Status(EnhancementsTraceState.Ready, 5, samples: 40, shot: 0))
    && !TracedShot.SaveAsIs(ShotPhase.Tracing, 5, Status(EnhancementsTraceState.Stale, 5, samples: 40))
    && !TracedShot.SaveAsIs(ShotPhase.Capturing, 5, Status(EnhancementsTraceState.Ready, 5, samples: 40))
    && !TracedShot.SaveAsIs(ShotPhase.Done, 5, Status(EnhancementsTraceState.Ready, 5, samples: 40)));

// The Enhancements category: its table, its values, and what the add-on gets for them.
EnhancementField Field(Enhancement id) => EnhancementsModel.Field(id);
var fresh = EnhancementValues.Defaults;
Check("the table holds 38 fields, each at its own id's place, with the master switch and seven effects' switches",
    EnhancementsModel.Fields.Length == 38 && EnhancementsModel.Fields.Select((field, at) => (int)field.Id == at).All(ok => ok)
    && string.Join(",", EnhancementsModel.Effects.Select(field => field.Label)) == "Ambient light,Contact shadows,Soft sun shadows,Haze,Light shafts,Wet surfaces,Tone map");
{
    var off = EnhancementsModel.Settings(fresh);
    Check("everything is off by default, the add-on's master switch too, and the path tracer has no part",
        !fresh.On(Enhancement.Enabled) && EnhancementsModel.Effects.All(field => !fresh.On(field.Id)) && off.Enabled == 0 && off.Ambient.Enabled == 0
        && off.ContactShadows.Enabled == 0 && off.SunShadows.Enabled == 0 && off.Atmosphere.Enabled == 0 && off.LightShafts.Enabled == 0 && off.Wetness.Enabled == 0
        && off.Tonemap.Enabled == 0 && off.Trace.Enabled == 0 && off.Gather.Enabled == 0 && off.View == EnhancementsView.Off && off.Flip == 1);
    Check("the defaults are the mockup's: the add-on's own, but for the AgX curve",
        off.Ambient is { Level: 0.6f, AoStrength: 0.5f, AoRadius: 1f, FoliageAoStrength: 0.5f } && off.ContactShadows is { Length: 0.6f, Strength: 1f, FoliageStrength: 0f }
        && off.SunShadows is { LightSize: 0.03f, Strength: 1f } && off.Atmosphere is { Density: 0.000325f, HeightFalloff: 0.02f, SunScatter: 1f, Anisotropy: 0.7f }
        && off.LightShafts is { Strength: 3f, Density: 0.006f, Height: 5f, Anisotropy: 0.6f, Distance: 150f, Patchiness: 1f, PatchSize: 6f, Ambient: 0.3f, Steps: 48 }
        && off.Wetness is { Wetness: 0f, Darkening: 0.35f, Puddles: 0.3f }
        && off.Tonemap is { Curve: EnhancementsCurve.AgX, ExposureEv: 0f, BloomStrength: 1f, Saturation: 1f, Contrast: 1f });
    Check("a field without a row keeps the add-on's default", off.ContactShadows.Thickness == 0.25f && off.SunShadows.MinPenumbra == 0.02f
        && off.Trace is { AoSamples: 64, SunSamples: 32, BudgetMs: 100f } && off.Gather is { Samples: 64, Filter: 2 });
}
{
    var set = fresh.With(Enhancement.Enabled, 1).With(Enhancement.AmbientLight, 1).With(Enhancement.AmbientLevel, 1.25f).With(Enhancement.AmbientOcclusion, 0.8f)
        .With(Enhancement.AmbientOcclusionRadius, 2.5f).With(Enhancement.AmbientFoliageOcclusion, 0.2f)
        .With(Enhancement.ContactShadows, 1).With(Enhancement.ContactShadowLength, 1.5f).With(Enhancement.ContactShadowStrength, 0.75f).With(Enhancement.ContactShadowFoliage, 0.25f)
        .With(Enhancement.SoftSunShadows, 1).With(Enhancement.SunShadowSoftness, 0.125f).With(Enhancement.SunShadowStrength, 0.5f)
        .With(Enhancement.Haze, 1).With(Enhancement.HazeDensity, 2f).With(Enhancement.HazeHeightFalloff, 0.125f).With(Enhancement.HazeSunGlow, 4.5f).With(Enhancement.HazeGlowTowardSun, 0.25f)
        .With(Enhancement.LightShafts, 1).With(Enhancement.LightShaftStrength, 7.5f).With(Enhancement.MistDensity, 0.032f).With(Enhancement.MistHeight, 12.5f)
        .With(Enhancement.LightShaftGlowTowardSun, 0.75f).With(Enhancement.LightShaftDistance, 600f).With(Enhancement.MistPatchiness, 0.5f).With(Enhancement.MistPatchSize, 40f)
        .With(Enhancement.MistSkyLight, 2.25f).With(Enhancement.LightShaftQuality, 2)
        .With(Enhancement.WetSurfaces, 1).With(Enhancement.Wetness, 3).With(Enhancement.WetDarkening, 0.5f).With(Enhancement.Puddles, 0.75f)
        .With(Enhancement.ToneMap, 1).With(Enhancement.ToneMapCurve, 2).With(Enhancement.ToneMapExposure, -1.5f).With(Enhancement.ToneMapBloom, 0f)
        .With(Enhancement.ToneMapSaturation, 1.25f).With(Enhancement.ToneMapContrast, 1.75f);
    var on = EnhancementsModel.Settings(set);
    Check("the master switch and Ambient light's rows reach the add-on's ambient block",
        on.Enabled == 1 && on.Ambient is { Enabled: 1, Level: 1.25f, AoStrength: 0.8f, AoRadius: 2.5f, FoliageAoStrength: 0.2f });
    Check("Contact shadows' and Soft sun shadows' rows reach theirs", on.ContactShadows is { Enabled: 1, Length: 1.5f, Strength: 0.75f, FoliageStrength: 0.25f, Thickness: 0.25f }
        && on.SunShadows is { Enabled: 1, LightSize: 0.125f, Strength: 0.5f, MinPenumbra: 0.02f });
    Check("Haze's Density counts in multiples of the default 0.000325",
        on.Atmosphere is { Enabled: 1, Density: 0.00065f, HeightFalloff: 0.125f, SunScatter: 4.5f, Anisotropy: 0.25f }
        && MathF.Abs(EnhancementsModel.Settings(fresh.With(Enhancement.HazeDensity, 10f)).Atmosphere.Density - 0.00325f) < 1e-9f
        && EnhancementsModel.Settings(fresh.With(Enhancement.HazeDensity, 0f)).Atmosphere.Density == 0f);
    Check("Light shafts' rows reach theirs, the mist's among them", on.LightShafts is
        { Enabled: 1, Strength: 7.5f, Density: 0.032f, Height: 12.5f, Anisotropy: 0.75f, Distance: 600f, Patchiness: 0.5f, PatchSize: 40f, Ambient: 2.25f, Steps: 96 });
    Check("Quality is Low, Medium or High: 24, 48 or 96 steps",
        string.Join(",", Field(Enhancement.LightShaftQuality).Options) == "Low,Medium,High"
        && string.Join(",", new[] { 0, 1, 2 }.Select(quality => EnhancementsModel.Settings(fresh.With(Enhancement.LightShaftQuality, quality)).LightShafts.Steps)) == "24,48,96");
    Check("Wetness follows the weather at 0, then 25%, 50%, 75% and soaked at 1",
        string.Join(",", Field(Enhancement.Wetness).Options) == "Follows the weather,25%,50%,75%,Soaked" && on.Wetness is { Enabled: 1, Wetness: 0.75f, Darkening: 0.5f, Puddles: 0.75f }
        && new[] { 0f, 0.25f, 0.5f, 0.75f, 1f }.Select((level, at) => EnhancementsModel.Settings(fresh.With(Enhancement.Wetness, at)).Wetness.Wetness == level).All(ok => ok));
    Check("Tone map's Curve is the game's, AgX or Neutral, and its rows reach the tone map block",
        on.Tonemap is { Enabled: 1, Curve: EnhancementsCurve.Neutral, ExposureEv: -1.5f, BloomStrength: 0f, Saturation: 1.25f, Contrast: 1.75f }
        && EnhancementsModel.Settings(fresh.With(Enhancement.ToneMapCurve, 0)).Tonemap.Curve == EnhancementsCurve.Game
        && string.Join(",", Field(Enhancement.ToneMapCurve).Options) == "Game,AgX,Neutral");
    Check("the trace and the gather stay off whatever is switched on", on.Trace.Enabled == 0 && on.Gather.Enabled == 0);
    Check("with the master switch off the add-on's is off, and an effect's switch stays as it is for when it comes back",
        EnhancementsModel.Settings(set.With(Enhancement.Enabled, 0)) is { Enabled: 0 } idle && idle.Atmosphere.Enabled == 1 && idle.Tonemap.BloomStrength == 0f);
}
Check("a value stays within its row's range, at the decimals the row shows",
    fresh.With(Enhancement.ToneMapExposure, 7f)[Enhancement.ToneMapExposure] == 3f && fresh.With(Enhancement.ToneMapExposure, -7f)[Enhancement.ToneMapExposure] == -3f
    && fresh.With(Enhancement.AmbientLevel, 0.6234f)[Enhancement.AmbientLevel] == 0.62f && fresh.With(Enhancement.LightShaftDistance, 437.4f)[Enhancement.LightShaftDistance] == 437f
    && fresh.With(Enhancement.SunShadowSoftness, 0f)[Enhancement.SunShadowSoftness] == 0.005f && fresh.With(Enhancement.MistDensity, 0.0126f)[Enhancement.MistDensity] == 0.013f
    && fresh.With(Enhancement.AmbientLevel, 0.6f + 0.05f)[Enhancement.AmbientLevel] == 0.65f);
Check("a switch is on or off, a choice one of its options, and a number that is none the default",
    fresh.With(Enhancement.Haze, 5f)[Enhancement.Haze] == 1f && fresh.With(Enhancement.LightShaftQuality, 9f)[Enhancement.LightShaftQuality] == 2f
    && fresh.With(Enhancement.Wetness, -1f)[Enhancement.Wetness] == 0f && fresh.With(Enhancement.ToneMapBloom, float.NaN)[Enhancement.ToneMapBloom] == 1f);
Check("a value set to what it is leaves the same values, and another value makes other ones",
    ReferenceEquals(fresh.With(Enhancement.Haze, 0f), fresh) && !fresh.With(Enhancement.Haze, 1f).Equals(fresh) && fresh.With(Enhancement.Haze, 1f).With(Enhancement.Haze, 0f).Equals(fresh));
Check("the rows' formats are the mockup's", Field(Enhancement.AmbientLevel).Format == "0.00" && Field(Enhancement.AmbientOcclusionRadius).Format == "0.0' m'"
    && Field(Enhancement.SunShadowSoftness).Format == "0.000" && Field(Enhancement.HazeDensity).Format == "0.0'×'" && Field(Enhancement.LightShaftDistance).Format == "0' m'"
    && Field(Enhancement.ToneMapExposure).Format == "0.0' EV'");

// The preferences: one entry a field, by the field's name.
Check("preferences that hold nothing give the defaults", EnhancementValues.From(_ => null).Equals(fresh));
{
    var stored = new Dictionary<string, float> { ["Enabled"] = 1f, ["Haze"] = 1f, ["HazeDensity"] = 25f, ["LightShaftQuality"] = 2f, ["ToneMapExposure"] = 0.5f };
    var read = EnhancementValues.From(field => stored.TryGetValue(field.Name, out float value) ? value : null);
    Check("each entry is read by its field's name, and a number past the row's range stops at its end",
        read.On(Enhancement.Enabled) && read.On(Enhancement.Haze) && !read.On(Enhancement.LightShafts) && read[Enhancement.HazeDensity] == 10f
        && read[Enhancement.LightShaftQuality] == 2f && read[Enhancement.ToneMapExposure] == 0.5f && read[Enhancement.AmbientLevel] == 0.6f);
}
Check("a choice's entry holds its option's label", Field(Enhancement.LightShaftQuality).Option("High") == 2f && Field(Enhancement.LightShaftQuality).Option("Ultra") == null
    && Field(Enhancement.Wetness).Option("Follows the weather") == 0f && Field(Enhancement.ToneMapCurve).Option("AgX") == 1f && Field(Enhancement.Haze).Option("On") == null);
{
    string text = fresh.With(Enhancement.Haze, 1f).With(Enhancement.HazeDensity, 2.5f).ToString();
    var skipped = new List<string>();
    Check("the values write as name=number pairs and read back the same",
        text.StartsWith("Enabled=0;AmbientLight=0;AmbientLevel=0.6;AmbientOcclusion=0.5;AmbientOcclusionRadius=1;") && text.Contains(";Haze=1;HazeDensity=2.5;")
        && text.EndsWith(";ToneMap=0;ToneMapCurve=1;ToneMapExposure=0;ToneMapBloom=1;ToneMapSaturation=1;ToneMapContrast=1")
        && EnhancementsModel.Parse(text, skipped).Equals(fresh.With(Enhancement.Haze, 1f).With(Enhancement.HazeDensity, 2.5f)) && skipped.Count == 0);
    var partial = EnhancementsModel.Parse("Haze=1; nonsense; Fog=3; HazeDensity=x; ToneMapBloom=0", skipped);
    Check("a text that leaves fields out takes their defaults, and names what is no field's",
        partial.Equals(fresh.With(Enhancement.Haze, 1f).With(Enhancement.ToneMapBloom, 0f)) && skipped.OrderBy(part => part).SequenceEqual(new[] { "Fog=3", "HazeDensity=x", "nonsense" }));
    Check("an empty text holds no values", EnhancementsModel.Parse("", skipped) == null && EnhancementsModel.Parse("7=1", new List<string>()) == null);
}

// What shows, what is greyed, and the heading's text.
{
    var haze = fresh.With(Enhancement.Haze, 1f);
    var running = haze.With(Enhancement.Enabled, 1f);
    Check("an effect's own rows show only while its switch is on, whatever the master switch says",
        !EnhancementsModel.Shown(Field(Enhancement.HazeDensity), fresh, false) && EnhancementsModel.Shown(Field(Enhancement.HazeDensity), haze, false)
        && EnhancementsModel.Shown(Field(Enhancement.HazeDensity), running, false) && !EnhancementsModel.Shown(Field(Enhancement.MistDensity), running, false));
    Check("every switch shows while the category is open", EnhancementsModel.Fields.Where(field => field.Kind == EnhancementKind.Switch)
        .All(field => EnhancementsModel.Shown(field, fresh, false)));
    Check("folded, none of the category's rows show", EnhancementsModel.Fields.All(field => !EnhancementsModel.Shown(field, running, true)));
    Check("every row but the master switch is greyed while the master switch is off",
        EnhancementsModel.Usable(Field(Enhancement.Enabled), fresh) && !EnhancementsModel.Usable(Field(Enhancement.Haze), haze)
        && !EnhancementsModel.Usable(Field(Enhancement.HazeDensity), haze) && EnhancementsModel.Fields.All(field => EnhancementsModel.Usable(field, running)));
    Check("the heading says Off, or On with the effects that are switched on",
        EnhancementsModel.Summary(fresh) == "Off" && EnhancementsModel.Summary(haze) == "Off" && EnhancementsModel.Summary(fresh.With(Enhancement.Enabled, 1f)) == "On · 0 effects"
        && EnhancementsModel.Summary(running) == "On · 1 effect" && EnhancementsModel.Summary(running.With(Enhancement.LightShafts, 1f).With(Enhancement.ToneMap, 1f)) == "On · 3 effects");
    Check("the log names the effects that are switched on", EnhancementsModel.Names(fresh) == "no effect"
        && EnhancementsModel.Names(running.With(Enhancement.ToneMap, 1f)) == "Haze, Tone map");
}

// The presets. A setting goes by its number in the game's settings, and a menu lists the option indices the game stores,
// lowest first: frame rate's 45 was added after 30 and 60, so its index comes last and its place is in the middle.
{
    const int FrameRate = 2, RenderResolution = 4, Bloom = 15, Reflections = 20;
    var lists = new Dictionary<int, OptionList>
    {
        [FrameRate] = new(new[] { 0, 2, 1 }),
        [RenderResolution] = new(new[] { 0, 1, 2, 3, 4, 5, 6, 7, 8 }),
        [Bloom] = new(new[] { 0, 1 }),
        // One option on this machine, so the setting has no row.
        [Reflections] = new(new[] { 0 }),
    };
    string[] rates = { "30", "45", "60" }, scales = { "0.6", "0.8", "0.9", "1.0", "1.1", "1.2", "1.3", "1.4", "1.5" }, offOn = { "Off", "On" };
    var highest = Presets.BuiltIn(PresetSlot.Highest,
        new Dictionary<int, Pick> { [FrameRate] = Pick.Keep, [RenderResolution] = Pick.Highest, [Bloom] = Pick.Highest, [Reflections] = Pick.Highest });
    var lowest = Presets.BuiltIn(PresetSlot.Lowest,
        new Dictionary<int, Pick> { [FrameRate] = Pick.Highest, [RenderResolution] = Pick.Lowest, [Bloom] = Pick.Lowest, [Reflections] = Pick.Lowest });
    // The user's own settings: 45 FPS, render resolution 1.0, bloom on.
    var mine = new Dictionary<int, int> { [FrameRate] = 2, [RenderResolution] = 3, [Bloom] = 1, [Reflections] = 0 };
    string Show(IEnumerable<SettingChange> changes) => string.Join(" ", changes.Select(change => $"{change.Setting}:{change.From}>{change.To}"));
    Dictionary<int, int> After(Dictionary<int, int> now, IEnumerable<SettingChange> changes)
    {
        var next = new Dictionary<int, int>(now);
        foreach (var change in changes)
            next[change.Setting] = change.To;
        return next;
    }

    Check("the built-in presets are named Highest and Lowest", highest.Name == "Highest" && lowest.Name == "Lowest");
    Check("the built-in Highest raises what is not at its highest, keeps the frame rate, and leaves a setting without a row alone",
        Show(Presets.Changes(highest, lists, mine)) == "4:3>8");
    Check("the built-in Lowest lowers each setting and takes the frame rate to the menu's last option, 60",
        Show(Presets.Changes(lowest, lists, mine)) == "2:2>1 4:3>0 15:1>0");
    Check("the built-in presets hold the values past the game's limits, and leave the enhancements alone",
        highest.Overrides == Overrides.None && highest.Lod == LodLevel.MostDetail && highest.Enhancements == null
        && lowest.Overrides == new Overrides(0.5f, false, false, false, 0) && lowest.Lod == LodLevel.LeastDetail && lowest.Enhancements == null);
    Check("the line under a row says what each built-in preset sets it to",
        Presets.SettingNote(highest, RenderResolution, lists[RenderResolution], scales) == "1.5" && Presets.SettingNote(lowest, RenderResolution, lists[RenderResolution], scales) == "0.6"
        && Presets.SettingNote(highest, FrameRate, lists[FrameRate], rates) == "Keep" && Presets.SettingNote(lowest, FrameRate, lists[FrameRate], rates) == "60"
        && Presets.SettingNote(highest, Bloom, lists[Bloom], offOn) == "On" && Presets.ScaleNote(highest) == "Game" && Presets.ScaleNote(lowest) == "0.50"
        && Presets.SwitchNote(highest, Enhancement.Enabled) == "Keep" && Presets.SwitchNote(lowest, Enhancement.Haze) == "Keep");

    // Bloom off in screenshots: Highest, then Bloom off and Light shafts on, then "Save current settings as Highest".
    var raised = After(mine, Presets.Changes(highest, lists, mine));
    var edited = new Dictionary<int, int>(raised) { [Bloom] = 0 };
    var shafts = fresh.With(Enhancement.Enabled, 1f).With(Enhancement.LightShafts, 1f).With(Enhancement.MistHeight, 12.5f);
    var rows = edited.Where(pair => lists[pair.Key].HasRow).ToDictionary(pair => pair.Key, pair => pair.Value);
    var saved = Presets.Saved(PresetSlot.Highest, rows, highest.Overrides, highest.Lod, shafts);
    Check("the built-in Highest forces Bloom on again", Show(Presets.Changes(highest, lists, edited)) == "15:0>1");
    Check("a saved Highest holds every row's value, so the screenshot turns Bloom off where the built-in one leaves it on",
        Show(Presets.Changes(saved, lists, mine)) == "4:3>8 15:1>0" && Show(Presets.Changes(saved, lists, edited)) == "");
    Check("after the shot, what the preset changed goes back: the render resolution to 1.0 and Bloom on",
        Show(Presets.Back(mine, After(mine, Presets.Changes(saved, lists, mine)))) == "4:8>3 15:0>1" && Show(Presets.Back(mine, mine)) == "");
    Check("a saved preset takes every enhancement switch and value along", saved.Enhancements.Equals(shafts) && saved.Name == "Highest"
        && saved.Overrides == Overrides.None && saved.Lod == LodLevel.MostDetail);
    Check("the lines then say what the saved preset sets: no row is kept, and each switch is On or Off",
        Presets.SettingNote(saved, Bloom, lists[Bloom], offOn) == "Off" && Presets.SettingNote(saved, FrameRate, lists[FrameRate], rates) == "45"
        && Presets.SettingNote(saved, RenderResolution, lists[RenderResolution], scales) == "1.5" && Presets.SwitchNote(saved, Enhancement.Enabled) == "On"
        && Presets.SwitchNote(saved, Enhancement.LightShafts) == "On" && Presets.SwitchNote(saved, Enhancement.Haze) == "Off");

    // The screenshot runs the preset's enhancements without making them the user's own.
    var run = new EnhancementRun(fresh);
    var shooting = run.Hold(saved.Enhancements);
    Check("a screenshot runs its preset's enhancements, and the user's own stay as they are", shooting.Running.Equals(shafts) && shooting.Own.Equals(fresh));
    Check("after the shot the user's own run again", shooting.Release().Running.Equals(fresh) && ReferenceEquals(run.Release(), run));
    Check("a preset that keeps the enhancements, or holds the ones that run, changes nothing in a shot",
        ReferenceEquals(run.Hold(highest.Enhancements), run) && ReferenceEquals(run.Hold(fresh), run) && ReferenceEquals(shooting.Hold(shafts), shooting));

    // What the preferences hold of a saved preset.
    string text = Presets.Text(saved);
    var skipped = new List<string>();
    var read = Presets.Parse(PresetSlot.Highest, text, skipped);
    Check("a saved preset writes its settings, its values past the game's limits, its detail level and its enhancements",
        text.StartsWith("2=2;4=8;15=0;Detail=MostDetail;Enabled=1;AmbientLight=0;") && text.Contains(";LightShafts=1;") && text.Contains(";MistHeight=12.5;"));
    Check("and reads back the same", skipped.Count == 0 && Presets.Text(read) == text && Show(Presets.Changes(read, lists, mine)) == "4:3>8 15:1>0"
        && read.Overrides == saved.Overrides && read.Lod == saved.Lod && read.Enhancements.Equals(shafts));
    var all = Presets.Saved(PresetSlot.Lowest, new Dictionary<int, int>(), lowest.Overrides, lowest.Lod, null);
    Check("every value past the game's limits writes and reads, and a preset without enhancements keeps them",
        Presets.Text(all) == "RenderScale=0.5;Shadows=Off;DistantShadows=Off;Fog=Off;Particles=0;Detail=LeastDetail"
        && Presets.Parse(PresetSlot.Lowest, Presets.Text(all), skipped) is { Enhancements: null, Lod: LodLevel.LeastDetail } low && low.Overrides == lowest.Overrides
        && low.Name == "Lowest" && skipped.Count == 0);
    Check("a preset saved with a shadow distance reads without it and without a warning, and saves without it",
        Presets.Parse(PresetSlot.Highest, "2=2;ShadowDistance=300;Detail=MostDetail", skipped) is { } old && skipped.Count == 0
        && old.Overrides == Overrides.None && Presets.Text(old) == "2=2;Detail=MostDetail");
    Check("a render scale left to the game is left to the game by the saved preset",
        Presets.ScaleNote(saved) == "Game" && Presets.ScaleNote(saved with { Overrides = saved.Overrides with { RenderScale = 1.25f } }) == "1.25");
    Check("an empty entry stands for the built-in preset", Presets.Parse(PresetSlot.Highest, "", skipped) == null && Presets.Parse(PresetSlot.Highest, " ; ", skipped) == null);
    var odd = Presets.Parse(PresetSlot.Highest, "15=0; Shadows=On; Detail=Best; what; Haze=1; 4=x", skipped);
    Check("an entry is read without the parts that are none of its values, and names them",
        Show(Presets.Changes(odd, lists, mine)) == "15:1>0" && odd.Overrides == Overrides.None && odd.Lod == null && odd.Enhancements.Equals(fresh.With(Enhancement.Haze, 1f))
        && skipped.OrderBy(part => part).SequenceEqual(new[] { "4=x", "Detail=Best", "Shadows=On", "what" }));
    var stale = Presets.Saved(PresetSlot.Highest, new Dictionary<int, int> { [Bloom] = 5, [Reflections] = 0, [99] = 1 }, Overrides.None, null, null);
    Check("a saved option the game no longer offers, and a setting without a row, are left alone",
        Show(Presets.Changes(stale, lists, mine)) == "" && Presets.SettingNote(stale, Bloom, lists[Bloom], offOn) == "Keep");

    // Applying a preset to the enhancements, and Restore.
    var haze = fresh.With(Enhancement.Haze, 1f);
    var second = saved with { Enhancements = haze.With(Enhancement.Enabled, 1f) };
    var (first, remembered) = Presets.Enhance(saved, haze, null);
    Check("a preset sets the enhancements it holds, and the ones from before it are kept for Restore", first.Equals(shafts) && remembered.Equals(haze));
    Check("a second preset leaves what Restore returns to: the enhancements from before the first one",
        Presets.Enhance(second, first, remembered) is var (next, before) && next.Equals(second.Enhancements) && before.Equals(haze));
    Check("a built-in preset keeps the enhancements, and nothing is kept for Restore",
        Presets.Enhance(highest, haze, null) is var (same, none) && ReferenceEquals(same, haze) && none == null);
    Check("a preset that holds the enhancements already set keeps nothing for Restore either",
        Presets.Enhance(saved, shafts, null) is var (unchanged, nothing) && ReferenceEquals(unchanged, shafts) && nothing == null);
}

// A camera path: how fast the camera moves and turns just before and just after a node, over a ten-thousandth of the path.
{
    CameraNode Node(float x, float yaw) => new(new Vec3(x, 0, 0), Quat.CreateFromYawPitchRoll(yaw * MathF.PI / 180f, 0, 0), 45f);
    float Step(PathSampler path, float from, float to, bool turn)
    {
        var a = path.Sample(from);
        var b = path.Sample(to);
        // The turn's angle from atan2, since acos of a dot product near 1 has too few digits in a float for a frame's turn.
        var turned = Quat.Inverse(a.Rotation) * b.Rotation;
        float angle = 2f * MathF.Atan2(new Vec3(turned.X, turned.Y, turned.Z).Length(), MathF.Abs(turned.W));
        return (turn ? angle : Vec3.Distance(a.Position, b.Position)) / (to - from);
    }
    float Across(PathSampler path, int node, bool turn)
    {
        float at = path.NodeProgress(node), frame = 1e-4f;
        return Step(path, at, at + frame, turn) / Step(path, at - frame, at, turn);
    }
    bool Near(float value, float expected, float tolerance) => MathF.Abs(value - expected) <= tolerance;

    var uneven = new[] { Node(0, 0), Node(3, 20), Node(19, 40) };
    var timed = new PathSampler(uneven, constantSpeed: false, 10f);
    var even = new PathSampler(uneven, constantSpeed: true, 10f);
    Check("with equal time a node, the camera reaches the middle node halfway, and keeps its speed into a segment five times longer",
        timed.NodeProgress(1) == 0.5f && Near(Across(timed, 1, turn: false), 1f, 0.02f));
    Check("with equal time a node, the camera also keeps its turn rate across the node", Near(Across(timed, 1, turn: true), 1f, 0.02f));
    Check("with constant speed, the camera reaches the middle node at 3/19 of the way, and keeps its speed",
        Near(even.NodeProgress(1), 3f / 19f, 1e-4f) && Near(Across(even, 1, turn: false), 1f, 0.02f));
    Check("with constant speed, the camera keeps its turn rate across the node, though the segments' times differ fivefold",
        Near(Across(even, 1, turn: true), 1f, 0.02f));
    Check("both ways, the camera is at each node at its progress",
        new[] { timed, even }.All(path => Enumerable.Range(0, 3).All(index => Vec3.Distance(path.Sample(path.NodeProgress(index)).Position, uneven[index].Position) < 1e-3f)));

    var turnFirst = new[] { Node(0, 0), Node(0, 30), Node(8, 30) };
    var samples = new[] { false, true }.SelectMany(constant => Enumerable.Range(0, 101).Select(step => new PathSampler(turnFirst, constant, 10f).Sample(step / 100f))).ToList();
    Check("a path that turns in place before it moves samples to real numbers both ways",
        samples.All(node => float.IsFinite(node.Position.X) && float.IsFinite(node.Rotation.W) && float.IsFinite(node.Fov)));

    // The sampler's output before nodes could have times, for a path whose nodes have none.
    var four = new[] { new CameraNode(new Vec3(0, 0, 0), Quat.CreateFromYawPitchRoll(0f, 0, 0), 45f),
        new CameraNode(new Vec3(3, 1, 0), Quat.CreateFromYawPitchRoll(20f * MathF.PI / 180f, 0, 0), 50f),
        new CameraNode(new Vec3(19, 0, 0), Quat.CreateFromYawPitchRoll(40f * MathF.PI / 180f, 0, 0), 30f),
        new CameraNode(new Vec3(20, 5, 0), Quat.CreateFromYawPitchRoll(90f * MathF.PI / 180f, 0, 0), 60f) };
    bool Was(PathSampler path, float progress, float x, float y, float turn, float fov)
        => path.Sample(progress) is var node && Near(node.Position.X, x, 1e-4f) && Near(node.Position.Y, y, 1e-4f) && Near(node.Rotation.Y, turn, 1e-5f) && Near(node.Fov, fov, 1e-3f);
    var plain = new PathSampler(four, constantSpeed: false, 10f);
    var steady = new PathSampler(four, constantSpeed: true, 10f);
    Check("a path without times samples as before, with equal time a node and with constant speed",
        !plain.Pinned && Was(plain, 0.1f, 0.76013625f, 0.30128196f, 0.05237828f, 47.287502f) && Was(plain, 0.37f, 3.9206607f, 1.0347897f, 0.19122072f, 48.619915f)
        && Was(plain, 0.8f, 19.861443f, 2.2447517f, 0.48254833f, 38.399998f) && Was(steady, 0.1f, 2.3000188f, 0.8720409f, 0.15074894f, 49.88515f)
        && Was(steady, 0.37f, 8.892317f, 0.39530164f, 0.23067355f, 41.425865f) && Was(steady, 0.8f, 19.235413f, 0.21760967f, 0.35228732f, 30.34957f));

    // Pinned nodes: the camera reaches each at its time.
    var two = new[] { Node(0, 0) with { Time = 2f }, Node(8, 30) with { Time = 6f } };
    bool At(PathSampler path, float progress, CameraNode node) => Vec3.Distance(path.Sample(progress).Position, node.Position) < 1e-3f;
    foreach (bool constant in new[] { false, true })
    {
        var pinned = new PathSampler(two, constant, 10f);
        Check($"two nodes pinned at 2 s and 6 s of 10 s show the first from 0 to 0.2 and the second from 0.6 to 1{(constant ? ", with constant speed" : "")}",
            pinned.Pinned && At(pinned, 0f, two[0]) && At(pinned, 0.1f, two[0]) && At(pinned, 0.2f, two[0]) && !At(pinned, 0.4f, two[0]) && !At(pinned, 0.4f, two[1])
            && At(pinned, 0.6f, two[1]) && At(pinned, 0.8f, two[1]) && At(pinned, 1f, two[1]));
    }
    var between = new[] { Node(0, 0) with { Time = 2f }, Node(3, 0), Node(19, 0) with { Time = 6f } };
    Check("an unpinned node between pins takes its share of their time by distance with constant speed, and evenly without",
        Near(new PathSampler(between, true, 10f).NodeProgress(1), 0.2f + 0.4f * 3f / 19f, 1e-4f) && Near(new PathSampler(between, false, 10f).NodeProgress(1), 0.4f, 1e-6f));
    var ends = new[] { Node(0, 0), Node(3, 0) with { Time = 2.5f }, Node(4, 0), Node(19, 0) };
    var spread = new PathSampler(ends, false, 10f);
    Check("unpinned ends count as pinned at 0 and at the duration, and the nodes after a pin share what is left evenly",
        spread.NodeProgress(0) == 0f && spread.NodeProgress(1) == 0.25f && Near(spread.NodeProgress(2), 0.625f, 1e-6f) && spread.NodeProgress(3) == 1f);
    // Pinned moments 2 s, 3 s and 2 s apart, turning one way: speed, acceleration and turn all carry on through the nodes between them.
    var smooth = new PathSampler(new[] { Node(0, 0) with { Time = 0f }, Node(10, 10) with { Time = 2f }, Node(18, 20) with { Time = 5f }, Node(26, 35) with { Time = 7f } }, false, 7f);
    float X(PathSampler path, float seconds) => path.Sample(seconds / 7f).Position.X;
    float Speed(PathSampler path, float seconds) => (X(path, seconds + 0.05f) - X(path, seconds - 0.05f)) / 0.1f;
    float Accel(float from, float to) => (Speed(smooth, to) - Speed(smooth, from)) / (to - from);
    float Yaw(float seconds) => smooth.Sample(seconds / 7f).Rotation.Y;
    float TurnAccel(float seconds) => (Yaw(seconds + 0.02f) - 2f * Yaw(seconds) + Yaw(seconds - 0.02f)) / 0.0004f;
    Check("a pinned path's acceleration and turn carry on through its nodes",
        Math.Abs(Accel(1.75f, 2f) - Accel(2f, 2.25f)) < 0.3f && Math.Abs(TurnAccel(4.95f) - TurnAccel(5.05f)) < 0.05f * Math.Abs(TurnAccel(4.95f)));
    // A node between a 0.5 m/s segment and an 18 m/s one, where the smooth curve alone would run the camera backward.
    var lurch = new PathSampler(new[] { Node(0, 0) with { Time = 0f }, Node(10, 0) with { Time = 2f }, Node(12, 0) with { Time = 6f }, Node(30, 0) with { Time = 7f } }, false, 7f);
    Check("a pinned path never stops or runs backward: at a node it keeps at least half the slower segment's speed",
        Speed(lurch, 2f) >= 0.25f && Speed(lurch, 6f) >= 0.25f && Enumerable.Range(1, 69).All(step => Speed(lurch, step * 0.1f) > 0f));
    var eased = new PathSampler(two, false, 10f, easeIn: true, easeOut: true);
    var easedPlain = new PathSampler(four, false, 10f, easeIn: true, easeOut: true);
    Check("easing keeps a pinned path's progress and ticks at its nodes' moments; a path without pins eases its progress",
        eased.Progress(0.25f) == 0.25f && eased.NodeTimes().SequenceEqual(new[] { 0.2f, 0.6f })
        && easedPlain.Progress(0.25f) == 0.15625f && easedPlain.NodeTimes()[0] == 0f && easedPlain.NodeTimes()[3] == 1f);
    // The pinned path above, eased at either end or neither: the camera's speed and turn rate just inside its first and last nodes.
    PathSampler Ends(bool easeIn, bool easeOut) => new(new[] { Node(0, 0) with { Time = 0f }, Node(10, 0) with { Time = 2f }, Node(18, 20) with { Time = 5f },
        Node(26, 0) with { Time = 7f } }, false, 7f, easeIn, easeOut);
    float Rate(PathSampler path, float seconds, bool turn) => Step(path, seconds / 7f, (seconds + 0.01f) / 7f, turn) / 7f;
    var stops = Ends(true, true);
    var runs = Ends(false, false);
    Check("Ease in and Ease out start and stop a pinned path's camera at its first and last nodes, in speed and in turn",
        Rate(stops, 0f, false) < 0.1f && Rate(stops, 6.99f, false) < 0.1f && Rate(stops, 6.99f, true) < 0.005f
        && Rate(runs, 0f, false) > 3f && Rate(runs, 6.99f, false) > 3f);
    var outOnly = Ends(false, true);
    Check("Ease out alone leaves the start at speed, and the eased path never runs backward",
        Rate(outOnly, 0f, false) > 3f && Rate(outOnly, 6.99f, false) < 0.1f
        && Enumerable.Range(1, 69).All(step => Speed(stops, step * 0.1f) > 0f && Speed(outOnly, step * 0.1f) > 0f));
    // An eased end node's moment moves only as far as a smooth stop needs; the nodes between keep theirs.
    var quickEnd = new PathSampler(new[] { Node(0, 0) with { Time = 0f }, Node(10, 0) with { Time = 2f }, Node(20, 0) with { Time = 4f },
        Node(22, 0) with { Time = 6f } }, false, 10f, easeIn: true, easeOut: true);
    Check("a camera reaching a short last segment fast stops on its node before the node's moment, and rests there through it",
        quickEnd.NodeProgress(1) == 0.2f && quickEnd.NodeProgress(2) == 0.4f && quickEnd.NodeProgress(3) > 0.4f && quickEnd.NodeProgress(3) < 0.6f
        && Vec3.Distance(quickEnd.Sample(0.6f).Position, new Vec3(22, 0, 0)) < 1e-3f);
    Check("the timeline shows an eased end node at the moment it was placed, though the camera rests on it from earlier",
        quickEnd.NodeTimes()[3] == 0.6f && quickEnd.NodeProgress(3) < 0.6f);
    // A route that turns a corner at node 1 and doubles back at node 2.
    CameraNode At3(float x, float z, float time) => new(new Vec3(x, 0, z), Quat.Identity, 45f) { Time = time };
    var corner = new PathSampler(new[] { At3(0, 0, 0f), At3(10, 0, 2f), At3(15, 5, 3.4f), At3(15, 15, 5.4f), At3(5, 15, 7.4f) }, false, 7.4f);
    Vec3 Velocity(PathSampler path, float duration, float seconds)
        => (path.Sample((seconds + 0.05f) / duration).Position - path.Sample((seconds - 0.05f) / duration).Position) / 0.1f;
    // A long straight segment into a short turn: the old route kept its direction through node 1 but bent sharply after it.
    var bend = new PathSampler(new[] { At3(0, 0, 0f), At3(20, 0, 2f), At3(22, 2, 3f), At3(22, 22, 5f), At3(0, 30, 7f) }, false, 7f);
    var bendBefore = (Velocity(bend, 7f, 2f) - Velocity(bend, 7f, 1.75f)) / 0.25f;
    var bendAfter = (Velocity(bend, 7f, 2.25f) - Velocity(bend, 7f, 2f)) / 0.25f;
    Check("a pinned path's route bends as smoothly through a node as between nodes",
        Vec3.Distance(bendBefore, bendAfter) < 0.2f * Math.Max(bendBefore.Length(), bendAfter.Length()));
    var hairpin = new PathSampler(new[] { At3(0, 0, 0f), At3(10, 0, 2f), At3(0, 1, 4f) }, false, 4f);
    Check("the camera slows almost to a stop where the route doubles back at a node, and keeps its speed through a right angle",
        Velocity(hairpin, 4f, 2f).Length() < 1f && Velocity(corner, 7.4f, 2f).Length() > 2.5f);
    // A small turn between nodes 2 and 3 before a big quick turn after node 3.
    var windUp = new PathSampler(new[] { Node(0, 0) with { Time = 0f }, Node(10, 10) with { Time = 2f }, Node(20, 20) with { Time = 4f },
        Node(25, 170) with { Time = 5f } }, false, 5f);
    float Apart(Quat a, Quat b) => 2f * MathF.Acos(Math.Min(1f, MathF.Abs(Quat.Dot(Quat.Normalize(a), Quat.Normalize(b))))) * 180f / MathF.PI;
    var (node2, node3) = (Quat.CreateFromYawPitchRoll(10f * MathF.PI / 180f, 0, 0), Quat.CreateFromYawPitchRoll(20f * MathF.PI / 180f, 0, 0));
    Check("between two nodes the camera turns from one node's view toward the other's, never away to wind up for a big turn after",
        Enumerable.Range(1, 19).All(step => windUp.Sample((2f + step * 0.1f) / 5f).Rotation is var turn
            && Apart(turn, node2) + Apart(turn, node3) - Apart(node2, node3) < 1f));
    var slowEnd = new PathSampler(new[] { Node(0, 0) with { Time = 0f }, Node(10, 0) with { Time = 2f }, Node(10.5f, 0) with { Time = 6f },
        Node(20, 0) with { Time = 7f } }, false, 10f, easeIn: false, easeOut: true);
    Check("a camera reaching a long last segment slowly takes at most half as long again to ease onto its node",
        slowEnd.NodeProgress(2) == 0.6f && slowEnd.NodeProgress(3) > 0.7f && slowEnd.NodeProgress(3) <= 0.75f + 1e-6f);
}

// A camera path's pinned times as its duration changes, nodes placed at a time, and CameraPaths.json.
{
    CameraNode At(float x, float? time = null) => new(new Vec3(x, 0, 0), Quat.Identity, 45f, time);
    var path = new CameraPath { Duration = 10f };
    path.Nodes.AddRange(new[] { At(0, 2f), At(5), At(9, 6f) });
    path.SetDuration(20f);
    Check("a longer duration moves the pinned times with it, so each keeps its share, and leaves an unpinned node unpinned",
        path.Duration == 20f && path.Nodes[0].Time == 4f && path.Nodes[1].Time == null && path.Nodes[2].Time == 12f);
    path.SetDuration(500f);
    Check("the duration stops at 310 s, and the times keep their shares of it", path.Duration == 310f && path.Nodes[0].Time == 62f && path.Nodes[2].Time == 186f);

    var even = new CameraPath { Duration = 10f };
    even.Nodes.AddRange(new[] { At(0), At(1), At(2) });
    int middle = even.Place(At(7), 3f, constantSpeed: false);
    Check("a node placed at 3 s goes in after the node the camera reaches at 0 s and before the one at 5 s",
        middle == 1 && even.Nodes[1].Position.X == 7f && even.Nodes[1].Time == 3f && even.Duration == 10f);
    int late = even.Place(At(8), 12.5f, constantSpeed: false);
    Check("a node placed past the end goes last and makes the path that long", late == 4 && even.Duration == 12.5f
        && even.Moments(false).Select(moment => MathF.Round(moment, 3)).SequenceEqual(new[] { 0f, 3f, 6.167f, 9.333f, 12.5f }));

    var library = new PathLibrary(new List<CameraPath> { path, new CameraPath { Name = "Waterfall dolly" } }, 0, 1, new PathOptions { Loop = true });
    string text = PathFile.Write(library);
    var read = PathFile.Read(text, out bool gaveIds);
    Check("a saved library reads back with its times, and an unpinned node has no Time in the file",
        !gaveIds && read.Paths.Count == 2 && read.Paths[0].Nodes.Select(node => node.Time).SequenceEqual(new float?[] { 62f, null, 186f })
        && read.Paths[0].Duration == 310f && read.Paths[1].Name == "Waterfall dolly" && read.Active == 0 && read.Node == 1 && read.Options.Loop
        && text.Split("\"Time\"").Length == 3);
    const string old = """
        { "Paths": [ { "Duration": 12, "Nodes": [ { "Position": [1, 2, 3], "Rotation": [0, 0, 0, 1], "Fov": 50 } ] } ], "ActivePath": 0, "ActiveNode": 0 }
        """;
    var before = PathFile.Read(old, out bool given);
    Check("a file from before nodes had times loads, its node unpinned and its path given an Id",
        given && before.Paths[0].Nodes.Single() is { Time: null, Fov: 50f } && before.Paths[0].Id.Length == 8 && before.Paths[0].Duration == 12f);
    Check("a time past the path's end reads as its end, and a time that is not a number fails the read",
        PathFile.Read(old.Replace("\"Fov\": 50", "\"Fov\": 50, \"Time\": 40"), out _).Paths[0].Nodes[0].Time == 12f
        && Fails(() => PathFile.Read(old.Replace("\"Fov\": 50", "\"Fov\": 50, \"Time\": \"NaN\""), out _)));
}

// The long exposure.
Check("the shutters run 1/8 s to 120 s, inside RealLongExposure.fx's 0.1 s to 120 s",
    Exposure.ShutterLabels.First() == "1/8 s" && Exposure.ShutterLabels.Last() == "120 s" && Exposure.Shutters.All(seconds => seconds >= 0.1f && seconds <= 120f));
Check("a saved shutter time rounds to the nearest full stop: 7 s to 8 s, 0.1 s to 1/8 s, 500 s to 120 s",
    Exposure.Shutters[Exposure.NearestShutter(7f)] == 8f && Exposure.NearestShutter(0.1f) == 0 && Exposure.Shutters[Exposure.NearestShutter(500f)] == 120f);
{
    var open = new Exposure(8f, 400, 2f).Uniforms(open: true).ToDictionary(uniform => uniform.Name, uniform => uniform.Value);
    Check("an open shutter sets the duration, ISO and boost, starts the exposure, and keeps the shader's marks off the picture",
        open["UI_ExposureDuration"] == 8f && open["UI_ISO"] == 400f && open["UI_Gamma"] == 2f && open["UI_StartExposure"] == 1f
        && open["UI_ShowProgress"] == 0f && open["UI_ShowGreenOnFinish"] == 0f && open["UI_Delay"] == 0f);
    Check("a closed shutter does not start the exposure", new Exposure(8f, 400, 2f).Uniforms(open: false).Single(uniform => uniform.Name == "UI_StartExposure").Value == 0f);
}

// A replay's timeline, which character is on the field, and the Replays tab's words.
{
    var timeline = new Timeline<string>();
    timeline.Add(1f, "a");
    timeline.Add(1.1f, "b");
    timeline.Add(2f, "c");
    Check("a timeline finds the last sample at or before a time, and none before its first",
        timeline.IndexAt(0.5f) == -1 && timeline.IndexAt(1f) == 0 && timeline.IndexAt(1.05f) == 0 && timeline.IndexAt(1.5f) == 1 && timeline.IndexAt(9f) == 2);
    Check("a sample blends toward the next one within a quarter second",
        timeline.Sample(1.05f, out var a, out var b, out float blend) && a == "a" && b == "b" && Math.Abs(blend - 0.5f) < 1e-4f);
    Check("a sample holds across a longer gap, holds the first sample before it, and there is none on an empty timeline",
        timeline.Sample(1.5f, out a, out b, out blend) && a == "b" && b == "b" && blend == 0f
        && timeline.Sample(0.5f, out a, out b, out blend) && a == "a" && b == "a" && blend == 0f && !new Timeline<string>().Sample(1f, out _, out _, out _));

    var kaeya = (Name: "Kaeya", Poses: new Timeline<int>());
    var noel = (Name: "Noel", Poses: new Timeline<int>());
    foreach (float time in new[] { 0f, 0.5f, 1f, 4f })
        kaeya.Poses.Add(time, 0);
    foreach (float time in new[] { 1.2f, 2f, 3.9f })
        noel.Poses.Add(time, 0);
    Check("a timeline's sample after and before a time, past one taken at that time, and none past its ends",
        timeline.After(1f) == 1.1f && timeline.After(1.05f) == 1.1f && timeline.After(2f) == null && timeline.Before(1.1f) == 1f
        && timeline.Before(1.05f) == 1f && timeline.Before(1f) == null && timeline.Before(9f) == 2f);
    string OnField(float time) => Timeline<int>.Latest(new[] { kaeya, noel }.Select(each => Tuple.Create(each.Name, each.Poses)), each => each.Item2, time)?.Item1;
    Check("the character on the field is the one recorded most recently, through a hitch in its recording, and the first one before any pose",
        OnField(0.8f) == "Kaeya" && OnField(3.5f) == "Noel" && OnField(4.1f) == "Kaeya" && OnField(-0.1f) == "Kaeya");
    var party = new[] { kaeya.Poses, noel.Poses };
    Check("a frame step goes to the next or last frame of any character, and stays at either end",
        EditTimeline.Step(party, 1f, 1) == 1.2f && EditTimeline.Step(party, 1.2f, -1) == 1f && EditTimeline.Step(party, 3.9f, 1) == 4f
        && EditTimeline.Step(party, 4f, 1) == 4f && EditTimeline.Step(party, 0f, -1) == 0f && EditTimeline.Step(Array.Empty<Timeline<int>>(), 2f, 1) == 2f);
    Check("the party switches where another character becomes the one recorded last", EditTimeline.Switches(party).SequenceEqual(new[] { 1.2f, 4f }));
    Check("before any pose, the first character is the one recorded first, whatever the order of the tracks",
        Timeline<int>.Latest(new[] { noel, kaeya }.Select(each => Tuple.Create(each.Name, each.Poses)), each => each.Item2, -1f)?.Item1 == "Kaeya"
        && Timeline<int>.Latest(Array.Empty<Tuple<string, Timeline<int>>>(), each => each.Item2, 1f) == null);
}
{
    var calls = new[]
    {
        CallTarget.Of(CallKind.Float, 0, 5, null), CallTarget.Of(CallKind.Float, 0, 5, null),
        CallTarget.Of(CallKind.KeywordOn, 0, 0, "_RIM"), CallTarget.Of(CallKind.KeywordOff, 0, 0, "_RIM"),
        CallTarget.Of(CallKind.Swap, 1, 0, null), CallTarget.Of(CallKind.KeywordOn, 1, 0, "_HIT"), CallTarget.Of(CallKind.Swap, 1, 0, null),
        CallTarget.Of(CallKind.Float, 1, 5, null), CallTarget.Of(CallKind.CustomFloat, 1, 5, null), CallTarget.Of(CallKind.Color, 0, 5, null),
        CallTarget.Of(CallKind.KeywordIdOn, 2, 7, null), CallTarget.Of(CallKind.KeywordIdOff, 2, 7, null),
    };
    string Live(int count, bool swaps) => string.Join(",", LookCalls.Live(count, index => calls[index], swaps));
    Check("a seek repeats the last call on each target, none from before a slot's last swap, and a renderer's calls whatever its material slots do",
        Live(10, swaps: true) == "1,3,6,7,8,9" && Live(12, swaps: true) == "1,3,6,7,8,9,11");
    Check("a character's swaps do not run, so they hide nothing, and a seek to an earlier call repeats only the calls up to it",
        Live(10, swaps: false) == "1,3,5,6,7,8,9" && Live(4, swaps: true) == "1,3" && Live(0, swaps: true) == "");
}
{
    var moments = new[] { 0.5f, 3f, 3.08f, 8f };
    Check("the node at the playhead is the nearest within 0.1 s, and there is none further off",
        EditTimeline.NodeAt(moments, 3.05f) == 2 && EditTimeline.NodeAt(moments, 2.95f) == 1 && EditTimeline.NodeAt(moments, 3.2f) == -1);
    Check("the next and previous nodes go past the one at the playhead, and there are none past the ends",
        EditTimeline.Neighbour(moments, 3.08f, 1) == 3 && EditTimeline.Neighbour(moments, 3.085f, 1) == 3 && EditTimeline.Neighbour(moments, 3f, -1) == 0
        && EditTimeline.Neighbour(moments, 8f, 1) == -1 && EditTimeline.Neighbour(moments, 0.5f, -1) == -1);
    Check("holding a frame step scrubs at half speed, doubles every 0.75 s and stops at 16 times",
        EditTimeline.ScrubRate(0f) == 0.5f && EditTimeline.ScrubRate(0.75f) == 1f && EditTimeline.ScrubRate(3f) == 8f && EditTimeline.ScrubRate(10f) == 16f);
    Check("the timeline's line names the replay, the playhead and the duration, and whether it plays",
        EditTimeline.Meta("Replay 1", 6.126f, 18.44f, false) == "Replay 1 · 6.13 / 18.4 s · paused" && EditTimeline.Meta("Replay 2", 0f, 5f, true) == "Replay 2 · 0.00 / 5.0 s · playing");
    Check("the mode line names the path, its nodes and the camera, and a frozen world",
        EditTimeline.Mode("Path 1", 8, false, ReplayWorld.Running) == "Path 1 · 8 nodes · free camera"
        && EditTimeline.Mode("Waterfall dolly", 1, true, ReplayWorld.Frozen) == "Waterfall dolly · 1 node · looking through the path · world frozen");
}
Check("a character's name is the end of its root's name", ReplayText.CharacterName("Avatar_Girl_Claymore_Noel(Clone)") == "Noel"
    && ReplayText.CharacterName("Avatar_Male_Sword_Kaeya") == "Kaeya" && ReplayText.CharacterName("Paimon") == "Paimon");
Check("a replay's line gives its length, its party and its enemies, leaving out what it has none of",
    ReplayText.Note(18.44f, new[] { "Kaeya", "Noel" }, 6) == "18.4 s · Kaeya, Noel · 6 enemies"
    && ReplayText.Note(2f, new[] { "Lisa" }, 1) == "2.0 s · Lisa · 1 enemy" && ReplayText.Note(2f, Array.Empty<string>(), 0) == "2.0 s");
Check("a new replay is numbered after the highest number, so a deleted one's number is not taken again while a later one is there",
    ReplayText.NextName(new[] { "Replay 1", "Replay 3" }) == "Replay 4" && ReplayText.NextName(Array.Empty<string>()) == "Replay 1");
Check("the recorded toast names the linked path", ReplayText.Recorded("Replay 3", 18.44f, "Waterfall dolly") == "Replay 3 recorded · 18.4 s · plays with Waterfall dolly"
    && ReplayText.Recorded("Replay 2", 5f, null) == "Replay 2 recorded · 5.0 s");
Check("a linked replay starts its offset into the path, holds its first frame before and its last frame after",
    ReplayText.LinkedTime(1f, 2f, 10f) == 0f && ReplayText.LinkedTime(5f, 2f, 10f) == 3f && ReplayText.LinkedTime(30f, 2f, 10f) == 10f);

Check("the first supersampling offsets are Halton's in bases 2 and 3 about the pixel's centre",
    SupersamplingPattern.Offset(0) == (0f, 1f / 3f - 0.5f) && SupersamplingPattern.Offset(1) == (-0.25f, 2f / 3f - 0.5f)
    && SupersamplingPattern.Offset(2) == (0.25f, 1f / 9f - 0.5f));
var supersamples = Enumerable.Range(0, 32).Select(SupersamplingPattern.Offset).ToArray();
Check("32 supersampling offsets stay inside the pixel, differ, and average within a 32nd of a pixel of its centre",
    supersamples.All(o => o.X >= -0.5f && o.X < 0.5f && o.Y >= -0.5f && o.Y < 0.5f) && supersamples.Distinct().Count() == 32
    && MathF.Abs(supersamples.Average(o => o.X)) < 1f / 32 && MathF.Abs(supersamples.Average(o => o.Y)) < 1f / 32);

// The default bindings.
Check("a file from before layout 2 moves to WASD, Tab, Z, X, H and the pad's L3+R3 toggle",
    ControlsModel.Migrate(CamAction.Forward, UnityEngine.KeyCode.I, "LeftStickUp", 1) == (UnityEngine.KeyCode.W, "LeftStickUp")
    && ControlsModel.Migrate(CamAction.ToggleGUI, UnityEngine.KeyCode.F10, "Start", 1) == (UnityEngine.KeyCode.Tab, "Start")
    && ControlsModel.Migrate(CamAction.TogglePause, UnityEngine.KeyCode.Delete, "X", 1) == (UnityEngine.KeyCode.Z, "X")
    && ControlsModel.Migrate(CamAction.ToggleHUD, UnityEngine.KeyCode.PageDown, "B", 1) == (UnityEngine.KeyCode.H, "B")
    && ControlsModel.Migrate(CamAction.ToggleFreecam, UnityEngine.KeyCode.Insert, "a", 1) == (UnityEngine.KeyCode.Insert, "L3+R3"));
Check("a binding the user changed stays, and a file already at layout 2 is left alone",
    ControlsModel.Migrate(CamAction.Forward, UnityEngine.KeyCode.UpArrow, "RightStickUp", 1) == (UnityEngine.KeyCode.UpArrow, "RightStickUp")
    && ControlsModel.Migrate(CamAction.Forward, UnityEngine.KeyCode.I, "LeftStickUp", 2) == (UnityEngine.KeyCode.I, "LeftStickUp")
    && ControlsModel.Migrate(CamAction.ToggleSpeedTo5, UnityEngine.KeyCode.CapsLock, "Back+Y", 1) == (UnityEngine.KeyCode.CapsLock, "Back+Y"));
var defaultKeys = ControlsModel.Defaults.Where(row => row.Key != UnityEngine.KeyCode.None).Select(row => row.Key).ToList();
var defaultPads = ControlsModel.Defaults.Where(row => row.Pad != "").Select(row => row.Pad).ToList();
Check("no two actions share a default key or controller binding",
    defaultKeys.Distinct().Count() == defaultKeys.Count && defaultPads.Distinct().Count() == defaultPads.Count);

// Rebinding.
var bound = ControlsModel.Defaults.Select(row => row.Key).ToArray();
var swap = ControlsModel.Swap(bound, CamAction.TogglePause, UnityEngine.KeyCode.H, UnityEngine.KeyCode.None);
Check("putting H on Toggle pause swaps: Hide UI takes Z",
    swap == new Rebind(true, CamAction.ToggleHUD) && bound[(int)CamAction.TogglePause] == UnityEngine.KeyCode.H
    && bound[(int)CamAction.ToggleHUD] == UnityEngine.KeyCode.Z);
var cleared = ControlsModel.Swap(bound, CamAction.TogglePause, UnityEngine.KeyCode.None, UnityEngine.KeyCode.None);
Check("clearing a key unbinds only that action", cleared == new Rebind(true, null) && bound[(int)CamAction.TogglePause] == UnityEngine.KeyCode.None);
var lockedOut = ControlsModel.Swap(bound, CamAction.ToggleFreecam, UnityEngine.KeyCode.None, UnityEngine.KeyCode.None);
var stolen = ControlsModel.Swap(bound, CamAction.TogglePause, UnityEngine.KeyCode.Insert, UnityEngine.KeyCode.None);
Check("the free camera's toggle keeps a key: clearing it, or taking it for an unbound action, is refused",
    lockedOut == new Rebind(false, null) && stolen == new Rebind(false, null)
    && bound[(int)CamAction.ToggleFreecam] == UnityEngine.KeyCode.Insert && bound[(int)CamAction.TogglePause] == UnityEngine.KeyCode.None);
var pads = ControlsModel.Defaults.Select(row => PadBinding.TryParse(row.Pad, out var pad) ? pad : default).ToArray();
var padSwap = ControlsModel.Swap(pads, CamAction.ResetSpeed, new PadBinding(PadButtons.B, PadAxis.None), default(PadBinding));
Check("a controller binding swaps the same way: B on Reset speed gives Hide UI Y",
    padSwap == new Rebind(true, CamAction.ToggleHUD) && pads[(int)CamAction.ToggleHUD] == new PadBinding(PadButtons.Y, PadAxis.None));

Console.WriteLine($"CameraTools tests: {failures} failed");
return failures == 0 ? 0 : 1;
