using System.Runtime.InteropServices;
using UnityEngine;

namespace CameraTools
{
    // Values below 0x10000 are XInput's button bits; LT and RT are the triggers read as buttons.
    [Flags]
    public enum PadButtons : uint
    {
        None = 0,
        DpadUp = 0x0001,
        DpadDown = 0x0002,
        DpadLeft = 0x0004,
        DpadRight = 0x0008,
        Start = 0x0010,
        Back = 0x0020,
        L3 = 0x0040,
        R3 = 0x0080,
        LB = 0x0100,
        RB = 0x0200,
        A = 0x1000,
        B = 0x2000,
        X = 0x4000,
        Y = 0x8000,
        LT = 0x10000,
        RT = 0x20000,
    }

    public enum PadAxis
    {
        None,
        LeftStickUp,
        LeftStickDown,
        LeftStickLeft,
        LeftStickRight,
        RightStickUp,
        RightStickDown,
        RightStickLeft,
        RightStickRight,
        LT,
        RT,
    }

    public readonly record struct PadState(PadButtons Buttons, float LeftX, float LeftY, float RightX, float RightY,
        float LeftTrigger, float RightTrigger)
    {
        public float Axis(PadAxis axis) => axis switch
        {
            PadAxis.LeftStickUp => Math.Max(LeftY, 0f),
            PadAxis.LeftStickDown => Math.Max(-LeftY, 0f),
            PadAxis.LeftStickLeft => Math.Max(-LeftX, 0f),
            PadAxis.LeftStickRight => Math.Max(LeftX, 0f),
            PadAxis.RightStickUp => Math.Max(RightY, 0f),
            PadAxis.RightStickDown => Math.Max(-RightY, 0f),
            PadAxis.RightStickLeft => Math.Max(-RightX, 0f),
            PadAxis.RightStickRight => Math.Max(RightX, 0f),
            PadAxis.LT => LeftTrigger,
            PadAxis.RT => RightTrigger,
            _ => 0f,
        };
    }

    // Either a chord of buttons or one analog axis direction. The default value never fires.
    public readonly record struct PadBinding(PadButtons Chord, PadAxis Axis)
    {
        private static readonly Dictionary<string, PadButtons> ButtonNames = Enum.GetValues<PadButtons>()
            .Where(button => button != PadButtons.None)
            .ToDictionary(button => button.ToString(), StringComparer.OrdinalIgnoreCase);
        private static readonly Dictionary<string, PadAxis> AxisNames = Enum.GetValues<PadAxis>()
            .Where(axis => axis != PadAxis.None)
            .ToDictionary(axis => axis.ToString(), StringComparer.OrdinalIgnoreCase);

        public static bool TryParse(string text, out PadBinding binding)
        {
            binding = default;
            text = text?.Trim() ?? "";
            if (text.Length == 0)
                return true;
            if (AxisNames.TryGetValue(text, out var axis))
            {
                binding = new PadBinding(PadButtons.None, axis);
                return true;
            }
            var chord = PadButtons.None;
            foreach (var part in text.Split('+'))
            {
                if (!ButtonNames.TryGetValue(part.Trim(), out var button))
                    return false;
                chord |= button;
            }
            binding = new PadBinding(chord, PadAxis.None);
            return true;
        }

        public float Value(PadState state) => Axis != PadAxis.None ? state.Axis(Axis) : Held(state) ? 1f : 0f;

        // Back is a modifier: a chord with Back needs it held, and a chord without Back is silent while it is.
        public bool Held(PadState state)
        {
            if (Axis != PadAxis.None)
                return state.Axis(Axis) >= 0.5f;
            return Chord != PadButtons.None
                && (state.Buttons & Chord) == Chord
                && ((Chord & PadButtons.Back) != 0 || (state.Buttons & PadButtons.Back) == 0);
        }

        // A chord fires when one of its own buttons goes down, so releasing Back while A is held does not fire A.
        public bool Pressed(PadState state, PadState previous)
        {
            if (Axis != PadAxis.None)
                return Held(state) && !Held(previous);
            return Held(state) && (state.Buttons & ~previous.Buttons & Chord) != 0;
        }
    }

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
        private static int slot = -1;
        private static float nextScan;
        private static float rumbleUntil;

        public static PadState Current { get; private set; }
        public static PadState Previous { get; private set; }
        public static bool Connected => slot >= 0;

        // Polling an empty slot is slow, so a missing pad is looked for at most every two seconds.
        public static void Poll()
        {
            Previous = Current;
            if (library == Library.Untried)
                LoadLibrary();
            if (library == Library.Missing)
                return;

            if (slot >= 0 && GetState((uint)slot, out var raw) == ErrorSuccess)
            {
                Current = Read(raw);
            }
            else
            {
                slot = -1;
                rumbleUntil = 0;
                Current = default;
                if (Time.unscaledTime >= nextScan)
                    Scan();
            }

            if (rumbleUntil > 0 && Time.unscaledTime >= rumbleUntil)
                StopRumble();
        }

        public static void Rumble()
        {
            if (slot < 0)
                return;
            var vibration = new XInputVibration { leftMotorSpeed = 24000, rightMotorSpeed = 24000 };
            SetState((uint)slot, ref vibration);
            rumbleUntil = Time.unscaledTime + 0.15f;
        }

        public static void StopRumble()
        {
            rumbleUntil = 0;
            if (slot < 0)
                return;
            var vibration = new XInputVibration();
            SetState((uint)slot, ref vibration);
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

        private static void Scan()
        {
            nextScan = Time.unscaledTime + 2f;
            for (uint index = 0; index < 4; index++)
            {
                if (GetState(index, out var raw) == ErrorSuccess)
                {
                    slot = (int)index;
                    Current = Read(raw);
                    CameraTools.LogOnce($"Controller: found a pad in XInput slot {index}.");
                    return;
                }
            }
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
