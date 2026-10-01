using MelonLoader;
using UnityEngine;

namespace CameraTools
{
    // Member names are the MelonPreferences entry names, so renaming one drops users' saved binding for it.
    public enum CamAction
    {
        Inject,
        ToggleFreecam,
        // Apply resolution slots 2 and 1.
        SetResolutionTo4K,
        SetResolutionTo1080p,
        ToggleHUD,
        RemoveHP,
        ToggleDamage,
        SpeedInc1,
        SpeedInc5,
        SpeedDec1,
        SpeedDec5,
        ToggleSpeedTo5,
        TogglePause,
        ResetSpeed,
        ToggleGUI,
        ToggleCursorFocus,
        RollLeft,
        RollRight,
        ResetRoll,
        Forward,
        Back,
        Left,
        Right,
        Up,
        Down,
        IncreaseFOV,
        DecreaseFOV,
        ResetFOV,
        FastMovement,
        SlowMovement,
        ToggleMaxDetail,
        AddPathNode,
        PlayPath,
        SelectPath1,
        SelectPath2,
        SelectPath3,
        SelectPath4,
        SelectPath5,
        SelectPath6,
        SelectPath7,
        SelectPath8,
        SelectPath9,
        CenterFocusPoint,
    }

    public enum PadOwner
    {
        Game,
        CameraTools,
    }

    public enum InputDevice
    {
        Keyboard,
        Pad,
    }

    public static class Controls
    {
        private static readonly (CamAction action, KeyCode key, string pad)[] Defaults =
        {
            (CamAction.Inject, KeyCode.F9, ""),
            (CamAction.ToggleFreecam, KeyCode.Insert, "A"),
            (CamAction.SetResolutionTo4K, KeyCode.Equals, ""),
            (CamAction.SetResolutionTo1080p, KeyCode.Minus, ""),
            (CamAction.ToggleHUD, KeyCode.PageDown, "B"),
            (CamAction.RemoveHP, KeyCode.LeftBracket, "Back+X"),
            (CamAction.ToggleDamage, KeyCode.RightBracket, "Back+B"),
            (CamAction.SpeedInc1, KeyCode.UpArrow, "DpadRight"),
            (CamAction.SpeedInc5, KeyCode.RightArrow, "Back+DpadUp"),
            (CamAction.SpeedDec1, KeyCode.DownArrow, "DpadLeft"),
            (CamAction.SpeedDec5, KeyCode.LeftArrow, "Back+DpadDown"),
            (CamAction.ToggleSpeedTo5, KeyCode.CapsLock, "Back+Y"),
            (CamAction.TogglePause, KeyCode.Delete, "X"),
            (CamAction.ResetSpeed, KeyCode.End, "Y"),
            (CamAction.ToggleGUI, KeyCode.F10, "Start"),
            (CamAction.ToggleCursorFocus, KeyCode.Quote, ""),
            (CamAction.RollLeft, KeyCode.Comma, "LB"),
            (CamAction.RollRight, KeyCode.Period, "RB"),
            (CamAction.ResetRoll, KeyCode.RightShift, "R3"),
            (CamAction.Forward, KeyCode.I, "LeftStickUp"),
            (CamAction.Back, KeyCode.K, "LeftStickDown"),
            (CamAction.Left, KeyCode.J, "LeftStickLeft"),
            (CamAction.Right, KeyCode.L, "LeftStickRight"),
            (CamAction.Up, KeyCode.O, "RT"),
            (CamAction.Down, KeyCode.U, "LT"),
            (CamAction.IncreaseFOV, KeyCode.Alpha9, "DpadUp"),
            (CamAction.DecreaseFOV, KeyCode.Alpha8, "DpadDown"),
            (CamAction.ResetFOV, KeyCode.Alpha0, "Back+A"),
            (CamAction.FastMovement, KeyCode.RightAlt, "Back+RB"),
            (CamAction.SlowMovement, KeyCode.Semicolon, "Back+LB"),
            (CamAction.ToggleMaxDetail, KeyCode.Home, "Back+Start"),
            (CamAction.AddPathNode, KeyCode.KeypadPlus, "Back+LT"),
            (CamAction.PlayPath, KeyCode.KeypadEnter, "Back+RT"),
            (CamAction.SelectPath1, KeyCode.Keypad1, ""),
            (CamAction.SelectPath2, KeyCode.Keypad2, ""),
            (CamAction.SelectPath3, KeyCode.Keypad3, ""),
            (CamAction.SelectPath4, KeyCode.Keypad4, ""),
            (CamAction.SelectPath5, KeyCode.Keypad5, ""),
            (CamAction.SelectPath6, KeyCode.Keypad6, ""),
            (CamAction.SelectPath7, KeyCode.Keypad7, ""),
            (CamAction.SelectPath8, KeyCode.Keypad8, ""),
            (CamAction.SelectPath9, KeyCode.Keypad9, ""),
            (CamAction.CenterFocusPoint, KeyCode.None, "Back+R3"),
        };

        private static readonly KeyCode[] keys = new KeyCode[Defaults.Length];
        private static readonly PadBinding[] pads = new PadBinding[Defaults.Length];
        private static readonly PadBinding OwnerSwitch = new PadBinding(PadButtons.L3 | PadButtons.R3, PadAxis.None);

        private static InputDevice lastUsed;

        // While the game owns the pad, only the L3+R3 switch is read from it.
        public static PadOwner Owner { get; set; }

        // Hints follow the device used last, like the game's own, but only the keyboard reaches CameraTools while the game
        // owns the pad.
        public static InputDevice Layout => Owner == PadOwner.CameraTools && lastUsed == InputDevice.Pad ? InputDevice.Pad : InputDevice.Keyboard;

        // While a text field takes typing, the keyboard fires no actions, so typing 8 does not narrow the field of view.
        public static bool TextCapture { get; set; }

        // While the settings panel is open it reads the pad itself, so the camera ignores the pad.
        private static bool PadActive => Owner == PadOwner.CameraTools && CameraUi.View != View.Panel;

        public static void Load()
        {
            var keyboard = MelonPreferences.CreateCategory("CameraTools");
            var controller = MelonPreferences.CreateCategory("CameraToolsController");
            for (int i = 0; i < Defaults.Length; i++)
            {
                var (action, key, pad) = Defaults[i];
                if ((int)action != i || !PadBinding.TryParse(pad, out var fallback))
                    throw new InvalidOperationException($"Controls.Defaults row {i} ({action}) is out of CamAction order or has an invalid pad binding.");
                string name = action.ToString();
                keys[i] = keyboard.CreateEntry(name, key).Value;
                string text = controller.CreateEntry(name, pad).Value;
                if (!PadBinding.TryParse(text, out pads[i]))
                {
                    Melon<CameraTools>.Logger.Warning($"CameraToolsController {name} = \"{text}\" is not a controller binding; using \"{pad}\".");
                    pads[i] = fallback;
                }
            }
            GameCamera.Enabled = keyboard.CreateEntry("DetachGameCamera", true,
                description: "Point the game's camera system at a hidden camera while the free camera is on. Set to false if the game misbehaves.").Value;
            if (Defaults.Length != Enum.GetValues<CamAction>().Length)
                throw new InvalidOperationException("Controls.Defaults is missing a CamAction.");
            MelonPreferences.Save();
        }

        // After Gamepad.Poll. Unity may also report a pad button as a key, so the pad wins a tie. On the Deck, holding LB or
        // RB switched the hints to the keyboard until release, so a held button or tilted stick counts as pad use every frame.
        public static void Update()
        {
            if (!TextCapture && Input.anyKeyDown || Input.GetAxis("Mouse X") != 0f || Input.GetAxis("Mouse Y") != 0f)
                lastUsed = InputDevice.Keyboard;
            if (Gamepad.Current != Gamepad.Previous || Gamepad.Current != default)
                lastUsed = InputDevice.Pad;
        }

        public static (KeyCode Key, PadBinding Pad) Binding(CamAction action) => (keys[(int)action], pads[(int)action]);

        public static bool OwnerSwitchPressed => OwnerSwitch.Pressed(Gamepad.Current, Gamepad.Previous);

        public static (float x, float y) Look
            => PadActive && !DepthOfField.Steering ? (Gamepad.Current.RightX, Gamepad.Current.RightY) : (0f, 0f);

        public static bool Pressed(CamAction action, bool keyboard = true)
            => keyboard && !TextCapture && Input.GetKeyDown(keys[(int)action])
                || PadActive && pads[(int)action].Pressed(Gamepad.Current, Gamepad.Previous);

        public static bool Held(CamAction action, bool keyboard = true)
            => keyboard && !TextCapture && Input.GetKey(keys[(int)action])
                || PadActive && pads[(int)action].Held(Gamepad.Current);

        public static float Value(CamAction action, bool keyboard = true)
            => keyboard && !TextCapture && Input.GetKey(keys[(int)action]) ? 1f
                : PadActive ? pads[(int)action].Value(Gamepad.Current) : 0f;
    }
}
