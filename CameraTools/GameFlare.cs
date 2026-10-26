using MelonLoader;
using MoleMole;
using UnityEngine;

namespace CameraTools
{
    // The game's own sun flare, a LensFlare that EnviroSky keeps (FDFJLNHBLKA), off while the Enhancements add-on draws
    // CameraTools' lens flare, and back as the game had it once that stops.
    internal static class GameFlare
    {
        // The flare switched off, null while none is.
        private static LensFlare held;
        private static bool reported;

        // Once a frame: whether CameraTools' flare runs.
        public static void Update(bool ours)
        {
            if (!ours)
            {
                if (held != null)
                    held.enabled = true;
                held = null;
                return;
            }
            var sky = EnviroSky.Instance;
            var flare = sky != null ? sky.FDFJLNHBLKA : null;
            if (!reported && sky != null)
            {
                reported = true;
                Melon<CameraTools>.Logger.Msg($"Enhancements: the game's sun flare is {(flare == null ? "absent" : flare.enabled ? "on" : "off")}"
                    + (flare == null ? "" : $", brightness {flare.brightness:0.###}") + "; it stays off while the lens flare runs.");
            }
            if (flare == null || !flare.enabled)
                return;
            flare.enabled = false;
            held = flare;
        }
    }
}
