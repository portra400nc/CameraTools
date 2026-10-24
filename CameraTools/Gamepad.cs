using System.Runtime.InteropServices;
using UnityEngine;

namespace CameraTools
{
    public static class Gamepad
    {
        private const int LeftStickDeadzone = 7849;
        private const int RightStickDeadzone = 8689;
        private const int TriggerDeadzone = 30;
        private const uint ErrorSuccess = 0;

        [StructLayout(LayoutKind.Sequential)]
        private struct XInputState
        {
            public uint packetNumber;
            public ushort buttons;
            public byte leftTrigger;
            public byte rightTrigger;
            public short thumbLX;
            public short thumbLY;
            public short thumbRX;
            public short thumbRY;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct XInputVibration
        {
            public ushort leftMotorSpeed;
            public ushort rightMotorSpeed;
        }

        private static class XInput14
        {
            [DllImport("xinput1_4.dll")]
            public static extern uint XInputGetState(uint userIndex, out XInputState state);

            [DllImport("xinput1_4.dll")]
            public static extern uint XInputSetState(uint userIndex, ref XInputVibration vibration);
        }

        private static class XInput910
        {
            [DllImport("xinput9_1_0.dll")]
            public static extern uint XInputGetState(uint userIndex, out XInputState state);

            [DllImport("xinput9_1_0.dll")]
            public static extern uint XInputSetState(uint userIndex, ref XInputVibration vibration);
        }

        private enum Library { Untried, XInput14, XInput910, Missing }

        private static Library library;
        // Every connected pad is read and their input combined, as the game reads them all: a second pad, such as a
        // virtual one that drives the Deck remotely, took slot 0 and moved the Deck's own pad, and reading one slot
        // then missed the other.
        private static readonly bool[] connected = new bool[4];
        private static float nextScan;
        private static float rumbleUntil;

        public static PadState Current { get; private set; }
        public static PadState Previous { get; private set; }
        public static bool Connected => Array.IndexOf(connected, true) >= 0;

        // Polling an empty slot is slow, so a missing pad is looked for at most every two seconds.
        public static void Poll()
        {
            Previous = Current;
            if (library == Library.Untried)
                LoadLibrary();
            if (library == Library.Missing)
                return;

            if (Time.unscaledTime >= nextScan)
                Scan();
            PadState? merged = null;
            for (uint index = 0; index < connected.Length; index++)
            {
                if (!connected[index])
                    continue;
                if (GetState(index, out var raw) != ErrorSuccess)
                {
                    connected[index] = false;
                    continue;
                }
                var state = Read(raw);
                merged = merged is { } other ? Merge(other, state) : state;
            }
            Current = merged ?? default;
            if (merged == null)
                rumbleUntil = 0;

            if (rumbleUntil > 0 && Time.unscaledTime >= rumbleUntil)
                StopRumble();
        }

        public static void Rumble()
        {
            if (!Connected)
                return;
            var vibration = new XInputVibration { leftMotorSpeed = 24000, rightMotorSpeed = 24000 };
            for (uint index = 0; index < connected.Length; index++)
                if (connected[index])
                    SetState(index, ref vibration);
            rumbleUntil = Time.unscaledTime + 0.15f;
        }

        public static void StopRumble()
        {
            rumbleUntil = 0;
            var vibration = new XInputVibration();
            for (uint index = 0; index < connected.Length; index++)
                if (connected[index])
                    SetState(index, ref vibration);
        }

        // Buttons pressed on any pad, the further pushed stick of each pair and the further pulled trigger.
        private static PadState Merge(PadState a, PadState b)
        {
            bool leftB = b.LeftX * b.LeftX + b.LeftY * b.LeftY > a.LeftX * a.LeftX + a.LeftY * a.LeftY;
            bool rightB = b.RightX * b.RightX + b.RightY * b.RightY > a.RightX * a.RightX + a.RightY * a.RightY;
            return new PadState(a.Buttons | b.Buttons, leftB ? b.LeftX : a.LeftX, leftB ? b.LeftY : a.LeftY, rightB ? b.RightX : a.RightX,
                rightB ? b.RightY : a.RightY, Math.Max(a.LeftTrigger, b.LeftTrigger), Math.Max(a.RightTrigger, b.RightTrigger));
        }

        private static void LoadLibrary()
        {
            foreach (var candidate in new[] { Library.XInput14, Library.XInput910 })
            {
                library = candidate;
                try
                {
                    GetState(0, out _);
                    CameraTools.LogOnce($"Controller: XInput loaded from {(candidate == Library.XInput14 ? "xinput1_4.dll" : "xinput9_1_0.dll")}.");
                    return;
                }
                catch (DllNotFoundException) { }
                catch (EntryPointNotFoundException) { }
            }
            library = Library.Missing;
            CameraTools.LogOnce("Controller: neither xinput1_4.dll nor xinput9_1_0.dll loaded; controller support is off.");
        }

        // Empty slots are looked at every two seconds.
        private static void Scan()
        {
            nextScan = Time.unscaledTime + 2f;
            for (uint index = 0; index < connected.Length; index++)
                if (!connected[index] && GetState(index, out _) == ErrorSuccess)
                {
                    connected[index] = true;
                    CameraTools.LogOnce($"Controller: found a pad in XInput slot {index}.");
                }
            if (!Connected)
                CameraTools.LogOnce("Controller: no pad in XInput slots 0-3; looking again every 2 s.");
        }

        private static uint GetState(uint index, out XInputState state)
            => library == Library.XInput14 ? XInput14.XInputGetState(index, out state) : XInput910.XInputGetState(index, out state);

        private static void SetState(uint index, ref XInputVibration vibration)
        {
            if (library == Library.XInput14)
                XInput14.XInputSetState(index, ref vibration);
            else
                XInput910.XInputSetState(index, ref vibration);
        }

        private static PadState Read(XInputState raw)
        {
            var (leftX, leftY) = Stick(raw.thumbLX, raw.thumbLY, LeftStickDeadzone);
            var (rightX, rightY) = Stick(raw.thumbRX, raw.thumbRY, RightStickDeadzone);
            float leftTrigger = Trigger(raw.leftTrigger);
            float rightTrigger = Trigger(raw.rightTrigger);
            var buttons = (PadButtons)raw.buttons;
            if (leftTrigger >= 0.5f)
                buttons |= PadButtons.LT;
            if (rightTrigger >= 0.5f)
                buttons |= PadButtons.RT;
            return new PadState(buttons, leftX, leftY, rightX, rightY, leftTrigger, rightTrigger);
        }

        // Radial deadzone, rescaled so the value starts at 0 just past the deadzone.
        private static (float x, float y) Stick(short x, short y, int deadzone)
        {
            float magnitude = MathF.Sqrt((float)x * x + (float)y * y);
            if (magnitude <= deadzone)
                return (0f, 0f);
            float scaled = Math.Min((magnitude - deadzone) / (32767f - deadzone), 1f);
            return (x / magnitude * scaled, y / magnitude * scaled);
        }

        private static float Trigger(byte value)
            => value <= TriggerDeadzone ? 0f : (value - TriggerDeadzone) / (255f - TriggerDeadzone);
    }
}
