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
        // A new sky after a teleport or a domain may start without the slot, so a frozen lock is sent again this often.
        private const float ApplyInterval = 1f;

        private static float hour;
        private static IntPtr sky;
        private static float nextApply;

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
            else if (Time.unscaledTime < nextApply && current.Pointer == sky)
                return;
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
            if (current.Pointer != sky)
            {
                if (sky != IntPtr.Zero)
                    Log($"the sky changed; holding {Clock(hour)} on the new one.");
                sky = current.Pointer;
            }
            nextApply = Time.unscaledTime + ApplyInterval;
            Send(current, true);
        }

        // A failing game call unlocks, so it is not repeated every frame.
        private static void Send(EnviroSky current, bool enable)
        {
            try
            {
                current.SetEnvironmentFixTime(Slot, enable, enable ? hour : 0f);
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
