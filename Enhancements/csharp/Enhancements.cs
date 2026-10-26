// The C# binding of CameraToolsEnhancements.addon64 (Enhancements/enhancements.h). A MelonLoader mod adds this file as is.
// ReShade loads the add-on, some time after the mod starts or never, so the binding looks for the loaded module and never
// loads a copy of its own, as CameraTools/ReShade.cs does for the bridge. Every call returns at once and takes effect at
// the add-on's next frame boundary, so callers watch Status or Describe().
using System.Runtime.InteropServices;
using System.Text;
using MelonLoader;
using UnityEngine;
using NativeLibrary = System.Runtime.InteropServices.NativeLibrary;

namespace CameraToolsEnhancements
{
    // ENHANCEMENTS_VIEW_* in enhancements.h, in order.
    internal enum EnhancementsView : uint
    {
        Off, Normals, Albedo, Specular, MaterialId, Smoothness, Character, Depth, Stencil,
        ShadowMask, QuarterShadow, AmbientDiffuse, AmbientSpecular, HdrScene, BloomFinal, SunAtlas, LightShafts, Trace, Bounce, SkyVisibility, SkyMap, Count,
    }

    // ENHANCEMENTS_ERROR_* in enhancements.h.
    internal enum EnhancementsError : uint { None, NotD3D11, Shader, Texture, State }

    // EnhancementsAmbient in enhancements.h, field for field.
    [StructLayout(LayoutKind.Sequential)]
    internal struct EnhancementsAmbient
    {
        public uint Enabled;
        public float Level;
        public float AoStrength;
        public float AoRadius;
        public float FoliageAoStrength;
    }

    // EnhancementsContactShadows in enhancements.h, field for field.
    [StructLayout(LayoutKind.Sequential)]
    internal struct EnhancementsContactShadows
    {
        public uint Enabled;
        public float Length;
        public float Strength;
        public float Thickness;
        public float FoliageStrength;
    }

    // EnhancementsAtmosphere in enhancements.h, field for field.
    [StructLayout(LayoutKind.Sequential)]
    internal struct EnhancementsAtmosphere
    {
        public uint Enabled;
        public float Density;
        public float HeightFalloff;
        public float SunScatter;
        public float Anisotropy;
    }

    // ENHANCEMENTS_CURVE_* in enhancements.h, in order.
    internal enum EnhancementsCurve : uint { Game, AgX, Neutral }

    // EnhancementsTonemap in enhancements.h, field for field.
    [StructLayout(LayoutKind.Sequential)]
    internal struct EnhancementsTonemap
    {
        public uint Enabled;
        public float ExposureEv;
        public EnhancementsCurve Curve;
        public float BloomStrength;
        public float Saturation;
        public float Contrast;
    }

    // EnhancementsSunShadows in enhancements.h, field for field.
    [StructLayout(LayoutKind.Sequential)]
    internal struct EnhancementsSunShadows
    {
        public uint Enabled;
        public float LightSize;
        public float MinPenumbra;
        public float Strength;
    }

    // EnhancementsWetness in enhancements.h, field for field.
    [StructLayout(LayoutKind.Sequential)]
    internal struct EnhancementsWetness
    {
        public uint Enabled;
        public float Wetness;
        public float Darkening;
        public float Puddles;
    }

    // EnhancementsLightShafts in enhancements.h, field for field.
    [StructLayout(LayoutKind.Sequential)]
    internal struct EnhancementsLightShafts
    {
        public uint Enabled;
        public float Strength;
        public float Density;
        public float Height;
        public float Anisotropy;
        public float Distance;
        public float Patchiness;
        public float PatchSize;
        public float Ambient;
        public uint Steps;
    }

    // ENHANCEMENTS_TRACE_AO and ENHANCEMENTS_TRACE_SUN in enhancements.h: what the trace pass writes. None reads as both.
    [Flags]
    internal enum EnhancementsTraceOutputs : uint { None = 0, Ao = 1 << 0, Sun = 1 << 1 }

    // ENHANCEMENTS_TRACE_CAST_* in enhancements.h: what casts in the traced scene besides the world.
    [Flags]
    internal enum EnhancementsTraceCasters : uint { None = 0, Characters = 1 << 0, AlphaCards = 1 << 2, WholeCards = 1 << 3 }

    // ENHANCEMENTS_TRACE_STATE_* in enhancements.h, in order.
    internal enum EnhancementsTraceState : uint { None, Building, Uploading, Ready, Stale, Failed }

    // ENHANCEMENTS_TRACE_RESTART_* in enhancements.h, in order: why the trace's samples last started again.
    internal enum EnhancementsTraceRestart : uint { None, Scene, RenderSize, Settings, Camera, SunDirection, Light }

    // EnhancementsTrace in enhancements.h, field for field.
    [StructLayout(LayoutKind.Sequential)]
    internal struct EnhancementsTrace
    {
        public uint Enabled;
        public EnhancementsTraceOutputs Outputs;
        public EnhancementsTraceCasters Casters;
        public float AoDistance;
        public float AoStrength;
        public float SunSize;
        public float SunStrength;
        public uint AoSamples;
        public uint SunSamples;
        public float BudgetMs;
    }

    // ENHANCEMENTS_GATHER_SKY_* in enhancements.h, in order: how the gathered light meets the game's sky light.
    internal enum EnhancementsGatherSky : uint { Occlusion, Visibility, Weighted, Map }

    // ENHANCEMENTS_TRACE_LIGHT_* in enhancements.h: what of the traced scene's light came from the game.
    [Flags]
    internal enum EnhancementsTraceLight : uint { None = 0, Albedo = 1 << 0, Sky = 1 << 1, Sun = 1 << 2, Ambient = 1 << 3 }

    // EnhancementsGather in enhancements.h, field for field.
    [StructLayout(LayoutKind.Sequential)]
    internal struct EnhancementsGather
    {
        public uint Enabled;
        public uint Samples;
        public EnhancementsGatherSky Sky;
        public uint Filter;
        public float Strength;
        public float HitAmbient;
    }

    // ENHANCEMENTS_DOF_PLACE_* in enhancements.h: in the HDR scene at the bloom step, or on the back buffer after the ReShade
    // effects that read the depth buffer.
    internal enum EnhancementsDofPlace : uint { Scene, Display }

    // EnhancementsDepthOfField in enhancements.h, field for field; focus_point is FocusX and FocusY.
    [StructLayout(LayoutKind.Sequential)]
    internal struct EnhancementsDepthOfField
    {
        public uint Enabled;
        public float FNumber;
        public float SensorHeightMm;    // 4 to 20000; N times taller blurs as a 1:N miniature
        public float FocusDistance;     // meters; 0 autofocuses at the focus point
        public float FocusX, FocusY;    // 0 to 1 from the top left
        public float FocusSeconds;      // the seconds the autofocus takes to cover 99% of the way; 0 jumps
        public float MaxBlur;           // the largest CoC radius over the render height
        public uint Blades;             // 0 round, else 5 to 16
        public float BladeRoundness;
        public float BladeRotation;     // degrees
        public float SphericalAberration;
        public float CatEye;
        public float Swirl;
        public float Anamorphic;
        public float Chromatic;
        public uint Samples;
        public float HighlightThreshold;    // over the frame's mean luminance
        public float HighlightGain;         // 0: highlights keep their own light
        public uint Scatter;                // 1: highlights drawn as sprites of their disc
        public float BokehContrast;         // 0: sources keep their own light behind focus
        public EnhancementsDofPlace Place;
        public float EyeDistance;           // meters to an eye's surface under the focus point as reckoned; 0: no eye
        public float EyeSize;               // the eye's window's width in meters
    }

    // ENHANCEMENTS_FLARE_SOURCE_* in enhancements.h: the sun, dimmed where anything blocks it, or a light at Position.
    internal enum EnhancementsFlareSource : uint { Sun, Manual }

    // EnhancementsLensFlare in enhancements.h, field for field; position is X and Y.
    [StructLayout(LayoutKind.Sequential)]
    internal struct EnhancementsLensFlare
    {
        public uint Enabled;
        public EnhancementsFlareSource Source;
        public float X, Y;                  // the manual source, 0 to 1 from the top left, and beyond it to -2 and 3
        public float Strength;              // the whole flare, 0 to 4
        public float Arcs, Ghosts, Streaks, Glare;  // each part's light, 0 to 4
        public float Size;                  // 0.25 to 4
        public float Warmth;                // 0 cool, 0.5 the lens's own orange, 1 deeper
        public float Chromatic;             // the red fringe, 0 to 4
        public float VisibilitySeconds;     // how long the flare takes to follow the sun's visibility, 99% of the way
    }

    // EnhancementsSettings in enhancements.h, field for field.
    [StructLayout(LayoutKind.Sequential)]
    internal struct EnhancementsSettings
    {
        public uint Size;
        public uint Enabled;
        public EnhancementsView View;
        public uint Flip;
        public EnhancementsAmbient Ambient;
        public EnhancementsContactShadows ContactShadows;
        public EnhancementsAtmosphere Atmosphere;
        public EnhancementsTonemap Tonemap;
        public EnhancementsSunShadows SunShadows;
        public EnhancementsWetness Wetness;
        public EnhancementsLightShafts LightShafts;
        public EnhancementsTrace Trace;
        public EnhancementsGather Gather;
        public EnhancementsDepthOfField DepthOfField;
        public EnhancementsLensFlare LensFlare;

        // The add-on's defaults: everything off, game targets upside down, and when switched on ambient level 0.6, AO
        // strength 0.5 at a 1 m radius, half that strength on grass, vegetation and foliage, full-strength contact
        // shadows 0.6 m long that take surfaces to be 0.25 m thick and leave grass, vegetation and foliage unshadowed, and
        // haze that dims a pixel 500 m away by 15%, thins by e every 50 m up, and glows around the sun, a tone map that
        // reproduces the game's, full-strength sun shadows whose soft edge is 0.03 m wide per meter from the caster and at
        // least 0.02 m, wet surfaces that follow the weather, darken by 0.35 when soaked and grow puddles on flat ground at
        // 0.3, and light shafts at strength 3 through a mist that hides 26% of a pixel 50 m away, thins by e every 5 m up,
        // scatters sun light forward with anisotropy 0.6, is marched 150 m in 48 steps, lies in patches 6 m across at
        // patchiness 1, and takes the sky's light at 0.3, and a trace of occlusion within 2 m and sun shadows of the real
        // sun's size, both at full strength and cast by characters and cards too, to 64 and 32 samples a pixel within 100 ms a
        // frame, applied with the ambient level, and when switched on a gather of one bounce, 64 samples a pixel, its sky
        // light by visibility, filtered twice, at full strength, with half the game's ambient light on the surfaces it meets,
        // and a depth of field at f/2.8 on a full-frame sensor that autofocuses at the center at 0.9 of the way a second, blurs at
        // most 0.03 of the height, has round discs with spherical aberration 0.6, gathers 49 samples a pixel, and draws highlights
        // above twice the frame's mean luminance as sprites, growing small lamps at gain 2, with the bokeh contrast off
        // and no eye, whose window would be 15 mm wide, and a lens flare from the sun at full strength in every part, at the lens's
        // own size and orange, that follows the sun's visibility over 0.15 s.
        public static EnhancementsSettings Defaults => new()
        {
            Size = (uint)Marshal.SizeOf<EnhancementsSettings>(),
            Flip = 1,
            Ambient = new EnhancementsAmbient { Level = 0.6f, AoStrength = 0.5f, AoRadius = 1, FoliageAoStrength = 0.5f },
            ContactShadows = new EnhancementsContactShadows { Length = 0.6f, Strength = 1, Thickness = 0.25f, FoliageStrength = 0 },
            Atmosphere = new EnhancementsAtmosphere { Density = 0.000325f, HeightFalloff = 0.02f, SunScatter = 1, Anisotropy = 0.7f },
            Tonemap = new EnhancementsTonemap { Curve = EnhancementsCurve.Game, BloomStrength = 1, Saturation = 1, Contrast = 1 },
            SunShadows = new EnhancementsSunShadows { LightSize = 0.03f, MinPenumbra = 0.02f, Strength = 1 },
            Wetness = new EnhancementsWetness { Wetness = 0, Darkening = 0.35f, Puddles = 0.3f },
            LightShafts = new EnhancementsLightShafts
            {
                Strength = 3, Density = 0.006f, Height = 5, Anisotropy = 0.6f, Distance = 150, Patchiness = 1, PatchSize = 6, Ambient = 0.3f, Steps = 48,
            },
            Trace = new EnhancementsTrace
            {
                Outputs = EnhancementsTraceOutputs.Ao | EnhancementsTraceOutputs.Sun, Casters = EnhancementsTraceCasters.Characters | EnhancementsTraceCasters.AlphaCards,
                AoDistance = 2, AoStrength = 1, SunSize = 0.0093f, SunStrength = 1, AoSamples = 64, SunSamples = 32, BudgetMs = 100,
            },
            Gather = new EnhancementsGather { Samples = 64, Sky = EnhancementsGatherSky.Visibility, Filter = 2, Strength = 1, HitAmbient = 0.5f },
            DepthOfField = new EnhancementsDepthOfField
            {
                FNumber = 2.8f, SensorHeightMm = 24, FocusX = 0.5f, FocusY = 0.5f, FocusSeconds = 2, MaxBlur = 0.03f, BladeRoundness = 0.5f,
                SphericalAberration = 0.6f, Anamorphic = 1, Samples = 49, HighlightThreshold = 2, HighlightGain = 2, EyeSize = 0.015f,
            },
            LensFlare = new EnhancementsLensFlare
            {
                Source = EnhancementsFlareSource.Sun, X = 0.8f, Y = 0.12f, Strength = 1, Arcs = 1, Ghosts = 1, Streaks = 1, Glare = 1, Size = 1, Warmth = 0.5f,
                Chromatic = 1, VisibilitySeconds = 0.15f,
            },
        };
    }

    // ENHANCEMENTS_ACCUMULATE_* in enhancements.h, in order.
    internal enum EnhancementsAccumulateMode : uint { Off, Add, Present }

    // EnhancementsAccumulate in enhancements.h, field for field.
    [StructLayout(LayoutKind.Sequential)]
    internal struct EnhancementsAccumulate
    {
        public uint Size;
        public EnhancementsAccumulateMode Mode;
        public uint Generation;
        public float CatEye;
        public float CatEyeFalloff;
    }

    // EnhancementsCamera.lens_sample in enhancements.h: the aperture point the camera was moved to, x right and y up in the unit
    // disk, the sample's index, and whether the accumulator adds this frame.
    internal readonly record struct LensSample(float X, float Y, uint Index, bool Mark);

    // EnhancementsVariant in enhancements.h: a 32-byte UTF-8 name, then the settings.
    [StructLayout(LayoutKind.Sequential)]
    internal struct EnhancementsVariant
    {
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)]
        public byte[] Name;
        public EnhancementsSettings Settings;
    }

    // ENHANCEMENTS_FACE_* in enhancements.h, in order, and ENHANCEMENTS_FACE_NONE.
    internal enum EnhancementsFace : uint { PosX, NegX, PosY, NegY, PosZ, NegZ, View, None = 0xFFFFFFFF }

    // EnhancementsShot in enhancements.h, field for field.
    [StructLayout(LayoutKind.Sequential)]
    internal struct EnhancementsShot
    {
        public uint Size;
        public uint Active;
        public uint Samples;        // 0: the applied gather's
        public float BudgetMs;      // 0: the applied trace's
    }

    // ENHANCEMENTS_SCENE_* in enhancements.h. Trace with no faces traces the self-test scene; Release stops a capture that runs
    // and frees the traced scene.
    [Flags]
    internal enum EnhancementsSceneFlags : uint
    {
        None = 0, Raw = 1 << 0, Ply = 1 << 1, Trace = 1 << 2, NoMeshes = 1 << 3, GBuffer = 1 << 4, Release = 1 << 5, Light = 1 << 6,
    }

    // EnhancementsSceneRequest in enhancements.h, field for field. Zero fields read as the add-on's defaults: all seven faces
    // (with Trace, none: the self-test scene), 45 presents of settle and 4000000 triangles a face.
    [StructLayout(LayoutKind.Sequential)]
    internal struct EnhancementsSceneRequest
    {
        public uint Size;
        public uint Faces;          // bits EnhancementsFaces.Bit(face)
        public uint Settle;
        public EnhancementsSceneFlags Flags;
        public uint MaxTriangles;
    }

    // Where each cube face of a scene capture looks, in Unity world space: D3D cube-map order and up vectors, as
    // enhancements.h lists them, at a 90-degree vertical field of view.
    internal static class EnhancementsFaces
    {
        public const float FieldOfView = 90;

        public static uint Bit(EnhancementsFace face) => 1u << (int)face;

        // Null for the view and None, which keep the camera as it is.
        public static (Vector3 Forward, Vector3 Up)? Direction(EnhancementsFace face) => face switch
        {
            EnhancementsFace.PosX => (new Vector3(1, 0, 0), new Vector3(0, 1, 0)),
            EnhancementsFace.NegX => (new Vector3(-1, 0, 0), new Vector3(0, 1, 0)),
            EnhancementsFace.PosY => (new Vector3(0, 1, 0), new Vector3(0, 0, -1)),
            EnhancementsFace.NegY => (new Vector3(0, -1, 0), new Vector3(0, 0, 1)),
            EnhancementsFace.PosZ => (new Vector3(0, 0, 1), new Vector3(0, 1, 0)),
            EnhancementsFace.NegZ => (new Vector3(0, 0, -1), new Vector3(0, 1, 0)),
            _ => null,
        };
    }

    // EnhancementsStatus in enhancements.h, field for field. Bit fields use ENHANCEMENTS_STEP_*, ENHANCEMENTS_PASS_* and ENHANCEMENTS_ENTRY_*.
    [StructLayout(LayoutKind.Sequential)]
    internal struct EnhancementsStatus
    {
        public uint Size;
        public uint Armed;
        public ulong Frames;
        public ulong SettingsApplied;
        public uint RenderWidth;
        public uint RenderHeight;
        public uint StepsFound;
        public uint StepsWanted;
        public uint PassesRan;
        public uint PassesSkipped;
        public uint EntriesMissing;
        public EnhancementsView ViewShown;
        public uint CameraAge;
        public uint Restarts;
        public EnhancementsError Error;
        public uint CompareRemaining;
        public EnhancementsAccumulateMode AccumMode;
        public uint AccumSamples;
        public uint AccumGeneration;
        public EnhancementsFace SceneFace;
        public uint SceneRemaining;
        public uint SceneSupport;   // 0 untried, 1 stream output wrote triangles, 2 the Deck's DXVK refused stream output
        public EnhancementsTraceState TraceState;
        public uint TraceTriangles;
        public uint TraceSamples;   // full passes: samples every traced pixel has
        public uint TraceMraysX100; // rays a second, in millions times 100
        public uint TraceGeneration;
        public uint TraceTarget;    // the samples every traced pixel gets
        public uint TraceConverged; // 1 when every traced pixel has TraceTarget samples and the last frame was written from them
        public uint TraceErrorX10000;   // the gathered light's mean relative standard error, times 10000
        public EnhancementsTraceLight TraceLight;
        public uint TraceRestarts;  // times the samples have started; while it rises and TraceSamples stays 0, none accumulates
        public EnhancementsTraceRestart TraceRestartReason;    // why they last did
        public uint ShotActive;     // 1 when the last frame ran with SetShot's shot, so TraceConverged is the shot's
        public uint FramesSaved;    // frames SaveFrame has saved since the add-on loaded
        public float DofFocusMeters;    // the depth of field's focus distance, read back a few frames late; 0 before it ran
        public float FlareVisibility;   // the lens flare's source's visibility, 0 to 1, read back a few frames late; 0 before it ran
    }

    // The only class that knows CameraToolsEnhancements.addon64.
    internal static class Enhancements
    {
        private const string ModuleName = "CameraToolsEnhancements.addon64";
        private const uint Version = 17;
        private const float LookInterval = 1f;

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate uint VersionCall();

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate ulong ApplyCall(ref EnhancementsSettings settings);

        // EnhancementsCamera is two float[16] and four float[4] in a row, so one float[48] passes it without a struct.
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void SetCameraCall([In] float[] camera);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void GetStatusCall(ref EnhancementsStatus status);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate uint CompareCall([In] EnhancementsVariant[] variants, uint count);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate uint DescribeCall([Out] byte[] text, uint size);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void SetAccumulateCall(ref EnhancementsAccumulate accumulate);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate float DepthAtCall(float u, float v);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate uint CaptureSceneCall(ref EnhancementsSceneRequest request);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate uint SaveTraceCall();

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void SetShotCall(ref EnhancementsShot shot);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate uint SaveFrameCall();

        private sealed record AddOn(IntPtr Module, ApplyCall Apply, SetCameraCall SetCamera, GetStatusCall GetStatus, DescribeCall Describe, CompareCall Compare,
            SetAccumulateCall SetAccumulate, DepthAtCall DepthAt, CaptureSceneCall CaptureScene, SaveTraceCall SaveTrace, SetShotCall SetShot, SaveFrameCall SaveFrame);

        private static readonly float[] camera = new float[48];
        private static readonly byte[] line = new byte[512];
        private static AddOn addOn;
        // A loaded module that is not an add-on of this version, so it is reported once and not tried again.
        private static IntPtr rejected;
        // The version of a loaded add-on that is too old or too new, or null.
        private static uint? rejectedVersion;
        private static float nextLook;

        public static bool Connected => addOn != null;

        // Why the add-on cannot be used, for a heading, or null while it can.
        public static string Missing => addOn != null ? null
            : rejectedVersion is uint found ? $"Add-on {found} loaded, {Version} needed"
            : "Add-on not found";

        // Rises with every connection. An add-on that ReShade loaded again has forgotten what the last one was told.
        public static int Connections { get; private set; }

        // Pushes the whole desired state. Returns its generation, which Status.SettingsApplied reaches once a frame used
        // it, or 0 while the add-on is not loaded.
        public static ulong Apply(EnhancementsSettings settings)
        {
            if (addOn == null)
                return 0;
            settings.Size = (uint)Marshal.SizeOf<EnhancementsSettings>();
            return addOn.Apply(ref settings);
        }

        // From Camera.onPreCull of the main camera, every frame while Enhancements are enabled. viewToClip is
        // GL.GetGPUProjectionMatrix(camera.projectionMatrix, false): Settings.Flip owns the vertical orientation.
        // towardSun is the world direction toward the sun, such as -sunLight.transform.forward, of any length;
        // Vector3.zero means no sun, and contact shadows then add nothing. sunColor is the sun light's color in linear RGB
        // times its intensity, and skyColor the sky's ambient light in linear RGB; the atmosphere pass scatters both, and
        // their alpha is ignored. lens is the aperture point this frame renders from, default while no depth of field is
        // being sampled. wetness is the weather's, 0 dry to 1 soaked, which the wetness pass follows while
        // Settings.Wetness.Wetness is 0.
        public static void SetCamera(Matrix4x4 worldToView, Matrix4x4 viewToClip, Vector3 towardSun, Color sunColor, Color skyColor, LensSample lens,
            float wetness)
        {
            if (addOn == null)
                return;
            ColumnMajor(worldToView, 0);
            ColumnMajor(viewToClip, 16);
            camera[32] = towardSun.x;
            camera[33] = towardSun.y;
            camera[34] = towardSun.z;
            camera[35] = 0;
            camera[36] = sunColor.r;
            camera[37] = sunColor.g;
            camera[38] = sunColor.b;
            camera[39] = 0;
            camera[40] = skyColor.r;
            camera[41] = skyColor.g;
            camera[42] = skyColor.b;
            camera[43] = wetness;
            camera[44] = lens.X;
            camera[45] = lens.Y;
            camera[46] = lens.Index;
            camera[47] = lens.Mark ? 1 : 0;
            addOn.SetCamera(camera);
        }

        // Starts, presents or stops lens-sampled depth of field; it takes effect at the add-on's next frame boundary. While
        // Mode is Add, each frame whose camera came with a marked lens sample is added into the sum, which a new Generation
        // empties first; Status.AccumGeneration and AccumSamples follow. While Mode is Present, the sum's average replaces
        // the HDR scene before the game's bloom and tone map.
        public static void SetAccumulate(EnhancementsAccumulate accumulate)
        {
            if (addOn == null)
                return;
            accumulate.Size = (uint)Marshal.SizeOf<EnhancementsAccumulate>();
            addOn.SetAccumulate(ref accumulate);
        }

        // The view-space depth in meters at a screen point, u and v from 0 at the top left to 1, or a negative number
        // until the add-on has read it from a frame after the first call for this point: call it once a frame until it is
        // not negative. Negative while the add-on is not loaded.
        public static float DepthAt(float u, float v) => addOn?.DepthAt(u, v) ?? -1f;

        // Starts a comparison capture of up to 16 variants: the add-on renders each one's settings on the following frames
        // and saves a PNG of each to <ReShade base path>\Enhancements\compare-<time>\, then restores the settings it had.
        // Names longer than 32 UTF-8 bytes are cut. False while the add-on is not loaded, a capture is still running
        // (Status.CompareRemaining is not 0), or the variants are invalid.
        public static bool Compare(params (string Name, EnhancementsSettings Settings)[] variants)
        {
            if (addOn == null)
                return false;
            var native = new EnhancementsVariant[variants.Length];
            for (int i = 0; i < variants.Length; i++)
            {
                byte[] name = new byte[32];
                byte[] utf8 = Encoding.UTF8.GetBytes(variants[i].Name ?? "");
                Array.Copy(utf8, name, Math.Min(utf8.Length, name.Length));
                EnhancementsSettings settings = variants[i].Settings;
                settings.Size = (uint)Marshal.SizeOf<EnhancementsSettings>();
                native[i] = new EnhancementsVariant { Name = name, Settings = settings };
            }
            return addOn.Compare(native, (uint)native.Length) != 0;
        }

        // Starts a scene capture: the add-on asks for each requested face in turn through Status.SceneFace, the view
        // first, captures the geometry of a frame whose camera shows it, and writes it to
        // <ReShade base path>\Enhancements\scene-<time>\. The caller turns the camera to the face's EnhancementsFaces.Direction
        // at EnhancementsFaces.FieldOfView while SceneFace names a cube face. Status.SceneRemaining counts the faces not yet
        // written. False while the add-on is not loaded, a scene capture is still running or saving, or Faces holds no
        // known face. A capture that was stopped is still saving for a moment, so a caller that released one asks again.
        public static bool CaptureScene(EnhancementsSceneRequest request)
        {
            if (addOn == null)
                return false;
            request.Size = (uint)Marshal.SizeOf<EnhancementsSceneRequest>();
            return addOn.CaptureScene(ref request) != 0;
        }

        // Stops a scene capture that runs and frees the traced scene, at the add-on's next present.
        public static void ReleaseScene() => CaptureScene(new EnhancementsSceneRequest { Flags = EnhancementsSceneFlags.Release });

        // Starts or ends a traced shot: while active, the trace pass runs with the gather on over whatever settings are
        // applied, also while their master switch is off, with samples and budgetMs in place of the applied ones when they
        // are not 0. Status.ShotActive follows a frame later; Status.TraceConverged then says when the frame is written from
        // all its samples.
        public static void SetShot(bool active, uint samples = 0, float budgetMs = 0)
        {
            if (addOn == null)
                return;
            var shot = new EnhancementsShot { Size = (uint)Marshal.SizeOf<EnhancementsShot>(), Active = active ? 1u : 0u, Samples = samples, BudgetMs = budgetMs };
            addOn.SetShot(ref shot);
        }

        // Saves the next frame the trace pass writes, with what the Mac's reference tracer needs, to
        // <ReShade base path>\Enhancements\trace-<time>\. False while the add-on is not loaded, the traced scene is not ready
        // (Status.TraceState), before its first full pass (Status.TraceSamples), or while a save waits.
        public static bool SaveTrace() => addOn != null && addOn.SaveTrace() != 0;

        // Saves the next frame's HDR scene and depth before the add-on's passes, with the camera, for the Mac's depth of field
        // harness, to <ReShade base path>\Enhancements\frame-<time>\; Status.FramesSaved rises once it is written. False
        // while the add-on is not loaded or a save waits.
        public static bool SaveFrame() => addOn != null && addOn.SaveFrame() != 0;

        // SceneFace reads None while the add-on is not loaded, so no caller turns the camera for a capture that is not there.
        public static EnhancementsStatus Status
        {
            get
            {
                var status = new EnhancementsStatus { Size = (uint)Marshal.SizeOf<EnhancementsStatus>(), SceneFace = EnhancementsFace.None };
                addOn?.GetStatus(ref status);
                return status;
            }
        }

        // The add-on's one-line report of the last frame, "" while it is not loaded. It holds no frame counter, so log it
        // when it changes.
        public static string Describe()
        {
            if (addOn == null)
                return "";
            uint length = addOn.Describe(line, (uint)line.Length);
            return Encoding.UTF8.GetString(line, 0, (int)Math.Min(length, (uint)line.Length));
        }

        // ReShade unloads its add-ons with the last device and loads them again with the next, so the module is looked up
        // every second even while connected: a module that left or moved must not be called through the old pointers.
        // A new connection has forgotten the settings, so the caller applies them again when Connections rises.
        public static void Update()
        {
            float now = Time.unscaledTime;
            if (now < nextLook)
                return;
            nextLook = now + LookInterval;
            try
            {
                IntPtr module = GetModuleHandleW(ModuleName);
                if (module == (addOn?.Module ?? rejected))
                    return;
                if (addOn != null)
                    MelonLogger.Msg($"Enhancements: {ModuleName} was unloaded.");
                addOn = null;
                rejected = IntPtr.Zero;
                rejectedVersion = null;
                if (module != IntPtr.Zero)
                    Connect(module);
            }
            catch (Exception e)
            {
                nextLook = float.PositiveInfinity;
                addOn = null;
                MelonLogger.Warning($"Enhancements: looking for {ModuleName} failed; stopped looking. {e}");
            }
        }

        private static void Connect(IntPtr module)
        {
            rejected = module;
            if (!TryExport<VersionCall>(module, "EnhancementsVersion", out var version))
            {
                MelonLogger.Warning($"Enhancements: {ModuleName} is loaded but has no EnhancementsVersion export.");
                return;
            }
            uint found = version();
            if (found != Version)
            {
                rejectedVersion = found;
                MelonLogger.Warning($"Enhancements: {ModuleName} is version {found}, and this binding needs version {Version}; update both together.");
                return;
            }
            if (!TryExport<ApplyCall>(module, "EnhancementsApply", out var apply)
                || !TryExport<SetCameraCall>(module, "EnhancementsSetCamera", out var setCamera)
                || !TryExport<GetStatusCall>(module, "EnhancementsGetStatus", out var getStatus)
                || !TryExport<DescribeCall>(module, "EnhancementsDescribe", out var describe)
                || !TryExport<CompareCall>(module, "EnhancementsCompare", out var compare)
                || !TryExport<SetAccumulateCall>(module, "EnhancementsSetAccumulate", out var setAccumulate)
                || !TryExport<DepthAtCall>(module, "EnhancementsDepthAt", out var depthAt)
                || !TryExport<CaptureSceneCall>(module, "EnhancementsCaptureScene", out var captureScene)
                || !TryExport<SaveTraceCall>(module, "EnhancementsSaveTrace", out var saveTrace)
                || !TryExport<SetShotCall>(module, "EnhancementsSetShot", out var setShot)
                || !TryExport<SaveFrameCall>(module, "EnhancementsSaveFrame", out var saveFrame))
            {
                MelonLogger.Warning($"Enhancements: {ModuleName} version {found} is missing an export.");
                return;
            }
            rejected = IntPtr.Zero;
            addOn = new AddOn(module, apply, setCamera, getStatus, describe, compare, setAccumulate, depthAt, captureScene, saveTrace, setShot, saveFrame);
            Connections++;
            MelonLogger.Msg($"Enhancements: connected to {ModuleName} version {found}.");
        }

        // Unity's Matrix4x4 memory order: m00, m10, m20, m30, m01, and so on. The game's interop type has the fields but
        // no indexer.
        private static void ColumnMajor(Matrix4x4 m, int offset)
        {
            camera[offset] = m.m00; camera[offset + 1] = m.m10; camera[offset + 2] = m.m20; camera[offset + 3] = m.m30;
            camera[offset + 4] = m.m01; camera[offset + 5] = m.m11; camera[offset + 6] = m.m21; camera[offset + 7] = m.m31;
            camera[offset + 8] = m.m02; camera[offset + 9] = m.m12; camera[offset + 10] = m.m22; camera[offset + 11] = m.m32;
            camera[offset + 12] = m.m03; camera[offset + 13] = m.m13; camera[offset + 14] = m.m23; camera[offset + 15] = m.m33;
        }

        private static bool TryExport<T>(IntPtr module, string name, out T call) where T : Delegate
        {
            call = NativeLibrary.TryGetExport(module, name, out IntPtr address) ? Marshal.GetDelegateForFunctionPointer<T>(address) : null;
            return call != null;
        }

        [DllImport("kernel32", CharSet = CharSet.Unicode, ExactSpelling = true)]
        private static extern IntPtr GetModuleHandleW(string name);
    }
}
