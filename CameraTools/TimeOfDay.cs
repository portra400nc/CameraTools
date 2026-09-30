using System.Runtime.InteropServices;
using MelonLoader;
using MoleMole;
using UnityEngine;
using EnviroTimeType = MoleMole.EnviroSky.BJFJJOABFFC;

namespace CameraTools
{
    // The World tab's time of day. It holds one of the sky's fixed-time slots, which only changes rendering (sky, sun,
    // light, fog); the world's own time, its day and night events, and the server never see it. Not saved.
    internal static class TimeOfDay
    {
        // Above the scene and domain slots and below Cutscene, so cutscenes keep their own lighting and the lock returns
        // after them.
        private const EnviroTimeType Slot = EnviroTimeType.BakeForceTime;
        // The sky's Milky Way shown flag, at +0xEA0 in this game build.
        private const int GalaxyShownFlag = 0xEA0;

        private static float hour;
        // The sky and hour last sent; IntPtr.Zero when nothing is held.
        private static IntPtr sky;
        private static float sentHour;

        public static bool Locked { get; private set; }

        // A multiple of the game's own day, one in-game hour per real minute; 0 holds the hour still.
        public static float Speed { get; private set; }

        public static float Hour
        {
            get
            {
                if (Locked)
                    return hour;
                var current = EnviroSky.Instance;
                return current != null ? current.curTimeOfDay24 : hour;
            }
        }

        public static void SetLocked(bool on)
        {
            if (on)
                Lock(Hour);
            else
                Unlock();
        }

        public static void SetHour(float value) => Lock(value);

        public static void SetSpeed(float value)
        {
            Speed = Math.Max(value, 0f);
            if (Speed > 0f && !Locked)
                Lock(Hour);
        }

        public static void Update()
        {
            if (!Locked)
                return;
            var current = EnviroSky.Instance;
            if (current == null)
                return;
            if (Speed > 0f)
                hour = Wrap(hour + Speed * Time.unscaledDeltaTime / 60f);
            Apply(current);
        }

        // 24 and anything that rounds up to it show as 00:00.
        public static string Clock(float value)
        {
            int minutes = (int)Math.Round(Wrap(value) * 60f) % (24 * 60);
            return $"{minutes / 60:00}:{minutes % 60:00}";
        }

        private static void Lock(float start)
        {
            hour = Wrap(start);
            if (!Locked)
            {
                Locked = true;
                Log($"locked at {Clock(hour)}.");
            }
            var current = EnviroSky.Instance;
            if (current != null)
                Apply(current);
        }

        private static void Unlock()
        {
            if (!Locked)
                return;
            Locked = false;
            sky = IntPtr.Zero;
            Log("unlocked; the sky follows the server's time.");
            var current = EnviroSky.Instance;
            if (current != null)
                Send(current, false);
        }

        private static void Apply(EnviroSky current)
        {
            if (!NeedsSend(current))
                return;
            if (current.Pointer != sky)
            {
                if (sky != IntPtr.Zero)
                    Log($"the sky changed; holding {Clock(hour)} on the new one.");
                sky = current.Pointer;
            }
            sentHour = hour;
            Send(current, true);
        }

        // Every send refreshes the sky, so send only what it lacks: a new sky, a new hour, or our slot lost to a reset.
        // The sky reports its highest enabled slot, so a Cutscene or MoonCannon above ours is left alone; ours shows
        // again when they release the sky.
        private static bool NeedsSend(EnviroSky current) =>
            current.Pointer != sky || hour != sentHour || current.enviroTimeType < Slot;

        // A failing game call unlocks, so it is not repeated every frame.
        private static void Send(EnviroSky current, bool enable)
        {
            try
            {
                var components = current.Components;
                var galaxy = components != null ? components.Galaxy : null;
                bool galaxyShown = galaxy != null && galaxy.activeSelf;
                bool wasNight = current.IsNight;
                byte shownFlag = Marshal.ReadByte(current.Pointer, GalaxyShownFlag);
                current.SetEnvironmentFixTime(Slot, enable, enable ? hour : 0f);
                // Each send re-rolls the Milky Way, but the game rolls once at nightfall, so keep only a nightfall's roll.
                if (!wasNight && current.IsNight)
                    return;
                Marshal.WriteByte(current.Pointer, GalaxyShownFlag, shownFlag);
                if (galaxy != null && galaxy.activeSelf != galaxyShown)
                    galaxy.SetActive(galaxyShown);
            }
            catch (Exception e)
            {
                Locked = false;
                sky = IntPtr.Zero;
                Melon<CameraTools>.Logger.Warning($"Time of day: SetEnvironmentFixTime({enable}) failed ({e.Message}); unlocked.");
            }
        }

        private static float Wrap(float value) => (value % 24f + 24f) % 24f;

        private static void Log(string message) => Melon<CameraTools>.Logger.Msg($"Time of day: {message}");
    }
}
