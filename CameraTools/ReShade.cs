using System.Runtime.InteropServices;
using System.Text;
using MelonLoader;
using UnityEngine;
using NativeLibrary = System.Runtime.InteropServices.NativeLibrary;

namespace CameraTools
{
    // BridgeStatus in GenshinReShadeBridge's bridge.h, field for field.
    [StructLayout(LayoutKind.Sequential)]
    internal readonly record struct BridgeStatus(uint Runtime, uint EffectsEnabled, uint EffectsReady, uint Width, uint Height,
        ulong Presents, ulong Screenshots);

    // The only class that knows GenshinReShadeBridge.addon64, the add-on through which a mod drives ReShade. ReShade loads
    // the add-on, some time after CameraTools starts or never, so CameraTools looks for the loaded module and never loads
    // a copy of its own. Every call returns at once and takes effect at ReShade's next present, so callers watch Status.
    internal static class ReShade
    {
        private const string ModuleName = "GenshinReShadeBridge.addon64";
        private const uint Version = 1;
        private const float LookInterval = 1f;
        // Longer than Windows' MAX_PATH in UTF-8.
        private const int PathBytes = 1024;

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate uint VersionCall();

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void GetStatusCall(out BridgeStatus status);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void SetEffectsCall(uint enabled);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void SaveScreenshotCall();

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate uint GetLastScreenshotCall([Out] byte[] path, uint size);

        private sealed record Bridge(IntPtr Module, GetStatusCall GetStatus, SetEffectsCall SetEffects, SaveScreenshotCall SaveScreenshot,
            GetLastScreenshotCall GetLastScreenshot);

        private static readonly byte[] pathBuffer = new byte[PathBytes];
        private static Bridge bridge;
        // A loaded module that is not a version 1 bridge, so it is reported once and not tried again.
        private static IntPtr rejected;
        private static float nextLook;

        public static bool Connected => bridge != null;

        public static BridgeStatus Status
        {
            get
            {
                if (bridge == null)
                    return default;
                bridge.GetStatus(out var status);
                return status;
            }
        }

        public static string LastScreenshot
        {
            get
            {
                if (bridge == null)
                    return "";
                uint length = bridge.GetLastScreenshot(pathBuffer, PathBytes);
                return Encoding.UTF8.GetString(pathBuffer, 0, (int)Math.Min(length, PathBytes));
            }
        }

        public static void SetEffects(bool enabled) => bridge?.SetEffects(enabled ? 1u : 0u);

        public static void SaveScreenshot() => bridge?.SaveScreenshot();

        // ReShade unloads its add-ons with the last device and loads them again with the next, so the module is looked up
        // every second even while connected: a module that left or moved must not be called through the old pointers.
        public static void Update()
        {
            float now = Time.unscaledTime;
            if (now < nextLook)
                return;
            nextLook = now + LookInterval;
            try
            {
                IntPtr module = GetModuleHandleW(ModuleName);
                if (module == (bridge?.Module ?? rejected))
                    return;
                if (bridge != null)
                    Melon<CameraTools>.Logger.Msg($"ReShade: {ModuleName} was unloaded.");
                bridge = null;
                rejected = IntPtr.Zero;
                if (module != IntPtr.Zero)
                    Connect(module);
            }
            catch (Exception e)
            {
                nextLook = float.PositiveInfinity;
                bridge = null;
                Melon<CameraTools>.Logger.Warning($"ReShade: looking for {ModuleName} failed; CameraTools stops looking. {e}");
            }
        }

        private static void Connect(IntPtr module)
        {
            rejected = module;
            var log = Melon<CameraTools>.Logger;
            if (!TryExport<VersionCall>(module, "BridgeVersion", out var version))
            {
                log.Warning($"ReShade: {ModuleName} is loaded but has no BridgeVersion export.");
                return;
            }
            uint found = version();
            if (found != Version)
            {
                log.Warning($"ReShade: {ModuleName} is bridge version {found}, and CameraTools needs version {Version}; update both together.");
                return;
            }
            if (!TryExport<GetStatusCall>(module, "BridgeGetStatus", out var getStatus)
                || !TryExport<SetEffectsCall>(module, "BridgeSetEffects", out var setEffects)
                || !TryExport<SaveScreenshotCall>(module, "BridgeSaveScreenshot", out var saveScreenshot)
                || !TryExport<GetLastScreenshotCall>(module, "BridgeGetLastScreenshot", out var getLastScreenshot))
            {
                log.Warning($"ReShade: {ModuleName} version {found} is missing an export.");
                return;
            }
            rejected = IntPtr.Zero;
            bridge = new Bridge(module, getStatus, setEffects, saveScreenshot, getLastScreenshot);
            log.Msg($"ReShade: connected to {ModuleName} version {found}; {Status}.");
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
