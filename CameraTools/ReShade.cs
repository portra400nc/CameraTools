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

    // A uniform or a technique as the bridge names it: its effect file and its name in it, as UTF-8 with a terminator.
    // The bytes are encoded once, so a call passes them without allocating.
    internal sealed class EffectName
    {
        public readonly byte[] Effect;
        public readonly byte[] Name;

        public EffectName(string effect, string name)
        {
            Effect = Encoding.UTF8.GetBytes(effect + '\0');
            Name = Encoding.UTF8.GetBytes(name + '\0');
        }
    }

    // The only class that knows GenshinReShadeBridge.addon64, the add-on through which a mod drives ReShade. ReShade loads
    // the add-on, some time after CameraTools starts or never, so CameraTools looks for the loaded module and never loads
    // a copy of its own. Every call returns at once and takes effect at ReShade's next present, so callers watch Status.
    internal static class ReShade
    {
        private const string ModuleName = "GenshinReShadeBridge.addon64";
        private const uint Version = 4;
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

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void SetFloatCall([In] byte[] effect, [In] byte[] name, [In] float[] values, uint count);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void SetIntCall([In] byte[] effect, [In] byte[] name, [In] int[] values, uint count);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void SetTechniqueCall([In] byte[] effect, [In] byte[] technique, uint enabled);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate uint GetFloatCall([In] byte[] effect, [In] byte[] name, [Out] float[] values, uint count);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate uint GetIntCall([In] byte[] effect, [In] byte[] name, [Out] int[] values, uint count);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate uint GetTechniqueCall([In] byte[] effect, [In] byte[] technique, out uint enabled);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate uint GetDefinitionCall([In] byte[] effect, [In] byte[] name, [Out] byte[] value, uint size);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void SetOverrideCall([In] byte[] forcedEffects, uint solo);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void SavePresetCall();

        private sealed record Bridge(IntPtr Module, GetStatusCall GetStatus, SetEffectsCall SetEffects, SaveScreenshotCall SaveScreenshot,
            GetLastScreenshotCall GetLastScreenshot, SetFloatCall SetFloat, SetIntCall SetInt, SetTechniqueCall SetTechnique,
            GetFloatCall GetFloat, GetIntCall GetInt, GetTechniqueCall GetTechnique, GetDefinitionCall GetDefinition, SetOverrideCall SetOverride, SavePresetCall SavePreset);

        private static readonly byte[] pathBuffer = new byte[PathBytes];
        // The values of one call. The marshaller pins an array of floats or ints, so a call copies and allocates nothing.
        private static readonly float[] floats = new float[2];
        private static readonly int[] ints = new int[4];
        private static readonly byte[] definitionBuffer = new byte[256];
        private static Bridge bridge;
        // A loaded module that is not a bridge of this version, so it is reported once and not tried again.
        private static IntPtr rejected;
        // The version of a loaded bridge that is too old or too new, or null.
        private static uint? rejectedVersion;
        private static float nextLook;

        public static bool Connected => bridge != null;

        // Why the bridge cannot be used, for a section header, or null while it can.
        public static string Missing => bridge != null ? null
            : rejectedVersion is uint found ? $"ReShade bridge {found} loaded, {Version} needed"
            : "ReShade bridge not found";

        // Rises with every connection. A bridge that ReShade loaded again has forgotten what the last one was told.
        public static int Connections { get; private set; }

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

        // A set is queued and applied at ReShade's next present, or once the effect has loaded. ReShade forgets it when
        // it loads its effects again, as it does after a resize, unless SavePreset wrote it to the preset first.
        public static void SetFloat(EffectName uniform, float x, float y = 0f, uint count = 1)
        {
            if (bridge == null)
                return;
            floats[0] = x;
            floats[1] = y;
            bridge.SetFloat(uniform.Effect, uniform.Name, floats, count);
        }

        public static void SetInt(EffectName uniform, int value)
        {
            if (bridge == null)
                return;
            ints[0] = value;
            bridge.SetInt(uniform.Effect, uniform.Name, ints, 1);
        }

        // A uint4 such as ReLight's light slots takes the same bits through the int setter: ReShade copies them as they are
        // for both int and uint uniforms.
        public static void SetInt4(EffectName uniform, uint x, uint y, uint z, uint w)
        {
            if (bridge == null)
                return;
            ints[0] = unchecked((int)x);
            ints[1] = unchecked((int)y);
            ints[2] = unchecked((int)z);
            ints[3] = unchecked((int)w);
            bridge.SetInt(uniform.Effect, uniform.Name, ints, 4);
        }

        public static void SetTechnique(EffectName technique, bool enabled) => bridge?.SetTechnique(technique.Effect, technique.Name, enabled ? 1u : 0u);

        // A get fails while the bridge has no copy of the value: on the first call for a name, while ReShade loads its
        // effects, and for a name the loaded effects do not have. A value that was set reads back after the present that
        // applied it.
        public static bool TryGetFloat(EffectName uniform, out float x, out float y, uint count = 1)
        {
            x = y = 0f;
            if (bridge == null || bridge.GetFloat(uniform.Effect, uniform.Name, floats, count) == 0)
                return false;
            x = floats[0];
            y = count > 1 ? floats[1] : 0f;
            return true;
        }

        public static bool TryGetInt(EffectName uniform, out int value)
        {
            value = 0;
            if (bridge == null || bridge.GetInt(uniform.Effect, uniform.Name, ints, 1) == 0)
                return false;
            value = ints[0];
            return true;
        }

        public static bool TryGetTechnique(EffectName technique, out bool enabled)
        {
            enabled = false;
            if (bridge == null || bridge.GetTechnique(technique.Effect, technique.Name, out uint on) == 0)
                return false;
            enabled = on != 0;
            return true;
        }

        // A preprocessor definition as the effect sees it, such as RESHADE_DEPTH_LINEARIZATION_FAR_PLANE. It fails like a
        // get, and a name the effect has no definition of reads as an empty string.
        public static bool TryGetDefinition(EffectName definition, out string value)
        {
            value = "";
            if (bridge == null)
                return false;
            uint written = bridge.GetDefinition(definition.Effect, definition.Name, definitionBuffer, (uint)definitionBuffer.Length);
            if (written == 0)
                return false;
            value = Encoding.UTF8.GetString(definitionBuffer, 0, (int)written - 1);
            return true;
        }

        // Keeps every technique of the forced effect files on, and with solo every other technique off, until a call with
        // no effects and solo off puts each technique back as it was. A preset save writes the states from before.
        public static void SetOverride(IEnumerable<string> forcedEffects, bool solo)
            => bridge?.SetOverride(Encoding.UTF8.GetBytes(string.Join('\n', forcedEffects) + '\0'), solo ? 1u : 0u);

        // Writes the uniforms and technique states to ReShade's current preset, after the sets that are still queued.
        public static void SavePreset() => bridge?.SavePreset();

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
                rejectedVersion = null;
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
                rejectedVersion = found;
                log.Warning($"ReShade: {ModuleName} is bridge version {found}, and CameraTools needs version {Version}; update both together.");
                return;
            }
            if (!TryExport<GetStatusCall>(module, "BridgeGetStatus", out var getStatus)
                || !TryExport<SetEffectsCall>(module, "BridgeSetEffects", out var setEffects)
                || !TryExport<SaveScreenshotCall>(module, "BridgeSaveScreenshot", out var saveScreenshot)
                || !TryExport<GetLastScreenshotCall>(module, "BridgeGetLastScreenshot", out var getLastScreenshot)
                || !TryExport<SetFloatCall>(module, "BridgeSetFloat", out var setFloat)
                || !TryExport<SetIntCall>(module, "BridgeSetInt", out var setInt)
                || !TryExport<SetTechniqueCall>(module, "BridgeSetTechnique", out var setTechnique)
                || !TryExport<GetFloatCall>(module, "BridgeGetFloat", out var getFloat)
                || !TryExport<GetIntCall>(module, "BridgeGetInt", out var getInt)
                || !TryExport<GetTechniqueCall>(module, "BridgeGetTechnique", out var getTechnique)
                || !TryExport<GetDefinitionCall>(module, "BridgeGetDefinition", out var getDefinition)
                || !TryExport<SetOverrideCall>(module, "BridgeSetOverride", out var setOverride)
                || !TryExport<SavePresetCall>(module, "BridgeSavePreset", out var savePreset))
            {
                log.Warning($"ReShade: {ModuleName} version {found} is missing an export.");
                return;
            }
            rejected = IntPtr.Zero;
            bridge = new Bridge(module, getStatus, setEffects, saveScreenshot, getLastScreenshot, setFloat, setInt, setTechnique,
                getFloat, getInt, getTechnique, getDefinition, setOverride, savePreset);
            Connections++;
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
