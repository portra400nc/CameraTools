using MelonLoader;
using UnityEngine;

namespace CameraTools
{
    // Member names are the MelonPreferences entry names, so renaming one drops users' saved binding for it.
    public enum CamAction
    {
        Inject,
        ToggleFreecam,
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
    }

    public enum PadOwner
    {
        Game,
        CameraTools,
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
        };

        private static readonly KeyCode[] keys = new KeyCode[Defaults.Length];
        private static readonly PadBinding[] pads = new PadBinding[Defaults.Length];
        private static readonly PadBinding OwnerSwitch = new PadBinding(PadButtons.L3 | PadButtons.R3, PadAxis.None);

        // While the game owns the pad, only the L3+R3 switch is read from it.
        public static PadOwner Owner { get; set; }

        private static bool PadActive => Owner == PadOwner.CameraTools;

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

        public static bool OwnerSwitchPressed => OwnerSwitch.Pressed(Gamepad.Current, Gamepad.Previous);

        public static (float x, float y) Look
            => PadActive ? (Gamepad.Current.RightX, Gamepad.Current.RightY) : (0f, 0f);

        public static bool Pressed(CamAction action, bool keyboard = true)
            => keyboard && Input.GetKeyDown(keys[(int)action])
                || PadActive && pads[(int)action].Pressed(Gamepad.Current, Gamepad.Previous);

        public static bool Held(CamAction action, bool keyboard = true)
            => keyboard && Input.GetKey(keys[(int)action])
                || PadActive && pads[(int)action].Held(Gamepad.Current);

        public static float Value(CamAction action, bool keyboard = true)
            => keyboard && Input.GetKey(keys[(int)action]) ? 1f
                : PadActive ? pads[(int)action].Value(Gamepad.Current) : 0f;
    }
}
