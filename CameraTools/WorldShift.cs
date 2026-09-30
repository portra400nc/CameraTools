using MoleMole;
using UnityEngine;

namespace CameraTools
{
    // Genshin moves its world origin as the player travels, so a scene position is relative to the current shift. Camera
    // path nodes are stored absolute. If the game's conversion fails once, positions are used as they are from then on,
    // so a path recorded and played in one area still works.
    internal static class WorldShift
    {
        private static bool failed;

        public static Vector3 Absolute(Vector3 relative)
        {
            if (failed)
                return relative;
            try
            {
                return WorldShiftManager.GetAbsolutePosition(relative);
            }
            catch (Exception e)
            {
                Fail(e);
                return relative;
            }
        }

        public static Vector3 Relative(Vector3 absolute)
        {
            if (failed)
                return absolute;
            try
            {
                return WorldShiftManager.GetRelativePosition(absolute);
            }
            catch (Exception e)
            {
                Fail(e);
                return absolute;
            }
        }

        private static void Fail(Exception e)
        {
            failed = true;
            CameraTools.LogOnce($"World shift: WorldShiftManager failed ({e.Message}); camera positions are no longer corrected for world shifts.");
        }
    }
}
