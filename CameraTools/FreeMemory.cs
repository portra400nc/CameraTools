using System.Runtime.InteropServices;
using UnityEngine;

namespace CameraTools
{
    // The RAM free for the game, read at most once a second. The Deck's GPU memory comes out of the same RAM, and a long
    // session's game leaves so little of it that a traced shot's buffers are moved out and tracing slows twenty times.
    internal static class FreeMemory
    {
        // Below this, traced shots on the Deck slowed to a crawl (1.7 GB free on 2026-11-21) until the game restarted.
        private const ulong LowBytes = 3UL << 30;

        private static float nextRead;
        private static bool low;

        public static bool Low
        {
            get
            {
                if (Time.unscaledTime >= nextRead)
                {
                    nextRead = Time.unscaledTime + 1f;
                    var status = new MemoryStatus { Length = (uint)Marshal.SizeOf<MemoryStatus>() };
                    low = GlobalMemoryStatusEx(ref status) && status.AvailablePhysical < LowBytes;
                }
                return low;
            }
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct MemoryStatus
        {
            public uint Length;
            public uint Load;
            public ulong TotalPhysical;
            public ulong AvailablePhysical;
            public ulong TotalPageFile;
            public ulong AvailablePageFile;
            public ulong TotalVirtual;
            public ulong AvailableVirtual;
            public ulong AvailableExtendedVirtual;
        }

        [DllImport("kernel32", SetLastError = true)]
        private static extern bool GlobalMemoryStatusEx(ref MemoryStatus status);
    }
}
