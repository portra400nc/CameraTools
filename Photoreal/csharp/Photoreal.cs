// The C# binding of CameraToolsPhotoreal.addon64 (Photoreal/photoreal.h). A MelonLoader mod adds this file as is.
// ReShade loads the add-on, some time after the mod starts or never, so the binding looks for the loaded module and never
// loads a copy of its own, as CameraTools/ReShade.cs does for the bridge. Every call returns at once and takes effect at
// the add-on's next frame boundary, so callers watch Status or Describe().
using System.Runtime.InteropServices;
using System.Text;
using MelonLoader;
using UnityEngine;
using NativeLibrary = System.Runtime.InteropServices.NativeLibrary;

namespace CameraToolsPhotoreal
{
    // PHOTOREAL_VIEW_* in photoreal.h, in order.
    internal enum PhotorealView : uint
    {
        Off, Normals, Albedo, Specular, MaterialId, Smoothness, Character, Depth, Stencil,
        ShadowMask, QuarterShadow, AmbientDiffuse, AmbientSpecular, HdrScene, BloomFinal, SunAtlas, LightShafts, Trace, Bounce, SkyVisibility, SkyMap, Count,
    }

    // PHOTOREAL_ERROR_* in photoreal.h.
    internal enum PhotorealError : uint { None, NotD3D11, Shader, Texture, State }

    // PhotorealAmbient in photoreal.h, field for field.
    [StructLayout(LayoutKind.Sequential)]
    internal struct PhotorealAmbient
    {
        public uint Enabled;
        public float Level;
        public float AoStrength;
        public float AoRadius;
        public float FoliageAoStrength;
    }

    // PhotorealContactShadows in photoreal.h, field for field.
    [StructLayout(LayoutKind.Sequential)]
    internal struct PhotorealContactShadows
    {
        public uint Enabled;
        public float Length;
        public float Strength;
        public float Thickness;
        public float FoliageStrength;
    }

    // PhotorealAtmosphere in photoreal.h, field for field.
    [StructLayout(LayoutKind.Sequential)]
    internal struct PhotorealAtmosphere
    {
        public uint Enabled;
        public float Density;
        public float HeightFalloff;
        public float SunScatter;
        public float Anisotropy;
    }

    // PHOTOREAL_CURVE_* in photoreal.h, in order.
    internal enum PhotorealCurve : uint { Game, AgX, Neutral }

    // PhotorealTonemap in photoreal.h, field for field.
    [StructLayout(LayoutKind.Sequential)]
    internal struct PhotorealTonemap
    {
        public uint Enabled;
        public float ExposureEv;
        public PhotorealCurve Curve;
        public float BloomStrength;
        public float Saturation;
        public float Contrast;
    }

    // PhotorealSunShadows in photoreal.h, field for field.
    [StructLayout(LayoutKind.Sequential)]
    internal struct PhotorealSunShadows
    {
        public uint Enabled;
        public float LightSize;
        public float MinPenumbra;
        public float Strength;
    }

    // PhotorealWetness in photoreal.h, field for field.
    [StructLayout(LayoutKind.Sequential)]
    internal struct PhotorealWetness
    {
        public uint Enabled;
        public float Wetness;
        public float Darkening;
        public float Puddles;
    }

    // PhotorealLightShafts in photoreal.h, field for field.
    [StructLayout(LayoutKind.Sequential)]
    internal struct PhotorealLightShafts
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

    // PHOTOREAL_TRACE_AO and PHOTOREAL_TRACE_SUN in photoreal.h: what the trace pass writes. None reads as both.
    [Flags]
    internal enum PhotorealTraceOutputs : uint { None = 0, Ao = 1 << 0, Sun = 1 << 1 }

    // PHOTOREAL_TRACE_CAST_* in photoreal.h: what casts in the traced scene besides the world.
    [Flags]
    internal enum PhotorealTraceCasters : uint { None = 0, Characters = 1 << 0, AlphaCards = 1 << 2, WholeCards = 1 << 3 }

    // PHOTOREAL_TRACE_STATE_* in photoreal.h, in order.
    internal enum PhotorealTraceState : uint { None, Building, Uploading, Ready, Stale, Failed }

    // PhotorealTrace in photoreal.h, field for field.
    [StructLayout(LayoutKind.Sequential)]
    internal struct PhotorealTrace
    {
        public uint Enabled;
        public PhotorealTraceOutputs Outputs;
        public PhotorealTraceCasters Casters;
        public float AoDistance;
        public float AoStrength;
        public float SunSize;
        public float SunStrength;
        public uint AoSamples;
        public uint SunSamples;
        public float BudgetMs;
    }

    // PHOTOREAL_GATHER_SKY_* in photoreal.h, in order: how the gathered light meets the game's sky light.
    internal enum PhotorealGatherSky : uint { Occlusion, Visibility, Weighted, Map }

    // PHOTOREAL_TRACE_LIGHT_* in photoreal.h: what of the traced scene's light came from the game.
    [Flags]
    internal enum PhotorealTraceLight : uint { None = 0, Albedo = 1 << 0, Sky = 1 << 1, Sun = 1 << 2, Ambient = 1 << 3 }

    // PhotorealGather in photoreal.h, field for field.
    [StructLayout(LayoutKind.Sequential)]
    internal struct PhotorealGather
    {
        public uint Enabled;
        public uint Samples;
        public PhotorealGatherSky Sky;
        public uint Filter;
        public float Strength;
        public float HitAmbient;
    }

    // PhotorealSettings in photoreal.h, field for field.
    [StructLayout(LayoutKind.Sequential)]
    internal struct PhotorealSettings
    {
        public uint Size;
        public uint Enabled;
        public PhotorealView View;
        public uint Flip;
        public PhotorealAmbient Ambient;
        public PhotorealContactShadows ContactShadows;
        public PhotorealAtmosphere Atmosphere;
        public PhotorealTonemap Tonemap;
        public PhotorealSunShadows SunShadows;
        public PhotorealWetness Wetness;
        public PhotorealLightShafts LightShafts;
        public PhotorealTrace Trace;
        public PhotorealGather Gather;

        // The add-on's defaults: everything off, game targets upside down, and when switched on ambient level 0.6, AO
        // strength 0.5 at a 1 m radius, half that strength on grass, vegetation and foliage, full-strength contact
        // shadows 0.6 m long that take surfaces to be 0.25 m thick and leave grass, vegetation and foliage unshadowed, and
        // haze that dims a pixel 500 m away by 15%, thins by e every 50 m up, and glows around the sun, a tone map that
        // reproduces the game's, full-strength sun shadows whose soft edge is 0.03 m wide per meter from the caster and at
        // least 0.02 m, wet surfaces that follow the weather, darken by 0.35 when soaked and grow puddles on flat ground at
        // 0.3, and light shafts at strength 3 through a mist that hides 26% of a pixel 50 m away, thins by e every 5 m up,
        // scatters sun light forward with anisotropy 0.6, is marched 150 m in 48 steps, lies in patches 6 m across at
        // patchiness 1, and takes the sky's light at 0.3, and a trace of occlusion within 2 m and sun shadows of the real
        // sun's size, both at full strength and cast by characters and cards too, to 64 and 32 samples a pixel within 20 ms a
        // frame, applied with the ambient level, and when switched on a gather of one bounce, 128 samples a pixel, its sky
        // light by visibility, filtered twice, at full strength, with half the game's ambient light on the surfaces it meets.
        public static PhotorealSettings Defaults => new()
        {
            Size = (uint)Marshal.SizeOf<PhotorealSettings>(),
            Flip = 1,
            Ambient = new PhotorealAmbient { Level = 0.6f, AoStrength = 0.5f, AoRadius = 1, FoliageAoStrength = 0.5f },
            ContactShadows = new PhotorealContactShadows { Length = 0.6f, Strength = 1, Thickness = 0.25f, FoliageStrength = 0 },
            Atmosphere = new PhotorealAtmosphere { Density = 0.000325f, HeightFalloff = 0.02f, SunScatter = 1, Anisotropy = 0.7f },
            Tonemap = new PhotorealTonemap { Curve = PhotorealCurve.Game, BloomStrength = 1, Saturation = 1, Contrast = 1 },
            SunShadows = new PhotorealSunShadows { LightSize = 0.03f, MinPenumbra = 0.02f, Strength = 1 },
            Wetness = new PhotorealWetness { Wetness = 0, Darkening = 0.35f, Puddles = 0.3f },
            LightShafts = new PhotorealLightShafts
            {
                Strength = 3, Density = 0.006f, Height = 5, Anisotropy = 0.6f, Distance = 150, Patchiness = 1, PatchSize = 6, Ambient = 0.3f, Steps = 48,
            },
            Trace = new PhotorealTrace
            {
                Outputs = PhotorealTraceOutputs.Ao | PhotorealTraceOutputs.Sun, Casters = PhotorealTraceCasters.Characters | PhotorealTraceCasters.AlphaCards,
                AoDistance = 2, AoStrength = 1, SunSize = 0.0093f, SunStrength = 1, AoSamples = 64, SunSamples = 32, BudgetMs = 20,
            },
            Gather = new PhotorealGather { Samples = 128, Sky = PhotorealGatherSky.Visibility, Filter = 2, Strength = 1, HitAmbient = 0.5f },
        };
    }

    // PHOTOREAL_ACCUMULATE_* in photoreal.h, in order.
    internal enum PhotorealAccumulateMode : uint { Off, Add, Present }

    // PhotorealAccumulate in photoreal.h, field for field.
    [StructLayout(LayoutKind.Sequential)]
    internal struct PhotorealAccumulate
    {
        public uint Size;
        public PhotorealAccumulateMode Mode;
        public uint Generation;
        public float CatEye;
        public float CatEyeFalloff;
    }

    // PhotorealCamera.lens_sample in photoreal.h: the aperture point the camera was moved to, x right and y up in the unit
    // disk, the sample's index, and whether the accumulator adds this frame.
    internal readonly record struct LensSample(float X, float Y, uint Index, bool Mark);

    // PhotorealVariant in photoreal.h: a 32-byte UTF-8 name, then the settings.
    [StructLayout(LayoutKind.Sequential)]
    internal struct PhotorealVariant
    {
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)]
        public byte[] Name;
        public PhotorealSettings Settings;
    }

    // PHOTOREAL_FACE_* in photoreal.h, in order, and PHOTOREAL_FACE_NONE.
    internal enum PhotorealFace : uint { PosX, NegX, PosY, NegY, PosZ, NegZ, View, None = 0xFFFFFFFF }

    // PHOTOREAL_SCENE_* in photoreal.h. Trace with no faces traces the self-test scene; Release frees the traced scene.
    [Flags]
    internal enum PhotorealSceneFlags : uint
    {
        None = 0, Raw = 1 << 0, Ply = 1 << 1, Trace = 1 << 2, NoMeshes = 1 << 3, GBuffer = 1 << 4, Release = 1 << 5, Light = 1 << 6,
    }

    // PhotorealSceneRequest in photoreal.h, field for field. Zero fields read as the add-on's defaults: all seven faces
    // (with Trace, none: the self-test scene), 45 presents of settle and 4000000 triangles a face.
    [StructLayout(LayoutKind.Sequential)]
    internal struct PhotorealSceneRequest
    {
        public uint Size;
        public uint Faces;          // bits PhotorealFaces.Bit(face)
        public uint Settle;
        public PhotorealSceneFlags Flags;
        public uint MaxTriangles;
    }

    // Where each cube face of a scene capture looks, in Unity world space: D3D cube-map order and up vectors, as
    // photoreal.h lists them, at a 90-degree vertical field of view.
    internal static class PhotorealFaces
    {
        public const float FieldOfView = 90;

        public static uint Bit(PhotorealFace face) => 1u << (int)face;

        // Null for the view and None, which keep the camera as it is.
        public static (Vector3 Forward, Vector3 Up)? Direction(PhotorealFace face) => face switch
        {
            PhotorealFace.PosX => (new Vector3(1, 0, 0), new Vector3(0, 1, 0)),
            PhotorealFace.NegX => (new Vector3(-1, 0, 0), new Vector3(0, 1, 0)),
            PhotorealFace.PosY => (new Vector3(0, 1, 0), new Vector3(0, 0, -1)),
            PhotorealFace.NegY => (new Vector3(0, -1, 0), new Vector3(0, 0, 1)),
            PhotorealFace.PosZ => (new Vector3(0, 0, 1), new Vector3(0, 1, 0)),
            PhotorealFace.NegZ => (new Vector3(0, 0, -1), new Vector3(0, 1, 0)),
            _ => null,
        };
    }

    // PhotorealStatus in photoreal.h, field for field. Bit fields use PHOTOREAL_STEP_*, PHOTOREAL_PASS_* and PHOTOREAL_ENTRY_*.
    [StructLayout(LayoutKind.Sequential)]
    internal struct PhotorealStatus
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
        public PhotorealView ViewShown;
        public uint CameraAge;
        public uint Restarts;
        public PhotorealError Error;
        public uint CompareRemaining;
        public PhotorealAccumulateMode AccumMode;
        public uint AccumSamples;
        public uint AccumGeneration;
        public PhotorealFace SceneFace;
        public uint SceneRemaining;
        public uint SceneSupport;   // 0 untried, 1 stream output wrote triangles, 2 the Deck's DXVK refused stream output
        public PhotorealTraceState TraceState;
        public uint TraceTriangles;
        public uint TraceSamples;   // full passes: samples every traced pixel has
        public uint TraceMraysX100; // rays a second, in millions times 100
        public uint TraceGeneration;
        public uint TraceTarget;    // the samples every traced pixel gets
        public uint TraceConverged; // 1 when every traced pixel has TraceTarget samples and the last frame was written from them
        public uint TraceErrorX10000;   // the gathered light's mean relative standard error, times 10000
        public PhotorealTraceLight TraceLight;
    }

    // The only class that knows CameraToolsPhotoreal.addon64.
    internal static class Photoreal
    {
        private const string ModuleName = "CameraToolsPhotoreal.addon64";
        private const uint Version = 9;
        private const float LookInterval = 1f;

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate uint VersionCall();

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate ulong ApplyCall(ref PhotorealSettings settings);

        // PhotorealCamera is two float[16] and four float[4] in a row, so one float[48] passes it without a struct.
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void SetCameraCall([In] float[] camera);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void GetStatusCall(ref PhotorealStatus status);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate uint CompareCall([In] PhotorealVariant[] variants, uint count);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate uint DescribeCall([Out] byte[] text, uint size);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void SetAccumulateCall(ref PhotorealAccumulate accumulate);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate float DepthAtCall(float u, float v);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate uint CaptureSceneCall(ref PhotorealSceneRequest request);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate uint SaveTraceCall();

        private sealed record AddOn(IntPtr Module, ApplyCall Apply, SetCameraCall SetCamera, GetStatusCall GetStatus, DescribeCall Describe, CompareCall Compare,
            SetAccumulateCall SetAccumulate, DepthAtCall DepthAt, CaptureSceneCall CaptureScene, SaveTraceCall SaveTrace);

        private static readonly float[] camera = new float[48];
        private static readonly byte[] line = new byte[512];
        private static AddOn addOn;
        // A loaded module that is not an add-on of this version, so it is reported once and not tried again.
        private static IntPtr rejected;
        private static float nextLook;

        public static bool Connected => addOn != null;

        // Pushes the whole desired state. Returns its generation, which Status.SettingsApplied reaches once a frame used
        // it, or 0 while the add-on is not loaded.
        public static ulong Apply(PhotorealSettings settings)
        {
            if (addOn == null)
                return 0;
            settings.Size = (uint)Marshal.SizeOf<PhotorealSettings>();
            return addOn.Apply(ref settings);
        }

        // From Camera.onPreCull of the main camera, every frame while Photoreal is enabled. viewToClip is
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
        public static void SetAccumulate(PhotorealAccumulate accumulate)
        {
            if (addOn == null)
                return;
            accumulate.Size = (uint)Marshal.SizeOf<PhotorealAccumulate>();
            addOn.SetAccumulate(ref accumulate);
        }

        // The view-space depth in meters at a screen point, u and v from 0 at the top left to 1, or a negative number
        // until the add-on has read it from a frame after the first call for this point: call it once a frame until it is
        // not negative. Negative while the add-on is not loaded.
        public static float DepthAt(float u, float v) => addOn?.DepthAt(u, v) ?? -1f;

        // Starts a comparison capture of up to 16 variants: the add-on renders each one's settings on the following frames
        // and saves a PNG of each to <ReShade base path>\Photoreal\compare-<time>\, then restores the settings it had.
        // Names longer than 32 UTF-8 bytes are cut. False while the add-on is not loaded, a capture is still running
        // (Status.CompareRemaining is not 0), or the variants are invalid.
        public static bool Compare(params (string Name, PhotorealSettings Settings)[] variants)
        {
            if (addOn == null)
                return false;
            var native = new PhotorealVariant[variants.Length];
            for (int i = 0; i < variants.Length; i++)
            {
                byte[] name = new byte[32];
                byte[] utf8 = Encoding.UTF8.GetBytes(variants[i].Name ?? "");
                Array.Copy(utf8, name, Math.Min(utf8.Length, name.Length));
                PhotorealSettings settings = variants[i].Settings;
                settings.Size = (uint)Marshal.SizeOf<PhotorealSettings>();
                native[i] = new PhotorealVariant { Name = name, Settings = settings };
            }
            return addOn.Compare(native, (uint)native.Length) != 0;
        }

        // Starts a scene capture: the add-on asks for each requested face in turn through Status.SceneFace, the view
        // first, captures the geometry of a frame whose camera shows it, and writes it to
        // <ReShade base path>\Photoreal\scene-<time>\. The caller turns the camera to the face's PhotorealFaces.Direction
        // at PhotorealFaces.FieldOfView while SceneFace names a cube face. Status.SceneRemaining counts the faces not yet
        // written. False while the add-on is not loaded, a scene capture is still running or saving, or Faces holds no
        // known face.
        public static bool CaptureScene(PhotorealSceneRequest request)
        {
            if (addOn == null)
                return false;
            request.Size = (uint)Marshal.SizeOf<PhotorealSceneRequest>();
            return addOn.CaptureScene(ref request) != 0;
        }

        // Saves the next frame the trace pass writes, with what the Mac's reference tracer needs, to
        // <ReShade base path>\Photoreal\trace-<time>\. False while the add-on is not loaded, the traced scene is not ready
        // (Status.TraceState), before its first full pass (Status.TraceSamples), or while a save waits.
        public static bool SaveTrace() => addOn != null && addOn.SaveTrace() != 0;

        // SceneFace reads None while the add-on is not loaded, so no caller turns the camera for a capture that is not there.
        public static PhotorealStatus Status
        {
            get
            {
                var status = new PhotorealStatus { Size = (uint)Marshal.SizeOf<PhotorealStatus>(), SceneFace = PhotorealFace.None };
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
        // A new connection has forgotten the settings, so the caller applies them again when Connected turns true.
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
                    MelonLogger.Msg($"Photoreal: {ModuleName} was unloaded.");
                addOn = null;
                rejected = IntPtr.Zero;
                if (module != IntPtr.Zero)
                    Connect(module);
            }
            catch (Exception e)
            {
                nextLook = float.PositiveInfinity;
                addOn = null;
                MelonLogger.Warning($"Photoreal: looking for {ModuleName} failed; stopped looking. {e}");
            }
        }

        private static void Connect(IntPtr module)
        {
            rejected = module;
            if (!TryExport<VersionCall>(module, "PhotorealVersion", out var version))
            {
                MelonLogger.Warning($"Photoreal: {ModuleName} is loaded but has no PhotorealVersion export.");
                return;
            }
            uint found = version();
            if (found != Version)
            {
                MelonLogger.Warning($"Photoreal: {ModuleName} is version {found}, and this binding needs version {Version}; update both together.");
                return;
            }
            if (!TryExport<ApplyCall>(module, "PhotorealApply", out var apply)
                || !TryExport<SetCameraCall>(module, "PhotorealSetCamera", out var setCamera)
                || !TryExport<GetStatusCall>(module, "PhotorealGetStatus", out var getStatus)
                || !TryExport<DescribeCall>(module, "PhotorealDescribe", out var describe)
                || !TryExport<CompareCall>(module, "PhotorealCompare", out var compare)
                || !TryExport<SetAccumulateCall>(module, "PhotorealSetAccumulate", out var setAccumulate)
                || !TryExport<DepthAtCall>(module, "PhotorealDepthAt", out var depthAt)
                || !TryExport<CaptureSceneCall>(module, "PhotorealCaptureScene", out var captureScene)
                || !TryExport<SaveTraceCall>(module, "PhotorealSaveTrace", out var saveTrace))
            {
                MelonLogger.Warning($"Photoreal: {ModuleName} version {found} is missing an export.");
                return;
            }
            rejected = IntPtr.Zero;
            addOn = new AddOn(module, apply, setCamera, getStatus, describe, compare, setAccumulate, depthAt, captureScene, saveTrace);
            MelonLogger.Msg($"Photoreal: connected to {ModuleName} version {found}.");
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
