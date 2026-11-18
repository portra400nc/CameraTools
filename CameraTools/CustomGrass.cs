using System.Runtime.InteropServices;
using System.Text;

namespace CameraTools
{
    // Whether the custom grass files Grass's Load custom texture draws are there: meadow.gpm and meadow.dds in ReShade's
    // Enhancements\grass-port folder, the base path ReShade reports. Looked at again every few seconds, so files copied in
    // while the game runs show; the add-on loads them once, so those need a restart.
    internal static class CustomGrass
    {
        private const float Every = 3f;
        private static float lookedAt = float.NegativeInfinity;
        private static bool? found;

        // null while ReShade's base path is not known.
        public static bool? Found
        {
            get
            {
                if (UnityEngine.Time.unscaledTime >= lookedAt + Every)
                {
                    lookedAt = UnityEngine.Time.unscaledTime;
                    found = BasePath() is { } path ? File.Exists(Path.Combine(path, "Enhancements", "grass-port", "meadow.gpm"))
                        && File.Exists(Path.Combine(path, "Enhancements", "grass-port", "meadow.dds")) : null;
                }
                return found;
            }
        }

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.U1)]
        private delegate bool GetBasePath(byte[] path, ref UIntPtr size);

        [DllImport("kernel32", CharSet = CharSet.Unicode)]
        private static extern IntPtr GetModuleHandleW(string name);

        [DllImport("kernel32", CharSet = CharSet.Ansi)]
        private static extern IntPtr GetProcAddress(IntPtr module, string name);

        // ReShade's ReShadeGetBasePath, from whichever of its usual names it was loaded under.
        private static string BasePath()
        {
            foreach (string name in new[] { "ReShade64.dll", "dxgi.dll", "d3d11.dll" })
            {
                IntPtr module = GetModuleHandleW(name), export = module == IntPtr.Zero ? IntPtr.Zero : GetProcAddress(module, "ReShadeGetBasePath");
                if (export == IntPtr.Zero)
                    continue;
                var get = Marshal.GetDelegateForFunctionPointer<GetBasePath>(export);
                UIntPtr size = UIntPtr.Zero;
                get(null, ref size);
                var bytes = new byte[(int)size.ToUInt64() + 1];
                size = (UIntPtr)bytes.Length;
                if (!get(bytes, ref size))
                    return null;
                return Encoding.UTF8.GetString(bytes).TrimEnd('\0');
            }
            return null;
        }
    }
}
