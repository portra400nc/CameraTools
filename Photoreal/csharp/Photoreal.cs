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
        ShadowMask, QuarterShadow, AmbientDiffuse, AmbientSpecular, HdrScene, Count,
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

    // PhotorealSettings in photoreal.h, field for field.
    [StructLayout(LayoutKind.Sequential)]
    internal struct PhotorealSettings
    {
        public uint Size;
        public uint Enabled;
        public PhotorealView View;
        public uint Flip;
        public PhotorealAmbient Ambient;

        // The add-on's defaults: everything off, game targets upside down, and when switched on ambient level 0.6, AO
        // strength 0.5 at a 1 m radius, and half that strength on grass, vegetation and foliage.
        public static PhotorealSettings Defaults => new()
        {
            Size = (uint)Marshal.SizeOf<PhotorealSettings>(),
            Flip = 1,
            Ambient = new PhotorealAmbient { Level = 0.6f, AoStrength = 0.5f, AoRadius = 1, FoliageAoStrength = 0.5f },
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
    }

    // The only class that knows CameraToolsPhotoreal.addon64.
    internal static class Photoreal
    {
        private const string ModuleName = "CameraToolsPhotoreal.addon64";
        private const uint Version = 1;
        private const float LookInterval = 1f;

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate uint VersionCall();

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate ulong ApplyCall(ref PhotorealSettings settings);

        // PhotorealCamera is two float[16] in a row, so one float[32] passes it without a struct.
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void SetCameraCall([In] float[] camera);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void GetStatusCall(ref PhotorealStatus status);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate uint DescribeCall([Out] byte[] text, uint size);

        private sealed record AddOn(IntPtr Module, ApplyCall Apply, SetCameraCall SetCamera, GetStatusCall GetStatus, DescribeCall Describe);

        private static readonly float[] camera = new float[32];
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
        public static void SetCamera(Matrix4x4 worldToView, Matrix4x4 viewToClip)
        {
            if (addOn == null)
                return;
            ColumnMajor(worldToView, 0);
            ColumnMajor(viewToClip, 16);
            addOn.SetCamera(camera);
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
                || !TryExport<DescribeCall>(module, "PhotorealDescribe", out var describe))
            {
                MelonLogger.Warning($"Photoreal: {ModuleName} version {found} is missing an export.");
                return;
            }
            rejected = IntPtr.Zero;
            addOn = new AddOn(module, apply, setCamera, getStatus, describe);
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
