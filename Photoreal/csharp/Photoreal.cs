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
        ShadowMask, QuarterShadow, AmbientDiffuse, AmbientSpecular, HdrScene, BloomFinal, SunAtlas, Count,
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

        // The add-on's defaults: everything off, game targets upside down, and when switched on ambient level 0.6, AO
        // strength 0.5 at a 1 m radius, half that strength on grass, vegetation and foliage, full-strength contact
        // shadows 0.6 m long that take surfaces to be 0.25 m thick and leave grass, vegetation and foliage unshadowed, and
        // haze that dims a pixel 500 m away by 15%, thins by e every 50 m up, and glows around the sun, a tone map that
        // reproduces the game's, and full-strength sun shadows whose soft edge is 0.03 m wide per meter from the caster
        // and at least 0.02 m.
        public static PhotorealSettings Defaults => new()
        {
            Size = (uint)Marshal.SizeOf<PhotorealSettings>(),
            Flip = 1,
            Ambient = new PhotorealAmbient { Level = 0.6f, AoStrength = 0.5f, AoRadius = 1, FoliageAoStrength = 0.5f },
            ContactShadows = new PhotorealContactShadows { Length = 0.6f, Strength = 1, Thickness = 0.25f, FoliageStrength = 0 },
            Atmosphere = new PhotorealAtmosphere { Density = 0.000325f, HeightFalloff = 0.02f, SunScatter = 1, Anisotropy = 0.7f },
            Tonemap = new PhotorealTonemap { Curve = PhotorealCurve.Game, BloomStrength = 1, Saturation = 1, Contrast = 1 },
            SunShadows = new PhotorealSunShadows { LightSize = 0.03f, MinPenumbra = 0.02f, Strength = 1 },
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
    }

    // The only class that knows CameraToolsPhotoreal.addon64.
    internal static class Photoreal
    {
        private const string ModuleName = "CameraToolsPhotoreal.addon64";
        private const uint Version = 5;
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

        private sealed record AddOn(IntPtr Module, ApplyCall Apply, SetCameraCall SetCamera, GetStatusCall GetStatus, DescribeCall Describe, CompareCall Compare,
            SetAccumulateCall SetAccumulate, DepthAtCall DepthAt);

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
        // being sampled.
        public static void SetCamera(Matrix4x4 worldToView, Matrix4x4 viewToClip, Vector3 towardSun, Color sunColor, Color skyColor, LensSample lens)
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
            camera[43] = 0;
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

        public static PhotorealStatus Status
        {
            get
            {
                var status = new PhotorealStatus { Size = (uint)Marshal.SizeOf<PhotorealStatus>() };
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
                || !TryExport<DepthAtCall>(module, "PhotorealDepthAt", out var depthAt))
            {
                MelonLogger.Warning($"Photoreal: {ModuleName} version {found} is missing an export.");
                return;
            }
            rejected = IntPtr.Zero;
            addOn = new AddOn(module, apply, setCamera, getStatus, describe, compare, setAccumulate, depthAt);
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
