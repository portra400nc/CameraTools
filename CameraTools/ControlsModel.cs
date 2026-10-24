using UnityEngine;

namespace CameraTools
{
    // Member names are the MelonPreferences entry names, so renaming one drops users' saved binding for it.
    public enum CamAction
    {
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
        RecordReplay,
        PlayReplay,
    }

    // Done is false when the change was refused. Swapped is the action that took the old binding, or null.
    public readonly record struct Rebind(bool Done, CamAction? Swapped);

    public static class ControlsModel
    {
        public static readonly (CamAction Action, KeyCode Key, string Pad)[] Defaults =
        {
            (CamAction.ToggleFreecam, KeyCode.Insert, "L3+R3"),
            (CamAction.SetResolutionTo4K, KeyCode.Equals, ""),
            (CamAction.SetResolutionTo1080p, KeyCode.Minus, ""),
            (CamAction.ToggleHUD, KeyCode.H, "B"),
            (CamAction.RemoveHP, KeyCode.LeftBracket, "Back+X"),
            (CamAction.ToggleDamage, KeyCode.RightBracket, "Back+B"),
            (CamAction.SpeedInc1, KeyCode.UpArrow, "DpadRight"),
            (CamAction.SpeedInc5, KeyCode.RightArrow, "Back+DpadUp"),
            (CamAction.SpeedDec1, KeyCode.DownArrow, "DpadLeft"),
            (CamAction.SpeedDec5, KeyCode.LeftArrow, "Back+DpadDown"),
            (CamAction.ToggleSpeedTo5, KeyCode.CapsLock, "Back+Y"),
            (CamAction.TogglePause, KeyCode.Z, "X"),
            (CamAction.ResetSpeed, KeyCode.X, "Y"),
            (CamAction.ToggleGUI, KeyCode.Tab, "Start"),
            (CamAction.ToggleCursorFocus, KeyCode.R, ""),
            (CamAction.RollLeft, KeyCode.Q, "LB"),
            (CamAction.RollRight, KeyCode.E, "RB"),
            (CamAction.ResetRoll, KeyCode.RightShift, "R3"),
            (CamAction.Forward, KeyCode.W, "LeftStickUp"),
            (CamAction.Back, KeyCode.S, "LeftStickDown"),
            (CamAction.Left, KeyCode.A, "LeftStickLeft"),
            (CamAction.Right, KeyCode.D, "LeftStickRight"),
            (CamAction.Up, KeyCode.Space, "RT"),
            (CamAction.Down, KeyCode.LeftControl, "LT"),
            (CamAction.IncreaseFOV, KeyCode.Alpha2, "DpadUp"),
            (CamAction.DecreaseFOV, KeyCode.Alpha1, "DpadDown"),
            (CamAction.ResetFOV, KeyCode.Alpha3, "Back+A"),
            (CamAction.FastMovement, KeyCode.LeftShift, "Back+RB"),
            (CamAction.SlowMovement, KeyCode.LeftAlt, "Back+LB"),
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
            (CamAction.RecordReplay, KeyCode.KeypadMinus, "Back+DpadLeft"),
            (CamAction.PlayReplay, KeyCode.KeypadMultiply, "Back+DpadRight"),
        };

        // Layout 2 moved the camera to WASD and made the free camera's toggle the controller's L3+R3. Files saved before
        // it have no layout entry and read as 1.
        public const int Layout = 2;

        // The layout 1 defaults that layout 2 changed.
        private static readonly (CamAction Action, KeyCode Key, string Pad)[] Layout1 =
        {
            (CamAction.ToggleFreecam, KeyCode.Insert, "A"),
            (CamAction.ToggleHUD, KeyCode.PageDown, "B"),
            (CamAction.TogglePause, KeyCode.Delete, "X"),
            (CamAction.ResetSpeed, KeyCode.End, "Y"),
            (CamAction.ToggleGUI, KeyCode.F10, "Start"),
            (CamAction.ToggleCursorFocus, KeyCode.Quote, ""),
            (CamAction.RollLeft, KeyCode.Comma, "LB"),
            (CamAction.RollRight, KeyCode.Period, "RB"),
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
        };

        // Binds an action's key or controller binding in bindings, indexed by action. A binding another action had swaps:
        // that action takes this one's old binding, and is returned. The free camera's toggle never loses its binding, so
        // the free camera can always be left; such a change is refused and changes nothing.
        public static Rebind Swap<T>(T[] bindings, CamAction action, T binding, T none)
        {
            var same = EqualityComparer<T>.Default;
            int index = (int)action;
            var old = bindings[index];
            if (same.Equals(old, binding))
                return new Rebind(true, null);
            int other = same.Equals(binding, none) ? -1 : Array.FindIndex(bindings, each => same.Equals(each, binding));
            const int toggle = (int)CamAction.ToggleFreecam;
            if (index == toggle && same.Equals(binding, none) || other == toggle && same.Equals(old, none))
                return new Rebind(false, null);
            bindings[index] = binding;
            if (other >= 0)
                bindings[other] = old;
            return new Rebind(true, other >= 0 ? (CamAction)other : null);
        }

        // A binding still at its old default moves to the new one; a binding the user changed stays.
        public static (KeyCode Key, string Pad) Migrate(CamAction action, KeyCode key, string pad, int savedLayout)
        {
            if (savedLayout >= Layout)
                return (key, pad);
            int old = Array.FindIndex(Layout1, row => row.Action == action);
            if (old < 0)
                return (key, pad);
            var (_, newKey, newPad) = Defaults[(int)action];
            return (key == Layout1[old].Key ? newKey : key,
                string.Equals(pad?.Trim(), Layout1[old].Pad, StringComparison.OrdinalIgnoreCase) ? newPad : pad);
        }
    }
}
