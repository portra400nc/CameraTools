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

        // The text TryParse reads back, with the Back modifier first as the defaults write it.
        public override string ToString()
        {
            var chord = Chord;
            return Axis != PadAxis.None ? Axis.ToString()
                : string.Join("+", Enum.GetValues<PadButtons>()
                    .Where(button => button != PadButtons.None && (chord & button) != 0)
                    .OrderBy(button => button != PadButtons.Back));
        }

        public float Value(PadState state) => Axis != PadAxis.None ? AxisValue(state) : Held(state) ? 1f : 0f;

        // Back is a modifier: a chord with Back needs it held, and a chord without Back is silent while it is.
        public bool Held(PadState state)
        {
            if (Axis != PadAxis.None)
                return AxisValue(state) >= 0.5f;
            return Chord != PadButtons.None
                && (state.Buttons & Chord) == Chord
                && ((Chord & PadButtons.Back) != 0 || (state.Buttons & PadButtons.Back) == 0);
        }

        // A trigger is also a button in Back chords such as Back+LT, so its axis reads 0 while Back is held. The sticks keep
        // working, because Back+LB slow movement is used with the left stick.
        private float AxisValue(PadState state)
            => Axis is PadAxis.LT or PadAxis.RT && (state.Buttons & PadButtons.Back) != 0 ? 0f : state.Axis(Axis);

        // A chord fires when one of its own buttons goes down, so releasing Back while A is held does not fire A.
        public bool Pressed(PadState state, PadState previous)
        {
            if (Axis != PadAxis.None)
                return Held(state) && !Held(previous);
            return Held(state) && (state.Buttons & ~previous.Buttons & Chord) != 0;
        }
    }
}
