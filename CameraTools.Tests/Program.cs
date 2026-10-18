// The traced screenshot's decisions (CameraTools/TracedShot.cs), the Enhancements category's rules and its settings for
// the add-on (CameraTools/EnhancementsModel.cs), the presets' decisions (CameraTools/GraphicsModel.cs), and the
// Enhancements binding's layout against Enhancements/enhancements.h's literal sizes, and the camera path's motion
// (CameraTools/PathSampler.cs), the long exposure's settings (CameraTools/Exposure.cs), and a replay's timeline and
// words (CameraTools/ReplayModel.cs). No game, no ReShade, no add-on: run with dotnet run.
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

// The binding: what enhancements.h's static_asserts say of version 10.
Check("the binding's status is enhancements.h's 144 bytes, with the restarts at 132, their reason at 136 and the shot at 140",
    Marshal.SizeOf<EnhancementsStatus>() == 144 && Offset<EnhancementsStatus>(nameof(EnhancementsStatus.TraceTarget)) == 116
    && Offset<EnhancementsStatus>(nameof(EnhancementsStatus.TraceLight)) == 128 && Offset<EnhancementsStatus>(nameof(EnhancementsStatus.TraceRestarts)) == 132
    && Offset<EnhancementsStatus>(nameof(EnhancementsStatus.TraceRestartReason)) == 136 && Offset<EnhancementsStatus>(nameof(EnhancementsStatus.ShotActive)) == 140);
Check("the binding's shot is 16 bytes, its settings 236 and its scene request 20",
    Marshal.SizeOf<EnhancementsShot>() == 16 && Offset<EnhancementsShot>(nameof(EnhancementsShot.BudgetMs)) == 12 && Marshal.SizeOf<EnhancementsSettings>() == 236
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
        highest.Overrides == new Overrides(ShadowDistance: 300f) && highest.Lod == LodLevel.MostDetail && highest.Enhancements == null
        && lowest.Overrides == new Overrides(0.5f, false, 20f, false, false, 0) && lowest.Lod == LodLevel.LeastDetail && lowest.Enhancements == null);
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
        && saved.Overrides == new Overrides(ShadowDistance: 300f) && saved.Lod == LodLevel.MostDetail);
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
        text.StartsWith("2=2;4=8;15=0;ShadowDistance=300;Detail=MostDetail;Enabled=1;AmbientLight=0;") && text.Contains(";LightShafts=1;") && text.Contains(";MistHeight=12.5;"));
    Check("and reads back the same", skipped.Count == 0 && Presets.Text(read) == text && Show(Presets.Changes(read, lists, mine)) == "4:3>8 15:1>0"
        && read.Overrides == saved.Overrides && read.Lod == saved.Lod && read.Enhancements.Equals(shafts));
    var all = Presets.Saved(PresetSlot.Lowest, new Dictionary<int, int>(), lowest.Overrides, lowest.Lod, null);
    Check("every value past the game's limits writes and reads, and a preset without enhancements keeps them",
        Presets.Text(all) == "RenderScale=0.5;Shadows=Off;ShadowDistance=20;DistantShadows=Off;Fog=Off;Particles=0;Detail=LeastDetail"
        && Presets.Parse(PresetSlot.Lowest, Presets.Text(all), skipped) is { Enhancements: null, Lod: LodLevel.LeastDetail } low && low.Overrides == lowest.Overrides
        && low.Name == "Lowest" && skipped.Count == 0);
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
    var timed = new PathSampler(uneven, constantSpeed: false);
    var even = new PathSampler(uneven, constantSpeed: true);
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
    var samples = new[] { false, true }.SelectMany(constant => Enumerable.Range(0, 101).Select(step => new PathSampler(turnFirst, constant).Sample(step / 100f))).ToList();
    Check("a path that turns in place before it moves samples to real numbers both ways",
        samples.All(node => float.IsFinite(node.Position.X) && float.IsFinite(node.Rotation.W) && float.IsFinite(node.Fov)));
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
    Check("a sample holds across a longer gap, and there is none before the first",
        timeline.Sample(1.5f, out a, out b, out blend) && a == "b" && b == "b" && blend == 0f && !timeline.Sample(0.5f, out _, out _, out _));

    var kaeya = (Name: "Kaeya", Poses: new Timeline<int>());
    var noel = (Name: "Noel", Poses: new Timeline<int>());
    foreach (float time in new[] { 0f, 0.5f, 1f, 4f })
        kaeya.Poses.Add(time, 0);
    foreach (float time in new[] { 1.2f, 2f, 3.9f })
        noel.Poses.Add(time, 0);
    string OnField(float time) => Timeline<int>.Latest(new[] { kaeya, noel }.Select(each => Tuple.Create(each.Name, each.Poses)), each => each.Item2, time)?.Item1;
    Check("the character on the field is the one recorded most recently, through a hitch in its recording, and none before the first pose",
        OnField(0.8f) == "Kaeya" && OnField(3.5f) == "Noel" && OnField(4.1f) == "Kaeya" && OnField(-0.1f) == null);
}
Check("a character's name is the end of its root's name", ReplayText.CharacterName("Avatar_Girl_Claymore_Noel(Clone)") == "Noel"
    && ReplayText.CharacterName("Avatar_Male_Sword_Kaeya") == "Kaeya" && ReplayText.CharacterName("Paimon") == "Paimon");
Check("a replay's line gives its length, its party and its enemies, leaving out what it has none of",
    ReplayText.Note(18.44f, new[] { "Kaeya", "Noel" }, 6) == "18.4 s · Kaeya, Noel · 6 enemies"
    && ReplayText.Note(2f, new[] { "Lisa" }, 1) == "2.0 s · Lisa · 1 enemy" && ReplayText.Note(2f, Array.Empty<string>(), 0) == "2.0 s");
Check("a new replay is numbered after the highest number, so a deleted one's number is not taken again while a later one is there",
    ReplayText.NextName(new[] { "Replay 1", "Replay 3" }) == "Replay 4" && ReplayText.NextName(Array.Empty<string>()) == "Replay 1");
Check("the recorded toast names the linked path", ReplayText.Recorded("Replay 3", 18.44f, 1) == "Replay 3 recorded · 18.4 s · plays with path 1"
    && ReplayText.Recorded("Replay 2", 5f, null) == "Replay 2 recorded · 5.0 s");
Check("a linked replay starts its offset into the path, holds its first frame before and its last frame after",
    ReplayText.LinkedTime(1f, 2f, 10f) == 0f && ReplayText.LinkedTime(5f, 2f, 10f) == 3f && ReplayText.LinkedTime(30f, 2f, 10f) == 10f);

Console.WriteLine($"CameraTools tests: {failures} failed");
return failures == 0 ? 0 : 1;
