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
        TakeScreenshot,
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
            (CamAction.IncreaseFOV, KeyCode.Alpha1, "DpadDown"),
            (CamAction.DecreaseFOV, KeyCode.Alpha2, "DpadUp"),
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
            (CamAction.TakeScreenshot, KeyCode.Return, "A"),
        };

        // Layout 2 moved the camera to WASD and made the free camera's toggle the controller's L3+R3. Layout 3 removed the
        // damage numbers and enemy HP bar hotkeys, which the World tab and Hide UI cover. Layout 4 made 1 and the D-pad's
        // down zoom out, and 2 and its up zoom in. Layout 5 added the screenshot on Enter and A. Files saved before layout 2
        // have no layout entry and read as 1.
        public const int Layout = 5;

        // The actions each layout added. Files saved before it may already have their defaults on another action.
        public static readonly (int Layout, CamAction Action)[] Added = { (5, CamAction.TakeScreenshot) };

        // The entries each layout removed, deleted from files saved before it.
        public static readonly (int Layout, string Name)[] Removed = { (2, "Inject"), (3, "RemoveHP"), (3, "ToggleDamage") };

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

        // The layout 3 defaults that layout 4 changed.
        private static readonly (CamAction Action, KeyCode Key, string Pad)[] Layout3 =
        {
            (CamAction.IncreaseFOV, KeyCode.Alpha2, "DpadUp"),
            (CamAction.DecreaseFOV, KeyCode.Alpha1, "DpadDown"),
        };

        // Each layout that changed defaults, with the defaults it changed as they were before it.
        private static readonly (int Layout, (CamAction Action, KeyCode Key, string Pad)[] Before)[] Changes = { (2, Layout1), (4, Layout3) };

        // Enter is Done, Place and Play in the modes that read it themselves, so only the screenshot, which those modes
        // silence, may hold it.
        public static bool Holds(CamAction action, KeyCode key) => key != KeyCode.Return || action == CamAction.TakeScreenshot;

        // Binds an action's key or controller binding in bindings, indexed by action. A binding another action had swaps:
        // that action takes this one's old binding, or none when holds says it may not, and is returned. The free camera's
        // toggle never loses its binding, so the free camera can always be left; such a change is refused and changes
        // nothing, as is a binding holds refuses.
        public static Rebind Swap<T>(T[] bindings, CamAction action, T binding, T none, Func<CamAction, T, bool> holds = null)
        {
            holds ??= (_, _) => true;
            var same = EqualityComparer<T>.Default;
            int index = (int)action;
            var old = bindings[index];
            if (same.Equals(old, binding))
                return new Rebind(true, null);
            if (!holds(action, binding))
                return new Rebind(false, null);
            int other = same.Equals(binding, none) ? -1 : Array.FindIndex(bindings, each => same.Equals(each, binding));
            var given = other >= 0 && holds((CamAction)other, old) ? old : none;
            const int toggle = (int)CamAction.ToggleFreecam;
            if (index == toggle && same.Equals(binding, none) || other == toggle && same.Equals(given, none))
                return new Rebind(false, null);
            bindings[index] = binding;
            if (other >= 0)
                bindings[other] = given;
            return new Rebind(true, other >= 0 ? (CamAction)other : null);
        }

        // An action a later layout added keeps its default binding only where no other action holds it already.
        public static void Settle<T>(T[] bindings, CamAction action, T none)
        {
            var same = EqualityComparer<T>.Default;
            int index = (int)action;
            for (int other = 0; other < bindings.Length; other++)
                if (other != index && same.Equals(bindings[other], bindings[index]))
                    bindings[index] = none;
        }

        // A binding still at the default a later layout changed moves to that layout's default; a binding the user changed
        // stays.
        public static (KeyCode Key, string Pad) Migrate(CamAction action, KeyCode key, string pad, int savedLayout)
        {
            for (int change = 0; change < Changes.Length; change++)
            {
                var (layout, before) = Changes[change];
                int old = Array.FindIndex(before, row => row.Action == action);
                if (savedLayout >= layout || old < 0)
                    continue;
                var (newKey, newPad) = After(action, change);
                key = key == before[old].Key ? newKey : key;
                pad = string.Equals(pad?.Trim(), before[old].Pad, StringComparison.OrdinalIgnoreCase) ? newPad : pad;
            }
            return (key, pad);
        }

        // An action's default once Changes[change] is applied: the one a later change found, or the current one.
        private static (KeyCode Key, string Pad) After(CamAction action, int change)
        {
            for (int later = change + 1; later < Changes.Length; later++)
            {
                int row = Array.FindIndex(Changes[later].Before, each => each.Action == action);
                if (row >= 0)
                    return (Changes[later].Before[row].Key, Changes[later].Before[row].Pad);
            }
            return (Defaults[(int)action].Key, Defaults[(int)action].Pad);
        }
    }
}
